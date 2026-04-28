#ifndef CONTEXT_H
#define CONTEXT_H

#include <stdint.h>

/*
 *
 * Minimal and fast context-switching implementation.
 *
 * Instead of saving the full OS context using swapcontext(),
 * we only save:
 *
 *   - PC (instruction pointer)
 *   - SP (stack pointer)
 *   - callee-saved registers
 *
 * Because:
 * - Caller-saved registers are volatile: the caller must save them if needed,
 *   the callee is free to overwrite them.
 * - Callee-saved registers must survive function calls: the callee must restore
 *   them before returning, so they hold their value across a context switch.
 * - Stack pointer defines the execution stack.
 * - PC defines the execution location (address of the next instruction to execute).
 */

/* ================================================================
 * AARCH64 (ARM64 ARCHITECTURE)
 * ================================================================ */
#ifdef __aarch64__

/*
 * Structure representing a full CPU execution context.
 *
 * IMPORTANT:
 * - uint64_t is used to guarantee exact 64-bit size
 * - layout must match context.S offsets exactly
 */
struct fast_ctx {

  /* LR — link register: holds the return address on function call.
   * Used as entry point when the thread is first scheduled. */
  uint64_t lr; /* offset 0 */

  uint64_t sp; /* offset 8  — stack pointer */

  /* Callee-saved registers (must be preserved across calls) */
  uint64_t x19; /* offset 16 */
  uint64_t x20; /* offset 24 */
  uint64_t x21; /* offset 32 */
  uint64_t x22; /* offset 40 */
  uint64_t x23; /* offset 48 */
  uint64_t x24; /* offset 56 */
  uint64_t x25; /* offset 64 */
  uint64_t x26; /* offset 72 */
  uint64_t x27; /* offset 80 */
  uint64_t x28; /* offset 88 */

  uint64_t fp; /* offset 96 — frame pointer */
};

/*
 * Initialize a new thread context (ARM64)
 */
static inline void fast_ctx_init(struct fast_ctx *ctx, void *stack_top, void (*entry)(void)) {
  uintptr_t sp = (uintptr_t)stack_top;

  /* ARM64 requires 16-byte aligned stack */
  sp &= ~(uintptr_t)15;

  ctx->lr = (uint64_t)(uintptr_t)entry;
  ctx->sp = sp;

  /* Zero callee-saved registers: no previous execution context exists. */
  ctx->x19 = ctx->x20 = ctx->x21 = ctx->x22 = ctx->x23 = ctx->x24 = ctx->x25 = ctx->x26 = ctx->x27 =
      ctx->x28 = ctx->fp = 0;
}

/* ================================================================
 * X86-64 ARCHITECTURE (PC STANDARD)
 * ================================================================ */
#elif defined(__x86_64__)

struct fast_ctx {

  uint64_t rip; /* offset 0  — instruction pointer */
  uint64_t rsp; /* offset 8  — stack pointer */

  /* Callee-saved registers */
  uint64_t rbx; /* offset 16 */
  uint64_t rbp; /* offset 24 */
  uint64_t r12; /* offset 32 */
  uint64_t r13; /* offset 40 */
  uint64_t r14; /* offset 48 */
  uint64_t r15; /* offset 56 */
};

/*
 * Initialize a new thread context (x86-64)
 */
static inline void fast_ctx_init(struct fast_ctx *ctx, void *stack_top, void (*entry)(void)) {
  uintptr_t sp = (uintptr_t)stack_top;

  /* 16-byte alignment required by ABI */
  sp &= ~(uintptr_t)15;

  /*
   * Simulate a CALL: push a fake return address so the stack is 16-byte
   * aligned when the entry function executes its prologue (ABI requirement).
   * The address is zero — the thread must never return from entry().
   */
  sp -= 8;
  *(uint64_t *)sp = 0;

  ctx->rip = (uint64_t)(uintptr_t)entry;
  ctx->rsp = sp;

  /* Zero callee-saved registers: no previous execution context exists. */
  ctx->rbx = ctx->rbp = ctx->r12 = ctx->r13 = ctx->r14 = ctx->r15 = 0;
}

#else
#error "fast_ctx: unsupported architecture"
#endif

/*
 * Switch execution from one thread to another
 */
void fast_swap_context(struct fast_ctx *save, const struct fast_ctx *restore);

/*
 * Restore a context without saving current one
 * (used when a thread exits)
 */
void fast_restore_context(const struct fast_ctx *ctx);

#endif /* CONTEXT_H */