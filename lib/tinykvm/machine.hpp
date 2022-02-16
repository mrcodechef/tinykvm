#pragma once
#include "common.hpp"
#include "forward.hpp"
#include "memory.hpp"
#include "memory_bank.hpp"
#include "util/threadpool.h"
#include <array>
#include <cassert>
#include <deque>
#include <functional>
#include <memory>
#include <vector>

namespace tinykvm {

struct Machine
{
	using address_t = uint64_t;
	using syscall_t = void(*)(Machine&);
	using numbered_syscall_t = void(*)(Machine&, unsigned);
	using io_callback_t = void(*)(Machine&, unsigned, unsigned);
	using printer_func = std::function<void(const char*, size_t)>;

	/* Setup Linux env and run through main */
	void setup_argv(const std::vector<std::string>& args,
					const std::vector<std::string>& env = {});
	void setup_linux(const std::vector<std::string>& args,
					const std::vector<std::string>& env = {});
	void run(float timeout_secs = 0.f);

	/* Make a function call into the VM */
	template <typename... Args>
	void vmcall(const char*, Args&&...);
	template <typename... Args> constexpr
	void vmcall(address_t, Args&&...);
	template <typename... Args> constexpr
	void timed_vmcall(address_t, float timeout, Args&&...);
	/* Retrieve optional return value from a vmcall */
	long return_value() const;

	template <typename... Args>
	void timed_smpcall(size_t cpus,
		address_t stack, uint32_t stack_size,
		address_t addr, float tmo, Args&&...);
	void timed_smpcall_array(size_t cpus,
		address_t stack, uint32_t stack_size,
		address_t addr, float tmo,
		address_t array, uint32_t array_item_size);
	void timed_smpcall_clone(size_t num_cpus,
		address_t stack_base, uint32_t stack_size,
		float timeout, const tinykvm_x86regs& regs);

	bool is_forkable() const noexcept { return m_prepped; }
	void stop(bool = true);
	bool stopped() const noexcept { return vcpu.stopped; }
	void reset_to(Machine&, const MachineOptions&);

	/* When zeroes == true, new pages will be zeroed instead of duplicated */
	void copy_to_guest(address_t addr, const void*, size_t, bool zeroes = false);
	void copy_from_guest(void* dst, address_t addr, size_t);
	void unsafe_copy_from_guest(void* dst, address_t addr, size_t);
	/* Fill an array of buffers pointing to complete guest virtual [addr, len].
	   Throws an exception if there was a protection violation.
	   Returns the number of buffers filled, or an exception if not enough. */
	struct Buffer { const char* ptr; size_t len; };
	size_t gather_buffers_from_range(size_t cnt, Buffer[], address_t addr, size_t len);
	/* Efficiently copy between machines */
	void copy_from_machine(address_t dst, Machine& src, address_t sa, size_t size);

	template <typename T>
	uint64_t stack_push(__u64& sp, const T&);
	uint64_t stack_push(__u64& sp, const void*, size_t);
	uint64_t stack_push(__u64& sp, const std::string&);
	uint64_t stack_push_cstr(__u64& sp, const char*);

	/* Debugging */
	long step_one();
	long run_with_breakpoints(std::array<uint64_t, 4> bps);

	tinykvm_x86regs registers() const;
	void set_registers(const tinykvm_x86regs&);
	void get_special_registers(struct kvm_sregs&) const;
	void set_special_registers(const struct kvm_sregs&);
	std::pair<__u64, __u64> get_fsgs() const;
	void set_tls_base(__u64 baseaddr);

	static void install_syscall_handler(unsigned idx, syscall_t h) { m_syscalls.at(idx) = h; }
	static void install_unhandled_syscall_handler(numbered_syscall_t h) { m_unhandled_syscall = h; }
	static auto get_syscall_handler(unsigned idx) { return m_syscalls.at(idx); }
	void system_call(unsigned);
	static void install_input_handler(io_callback_t h) { m_on_input = h; }
	static void install_output_handler(io_callback_t h) { m_on_output = h; }

	template <typename T> void set_userdata(T* data) { m_userdata = data; }
	template <typename T> T* get_userdata() { return static_cast<T*> (m_userdata); }

	std::string_view memory_at(uint64_t a, size_t s) const;
	template <typename T = char>
	T* rw_memory_at(uint64_t a, size_t s);
	bool memory_safe_at(uint64_t a, size_t s) const;
	char* unsafe_memory_at(uint64_t a, size_t s) { return memory.at(a, s); }
	uint64_t translate(uint64_t virt) const;

	address_t start_address() const noexcept { return this->m_start_address; }
	address_t stack_address() const noexcept { return this->m_stack_address; }
	address_t heap_address() const noexcept { return this->m_heap_address; }
	address_t entry_address() const noexcept;
	address_t exit_address() const noexcept;
	void set_stack_address(address_t addr) { this->m_stack_address = addr; }
	address_t kernel_end_address() const noexcept { return m_kernel_end; }
	address_t mmap_start() const noexcept { return this->m_heap_address + BRK_MAX; }
	address_t max_address() const noexcept { return memory.physbase + memory.size; }
	address_t mmap_allocate(uint64_t addr, size_t bytes, int prot = 0, int flags = 0);
	static constexpr uint64_t BRK_MAX = 0x100000;

