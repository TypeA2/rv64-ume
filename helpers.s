

    .data

    # Need to (re)store gp/tp, else shit breaks hard
reg_storage:
    .quad 0
    .quad 0
    .quad 0

    .global safe_exit
    .type safe_exit, @function
    .text
safe_exit:
    # longjmp to return address, with parameter 1
    call restore_regs

    la a0, g_jmp_buf
    li a1, 1
    call longjmp

    .global restore_regs
    .type restore_regs, @function
restore_regs:
    la tp, reg_storage
    # ld sp, 0(tp)
    ld gp, 8(tp)
    ld tp, 16(tp)
    ret

    .global program_runner
    .type program_runner, @function
program_runner:
    # Basically, if x1 is part of a post-condition it will have been written to (I hope)
    mv ra, a0

    la a0, reg_storage
    # sd sp, 0(a0)
    sd gp, 8(a0)
    sd tp, 16(a0)

    # Disable threading (set libthread-db-search-path /foo/bar) for GDB to not die here
    li sp, 0
    li gp, 0
    li tp, 0

    # Everything important *should* be restored by our safe_exit call at the end
    li t0, 0
    li t1, 0
    li t2, 0
    li s0, 0
    li s1, 0
    li a0, 0
    li a1, 0
    li a2, 0
    li a3, 0
    li a4, 0
    li a5, 0
    li a6, 0
    li a7, 0
    li s2, 0
    li s3, 0
    li s4, 0
    li s5, 0
    li s6, 0
    li s7, 0
    li s8, 0
    li s9, 0
    li s10, 0
    li s11, 0
    li t3, 0
    li t4, 0
    li t5, 0
    li t6, 0

    # Jump to actual entrypoint
    jalr zero, ra
