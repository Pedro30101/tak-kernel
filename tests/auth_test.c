#include "../src/auth.c"
#include <stdio.h>

static struct fsops fake_ops;
static node fake_disk, fake_auth_file;
static u8 saved_record[sizeof(struct auth_record)];
static int saved;

node *vfs_find(const char *path, node *cwd) {
    (void)cwd;
    return !strcmp(path, AUTH_PATH) ? &fake_disk : NULL;
}
node *vfs_child(node *dir, const char *name) {
    return dir == &fake_disk && !strcmp(name, AUTH_FILE) && saved ? &fake_auth_file : NULL;
}
i64 vfs_rd(node *n, u64 off, void *buf, u64 len) {
    if (n != &fake_auth_file || off > sizeof saved_record || len > sizeof saved_record - off) return -1;
    memcpy(buf, saved_record + off, len);
    return (i64)len;
}
int vfs_save(node *dir, const char *name, const void *data, u64 len) {
    if (dir != &fake_disk || strcmp(name, AUTH_FILE) || len != sizeof saved_record) return -1;
    memcpy(saved_record, data, len);
    fake_auth_file.dir = 0;
    fake_auth_file.size = len;
    saved = 1;
    return 0;
}

static int check_kdf(u32 rounds, const char *expected) {
    u8 salt[AUTH_SALT_SIZE] = { 's', 'a', 'l', 't' };
    u8 result[AUTH_HASH_SIZE];
    char actual[AUTH_HASH_SIZE * 2 + 1];
    static const char digits[] = "0123456789abcdef";
    derive("password", salt, rounds, result);
    for (int i = 0; i < AUTH_HASH_SIZE; i++) {
        actual[i * 2] = digits[result[i] >> 4];
        actual[i * 2 + 1] = digits[result[i] & 15];
    }
    actual[sizeof actual - 1] = 0;
    auth_wipe(result, sizeof result);
    return strcmp(actual, expected) == 0;
}

int main(void) {
    fake_disk.dir = 1;
    fake_disk.ops = &fake_ops;
    if (!check_kdf(1, "3267a06614b9d090bc3e684eebdb6af8ee753cf80b7f7f7cf5e676c65422d054") ||
        !check_kdf(2, "8db3277ed085e8ab63e4fdd5c11e6c60bd3788ab4164f25d6349786fa6262023") ||
        !check_kdf(AUTH_ITERATIONS, "30b21944d0a92aff52c714d73cc8ed3e917f2fc02d6c548dbe59b09dc082449b")) {
        fputs("PBKDF2-HMAC-SHA-256 test vector mismatch\n", stderr);
        return 1;
    }
    if (!valid_username("astro_user-1") || valid_username("../home") ||
        valid_username(".") || valid_password("short") || !valid_password("a 12 character pass")) {
        fputs("Account input validation test failed\n", stderr);
        return 1;
    }
    if (auth_init() != AUTH_SETUP ||
        auth_create("Astro_User", "a 12 character pass") != 0 ||
        auth_verify("astro_user", "a 12 character pass") != 0 ||
        auth_verify("astro_user", "wrong password") == 0 ||
        auth_init() != AUTH_LOGIN) {
        fputs("Persistent account setup and login test failed\n", stderr);
        return 1;
    }
    auth_wipe(saved_record, sizeof saved_record);
    if (auth_init() != AUTH_ERROR || auth_create("attacker", "a 12 character pass") == 0) {
        fputs("Corrupt credential-store fail-closed test failed\n", stderr);
        return 1;
    }
    puts("Authentication KDF vectors, account flow, and fail-closed checks passed.");
    return 0;
}
