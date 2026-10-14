#ifndef PROC_H
#define PROC_H
#include "tak.h"

void proc_init(void);
int proc_spawn(const char *path, const char *arg, void *term);
int proc_alive(int pid);
void proc_stdin(int pid, int ch);
int proc_kill(int pid);
void proc_list(void (*cb)(int pid, const char *name, int state, u64 kb, void *ctx), void *ctx);
void proc_yield(void);
int proc_runnable(void);
void proc_wake(int pid);

void term_out(void *t, const char *b, int n);

#endif
