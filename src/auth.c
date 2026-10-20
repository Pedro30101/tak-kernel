#include "tak.h"
#include "auth.h"
#include "vfs.h"

#define AUTH_ITERATIONS 100000u
#define AUTH_SALT_SIZE 16
#define AUTH_HASH_SIZE 32
#define AUTH_MIN_PASSWORD 3
#define AUTH_MAX_PASSWORD 9999
#define AUTH_PATH "/disk"
#define AUTH_FILE ".astros-auth"

struct auth_record {
    char magic[8];
    u32 version;
    u32 iterations;
    u8 salt[AUTH_SALT_SIZE];
    u8 verifier[AUTH_HASH_SIZE];
    char username[32];
} __attribute__((packed));

struct sha256 {
    u32 h[8];
    u64 bits;
    u8 block[64];
    u32 used;
};

static const u32 sha_k[64] = {
    0x428A2F98, 0x71374491, 0xB5C0FBCF, 0xE9B5DBA5, 0x3956C25B, 0x59F111F1, 0x923F82A4, 0xAB1C5ED5,
    0xD807AA98, 0x12835B01, 0x243185BE, 0x550C7DC3, 0x72BE5D74, 0x80DEB1FE, 0x9BDC06A7, 0xC19BF174,
    0xE49B69C1, 0xEFBE4786, 0x0FC19DC6, 0x240CA1CC, 0x2DE92C6F, 0x4A7484AA, 0x5CB0A9DC, 0x76F988DA,
    0x983E5152, 0xA831C66D, 0xB00327C8, 0xBF597FC7, 0xC6E00BF3, 0xD5A79147, 0x06CA6351, 0x14292967,
    0x27B70A85, 0x2E1B2138, 0x4D2C6DFC, 0x53380D13, 0x650A7354, 0x766A0ABB, 0x81C2C92E, 0x92722C85,
    0xA2BFE8A1, 0xA81A664B, 0xC24B8B70, 0xC76C51A3, 0xD192E819, 0xD6990624, 0xF40E3585, 0x106AA070,
    0x19A4C116, 0x1E376C08, 0x2748774C, 0x34B0BCB5, 0x391C0CB3, 0x4ED8AA4A, 0x5B9CCA4F, 0x682E6FF3,
    0x748F82EE, 0x78A5636F, 0x84C87814, 0x8CC70208, 0x90BEFFFA, 0xA4506CEB, 0xBEF9A3F7, 0xC67178F2
};

static struct auth_record current;
static int current_state = AUTH_ERROR;

static u32 rotr(u32 x, u32 n) { return (x >> n) | (x << (32 - n)); }

static void sha_transform(struct sha256 *s, const u8 *p) {
    u32 w[64];
    for (int i = 0; i < 16; i++)
        w[i] = (u32)p[i * 4] << 24 | (u32)p[i * 4 + 1] << 16 | (u32)p[i * 4 + 2] << 8 | p[i * 4 + 3];
    for (int i = 16; i < 64; i++) {
        u32 a = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        u32 b = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + a + w[i - 7] + b;
    }
    u32 a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3];
    u32 e = s->h[4], f = s->h[5], g = s->h[6], h = s->h[7];
    for (int i = 0; i < 64; i++) {
        u32 s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        u32 ch = (e & f) ^ (~e & g);
        u32 t1 = h + s1 + ch + sha_k[i] + w[i];
        u32 s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        u32 maj = (a & b) ^ (a & c) ^ (b & c);
        u32 t2 = s0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d;
    s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
    auth_wipe(w, sizeof w);
}

static void sha_init(struct sha256 *s) {
    static const u32 init[8] = {
        0x6A09E667, 0xBB67AE85, 0x3C6EF372, 0xA54FF53A,
        0x510E527F, 0x9B05688C, 0x1F83D9AB, 0x5BE0CD19
    };
    memcpy(s->h, init, sizeof init);
    s->bits = 0;
    s->used = 0;
}

static void sha_update(struct sha256 *s, const void *data, size_t len) {
    const u8 *p = data;
    s->bits += (u64)len * 8;
    while (len) {
        u32 n = 64 - s->used;
        if (n > len) n = (u32)len;
        memcpy(s->block + s->used, p, n);
        s->used += n;
        p += n;
        len -= n;
        if (s->used == 64) {
            sha_transform(s, s->block);
            s->used = 0;
        }
    }
}

