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
#include <map>

#include <getopt.h>
#include <elf.h>
#include <unistd.h>
#include <signal.h>

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

std::map<std::string_view, uint8_t> reg_name_map {
    { "ra",   1 }, { "x1",   1 },
    { "sp",   2 }, { "x2",   2 },
    { "gp",   3 }, { "x3",   3 },
    { "tp",   4 }, { "x4",   4 }, 
    { "t0",   5 }, { "x5",   5 },
    { "t1",   6 }, { "x6",   6 },
    { "t2",   7 }, { "x7",   7 },
    { "s0",   8 }, { "x8",   8 }, { "fp", 8 },
    { "s1",   9 }, { "x9",   9 },
    { "a0",  10 }, { "x10", 10 },
    { "a1",  11 }, { "x11", 11 },
    { "a2",  12 }, { "x12", 12 }, 
    { "a3",  13 }, { "x13", 13 },
    { "a4",  14 }, { "x14", 14 },
    { "a5",  15 }, { "x15", 15 },
    { "a6",  16 }, { "x16", 16 }, 
    { "a7",  17 }, { "x17", 17 },
    { "s2",  18 }, { "x18", 18 },
    { "s3",  19 }, { "x19", 19 },
    { "s4",  20 }, { "x20", 20 }, 
    { "s5",  21 }, { "x21", 21 },
    { "s6",  22 }, { "x22", 22 },
    { "s7",  23 }, { "x23", 23 },
    { "s8",  24 }, { "x24", 24 }, 
    { "s9",  25 }, { "x25", 25 },
    { "s10", 26 }, { "x26", 26 },
    { "s11", 27 }, { "x27", 27 },
    { "t3",  28 }, { "x28", 28 }, 
    { "t4",  29 }, { "x29", 29 },
    { "t5",  30 }, { "x30", 30 }, 
    { "t6",  31 }, { "x31", 31 },
};

/* Can't use reg_name_map because this should be signal-safe(-ish) */
static constexpr const char* regnames[NGREG] {
    " pc", " ra", " sp", " gp", " tp", " t0", " t1", " t2",
    " fp", " s1", " a0", " a1", " a2", " a3", " a4", " a5",
    " a6", " a7", " s2", " s3", " s4", " s5", " s6", " s7",
    " s8", " s9", "s10", "s11", " t3", " t4", " t5", " t6"
};

using reg_val = uint64_t;
using reg_num = uint8_t;

static constexpr reg_num NUM_REGS = 32;

static constexpr uint32_t TEST_END_MARKER = 0xddffccff;

enum ExitTypes : int {
    InitialCall  = 0,
    ExitByStatus = 1,
    ExitByMarker = 2,
};

struct reg_init {
    reg_num num;
    reg_val val;

    reg_init(reg_num num, reg_val val) : num { num }, val { val } {
        if (num >= NGREG) {
            throw std::out_of_range("Register " + std::to_string(static_cast<int>(num)) + " is out of range");
        }
    }

    reg_init(std::string_view init) {
        size_t delim = init.find("=");
        if (delim == std::string::npos) {
            throw std::invalid_argument("Error: Invalid string format for initstr " + std::string{init});
        }

        std::string reg { init.substr(0, delim) };
        std::string val { init.substr(delim + 1) };

        this->num = (reg.front() == 'R') ? std::stoi(reg.substr(1)) : reg_name_map.at(reg);
        this->val = std::stoull(val.c_str(), nullptr, 0);
        
        if (num >= NGREG) {
            throw std::out_of_range("Register " + std::to_string(static_cast<int>(num)) + " is out of range");
        }
    }
};

static void crash_and_burn(const char* msg) {
    size_t chars = 0;
    const char* cur = msg;
    while (*cur++) ++chars;

    /* write isn't checked because if it fails we're screwed anyway */
    write(STDOUT_FILENO, msg, chars);
    if (msg[chars-1] != '\n') write(STDOUT_FILENO, "\n", 1);
    _exit(ExitCodes::SigHandlerFailure);
}

