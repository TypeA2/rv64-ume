#ifndef ARCH_H
#define ARCH_H

#include <cstdint>

using reg_val = uint64_t;
using reg_num = uint8_t;

static constexpr reg_num NUM_REGS = 32;

static constexpr uint32_t TEST_END_MARKER = 0xddffccff;

#endif /* ARCH_H */