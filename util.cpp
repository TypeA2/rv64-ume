#include "util.h"

#include <stdexcept>

#include <cstdio>
#include <cinttypes>

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

reg_init::reg_init(reg_num num, reg_val val) : num { num }, val { val } {
    if (num >= NGREG) {
        throw std::out_of_range("Register " + std::to_string(static_cast<int>(num)) + " is out of range");
    }
}

reg_init::reg_init(std::string_view init) {
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

void crash_and_burn(const char* msg) {
    size_t chars = 0;
    const char* cur = msg;
    while (*cur++) ++chars;

    /* write isn't checked because if it fails we're screwed anyway */
    write(STDOUT_FILENO, msg, chars);
    if (msg[chars-1] != '\n') write(STDOUT_FILENO, "\n", 1);
    _exit(ExitCodes::SigHandlerFailure);
}

void dump_regs(__riscv_mc_gp_state regs) {
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