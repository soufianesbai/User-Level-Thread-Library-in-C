#ifndef CONTEXT_H
#define CONTEXT_H

#include <stdint.h>

/*
 * Architecture-specific minimal saved context.
 *
 * Only callee-saved registers + SP + PC are stored — no signal-mask
 * roundtrip.  Replacing swapcontext() eliminates the sigprocmask syscall
 * that glibc's swapcontext/setcontext always performs, which is the
 * dominant per-switch cost for workloads with many short threads.
 */

/* ------------------------------------------------------------------ */
/* AArch64 (ARM64)                                                     */
/* ------------------------------------------------------------------ */
#ifdef __aarch64__

struct fast_ctx {
    uint64_t lr;  /* x30: return address / entry point — offset  0 */
    uint64_t sp;  /* stack pointer                     — offset  8 */
    uint64_t x19; /*                                   — offset 16 */
    uint64_t x20; /*                                   — offset 24 */
    uint64_t x21; /*                                   — offset 32 */
    uint64_t x22; /*                                   — offset 40 */
    uint64_t x23; /*                                   — offset 48 */
    uint64_t x24; /*                                   — offset 56 */
    uint64_t x25; /*                                   — offset 64 */
    uint64_t x26; /*                                   — offset 72 */
    uint64_t x27; /*                                   — offset 80 */
    uint64_t x28; /*                                   — offset 88 */
    uint64_t fp;  /* x29: frame pointer                — offset 96 */
};

/*
 * AArch64: SP must always be 16-byte aligned.
 * Return address lives in x30 (lr), not on the stack.
 */
static inline void fast_ctx_init(struct fast_ctx *ctx, void *stack_top,
                                  void (*entry)(void))
{
    uintptr_t sp = (uintptr_t)stack_top;
    sp &= ~(uintptr_t)15;          /* 16-byte align                   */

    ctx->lr  = (uint64_t)(uintptr_t)entry;
    ctx->sp  = sp;
    ctx->x19 = ctx->x20 = ctx->x21 = ctx->x22 = ctx->x23 =
    ctx->x24 = ctx->x25 = ctx->x26 = ctx->x27 = ctx->x28 = ctx->fp = 0;
}

/* ------------------------------------------------------------------ */
/* x86-64                                                              */
/* ------------------------------------------------------------------ */
#elif defined(__x86_64__)

struct fast_ctx {
    uint64_t rip; /* return address / entry point — offset  0 */
    uint64_t rsp; /* stack pointer                — offset  8 */
    uint64_t rbx; /*                              — offset 16 */
    uint64_t rbp; /*                              — offset 24 */
    uint64_t r12; /*                              — offset 32 */
    uint64_t r13; /*                              — offset 40 */
    uint64_t r14; /*                              — offset 48 */
    uint64_t r15; /*                              — offset 56 */
};

/*
 * x86-64 ABI: at function entry RSP ≡ 8 (mod 16) — as if reached
 * via a CALL that pushed an 8-byte return address onto a 16-byte-
 * aligned stack.  We replicate that by aligning stack_top to 16 bytes
 * then subtracting 8.
 */
static inline void fast_ctx_init(struct fast_ctx *ctx, void *stack_top,
                                  void (*entry)(void))
{
    uintptr_t sp = (uintptr_t)stack_top;
    sp &= ~(uintptr_t)15;   /* 16-byte align                    */
    sp -= 8;                 /* simulate the implicit CALL push  */
    *(uint64_t *)sp = 0;     /* NULL fake return address         */

    ctx->rip = (uint64_t)(uintptr_t)entry;
    ctx->rsp = sp;
    ctx->rbx = ctx->rbp = ctx->r12 = ctx->r13 = ctx->r14 = ctx->r15 = 0;
}

#else
#error "fast_ctx: unsupported architecture (only x86-64 and AArch64 supported)"
#endif

void fast_swap_context(struct fast_ctx *save, const struct fast_ctx *restore);
void fast_restore_context(const struct fast_ctx *ctx);

#endif /* CONTEXT_H */