static void dump_regs(__riscv_mc_gp_state regs) {
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

static __riscv_mc_gp_state g_init_regs;
static __riscv_mc_gp_state g_result_regs;
extern "C" {
    jmp_buf g_jmp_buf;
}

extern "C" [[noreturn]] void safe_exit();
extern "C" void restore_regs();
extern "C" uint64_t reg_storage[4];

static void signal_handler(int sig, siginfo_t* info, void* ucontext) {
    /* Restore _very_ important registers first, if they're set */
    if (reg_storage[0]) {
        restore_regs();
    }

    ucontext_t* ctx = static_cast<ucontext_t*>(ucontext);

    uintptr_t addr = reinterpret_cast<uintptr_t>(info->si_addr);

    // dump_regs(ctx->uc_mcontext.__gregs);

    /* Grab PC to load current instruction */
    uint64_t pc = ctx->uc_mcontext.__gregs[REG_PC];
    void* pc_ptr = reinterpret_cast<void*>(pc);

    if (sig == SIGILL) {
        /* Check for test end marker */
        uint32_t instr = *static_cast<uint32_t*>(pc_ptr);

        if (instr == TEST_END_MARKER) {
            std::copy_n(ctx->uc_mcontext.__gregs, NGREG, g_result_regs);
            ctx->uc_mcontext.__gregs[REG_PC] = reinterpret_cast<uintptr_t>(&safe_exit);
            ctx->uc_mcontext.__gregs[REG_A0] = ExitTypes::ExitByMarker;
        } else {
            crash_and_burn("Illegal instruction");
        }

    } else {

        uint8_t opcode = *static_cast<uint8_t*>(pc_ptr) & 0x7f;

        bool is_compressed = (opcode & 0b11) != 0b11;

        uint64_t written_value = 0;
        uint8_t write_width = 0;
        if (is_compressed) {
            if ((opcode & 0b11) == 0b00) {
                /* Quadrant 0 */
                uint16_t instr = *static_cast<uint16_t*>(pc_ptr);

                uint8_t funct3 = (instr >> 13) & 0b111;

                /* c.sd or c.sw */
                if (funct3 != 0b111 && funct3 != 0b110) crash_and_burn("unsupported quadrant 0 instruction");

                write_width = (funct3 == 0b111) ? 8 : 4;

                written_value = ctx->uc_mcontext.__gregs[8 + ((instr >> 2) & 0b111)];
            } else {
                crash_and_burn("unsupported compressed instruction");
            }
        } else {
            if (opcode != 0b0100011) crash_and_burn("unexpected access opcode");
            uint32_t word = *static_cast<uint32_t*>(pc_ptr);

            write_width = 1 << ((word >> 12) & 0b111);

            written_value = ctx->uc_mcontext.__gregs[(word >> 20) & 0b11111];
        }

        if (addr == 0x278) {
            /* Controlled exit */
            if (write_width != 1 && write_width != 4) crash_and_burn("unexpected write size for exit");

            std::copy_n(ctx->uc_mcontext.__gregs, NGREG, g_result_regs);
            ctx->uc_mcontext.__gregs[REG_PC] = reinterpret_cast<uintptr_t>(&safe_exit);
            ctx->uc_mcontext.__gregs[REG_A0] = ExitTypes::ExitByStatus;
        } else if (addr == 0x200) {
            /* Serial 1-byte output */
            if (write_width != 1) crash_and_burn("unexpected write size for serial");

            char ch = written_value & 0xff;

            if (write(STDOUT_FILENO, &ch, 1) != 1) {
                crash_and_burn("failed to write serial output");
            }

            /* Increment PC for when this handler returns */
            ctx->uc_mcontext.__gregs[REG_PC] += (is_compressed ? 2 : 4);

        } else if (addr == 0x208) {
            if (write_width != 8) crash_and_burn("unexpected write size for program start");

            /* Store a few important registers so we can restore them later */
            reg_storage[0] = 1;
            reg_storage[1] = ctx->uc_mcontext.__gregs[REG_TP - 1];
            reg_storage[2] = ctx->uc_mcontext.__gregs[REG_TP];
            reg_storage[3] = ctx->uc_mcontext.__gregs[REG_SP];

            /* Set PC */
            ctx->uc_mcontext.__gregs[REG_PC] = written_value;

            /* Load initial register values */
            /* Disable threading (set libthread-db-search-path /foo) for GDB to not when tp = 0 */
            std::copy(&g_init_regs[1], &g_init_regs[0] + NGREG, &ctx->uc_mcontext.__gregs[1]);

            /* Return context to program code with all registers set to 0 */
        } else {
            char msg[256];
            sprintf(msg, "Unexpected write of %i to %p at %lx\n",
                        static_cast<int>(write_width), info->si_addr, pc);
            crash_and_burn(msg);
        }
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
    sig.sa_sigaction = signal_handler;

    /* Block all signals, makes life easy */
    sigfillset(&sig.sa_mask);

    if (sigaction(SIGSEGV, &sig, nullptr) != 0) {
        throw std::runtime_error(std::string("Failed to set SIGSEGV handler: ")
                + strerrorname_np(errno) + " - " + strerror(errno));
    }

    if (sigaction(SIGILL, &sig, nullptr) != 0) {
        throw std::runtime_error(std::string("Failed to set SIGILL handler: ")
                + strerrorname_np(errno) + " - " + strerror(errno));
    }
}

static void load_conf(const std::string& path, std::vector<reg_init>& pre, std::vector<reg_init>& post) {
    /* Simpler, less generic .conf parsing */
    bool is_pre = false;
    bool is_post = false;

    std::ifstream in { path };

    for (std::string line; std::getline(in, line);) {
        if (line.empty()) {
            continue;
        }

        if (!is_pre && !is_post) {
            if (line == "[pre]") {
                is_pre = true;
            } else {
                throw std::runtime_error("Error: expected [pre] section, got " + line);
            }
        } else if (is_pre && !is_post) {
            if (line == "[post]") {
                is_pre = false;
                is_post = true;
            } else {
                pre.emplace_back(line);
            }
        } else if (!is_pre && is_post) {
            post.emplace_back(line);
        }
    }
}

static int run(const std::string& src, std::vector<reg_init> pre) {
    std::string executable;

    std::vector<reg_init> post;

    bool is_test = src.ends_with(".conf");

    if (is_test) {
        /* We're running a test file */
        load_conf(src, pre, post);
        
        executable = src.substr(0, src.size() - 4) + "bin";
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
    
    std::fill_n(&g_init_regs[0], NGREG, 0);
    for (const reg_init& reg : pre) {
        if (reg.num > 0) {
            /* addi.conf contains an R0 initializer, we have to ignore this */
            g_init_regs[reg.num] = reg.val;
        }
    }

    std::chrono::high_resolution_clock::time_point begin;
    bool test_marker_encountered = false;
    switch (setjmp(g_jmp_buf)) {
        case ExitTypes::InitialCall:
            begin = std::chrono::high_resolution_clock::now();

            /* Invoke SIGSEGV signal handler at 0x208 to start execution at entrypoint */
            *((uint64_t*)0x208) = elf.entry();
            __builtin_unreachable();
            break;

        case ExitByStatus:
            break;

        case ExitByMarker:
            test_marker_encountered = true;
            break;
    }

    auto elapsed = std::chrono::high_resolution_clock::now() - begin;

    if (!is_test) {
        if (test_marker_encountered) {
            std::cerr << "Test marker encountered at " << std::hex << g_result_regs[REG_PC] << std::endl;
        } else {
            std::cerr << "System halt requested at " << std::hex << g_result_regs[REG_PC] << std::endl;
        }

        std::cerr << "Took " << (elapsed.count() / 1e3) << " us" << std::endl;
        dump_regs(g_result_regs);
    }

    int res = ExitCodes::Success;

    for (const reg_init& reg : post) {
        /* Ignore stray R0 postconditions */
        if (reg.num == 0) {
            continue;
        }

        if (g_result_regs[reg.num] != reg.val) {
            std::cerr << "Register " << regnames[reg.num]
                << " expected " << reg.val
                << " (" << std::hex << std::showbase << reg.val
                << std::dec << std::noshowbase << ")"
                << " got " << g_result_regs[reg.num]
                << " (" << std::hex << std::showbase << g_result_regs[reg.num]
                << std::dec << std::noshowbase << ")"
                << std::endl;

            res = ExitCodes::UnitTestFailed;
        }
    }

    return res;
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

    while ((c = getopt(argc, argv, "pr:t:h")) != -1) {
        switch (c) {
            case 'p':
                /* ignore for compatibility */
                break;

            case 'r':
                if (testfile_name) {
                    std::cerr << "Error: Cannot set unit test and individual registers at the same time" << std::endl;
                    return ExitCodes::InitializationError;
                }

                try {
                    std::cerr << "initstring: " << optarg << std::endl;
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
        return run(testfile_name ? testfile_name : argv[0], std::move(inits));
    } catch (std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return ExitCodes::AbnormalTermination;
    }
}