static void sha_final(struct sha256 *s, u8 out[32]) {
    u64 bits = s->bits;
    u8 one = 0x80, zero = 0;
    sha_update(s, &one, 1);
    while (s->used != 56) sha_update(s, &zero, 1);
    u8 tail[8];
    for (int i = 0; i < 8; i++) tail[7 - i] = (u8)(bits >> (i * 8));
    sha_update(s, tail, sizeof tail);
    for (int i = 0; i < 8; i++) {
        out[i * 4] = (u8)(s->h[i] >> 24);
        out[i * 4 + 1] = (u8)(s->h[i] >> 16);
        out[i * 4 + 2] = (u8)(s->h[i] >> 8);
        out[i * 4 + 3] = (u8)s->h[i];
    }
    auth_wipe(tail, sizeof tail);
    auth_wipe(s, sizeof *s);
}

static void hmac_sha256(const u8 *key, size_t key_len, const u8 *data, size_t data_len, u8 out[32]) {
    u8 k[64] = {0}, inner[32], pad[64];
    struct sha256 s;
    if (key_len > sizeof k) {
        sha_init(&s);
        sha_update(&s, key, key_len);
        sha_final(&s, k);
    } else {
        memcpy(k, key, key_len);
    }
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x36;
    sha_init(&s);
    sha_update(&s, pad, sizeof pad);
    sha_update(&s, data, data_len);
    sha_final(&s, inner);
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x5C;
    sha_init(&s);
    sha_update(&s, pad, sizeof pad);
    sha_update(&s, inner, sizeof inner);
    sha_final(&s, out);
    auth_wipe(k, sizeof k);
    auth_wipe(inner, sizeof inner);
    auth_wipe(pad, sizeof pad);
}

static void derive(const char *password, const u8 salt[AUTH_SALT_SIZE], u32 rounds, u8 out[AUTH_HASH_SIZE]) {
    u8 input[AUTH_SALT_SIZE + 4], u[AUTH_HASH_SIZE], acc[AUTH_HASH_SIZE];
    u8 password_key[AUTH_HASH_SIZE];
    const u8 *key = (const u8 *)password;
    size_t key_len = strlen(password);
    if (key_len > 64) {
        struct sha256 s;
        sha_init(&s);
        sha_update(&s, password, key_len);
        sha_final(&s, password_key);
        key = password_key;
        key_len = sizeof password_key;
    }
    memcpy(input, salt, AUTH_SALT_SIZE);
    input[AUTH_SALT_SIZE] = 0;
    input[AUTH_SALT_SIZE + 1] = 0;
    input[AUTH_SALT_SIZE + 2] = 0;
    input[AUTH_SALT_SIZE + 3] = 1;
    hmac_sha256(key, key_len, input, sizeof input, u);
    memcpy(acc, u, sizeof acc);
    for (u32 i = 1; i < rounds; i++) {
        hmac_sha256(key, key_len, u, sizeof u, u);
        for (int j = 0; j < AUTH_HASH_SIZE; j++) acc[j] ^= u[j];
    }
    memcpy(out, acc, sizeof acc);
    auth_wipe(input, sizeof input);
    auth_wipe(u, sizeof u);
    auth_wipe(acc, sizeof acc);
    auth_wipe(password_key, sizeof password_key);
}

static int random_bytes(u8 *out, size_t len) {
#ifdef AUTH_TEST
    static u8 test_byte = 1;
    while (len--) *out++ = test_byte++;
    return 0;
#else
    u32 a = 1, b, c, d;
    __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "=c"(c), "=d"(d));
    (void)b; (void)d;
    if (!(c & (1u << 30))) return -1;
    while (len) {
        u64 value;
        u8 ready;
        int ok = 0;
        for (int i = 0; i < 10; i++) {
            __asm__ volatile("rdrand %0; setc %1" : "=r"(value), "=qm"(ready));
            if (ready) { ok = 1; break; }
        }
        if (!ok) return -1;
        size_t n = len < sizeof value ? len : sizeof value;
        memcpy(out, &value, n);
        out += n;
        len -= n;
        auth_wipe(&value, sizeof value);
    }
    return 0;
#endif
}

void auth_wipe(void *data, size_t len) {
    volatile u8 *p = data;
    while (len--) *p++ = 0;
}

