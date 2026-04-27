/* context.S — fast context switch, no sigprocmask syscall.
 *
 * This file implements a minimal context switch system.
 *
 * We manually save/restore only the minimal CPU state required to
 * suspend and resume execution of a thread:
 *
 *   - instruction pointer (PC / RIP / LR)
 *   - stack pointer (SP)
 *   - callee-saved registers
 *
 * This avoids kernel involvement and is significantly faster than
 * swapcontext() which performs signal mask syscalls.
 *
 * IMPORTANT:
 * ----------
 * The layout of struct fast_ctx in context.h MUST match exactly
 * the offsets used here.
 */

/* ================================================================
 * AARCH64 (ARM64)
 * ================================================================ */
#ifdef __aarch64__

.text

/*
 * fast_swap_context(save, restore)
 *
 * x0 = save context (of the current thread)
 * x1 = restore context (for the next thread)
 *
 * This function:
 *   1. saves current CPU state into *save
 *   2. loads CPU state from *restore
 *   3. jumps into restored execution
 */
.global fast_swap_context
.type   fast_swap_context, %function

fast_swap_context:

    /* ------------------------------------------------------------
     * SAVE CURRENT THREAD CONTEXT
     * ------------------------------------------------------------ */

    /* Save return address (LR = x30) */
    str  x30,      [x0, #0]       /* save LR */

    /* Save stack pointer */
    mov  x9, sp
    str  x9,       [x0, #8]

    /* Save callee-saved registers (must be preserved across calls) */
    stp  x19, x20, [x0, #16]
    stp  x21, x22, [x0, #32]
    stp  x23, x24, [x0, #48]
    stp  x25, x26, [x0, #64]
    stp  x27, x28, [x0, #80]

    /* Save frame pointer */
    str  x29,      [x0, #96]

    /* ------------------------------------------------------------
     * RESTORE NEXT THREAD CONTEXT
     * ------------------------------------------------------------ */

    /* Restore callee-saved registers */
    ldp  x19, x20, [x1, #16]
    ldp  x21, x22, [x1, #32]
    ldp  x23, x24, [x1, #48]
    ldp  x25, x26, [x1, #64]
    ldp  x27, x28, [x1, #80]

    /* Restore frame pointer */
    ldr  x29,      [x1, #96]

    /* Restore stack pointer */
    ldr  x9,       [x1, #8]
    mov  sp, x9

    /* Restore program counter (LR) */
    ldr  x30,      [x1, #0]

    /* Jump into restored execution context */
    ret

.size fast_swap_context, .-fast_swap_context


/*
 * fast_restore_context(ctx)
 *
 * Restore a context WITHOUT saving current one.
 * Used when a thread terminates.
 */
.global fast_restore_context
.type   fast_restore_context, %function

fast_restore_context:

    /* Restore callee-saved registers */
    ldp  x19, x20, [x0, #16]
    ldp  x21, x22, [x0, #32]
    ldp  x23, x24, [x0, #48]
    ldp  x25, x26, [x0, #64]
    ldp  x27, x28, [x0, #80]

    /* Restore frame pointer */
    ldr  x29,      [x0, #96]

    /* Restore stack pointer */
    ldr  x9,       [x0, #8]
    mov  sp, x9

    /* Restore program counter */
    ldr  x30,      [x0, #0]

    /* Jump directly (no return to caller) */
    ret

.size fast_restore_context, .-fast_restore_context


/* ================================================================
 * X86-64 (PC ARCHITECTURE)
 * ================================================================ */
#elif defined(__x86_64__)

.text

/*
 * fast_swap_context(save, restore)
 *
 * rdi = save context (current thread)
 * rsi = restore context (next thread)
 *
 * Saves current CPU state and switches execution.
 */
.global fast_swap_context
.type   fast_swap_context, @function

fast_swap_context:

    /* ------------------------------------------------------------
     * SAVE CURRENT THREAD CONTEXT
     * ------------------------------------------------------------ */

    /*
     * Save a return address for later resumption.
     * We simulate a function return point (.Lresume).
     */
    leaq    .Lresume(%rip), %rax
    movq    %rax,  0(%rdi)        /* RIP */

    /* Save stack pointer */
    movq    %rsp,  8(%rdi)

    /* Save callee-saved registers (System V ABI) */
    movq    %rbx, 16(%rdi)
    movq    %rbp, 24(%rdi)
    movq    %r12, 32(%rdi)
    movq    %r13, 40(%rdi)
    movq    %r14, 48(%rdi)
    movq    %r15, 56(%rdi)

    /* ------------------------------------------------------------
     * RESTORE NEXT THREAD CONTEXT
     * ------------------------------------------------------------ */

    movq    16(%rsi), %rbx
    movq    24(%rsi), %rbp
    movq    32(%rsi), %r12
    movq    40(%rsi), %r13
    movq    48(%rsi), %r14
    movq    56(%rsi), %r15

    /* Switch stack */
    movq    8(%rsi), %rsp

    /* Jump into next thread execution */
    jmpq    *0(%rsi)

.Lresume:
    /*
     * When a thread is resumed, execution continues here.
     * The 'ret' restores normal C execution flow.
     */
    ret

.size fast_swap_context, .-fast_swap_context


/*
 * fast_restore_context(ctx)
 *
 * Restore context without saving current one.
 * Used when a thread exits.
 */
.global fast_restore_context
.type   fast_restore_context, @function

fast_restore_context:

    /* Restore registers */
    movq    16(%rdi), %rbx
    movq    24(%rdi), %rbp
    movq    32(%rdi), %r12
    movq    40(%rdi), %r13
    movq    48(%rdi), %r14
    movq    56(%rdi), %r15

    /* Restore stack pointer */
    movq    8(%rdi), %rsp

    /* Jump directly to context */
    jmpq    *0(%rdi)

.size fast_restore_context, .-fast_restore_context

#endif /* architecture */


/* ================================================================
 * Prevent executable stack (security hardening)
 * ================================================================ */
#if defined(__linux__)
.section .note.GNU-stack,"",@progbits
#endif