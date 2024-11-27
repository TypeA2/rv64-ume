#include <iostream>
#include <regex>
#include <string_view>
#include <sys/mman.h>
#include <sys/ucontext.h>
#include <vector>
#include <span>
#include <fstream>
#include <cstring>
#include <cinttypes>
#include <csetjmp>
#include <chrono>

#include <getopt.h>
#include <elf.h>
#include <unistd.h>
#include <signal.h>

#include "arch.h"
#include "elf_file.h"

#if !defined(__riscv) || __riscv_xlen != 64
# error "Can only run on RV64"
#endif

/* From rv64-emu */
enum ExitCodes : int {
    Success = 0,
    AbnormalTermination = 1,
    HelpDisplayed = 2,
    InitializationError = 3,
    UnitTestFailed = 5,
    NotSupported = 6,
    SigHandlerFailure = 7
};

struct reg_init {
    reg_num num;
    reg_val val;

    reg_init(reg_num num, reg_val val) : num { num }, val { val } { }

    reg_init(const std::string& init) {
        std::regex init_regex("[rR]([0-9]{1,2})=(0x[0-9]+|[0-9]+)");
        std::smatch match;

        if (std::regex_match(init, match, init_regex)) {
            int regnum = std::stoi(match[1].str());

            if (regnum < 0 || regnum >= NUM_REGS) {
                throw std::out_of_range("Error: Register " + std::to_string(regnum) + " out of range");
            }

            num = regnum;

            /* Autodetect base */
            val = std::stoull(match[2].str(), nullptr, 0);
        }

        throw std::invalid_argument("Error: Unknown string format for initstr " + init);
    }
};

static void crash_and_burn(const char* msg) {
    size_t chars = 0;
    const char* cur = msg;
    while (*cur++) ++chars;

    write(STDOUT_FILENO, msg, chars);
    if (msg[chars-1] != '\n') write(STDOUT_FILENO, "\n", 1);
    _exit(ExitCodes::SigHandlerFailure);
}

static void dump_regs(__riscv_mc_gp_state regs) {
    /* write isn't checked because if it fails we're screwed anyway */
    const char* regnames[32] {
        " pc", " ra", " sp", " gp", " tp", " t0", " t1", " t2",
        " fp", " s1", " a0", " a1", " a2", " a3", " a4", " a5",
        " a6", " a7", " s2", " s3", " s4", " s5", " s6", " s7",
        " s8", " s9", "s10", "s11", " t3", " t4", " t5", " t6"
    };

    /* Holds at most a hex 64-bit integer, plus null terminator */
    char buf[16 + 1];
    int res = 0;

    for (size_t i = 0; i < 16; ++i) {
        /* Write regname and = */
        write(STDOUT_FILENO, regnames[i], 3);
        write(STDOUT_FILENO, "=", 1);

        res = snprintf(buf, sizeof(buf), "%.16" PRIx64, regs[i]);
        if (res < 0) crash_and_burn("snprintf fail (1)");
        write(STDOUT_FILENO, buf, res);

        write(STDOUT_FILENO, "  ", 2);

        write(STDOUT_FILENO, regnames[i + 16], 3);
        write(STDOUT_FILENO, "=", 1);

        res = snprintf(buf, sizeof(buf), "%.16" PRIx64, regs[i + 16]);
        if (res < 0) crash_and_burn("snprintf fail (2)");
        write(STDOUT_FILENO, buf, res);

        write(STDOUT_FILENO, "\n", 1);
    }
}

static __riscv_mc_gp_state g_saved_regs;
extern "C" {
    jmp_buf g_jmp_buf;
}

extern "C" [[noreturn]] void safe_exit();
extern "C" void restore_regs();
extern "C" [[noreturn]] void program_runner(uintptr_t entry);


static void sigsegv_handler(int sig, siginfo_t* info, void* ucontext) {
    /* Restore _very_ important registers first */
    restore_regs();

    ucontext_t* ctx = static_cast<ucontext_t*>(ucontext);

    uintptr_t addr = reinterpret_cast<uintptr_t>(info->si_addr);

    // dump_regs(ctx->uc_mcontext.__gregs);

    /* Grab PC to load current instruction */
    uint64_t pc = ctx->uc_mcontext.__gregs[REG_PC];
    void* pc_ptr = reinterpret_cast<void*>(pc);
    uint8_t opcode = *static_cast<uint8_t*>(pc_ptr) & 0x7f;

    bool is_compressed = (opcode & 0b11) != 0b11;

    /* Increment PC for when this handler returns */
    ctx->uc_mcontext.__gregs[REG_PC] += (is_compressed ? 2 : 4);

    uint64_t written_value = 0;
    uint8_t write_width = 0;
    if (is_compressed) {
        crash_and_burn("compressed store");
    } else {
        if (opcode != 0b0100011) crash_and_burn("unexpected access opcode");
        uint32_t word = *static_cast<uint32_t*>(pc_ptr);

        write_width = 1 << ((word >> 12) & 0b111);

        written_value = ctx->uc_mcontext.__gregs[(word >> 20) & 0b11111];
    }

    if (addr == 0x278) {
        /* Controlled exit */
        if (write_width != 1 && write_width != 4) crash_and_burn("unexpected write size");

        std::copy_n(ctx->uc_mcontext.__gregs, NGREG, g_saved_regs);
        ctx->uc_mcontext.__gregs[REG_PC] = reinterpret_cast<uintptr_t>(&safe_exit);
    } else if (addr == 0x200) {
        /* Serial 1-byte output */
        if (write_width != 1) crash_and_burn("unexpected write size");

        char ch = written_value & 0xff;

        if (write(STDOUT_FILENO, &ch, 1) != 1) {
            crash_and_burn("failed to write serial output");
        }
    } else {
        char msg[256];
        int written = sprintf(msg, "Unexpected write of %i to %p at %lx\n",
                                static_cast<int>(write_width), info->si_addr, pc);
        write(STDOUT_FILENO, msg, written);
        abort();
    }
}

