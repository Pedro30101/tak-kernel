#include "te.h"

int main(const char *arg) {
    printf("Hello from a .te program!\n");
    printf("pid %d, arg: '%s'\n", getpid(), arg && *arg ? arg : "(none)");
    printf("What's your name? ");
    char n[64];
    if (getline(n, sizeof n) > 0) printf("Nice to meet you, %s.\n", n);
    return 0;
}
