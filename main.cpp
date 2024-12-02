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
#include <thread>
#include <functional>
#include <csignal>

#include <getopt.h>
#include <elf.h>
#include <unistd.h>

#include "elf_file.h"
#include "util.h"

#ifdef ENABLE_FRAMEBUFFER
#include "framebuffer.h"
#endif

#if !defined(__riscv) || __riscv_xlen != 64
# error "Can only run on RV64"
#endif

static __riscv_mc_gp_state g_init_regs;
static __riscv_mc_gp_state g_result_regs;
extern "C" {
    jmp_buf g_jmp_buf;
}

#ifdef ENABLE_FRAMEBUFFER
static Framebuffer g_framebuffer;
#endif

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

        uint64_t value = 0;
        uint8_t width = 0;
        bool is_write = false;
        if (is_compressed) {
            if ((opcode & 0b11) == 0b00) {
                /* Quadrant 0 */
                uint16_t instr = *static_cast<uint16_t*>(pc_ptr);

                uint8_t funct3 = (instr >> 13) & 0b111;

                /* c.sd or c.sw */
                switch (funct3) {
                    case 0b111: is_write = true;  width = 8; break;
                    case 0b110: is_write = true;  width = 4; break;
                    case 0b011: is_write = false; width = 8; break;
                    case 0b010: is_write = false; width = 4; break;

                    default: {
                        char msg[160];
                        sprintf(msg, "unsupported quadrant 0 instruction at %p: %x", pc_ptr, instr);
                        crash_and_burn(msg);
                    }
                }

                if (is_write) {
                    value = ctx->uc_mcontext.__gregs[8 + ((instr >> 2) & 0b111)];
                } else {
                    value = 8 + ((instr >> 2) & 0b111);
                }
            } else {
                crash_and_burn("unsupported compressed instruction");
            }
        } else {
            switch (opcode) {
                case 0b0100011: is_write = true;  break;
                case 0b0000011: is_write = false; break;
                default: {
                    char msg[160];
                    sprintf(msg, "Unexpected access opcode 0x%x at %p (PC=%p)", opcode, info->si_addr, pc_ptr);
                    crash_and_burn(msg);
                }
            }

            uint32_t word = *static_cast<uint32_t*>(pc_ptr);

            width = 1 << ((word >> 12) & 0b111);

            if (is_write) {
                uint8_t reg = (word >> 20) & 0b11111;
                value = reg ? ctx->uc_mcontext.__gregs[reg] : 0;
            } else {
                /* Actually just register idx */
                value = (word >> 7) & 0b11111;
            }
        }

#ifdef ENABLE_FRAMEBUFFER
        uint64_t write_dummy;
        if (is_write && g_framebuffer.handle_write(addr, width, value)) {
            ctx->uc_mcontext.__gregs[REG_PC] += (is_compressed ? 2 : 4);

            /* Ignore writes to register 0, special case because the PC is stored at idx 0 */
        } else if (!is_write && g_framebuffer.handle_read(addr, width, value ? ctx->uc_mcontext.__gregs[value] : write_dummy)) {
            ctx->uc_mcontext.__gregs[REG_PC] += (is_compressed ? 2 : 4);
        } else 
#endif
        if (is_write && addr == 0x278) {
            /* Controlled exit */
            if (width != 1 && width != 4) crash_and_burn("unexpected write size for exit");

            std::copy_n(ctx->uc_mcontext.__gregs, NGREG, g_result_regs);
            ctx->uc_mcontext.__gregs[REG_PC] = reinterpret_cast<uintptr_t>(&safe_exit);
            ctx->uc_mcontext.__gregs[REG_A0] = ExitTypes::ExitByStatus;
        } else if (is_write && addr == 0x200) {
            /* Serial 1-byte output */
            if (width != 1) crash_and_burn("unexpected write size for serial");

            char ch = value & 0xff;

            if (write(STDOUT_FILENO, &ch, 1) != 1) {
                crash_and_burn("failed to write serial output");
            }

            /* Increment PC for when this handler returns */
            ctx->uc_mcontext.__gregs[REG_PC] += (is_compressed ? 2 : 4);

        } else if (is_write && addr == 0x208) {
            if (width != 8) crash_and_burn("unexpected write size for program start");

            /* Store a few important registers so we can restore them later */
            reg_storage[0] = 1;
            reg_storage[1] = ctx->uc_mcontext.__gregs[REG_TP - 1];
            reg_storage[2] = ctx->uc_mcontext.__gregs[REG_TP];
            reg_storage[3] = ctx->uc_mcontext.__gregs[REG_SP];

            /* Set PC */
            ctx->uc_mcontext.__gregs[REG_PC] = value;

            /* Load initial register values */
            /* Disable threading (set libthread-db-search-path /foo) for GDB to not when tp = 0 */
            std::copy(&g_init_regs[1], &g_init_regs[0] + NGREG, &ctx->uc_mcontext.__gregs[1]);

            /* Return context to program code with all registers set to 0 */
        } else {
            char msg[256];
            sprintf(msg, "Unexpected %s of %i to %p at %lx\n",
                        is_write ? "write" : "read", static_cast<int>(width), info->si_addr, pc);
            crash_and_burn(msg);
        }
    }
}

static std::vector<safe_map> bind_io(std::span<char> signal_stack) {
    std::vector<safe_map> res;

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
    }

    res.emplace_back(map, page_size);

    */

#ifdef ENABLE_FRAMEBUFFER
    void* fb_map = mmap(reinterpret_cast<void*>(fb_addr), fb_max_size,
                        PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
                        -1, 0);

    if (fb_map != reinterpret_cast<void*>(fb_addr)) {
        if (fb_map != MAP_FAILED) {
            munmap(fb_map, fb_max_size);
        }
        
        throw std::runtime_error(std::string("Mapping framebuffer failed: ")
                + strerrorname_np(errno) + " - " + strerror(errno));
    }

    res.emplace_back(fb_map, fb_max_size);
#endif

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

    return res;
}

void unbind_io() {
    struct sigaction sig { };
    sig.sa_flags = 0;
    sig.sa_handler = SIG_DFL;
    sigemptyset(&sig.sa_mask);

    sigaction(SIGSEGV, &sig, nullptr);
    sigaction(SIGILL, &sig, nullptr);
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
    
    auto io_mappings = bind_io(signal_stack);
    
    std::fill_n(&g_init_regs[0], NGREG, 0);
    for (const reg_init& reg : pre) {
        if (reg.num > 0) {
            /* addi.conf contains an R0 initializer, we have to ignore this */
            g_init_regs[reg.num] = reg.val;
        }
    }

#ifdef ENABLE_FRAMEBUFFER
    std::jthread fb_thread { [](std::stop_token stop) { g_framebuffer.entry(stop); } };
#endif

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

    unbind_io();

#ifdef ENABLE_FRAMEBUFFER
    fb_thread.request_stop();
    fb_thread.join();
#endif

    if (!is_test) {
        if (test_marker_encountered) {
            std::cerr << "Test marker encountered at " << std::hex << g_result_regs[REG_PC] << std::endl;
        } else {
            std::cerr << "System halt requested at " << std::hex << g_result_regs[REG_PC] << std::endl;
        }

        auto ns = elapsed.count();

        std::cerr << "Took ";
        if (ns < 1e3) {
            std::cerr << ns << " ns";
        } else if (ns < 1e6) {
            std::cerr << (ns / 1e3) << " us";
        } else if (ns < 1e9) {
            std::cerr << (ns / 1e6) << " ms";
        } else {
            std::cerr << (ns / 1e9) << " s";
        }

        std::cerr << std::endl;

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
