#ifndef UTIL_H
#define UTIL_H

#include <map>
#include <string_view>

#include <cstdint>

#include <signal.h>

/* From rv64-emu */
enum ExitCodes : int {
    Success = 0,
    AbnormalTermination = 1,
    HelpDisplayed = 2,
    InitializationError = 3,
    UnitTestFailed = 5,
    NotSupported = 6,
    SigHandlerFailure = 7,
    FramebufferError = 8
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

    reg_init(reg_num num, reg_val val);

    reg_init(std::string_view init);
};

void crash_and_burn(const char* msg);

void dump_regs(__riscv_mc_gp_state regs);

#endif /* UTIL_H */
