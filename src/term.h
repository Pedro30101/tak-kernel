#ifndef TERM_H
#define TERM_H
#include "wm.h"
win *term_open(const char *arg);
void term_out(void *t, const char *b, int n);
void term_run(win *w, const char *cmd);
#endif
