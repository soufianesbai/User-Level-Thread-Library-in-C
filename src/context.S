/* context.S — fast context switch, no sigprocmask syscall.
 *
 * Supports x86-64 and AArch64.  The C preprocessor (invoked by GCC for
 * upper-case .S files) selects the right block at build time.
 *
 * struct fast_ctx field offsets are defined in include/context.h and
 * must stay in sync with the constants used here.
 */

/* ================================================================== */
/* AArch64                                                             */
/* ================================================================== */
#ifdef __aarch64__

.text

/*
 * fast_swap_context(struct fast_ctx *save, const struct fast_ctx *restore)
 *
 * Saves the caller's callee-saved registers + SP into *save (x0),
 * then restores from *restore (x1) and returns via the restored lr.
 *
 * AArch64 callee-saved: x19-x28, x29 (fp), sp.
 * x30 (lr) holds the return address on entry (set by the bl instruction).
 *
 * struct fast_ctx offsets:
 *   0  lr   8  sp  16 x19  24 x20  32 x21  40 x22  48 x23
 *  56 x24  64 x25  72 x26  80 x27  88 x28  96 fp(x29)
 */
.global fast_swap_context
.type   fast_swap_context, %function
fast_swap_context:
    /* --- save current context into *save (x0) --- */
    str  x30,      [x0, #0]       /* lr  = return address    */
    mov  x9, sp
    str  x9,       [x0, #8]       /* sp                      */
    stp  x19, x20, [x0, #16]
    stp  x21, x22, [x0, #32]
    stp  x23, x24, [x0, #48]
    stp  x25, x26, [x0, #64]
    stp  x27, x28, [x0, #80]
    str  x29,      [x0, #96]      /* fp                      */

    /* --- restore from *restore (x1) --- */
    ldp  x19, x20, [x1, #16]
    ldp  x21, x22, [x1, #32]
    ldp  x23, x24, [x1, #48]
    ldp  x25, x26, [x1, #64]
    ldp  x27, x28, [x1, #80]
    ldr  x29,      [x1, #96]      /* fp                      */
    ldr  x9,       [x1, #8]
    mov  sp, x9                   /* sp                      */
    ldr  x30,      [x1, #0]       /* lr = return addr/entry  */
    ret                           /* branch to x30           */
.size fast_swap_context, .-fast_swap_context


/*
 * fast_restore_context(const struct fast_ctx *ctx)
 *
 * Restore without saving — for dying threads (thread_exit).
 * x0 = ctx
 */
.global fast_restore_context
.type   fast_restore_context, %function
fast_restore_context:
    ldp  x19, x20, [x0, #16]
    ldp  x21, x22, [x0, #32]
    ldp  x23, x24, [x0, #48]
    ldp  x25, x26, [x0, #64]
    ldp  x27, x28, [x0, #80]
    ldr  x29,      [x0, #96]
    ldr  x9,       [x0, #8]
    mov  sp, x9
    ldr  x30,      [x0, #0]
    ret
.size fast_restore_context, .-fast_restore_context


/* ================================================================== */
/* x86-64                                                              */
/* ================================================================== */
#elif defined(__x86_64__)

.text

/*
 * fast_swap_context(struct fast_ctx *save, const struct fast_ctx *restore)
 *
 * Saves callee-saved registers + RSP into *save (rdi),
 * then restores from *restore (rsi) and jumps to restore->rip.
 *
 * struct fast_ctx offsets:
 *   0 rip   8 rsp  16 rbx  24 rbp  32 r12  40 r13  48 r14  56 r15
 */
.global fast_swap_context
.type   fast_swap_context, @function
fast_swap_context:
    /* --- save current context into *save (rdi) --- */
    leaq    .Lresume(%rip), %rax
    movq    %rax,  0(%rdi)        /* rip = resume label      */
    movq    %rsp,  8(%rdi)        /* rsp                     */
    movq    %rbx, 16(%rdi)
    movq    %rbp, 24(%rdi)
    movq    %r12, 32(%rdi)
    movq    %r13, 40(%rdi)
    movq    %r14, 48(%rdi)
    movq    %r15, 56(%rdi)

    /* --- restore from *restore (rsi) --- */
    movq    16(%rsi), %rbx
    movq    24(%rsi), %rbp
    movq    32(%rsi), %r12
    movq    40(%rsi), %r13
    movq    48(%rsi), %r14
    movq    56(%rsi), %r15
    movq     8(%rsi), %rsp
    jmpq    *0(%rsi)              /* jump to rip             */

.Lresume:
    ret
.size fast_swap_context, .-fast_swap_context


/*
 * fast_restore_context(const struct fast_ctx *ctx)
 *
 * Restore without saving — for dying threads (thread_exit).
 * rdi = ctx
 */
.global fast_restore_context
.type   fast_restore_context, @function
fast_restore_context:
    movq    16(%rdi), %rbx
    movq    24(%rdi), %rbp
    movq    32(%rdi), %r12
    movq    40(%rdi), %r13
    movq    48(%rdi), %r14
    movq    56(%rdi), %r15
    movq     8(%rdi), %rsp
    jmpq    *0(%rdi)
.size fast_restore_context, .-fast_restore_context

#endif /* architecture */

/* Tell the linker this object does not need an executable stack. */
#if defined(__linux__)
.section .note.GNU-stack,"",@progbits
#endif