static void bind_io(std::span<char> signal_stack) {
    /* Bind IO by mapping unwritable memory at specific addresses */
    /**
     * Map page 0 as nonwritable:
     * - 0x200 (512): Serial
     * - 0x270 (624): SysStatus
     */

    /* Manually doing this needs vm.mmap_min_addr = 0 */
    /*
    uintptr_t page_size = sysconf(_SC_PAGESIZE);

    void* map = mmap(0, page_size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);

    if (map != 0) {
        if (map != MAP_FAILED) {
            munmap(map, page_size);
        }
        
        throw std::runtime_error(std::string("Mapping IO failed: ")
                + strerrorname_np(errno) + " - " + strerror(errno));
    } */

    /* This only needs to be unmapped at exit, just leave it be */

    /* Run signal on separate stack, since we don't know whether the program has a stack at all */
    
    stack_t stack {
        .ss_sp = signal_stack.data() + signal_stack.size_bytes(),
        .ss_flags = 0,
        .ss_size = signal_stack.size_bytes()
    };

    if (sigaltstack(&stack, nullptr) != 0) {
        throw std::runtime_error(std::string("sigaltstack fail: ")
                + strerrorname_np(errno) + " - " + strerror(errno));
    }

    /* Handle SIGSEGV */
    struct sigaction sig { };
    sig.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sig.sa_sigaction = sigsegv_handler;

    /* Block all signals, makes life easy */
    sigfillset(&sig.sa_mask);

    if (sigaction(SIGSEGV, &sig, nullptr) != 0) {
        throw std::runtime_error(std::string("Failed to set signal handler: ")
                + strerrorname_np(errno) + " - " + strerror(errno));
    }
}

static int run(const std::string& src, std::span<reg_init> inits) {
    std::string executable;

    if (src.ends_with(".conf")) {
        /* We're running a test file */
        std::cerr << "Test files not (yet) supported!" << std::endl;
        return ExitCodes::NotSupported;
    } else {
        executable = src;
    }

    /* Load & map executable, errors if it overlaps with our own process */
    elf_file elf { executable };

    std::vector<char> signal_stack;
    signal_stack.resize(SIGSTKSZ);

    
    uintptr_t page_size = sysconf(_SC_PAGESIZE);

    if (page_size != 4096) {
        /* Is this even RISC-V? */
        throw std::runtime_error("Unexpected page size");
    }
    
    bind_io(signal_stack);

    std::chrono::high_resolution_clock::time_point begin;
    if (setjmp(g_jmp_buf) == 0) {
        begin = std::chrono::high_resolution_clock::now();
        program_runner(elf.entry());        
    }

    auto elapsed = std::chrono::high_resolution_clock::now() - begin;

    std::cerr << "Finished execution!" << std::endl;
    std::cerr << "Took " << elapsed.count() << " ns" << std::endl;
    std::cerr << "Regs at time of end: " << std::endl;
    dump_regs(g_saved_regs);

    return 0;
}

static void help(const char* prog) {
    std::cerr << prog << 
R"HERE(
    [-r reginit] <program_filename>
        or

    -t testfile

        Where 'reginit' is a register initializer in the form
        rX=Y with X a register number and Y the initializer value.
        'testfile' is a unit test configuration file.

    -d enables debug mode in which every decoded instruction is printed
        to the terminal.
)HERE";
}

int main(int argc, char** argv) {
    int c;

    const char* prog = argv[0];
    const char* testfile_name = nullptr;

    std::vector<reg_init> inits;

    while ((c = getopt(argc, argv, "r:t:h")) != -1) {
        switch (c) {
            case 'r':
                if (testfile_name) {
                    std::cerr << "Error: Cannot set unit test and individual registers at the same time" << std::endl;
                    return ExitCodes::InitializationError;
                }

                try {
                    inits.emplace_back(optarg);
                } catch (std::exception& e) {
                    std::cerr << e.what() << std::endl;
                    return ExitCodes::InitializationError;
                }
                break;

            case 't':
                if (testfile_name) {
                    std::cerr << "Only one test file allowed" << std::endl;
                    return ExitCodes::InitializationError;
                }

                testfile_name = optarg;
                break;

            case 'h':
            default:
                help(prog);
                return ExitCodes::HelpDisplayed;
        }
    }

    argc -= optind;
    argv += optind;

    /* If no test file is specified, we're running a file as specified from the  */
    if (!testfile_name && argc < 1) {
        std::cerr << "Error: No executable\n" << std::endl;
        help(prog);
        return ExitCodes::InitializationError;
    }

    try {
        return run(testfile_name ? testfile_name : argv[0], inits);
    } catch (std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return ExitCodes::AbnormalTermination;
    }
}
