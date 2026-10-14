#include <stddef.h>
#include <stdint.h>
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_NO_GIF
#define STBI_NO_PSD
#define STBI_NO_PIC
#define STBI_NO_PNM
#define STBI_NO_TGA
#define STBI_NO_SIMD
#define STBI_NO_FAILURE_STRINGS
#define STBI_NO_THREAD_LOCALS
#define STBI_ASSERT(x) ((void)0)
#define STBI_MALLOC(sz) kalloc(sz)
#define STBI_REALLOC(p, n) krealloc(p, n)
#define STBI_FREE(p) kfree(p)
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

unsigned char *stb_decode(const unsigned char *d, int len, int *w, int *h) {
    int comp;
    return stbi_load_from_memory(d, len, w, h, &comp, 4);
}
