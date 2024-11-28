    .data

    # Need to (re)store gp/tp, else shit breaks hard
    .global reg_storage
reg_storage:
    .quad 0 # 1 if regs are set
    .quad 0 # gp
    .quad 0 # tp
    .quad 0 # sp

    .text
    .global safe_exit
    .type safe_exit, @function
safe_exit:
    # longjmp to return address, with parameter passed to this function
    la sp, reg_storage
    ld gp, 8(sp)
    ld tp, 16(sp)

    # restore_regs doesnt restore sp since it's meant to be called from a
    # signal handler, which already has a stack
    ld sp, 24(sp)

    mv a0, a1
    la a0, g_jmp_buf
    call longjmp

    .global restore_regs
    .type restore_regs, @function
restore_regs:
    la tp, reg_storage
    ld gp, 8(tp)
    ld tp, 16(tp)
    ret
