#include "machine.hpp"

#include "amd64/amd64.hpp"
#include "amd64/gdt.hpp"
#include "amd64/idt.hpp"
#include "amd64/memory_layout.hpp"
#include <linux/kvm.h>
#include <sys/ioctl.h>
#include <time.h>
#include <signal.h>

#define PRINTER(printer, buffer, fmt, ...) \
	printer(buffer, \
		snprintf(buffer, sizeof(buffer), \
		fmt, ##__VA_ARGS__));
extern "C" int gettid();

namespace tinykvm {
	thread_local bool timer_was_triggered = false;
}
extern "C"
void tinykvm_timer_signal_handler(int sig) {
	// The idea is that we will not migrate this VM while
	// it is running. This allows using TLS to determine if
	// the timer already expired.
	if (sig == SIGUSR2) {
		tinykvm::timer_was_triggered = true;
	}
}

namespace tinykvm {
	static constexpr bool VERBOSE_TIMER = false;

void vCPU::run(uint32_t ticks)
{
	timer_was_triggered = false;
	this->timer_ticks = ticks;
	if (timer_ticks != 0) {
		const struct itimerspec its {
			/* Interrupt every 20ms after timeout. This makes sure
			   that we will eventually exit all blocking calls and
			   at the end exit KVM_RUN to timeout the request. If
			   there is a blocking loop that doesn't exit properly,
			   the 20ms recurring interruption should not cause too
			   much wasted CPU-time. */
			.it_interval = {
				.tv_sec = 0, .tv_nsec = 20'000'000L
			},
			/* The execution timeout. */
			.it_value = {
				.tv_sec = ticks / 1000,
				.tv_nsec = (ticks % 1000) * 1000000L
			}
		};
		timer_settime(this->timer_id, 0, &its, nullptr);
		if constexpr (VERBOSE_TIMER) {
			printf("Timer %p enabled\n", timer_id);
		}
	}

	/* When an exception happens during KVM_RUN, we will need to
	   intercept it, in order to disable the timeout timer.
	   TODO: Convert timer disable to local destructor. */
	try {
		this->stopped = false;
		while(run_once());
	} catch (...) {
		disable_timer();
		throw;
	}

	disable_timer();
}
void vCPU::disable_timer()
{
	timer_was_triggered = false;
	if (timer_ticks != 0) {
		this->timer_ticks = 0;
		struct itimerspec its;
		__builtin_memset(&its, 0, sizeof(its));
		timer_settime(this->timer_id, 0, &its, nullptr);
		if constexpr (VERBOSE_TIMER) {
			printf("Timer %p disabled\n", timer_id);
		}
	}
}

long vCPU::run_once()
{
	int result = ioctl(this->fd, KVM_RUN, 0);
	// Handle potential KVM_RUN failure or execution timeout
	if (UNLIKELY(result < 0)) {
		if (this->timer_ticks) {
			if constexpr (VERBOSE_TIMER) {
				printf("Timer %p triggered\n", timer_id);
			}
			Machine::timeout_exception("Timeout Exception", this->timer_ticks);
		} else if (errno == EINTR) {
			/* XXX: EINTR but not a timeout, return to execution? */
			return KVM_EXIT_UNKNOWN;
		}
		else {
			Machine::machine_exception("KVM_RUN failed");
		}
	} else if (this->timer_ticks) {
		// Occasionally we miss timer interruptions, and we must catch it via TLS.
		if (UNLIKELY(timer_was_triggered)) {
			Machine::timeout_exception("Timeout Exception", this->timer_ticks);
		}
	}

	// Validate the integrity of the guests kernel space
	const auto& sregs = get_special_registers();
	const auto& memory = machine().main_memory();

	if (UNLIKELY(
		sregs.cr3 != machine().memory.page_tables
		|| sregs.gdt.base != memory.physbase + GDT_ADDR
		|| sregs.idt.base != memory.physbase + IDT_ADDR
		// Doesn't work on other vCPUs: || sregs.tr.base != TSS_ADDR
		)) {
		this->print_registers();
		Machine::machine_exception("Kernel integrity loss detected");
	}

	// Handle the KVM guest exit reason
	switch (kvm_run->exit_reason) {
	case KVM_EXIT_HLT:
		Machine::machine_exception("Halt from kernel space", 5);

	case KVM_EXIT_DEBUG:
		return KVM_EXIT_DEBUG;

	case KVM_EXIT_FAIL_ENTRY:
		Machine::machine_exception("Failed to start guest! Misconfigured?", KVM_EXIT_FAIL_ENTRY);

	case KVM_EXIT_SHUTDOWN:
		Machine::machine_exception("Shutdown! Triple fault?", 32);

	case KVM_EXIT_IO:
		if (kvm_run->io.direction == KVM_EXIT_IO_OUT) {
		if (kvm_run->io.port == 0x0) {
			const char* data = ((char *)kvm_run) + kvm_run->io.data_offset;
			const uint32_t intr = *(uint32_t *)data;
			if (intr != 0xFFFF) {
				machine().system_call(*this, intr);
				if (this->stopped) return 0;
				return KVM_EXIT_IO;
			} else {
				this->stopped = true;
				return 0;
			}
		}
		else if (kvm_run->io.port >= 0x80 && kvm_run->io.port < 0x100) {
			auto intr = kvm_run->io.port - 0x80;
			if (intr == 14) // Page fault
			{
				const auto& regs = registers();
				const uint64_t addr = regs.rdi & ~(uint64_t) 0x8000000000000FFF;
#ifdef VERBOSE_PAGE_FAULTS
				char buffer[256];
				#define PV(val, off) \
					{ uint64_t value; machine().unsafe_copy_from_guest(&value, regs.rsp + off, 8); \
					PRINTER(machine().m_printer, buffer, "Value %s: 0x%lX\n", val, value); }
				try {
					PV("Origin SS",  48);
					PV("Origin RSP", 40);
					PV("Origin RFLAGS", 32);
					PV("Origin CS",  24);
					PV("Origin RIP", 16);
					PV("Error code", 8);
				} catch (...) {}
				PRINTER(machine().m_printer, buffer,
					"*** %s on address 0x%lX (0x%llX)\n",
					amd64_exception_name(intr), addr, regs.rdi);
#endif
				/* Page fault handling */
				/* We should be in kernel mode, otherwise it's fishy! */
				auto& memory = machine().main_memory();
				if (UNLIKELY(regs.rip >= memory.physbase + INTR_ASM_ADDR+0x1000)
					|| sregs.cs.dpl != 0x0 || sregs.ss.dpl != 0x0) {
					Machine::machine_exception("Security violation", intr);
				}
				/* Remote call handling */
				if (UNLIKELY(machine().is_remote_access(addr)))
				{
					this->handle_remote_call(regs.rdi & ~(uint64_t) 0x8000000000000000);
					return KVM_EXIT_IO;
				}

				machine().memory.get_writable_page(addr, PDE64_USER | PDE64_RW, false);
				return KVM_EXIT_IO;
			}
			else if (intr == 1) /* Debug trap */
			{
				machine().m_on_breakpoint(*this);
				return KVM_EXIT_IO;
			}
			/* CPU Exception */
			this->handle_exception(intr);
			Machine::machine_exception(amd64_exception_name(intr), intr);
		} else {
			/* Custom Output handler */
			const char* data = ((char *)kvm_run) + kvm_run->io.data_offset;
			machine().m_on_output(*this, kvm_run->io.port, *(uint32_t *)data);
		}
		} else { // IN
			/* Custom Input handler */
			const char* data = ((char *)kvm_run) + kvm_run->io.data_offset;
			machine().m_on_input(*this, kvm_run->io.port, *(uint32_t *)data);
		}
		if (this->stopped) return 0;
		return KVM_EXIT_IO;

	case KVM_EXIT_MMIO: {
			const uint64_t addr = kvm_run->mmio.phys_addr;
			char buffer[256];
			PRINTER(machine().m_printer, buffer,
				"Write outside of physical memory at 0x%lX\n",
				addr);
			Machine::machine_exception(
				"Memory write outside physical memory (out of memory?)",
				addr);
		}
	case KVM_EXIT_INTERNAL_ERROR:
		Machine::machine_exception("KVM internal error");
	}
	char buffer[256];
	PRINTER(machine().m_printer, buffer,
		"Unexpected exit reason %d\n", kvm_run->exit_reason);
	Machine::machine_exception("Unexpected KVM exit reason",
							   kvm_run->exit_reason);
}

TINYKVM_COLD()
long Machine::step_one()
{
	struct kvm_guest_debug dbg;
	dbg.control = KVM_GUESTDBG_ENABLE | KVM_GUESTDBG_SINGLESTEP;

	if (ioctl(vcpu.fd, KVM_SET_GUEST_DEBUG, &dbg) < 0) {
		Machine::machine_exception("KVM_RUN failed");
	}

	return vcpu.run_once();
}

TINYKVM_COLD()
long Machine::run_with_breakpoints(std::array<uint64_t, 4> bp)
{
	struct kvm_guest_debug dbg {};

	dbg.control = KVM_GUESTDBG_ENABLE | KVM_GUESTDBG_USE_HW_BP;
	for (size_t i = 0; i < bp.size(); i++) {
		dbg.arch.debugreg[i] = bp[i];
		if (bp[i] != 0x0)
			dbg.arch.debugreg[7] |= 0x3 << (2 * i);
	}
	//printf("Continue with BPs at 0x%lX, 0x%lX, 0x%lX and 0x%lX\n",
	//	bp[0], bp[1], bp[2], bp[3]);

	if (ioctl(vcpu.fd, KVM_SET_GUEST_DEBUG, &dbg) < 0) {
		Machine::machine_exception("KVM_RUN failed");
	}

	return vcpu.run_once();
}

TINYKVM_COLD()
void vCPU::print_registers() const
{
	const auto& sregs = this->get_special_registers();
	const auto& printer = machine().m_printer;

	char buffer[1024];
	PRINTER(printer, buffer,
		"CR0: 0x%llX  CR3: 0x%llX\n", sregs.cr0, sregs.cr3);
	PRINTER(printer, buffer,
		"CR2: 0x%llX  CR4: 0x%llX\n", sregs.cr2, sregs.cr4);

	const auto& regs = registers();
	PRINTER(printer, buffer,
		"RAX: 0x%llX  RBX: 0x%llX  RCX: 0x%llX\n", regs.rax, regs.rbx, regs.rcx);
	PRINTER(printer, buffer,
		"RDX: 0x%llX  RSI: 0x%llX  RDI: 0x%llX\n", regs.rdx, regs.rsi, regs.rdi);
	PRINTER(printer, buffer,
		"RIP: 0x%llX  RBP: 0x%llX  RSP: 0x%llX\n", regs.rip, regs.rbp, regs.rsp);

	PRINTER(printer, buffer,
		"SS: 0x%X  CS: 0x%X  DS: 0x%X  FS: 0x%X  GS: 0x%X\n",
		sregs.ss.selector, sregs.cs.selector, sregs.ds.selector, sregs.fs.selector, sregs.gs.selector);

#if 0
	PRINTER(printer, buffer,
		"CR0 PE=%llu MP=%llu EM=%llu\n",
		sregs.cr0 & 1, (sregs.cr0 >> 1) & 1, (sregs.cr0 >> 2) & 1);
	PRINTER(printer, buffer,
		"CR4 OSFXSR=%llu OSXMMEXCPT=%llu OSXSAVE=%llu\n",
		(sregs.cr4 >> 9) & 1, (sregs.cr4 >> 10) & 1, (sregs.cr4 >> 18) & 1);
#endif
#if 0
	printf("IDT: 0x%llX (Size=%x)\n", sregs.idt.base, sregs.idt.limit);
	print_exception_handlers(machine().main_memory().at(sregs.idt.base));
#endif
#if 0
	print_gdt_entries(machine().main_memory().at(sregs.gdt.base), 7);
#endif
}

TINYKVM_COLD()
void vCPU::handle_exception(uint8_t intr)
{
	const auto& regs = registers();
	char buffer[1024];
	// Page fault
	const auto& printer = machine().m_printer;
	if (intr == 14) {
		auto& sregs = this->get_special_registers();
		PRINTER(printer, buffer,
			"*** %s on address 0x%llX\n",
			amd64_exception_name(intr), sregs.cr2);
		uint64_t code;
		machine().unsafe_copy_from_guest(&code, regs.rsp+8,  8);
		PRINTER(printer, buffer,
			"Error code: 0x%lX (%s)\n", code,
			(code & 0x02) ? "memory write" : "memory read");
		if (code & 0x01) {
			PRINTER(printer, buffer,
				"* Protection violation\n");
		} else {
			PRINTER(printer, buffer,
				"* Page not present\n");
		}
		if (code & 0x02) {
			PRINTER(printer, buffer,
				"* Invalid write on page\n");
		}
		if (code & 0x04) {
			PRINTER(printer, buffer,
				"* CPL=3 Page fault\n");
		}
		if (code & 0x08) {
			PRINTER(printer, buffer,
				"* Page contains invalid bits\n");
		}
		if (code & 0x10) {
			PRINTER(printer, buffer,
				"* Instruction fetch failed (NX-bit was set)\n");
		}
	} else {
		PRINTER(printer, buffer,
			"*** CPU EXCEPTION: %s (code: %s)\n",
			amd64_exception_name(intr),
			amd64_exception_code(intr) ? "true" : "false");
	}
	this->print_registers();
	//print_pagetables(memory);
	const bool has_code = amd64_exception_code(intr);

	try {
		uint64_t off = (has_code) ? (regs.rsp+8) : (regs.rsp+0);
		if (intr == 14) off += 8;
		uint64_t rip, rfl, cs = 0x0, rsp, ss;
		try {
			machine().unsafe_copy_from_guest(&rip, off+0,  8);
			machine().unsafe_copy_from_guest(&cs,  off+8,  8);
			machine().unsafe_copy_from_guest(&rfl, off+16, 8);
			machine().unsafe_copy_from_guest(&rsp, off+24, 8);
			machine().unsafe_copy_from_guest(&ss,  off+32, 8);

			PRINTER(printer, buffer,
				"Failing RIP: 0x%lX\n", rip);
			PRINTER(printer, buffer,
				"Fail RFLAGS: 0x%lX\n", rfl);
			PRINTER(printer, buffer,
				"Failing CS:  0x%lX\n", cs);
			PRINTER(printer, buffer,
				"Failing RSP: 0x%lX\n", rsp);
			PRINTER(printer, buffer,
				"Failing SS:  0x%lX\n", ss);

			PRINTER(printer, buffer,
				"RIP  0x%lX   %s\n",
				rip, machine().resolve(rip).c_str());

		} catch (...) {}

		/* General Protection Fault */
		if (has_code && intr == 13) {
			uint64_t code = 0x0;
			try {
				machine().unsafe_copy_from_guest(&code,  regs.rsp, 8);
			} catch (...) {}
			if (code != 0x0) {
				PRINTER(printer, buffer,
					"Reason: Failing segment 0x%lX\n", code);
			} else if (cs & 0x3) {
				/* Best guess: Privileged instruction */
				PRINTER(printer, buffer,
					"Reason: Executing a privileged instruction\n");
			} else {
				/* Kernel GPFs should be exceedingly rare */
				PRINTER(printer, buffer,
					"Reason: Protection fault in kernel mode\n");
			}
		}
	} catch (...) {}
}

void Machine::migrate_to_this_thread()
{
	timer_delete(vcpu.timer_id);
	vcpu.timer_id = create_vcpu_timer();
}

} // tinykvm
