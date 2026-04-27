#ifndef CONTEXT_H
#define CONTEXT_H

#include <stdint.h>

/*
 *
 * Minimal context-switching implementation.
 *
 * Instead of saving the full OS context using swapcontext(),
 * we only save:
 *
 *   - PC (instruction pointer)
 *   - SP (stack pointer)
 *   - callee-saved registers
 *
 * Because:
 * - Caller-saved registers are volatile (not needed)
 * These are registers that the called function is required to restore them before returning.
 * - Callee-saved registers must survive function calls
 * These are registers that the called function is required to save them to use them afterwards.
 * - Stack pointer defines execution stack
 * - PC defines execution location (contains the adress of the next instruction)
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
 * - layout MUST match context.S offsets exactly
 */
struct fast_ctx {

  /* (LR) - Link register (return address)
   * Where execution will resume when returning 
   or where a thread will start execution */
  uint64_t lr;  /* offset 0 */

  /* Stack pointer (SP) */
  uint64_t sp;  /* offset 8 */

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

  /* Frame pointer used for stack frames */
  uint64_t fp;  /* offset 96 */
};

/*
 * Initialize a new thread context (ARM64)
 */
static inline void fast_ctx_init(struct fast_ctx *ctx,
                                  void *stack_top,
                                  void (*entry)(void))
{
  uintptr_t sp = (uintptr_t)stack_top;

  /* ARM64 requires 16-byte aligned stack */
  // Align stack pointer down to nearest 16-byte boundary
  sp &= ~(uintptr_t)15;

  /*
   * When thread starts, LR must point to entry function.
   */
  ctx->lr = (uint64_t)(uintptr_t)entry;

  /* Initialize stack pointer */
  ctx->sp = sp;

  /*
   * Clear registers (clean initial state)
   * No previous execution context exists.
   */
  ctx->x19 = ctx->x20 = ctx->x21 = ctx->x22 =
  ctx->x23 = ctx->x24 = ctx->x25 = ctx->x26 =
  ctx->x27 = ctx->x28 = ctx->fp = 0;
}

/* ================================================================
 * X86-64 ARCHITECTURE (PC STANDARD)
 * ================================================================ */
#elif defined(__x86_64__)

struct fast_ctx {

  /* Instruction pointer (where execution resumes) */
  uint64_t rip;  /* offset 0 */

  /* Stack pointer */
  uint64_t rsp;  /* offset 8 */

  /* Callee-saved registers */
  uint64_t rbx;  /* offset 16 */
  uint64_t rbp;  /* offset 24 */
  uint64_t r12;  /* offset 32 */
  uint64_t r13;  /* offset 40 */
  uint64_t r14;  /* offset 48 */
  uint64_t r15;  /* offset 56 */
};

/*
 * Initialize a new thread context (x86-64)
 */
static inline void fast_ctx_init(struct fast_ctx *ctx,
                                  void *stack_top,
                                  void (*entry)(void))
{
  uintptr_t sp = (uintptr_t)stack_top;

  /* 16-byte alignment required by ABI */
  sp &= ~(uintptr_t)15;

  /*
   * Simulate a CALL instruction:
   * CALL pushes return address on stack.
   */
  // Move stack pointer down to reserve space for return address
  sp -= 8;

  /* Fake return address (never used but required for ret safety) */
  *(uint64_t *)sp = 0;

  /*
   * Entry point of thread execution
   */
  ctx->rip = (uint64_t)(uintptr_t)entry;

  /* Set stack pointer */
  ctx->rsp = sp;

  /* Clear callee-saved registers */
  ctx->rbx = ctx->rbp = ctx->r12 =
  ctx->r13 = ctx->r14 = ctx->r15 = 0;
}

#else
#error "fast_ctx: unsupported architecture"
#endif

/*
 * Switch execution from one thread to another
 */
void fast_swap_context(struct fast_ctx *save,
                       const struct fast_ctx *restore);

/*
 * Restore a context WITHOUT saving current one
 * (used when a thread exits)
 */
void fast_restore_context(const struct fast_ctx *ctx);

#endif /* CONTEXT_H */