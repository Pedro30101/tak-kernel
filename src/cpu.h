#ifndef CPU_H
#define CPU_H
#include "tak.h"

struct regs {
    u64 r15, r14, r13, r12, r11, r10, r9, r8, rbp, rdi, rsi, rdx, rcx, rbx, rax;
    u64 vec, err, rip, cs, rflags, rsp, ss;
};

void tss_rsp0(u64 sp);
void sched_tick(int from_user);
void proc_fault(struct regs *r);
long sys_call(struct regs *r);
void gfx_panic(const char *msg);
void enter_user(u64 rip, u64 rsp, u64 arg);
void task_done(void);
void switch_ctx(u64 *old, u64 nw);
void task_tramp(void);

#endif