static int valid_username(const char *name) {
    size_t n = strlen(name);
    if (!n || n > 31 || !strcmp(name, ".") || !strcmp(name, "..")) return 0;
    for (size_t i = 0; i < n; i++) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) return 0;
    }
    return 1;
}

static int valid_password(const char *password) {
    size_t n = strlen(password);
    if (n < AUTH_MIN_PASSWORD || n > AUTH_MAX_PASSWORD) return 0;
    for (size_t i = 0; i < n; i++)
        if ((u8)password[i] < 0x20 || (u8)password[i] > 0x7E) return 0;
    return 1;
}

static int has_terminator(const char *text, size_t cap) {
    for (size_t i = 0; i < cap; i++) if (!text[i]) return 1;
    return 0;
}

static node *auth_directory(void) {
    node *dir = vfs_find(AUTH_PATH, NULL);
    return dir && dir->dir && dir->ops ? dir : NULL;
}

static int record_valid(const struct auth_record *r) {
    if (memcmp(r->magic, "TAKAUTH1", 8) || r->version != 1 ||
        r->iterations != AUTH_ITERATIONS || !has_terminator(r->username, sizeof r->username) ||
        !valid_username(r->username)) return 0;
    u8 nonzero = 0;
    for (int i = 0; i < AUTH_SALT_SIZE; i++) nonzero |= r->salt[i];
    return nonzero != 0;
}

int auth_init(void) {
    auth_wipe(&current, sizeof current);
    current_state = AUTH_ERROR;
    node *dir = auth_directory();
    if (!dir) return AUTH_ERROR;
    node *file = vfs_child(dir, AUTH_FILE);
    if (!file) {
        current_state = AUTH_SETUP;
        return current_state;
    }
    if (file->dir || file->size != sizeof current ||
        vfs_rd(file, 0, &current, sizeof current) != (i64)sizeof current ||
        !record_valid(&current)) {
        auth_wipe(&current, sizeof current);
        return AUTH_ERROR;
    }
    current_state = AUTH_LOGIN;
    return current_state;
}

int auth_create(const char *username, const char *password) {
    if (current_state != AUTH_SETUP || !valid_username(username) || !valid_password(password))
        return -1;
    node *dir = auth_directory();
    if (!dir || vfs_child(dir, AUTH_FILE)) return -1;
    struct auth_record next;
    memset(&next, 0, sizeof next);
    memcpy(next.magic, "TAKAUTH1", 8);
    next.version = 1;
    next.iterations = AUTH_ITERATIONS;
    for (size_t i = 0; username[i]; i++)
        next.username[i] = (char)tolower((u8)username[i]);
    if (random_bytes(next.salt, sizeof next.salt) < 0) {
        auth_wipe(&next, sizeof next);
        return -2;
    }
    derive(password, next.salt, next.iterations, next.verifier);
    if (vfs_save(dir, AUTH_FILE, &next, sizeof next) < 0) {
        auth_wipe(&next, sizeof next);
        return -1;
    }
    memcpy(&current, &next, sizeof current);
    current_state = AUTH_LOGIN;
    auth_wipe(&next, sizeof next);
    return 0;
}

int auth_verify(const char *username, const char *password) {
    if (current_state != AUTH_LOGIN || !username || !password ||
        !valid_username(username) || !valid_password(password)) return -1;
    if (strcasecmp(username, current.username)) return -1;
    u8 check[AUTH_HASH_SIZE];
    derive(password, current.salt, current.iterations, check);
    u8 diff = 0;
    for (int i = 0; i < AUTH_HASH_SIZE; i++) diff |= check[i] ^ current.verifier[i];
    auth_wipe(check, sizeof check);
    return diff ? -1 : 0;
}

void auth_username(char *out, size_t cap) {
    if (!out || !cap) return;
    snprintf(out, cap, "%s", current_state == AUTH_LOGIN ? current.username : "");
}

node *auth_home_dir(void) {
    if (current_state != AUTH_LOGIN) return vfs_find("/home", NULL);
    node *disk = vfs_find(AUTH_PATH, NULL);
    if (!disk || !disk->dir || !disk->ops) return NULL;
    node *home = vfs_child(disk, "home");
    if (!home) home = vfs_mk(disk, "home", 1);
    if (!home || !home->dir) return NULL;
    node *user = vfs_child(home, current.username);
    if (!user) user = vfs_mk(home, current.username, 1);
    return user && user->dir ? user : NULL;
}
