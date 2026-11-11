/* aarch64 ucontext save/restore for Android (bionic lacks the functions).
 * mcontext_t == struct sigcontext: fault_address@0, regs[31]@8 (x0..x30),
 * sp@256, pc@264, pstate@272.
 *
 * Only callee-saved registers (x19..x28, sp, pc, nzcv) must round-trip for
 * cooperative fiber switching; caller-saved slots may hold clobbered values.
 */
    .text
    .align 4

/* void hos_a64_ctx_save(mcontext_t *m) — m in x0 */
    .globl hos_a64_ctx_save
    .type hos_a64_ctx_save,%function
hos_a64_ctx_save:
    mov x9, x0
    stp x0,  x1,  [x9, #8]
    stp x2,  x3,  [x9, #24]
    stp x4,  x5,  [x9, #40]
    stp x6,  x7,  [x9, #56]
    stp x8,  x9,  [x9, #72]
    stp x10, x11, [x9, #88]
    stp x12, x13, [x9, #104]
    stp x14, x15, [x9, #120]
    stp x16, x17, [x9, #136]
    stp x18, x19, [x9, #152]
    stp x20, x21, [x9, #168]
    stp x22, x23, [x9, #184]
    stp x24, x25, [x9, #200]
    stp x26, x27, [x9, #216]
    stp x28, x29, [x9, #232]
    str x30,      [x9, #248]     /* regs[30] */
    mov x10, sp
    str x10,      [x9, #256]     /* sp */
    str x30,      [x9, #264]     /* pc = return address */
    mrs x10, nzcv
    str x10,      [x9, #272]     /* pstate */
    mov x0, #0
    ret
    .size hos_a64_ctx_save, .-hos_a64_ctx_save

/* void hos_a64_ctx_restore(mcontext_t *m) — m in x0; never returns to its caller */
    .globl hos_a64_ctx_restore
    .type hos_a64_ctx_restore,%function
hos_a64_ctx_restore:
    mov x30, x0                  /* base pointer (x30 is free until the end) */
    ldr x16, [x30, #256]         /* sp  */
    ldr x18, [x30, #272]         /* pstate */
    msr nzcv, x18
    mov sp, x16
    ldp x0,  x1,  [x30, #8]
    ldp x2,  x3,  [x30, #24]
    ldp x4,  x5,  [x30, #40]
    ldp x6,  x7,  [x30, #56]
    ldp x8,  x9,  [x30, #72]
    ldp x10, x11, [x30, #88]
    ldp x12, x13, [x30, #104]
    ldp x14, x15, [x30, #120]
    ldp x16, x17, [x30, #136]
    ldp x18, x19, [x30, #152]
    ldp x20, x21, [x30, #168]
    ldp x22, x23, [x30, #184]
    ldp x24, x25, [x30, #200]
    ldp x26, x27, [x30, #216]
    ldp x28, x29, [x30, #232]
    ldr x30, [x30, #264]         /* pc */
    ret
    .size hos_a64_ctx_restore, .-hos_a64_ctx_restore

/* void hos_a64_swapcontext(mcontext_t *oucp, mcontext_t *ucp) — x0, x1.
 * Saves current context into oucp (pc = return into C wrapper), then restores ucp. */
    .globl hos_a64_swapcontext
    .type hos_a64_swapcontext,%function
hos_a64_swapcontext:
    mov x9, x0
    stp x0,  x1,  [x9, #8]
    stp x2,  x3,  [x9, #24]
    stp x4,  x5,  [x9, #40]
    stp x6,  x7,  [x9, #56]
    stp x8,  x9,  [x9, #72]
    stp x10, x11, [x9, #88]
    stp x12, x13, [x9, #104]
    stp x14, x15, [x9, #120]
    stp x16, x17, [x9, #136]
    stp x18, x19, [x9, #152]
    stp x20, x21, [x9, #168]
    stp x22, x23, [x9, #184]
    stp x24, x25, [x9, #200]
    stp x26, x27, [x9, #216]
    stp x28, x29, [x9, #232]
    str x30,      [x9, #248]
    mov x10, sp
    str x10,      [x9, #256]
    str x30,      [x9, #264]
    mrs x10, nzcv
    str x10,      [x9, #272]
    mov x0, x1                  /* ucp still live in x1 */
    b hos_a64_ctx_restore
    .size hos_a64_swapcontext, .-hos_a64_swapcontext

/* Entry point for makecontext'd functions: x9 = func, x0..x7 = args.
 * Calls func(args...); if it returns, end the thread like fiber_start's
 * ExitThread(0) does on the desktop port. */
    .globl hos_a64_ctx_trampoline
    .type hos_a64_ctx_trampoline,%function
hos_a64_ctx_trampoline:
    mov x16, x9
    blr x16
    mov x0, #0
    bl pthread_exit
    .size hos_a64_ctx_trampoline, .-hos_a64_ctx_trampoline
