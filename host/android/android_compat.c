/* Android compat for APIs bionic lacks or gates behind newer API levels.
 *
 * - pthread_{get,set}affinity_np: bionic only exports them at API 36; we target 30.
 *   Implement on top of pthread_gettid_np + sched_{get,set}affinity (both API 21+).
 * - ucontext fibers: bionic has <ucontext.h> types but no getcontext/makecontext/
 *   swapcontext. win32_posix.c's CreateFiberEx/SwitchToFiber need them for
 *   nn::os::FiberType. aarch64 implementations below save/restore x0..x30, sp,
 *   pc and nzcv — sufficient for cooperative fiber switching (caller-saved regs
 *   are dead at switch points; callee-saved x19..x28 round-trip exactly).
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdarg.h>
#include <sys/ucontext.h>
#include <sys/syscall.h>
#include <unistd.h>

#if defined(__ANDROID__) && defined(__aarch64__)

/* sigcontext (mcontext_t) layout, aarch64 kernel uapi:
 *   fault_address @0, regs[31] @8 (x0..x30), sp @256, pc @264, pstate @272 */
#define SC_REGS  8
#define SC_SP    256
#define SC_PC    264
#define SC_PSTATE 272

extern void hos_a64_ctx_save(void *mctx);          /* x0 = mcontext_t* */
extern void hos_a64_ctx_restore(void *mctx);       /* x0 = mcontext_t*, does not return to caller-of-restore in the usual sense */
extern void hos_a64_swapcontext(void *oucp, void *ucp);
extern void hos_a64_ctx_trampoline(void);          /* entered by a made context */

int getcontext(ucontext_t *ucp) {
    hos_a64_ctx_save(&ucp->uc_mcontext);
    return 0;
}

void makecontext(ucontext_t *ucp, void (*func)(void), int argc, ...) {
    mcontext_t *m = &ucp->uc_mcontext;
    va_list ap;
    va_start(ap, argc);
    int n = argc > 8 ? 8 : argc;
    for (int i = 0; i < n; i++)
        m->regs[i] = (uint32_t)va_arg(ap, unsigned);
    for (int i = n; i < 8; i++)
        m->regs[i] = 0;
    va_end(ap);
    /* x9 carries the function; the trampoline moves it to x16 and calls it. */
    m->regs[9] = (uint64_t)(uintptr_t)func;
    m->regs[30] = 0;
    m->pc = (uint64_t)(uintptr_t)hos_a64_ctx_trampoline;
    uintptr_t top = (uintptr_t)ucp->uc_stack.ss_sp + ucp->uc_stack.ss_size;
    m->sp = (top - 256) & ~(uintptr_t)15;
    m->pstate = 0;
}

int swapcontext(ucontext_t *oucp, ucontext_t *ucp) {
    hos_a64_swapcontext(&oucp->uc_mcontext, &ucp->uc_mcontext);
    return 0;   /* reached when oucp is later restored */
}

#else /* !aarch64-android: desktop targets have real ucontext */

/* still provide affinity on Android regardless of arch */
#endif

#if defined(__ANDROID__)
int pthread_setaffinity_np(pthread_t th, size_t cpusetsize, const cpu_set_t *set) {
    pid_t tid = pthread_gettid_np(th);
    if (tid <= 0) tid = (pid_t)syscall(__NR_gettid);
    return sched_setaffinity(tid, cpusetsize, set);
}
int pthread_getaffinity_np(pthread_t th, size_t cpusetsize, cpu_set_t *set) {
    pid_t tid = pthread_gettid_np(th);
    if (tid <= 0) tid = (pid_t)syscall(__NR_gettid);
    return sched_getaffinity(tid, cpusetsize, set);
}

/* bionic exports backtrace() only at API 34; we target 30. Crash reports lose the
 * host stack on Android — acceptable for now. */
#include <execinfo.h>
int backtrace(void **buffer, int size) { (void)buffer; (void)size; return 0; }
#endif
