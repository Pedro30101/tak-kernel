#ifndef AUTH_H
#define AUTH_H

#include "tak.h"

enum {
    AUTH_ERROR = -1,
    AUTH_SETUP = 0,
    AUTH_LOGIN = 1
};

int auth_init(void);
int auth_create(const char *username, const char *password);
int auth_verify(const char *username, const char *password);
void auth_username(char *out, size_t cap);
void auth_wipe(void *data, size_t len);

#endif