	uint64_t address_of(const char*) const;

	bool smp_active() const noexcept { return m_smp_active != 0; }
	int  smp_active_count() const noexcept { return m_smp_active; }
	void smp_wait();
	/* Retrieve return values from a smpcall */
	std::vector<long> gather_return_values();

	bool has_threads() const noexcept { return m_mt != nullptr; }
	const struct MultiThreading& threads() const;
	struct MultiThreading& threads();
	static void setup_multithreading();

	const auto& mmap() const { return m_mm; }
	auto& mmap() { return m_mm; }

	void set_printer(printer_func pf = m_default_printer) { m_printer = std::move(pf); }
	void print(const char*, size_t);
	void print_registers() { vcpu.print_registers(); }
	void print_pagetables() const;
	void print_exception_handlers() const;

	void install_memory(uint32_t idx, const VirtualMem&);
	void delete_memory(uint32_t idx);
	std::string_view binary() const noexcept { return m_binary; }

	template <typename... Args> constexpr
	void setup_call(tinykvm_x86regs&, uint64_t addr, uint32_t tix, uint64_t rsp, Args&&... args);
	void setup_clone(tinykvm_x86regs&, address_t stack);
	void prepare_copy_on_write();
	bool is_forked() const noexcept { return m_forked; }
	static void init();
	Machine(const std::vector<uint8_t>& binary, const MachineOptions&);
	Machine(std::string_view binary, const MachineOptions&);
	Machine(const Machine& other, const MachineOptions&);
	~Machine();

private:
	struct vCPU {
		void init(int id, Machine&, const MachineOptions&);
		void smp_init(int id, Machine&);
		void deinit();
		tinykvm_x86regs registers() const;
		void assign_registers(const struct tinykvm_x86regs&);
		void get_special_registers(struct kvm_sregs&) const;
		void set_special_registers(const struct kvm_sregs&);

		void run(uint32_t tix);
		long run_once();
		std::string_view io_data() const;

		void print_registers();
		void handle_exception(uint8_t intr);
		void decrement_smp_count();

		int fd = 0;
		int cpu_id = 0;
		bool stopped = true;
		uint32_t timer_ticks = 0;
	private:
		struct kvm_run *kvm_run = nullptr;
		Machine* machine = nullptr;
	};
	void setup_registers(tinykvm_x86regs&);
	void setup_argv(__u64&, const std::vector<std::string>&, const std::vector<std::string>&);
	void setup_linux(__u64&, const std::vector<std::string>&, const std::vector<std::string>&);
	void elf_loader(const MachineOptions&);
	void elf_load_ph(const MachineOptions&, const void*);
	void relocate_section(const char* section_name, const char* sym_section);
	void setup_long_mode(const Machine* other, const MachineOptions&);
	void prepare_cpus(size_t num_cpus);
	vCPU& smp_cpu(size_t idx);
	[[noreturn]] static void machine_exception(const char*, uint64_t = 0);
	[[noreturn]] static void timeout_exception(const char*, uint32_t = 0);

	vCPU  vcpu;
	int   fd = 0;
	bool  m_prepped = false;
	bool  m_forked = false;
	int   m_smp_active = 0;
	void* m_userdata = nullptr;

	std::string_view m_binary;

	vMemory memory;  // guest memory

	uint64_t m_stack_address;
	uint64_t m_heap_address;
	uint64_t m_start_address;
	uint64_t m_kernel_end;

	struct mm_map
	{
		uint64_t start; // start of the machine virtual address
		uint64_t end; // end of the machine virtual address
		uint64_t gva; // guest virtual address
		size_t size; // map size
		int prot;
		int flags;
	};

	std::vector<mm_map> mm_maps = {};
	uint32_t   m_slots = 0; //
	uint64_t m_mm = 0; // mmap address
	size_t   m_size = 0; // mmap size

	mutable std::unique_ptr<MultiThreading> m_mt;
	struct kvm_sregs* cached_sregs = nullptr;

	struct MPvCPU_data {
		vCPU* vcpu = nullptr;
		uint32_t ticks = 0;
		struct tinykvm_x86regs regs;
	};
	struct MPvCPU {
		void blocking_message(std::function<void(vCPU&)>);
		void async_exec(struct MPvCPU_data&);

		MPvCPU(int, Machine&, const struct kvm_sregs&);
		~MPvCPU();
		vCPU cpu;
		ThreadPool thpool;
	};
	std::deque<MPvCPU> m_cpus;
	std::vector<const struct MPvCPU_data*> m_smp_data;
	std::mutex m_smp_data_mtx;
	MPvCPU_data* smp_allocate_vcpu_data(size_t);

	/* How to print exceptions, register dumps etc. */
	printer_func m_printer = m_default_printer;

	static std::array<syscall_t, TINYKVM_MAX_SYSCALLS> m_syscalls;
	static numbered_syscall_t m_unhandled_syscall;
	static syscall_t          m_on_breakpoint;
	static io_callback_t      m_on_input;
	static io_callback_t      m_on_output;
	static printer_func       m_default_printer;

	static int create_kvm_vm();
	static int kvm_fd;
};

#include "machine_inline.hpp"
}
