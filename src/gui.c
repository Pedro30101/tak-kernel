#include "gui.h"
#include "font8x16.h"

#define GUI_COLS 80
#define GUI_ROWS 25
#define FB_TAG_TYPE 8
#define MB2_TAG_END 0
#define RGB_PIXEL 1

static volatile uint8_t *fb;
static uint32_t fb_width;
static uint32_t fb_height;
static uint32_t fb_pitch;
static uint8_t fb_bpp;
static uint8_t red_pos, red_size, green_pos, green_size, blue_pos, blue_size;
static uint32_t cell_width, cell_height, console_x, console_y;
enum { POINTER_WIDTH = 24, POINTER_HEIGHT = 32, POINTER_SCALE = 2 };
static int pointer_x, pointer_y, pointer_drawn, pointer_initialized;
static uint32_t pointer_saved[POINTER_HEIGHT][POINTER_WIDTH];
static const uint16_t pointer_shape[POINTER_HEIGHT] = {
    0x800, 0xC00, 0xA00, 0x900, 0x880, 0x840, 0x820, 0x810,
    0x808, 0x808, 0x848, 0x890, 0xA20, 0xC40, 0x880, 0x070
};
static const uint32_t palette[16] = {
    0xFF000000, 0xFF0000AA, 0xFF00AA00, 0xFF00AAAA,
    0xFFAA0000, 0xFFAA00AA, 0xFFAA5500, 0xFFAAAAAA,
    0xFF555555, 0xFF5555FF, 0xFF55FF55, 0xFF55FFFF,
    0xFFFF5555, 0xFFFF55FF, 0xFFFFFF55, 0xFFFFFFFF
};

/*
 * Off-screen console. Cells are rendered here (RAM, fast) and only the cells
 * that actually changed are copied to the visible framebuffer in one pass.
 * This avoids the "blank panel, then repaint pixel by pixel" flashing.
 */
enum { MAX_CELL_W = 9, MAX_CELL_H = 16, CELL_UNKNOWN = 0xFFFFFFFFu };
static uint32_t console_back[GUI_COLS * MAX_CELL_W * GUI_ROWS * MAX_CELL_H];
static uint32_t shadow_cells[GUI_COLS * GUI_ROWS];
static uint32_t palette_native[16];
static uint32_t panel_native;
static uint32_t console_w, console_h;

static uint32_t scale_channel(uint8_t value, uint8_t bits) {
    if (bits == 0) return 0;
    if (bits == 8) return value;
    const uint32_t max_value = (UINT32_C(1) << bits) - 1;
    return ((uint32_t)value * max_value + 127) / 255;
}

/* Convert 0xAARRGGBB to the framebuffer's native pixel format. */
static uint32_t pack_color(uint32_t color) {
    const uint32_t r = scale_channel((uint8_t)(color >> 16), red_size);
    const uint32_t g = scale_channel((uint8_t)(color >> 8), green_size);
    const uint32_t b = scale_channel((uint8_t)color, blue_size);
    return (r << red_pos) | (g << green_pos) | (b << blue_pos);
}

static void put_pixel(uint32_t x, uint32_t y, uint32_t color) {
    if (x >= fb_width || y >= fb_height) return;
    volatile uint32_t *row = (volatile uint32_t *)(fb + (size_t)y * fb_pitch);
    row[x] = pack_color(color);
}

static uint32_t get_pixel(uint32_t x, uint32_t y) {
    const volatile uint32_t *row = (const volatile uint32_t *)(fb + (size_t)y * fb_pitch);
    return row[x];
}

static void draw_pointer(void) {
    if (fb == NULL) return;
    if (pointer_x < 0) pointer_x = 0;
    if (pointer_y < 0) pointer_y = 0;
    if ((uint32_t)pointer_x + POINTER_WIDTH > fb_width) pointer_x = (int)(fb_width - POINTER_WIDTH);
    if ((uint32_t)pointer_y + POINTER_HEIGHT > fb_height) pointer_y = (int)(fb_height - POINTER_HEIGHT);
    for (uint32_t y = 0; y < POINTER_HEIGHT; ++y) {
        for (uint32_t x = 0; x < POINTER_WIDTH; ++x) {
            pointer_saved[y][x] = get_pixel((uint32_t)pointer_x + x, (uint32_t)pointer_y + y);
            const uint32_t source_x = x / POINTER_SCALE;
            const uint32_t source_y = y / POINTER_SCALE;
            const uint16_t bit = (uint16_t)(0x800u >> source_x);
            if ((pointer_shape[source_y] & bit) != 0)
                put_pixel((uint32_t)pointer_x + x, (uint32_t)pointer_y + y,
                          source_x == 0 ? 0xFF080808 : 0xFFFFFF00);
        }
    }
    pointer_drawn = 1;
}

static void fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
    if (x >= fb_width || y >= fb_height) return;
    if (w > fb_width - x) w = fb_width - x;
    if (h > fb_height - y) h = fb_height - y;
    for (uint32_t py = y; py < y + h; ++py) {
        for (uint32_t px = x; px < x + w; ++px) put_pixel(px, py, color);
    }
}

/* Draw one CP437 character with the VGA 8x16 font (used for headers). */
static void draw_glyph(uint32_t x, uint32_t y, char ch, uint32_t fg, uint32_t bg, uint32_t scale) {
    const uint8_t *rows = vga_font_8x16[(uint8_t)ch];
    fill_rect(x, y, 8 * scale, 16 * scale, bg);
    for (unsigned row = 0; row < 16; ++row) {
        for (unsigned col = 0; col < 8; ++col) {
            if ((rows[row] & (0x80u >> col)) != 0)
                fill_rect(x + col * scale, y + row * scale, scale, scale, fg);
        }
    }
}

static void draw_text(uint32_t x, uint32_t y, const char *text, uint32_t fg, uint32_t bg, uint32_t scale) {
    for (size_t i = 0; text[i] != '\0'; ++i)
        draw_glyph(x + (uint32_t)i * 8 * scale, y, text[i], fg, bg, scale);
}

int gui_initialize(const uint32_t *multiboot_info) {
    if (multiboot_info == NULL) return 0;
    const uint8_t *base = (const uint8_t *)multiboot_info;
    const uint32_t total_size = *(const uint32_t *)base;
    if (total_size < 16) return 0;
    uint32_t offset = 8;
    while (offset + 8 <= total_size) {
        const uint32_t *tag = (const uint32_t *)(base + offset);
        const uint32_t type = tag[0];
        const uint32_t size = tag[1];
        if (size < 8 || offset + size > total_size) return 0;
        if (type == MB2_TAG_END) break;
        if (type == FB_TAG_TYPE && size >= 38) {
            const uint8_t *data = base + offset;
            const uint64_t address = *(const uint64_t *)(data + 8);
            fb_pitch = *(const uint32_t *)(data + 16);
            fb_width = *(const uint32_t *)(data + 20);
            fb_height = *(const uint32_t *)(data + 24);
            fb_bpp = data[28];
            if (data[29] != RGB_PIXEL || fb_bpp != 32 || address >= UINT64_C(0x100000000)) return 0;
            red_pos=data[32]; red_size=data[33]; green_pos=data[34]; green_size=data[35]; blue_pos=data[36]; blue_size=data[37];
            if (fb_width < 640 || fb_height < 400 || fb_pitch < fb_width * 4 ||
                address + (uint64_t)fb_pitch * fb_height > UINT64_C(0x100000000)) return 0;
            fb = (volatile uint8_t *)(uintptr_t)address;
            /* Real VGA text mode: 9x16 cells (720x400); 8-wide if the screen is narrow. */
            cell_width = fb_width >= 800 ? 9 : 8;
            cell_height = 16;
            console_x = fb_width >= 800 ? 32 : 0;
            console_y = fb_height >= 600 ? 116 : 92;
            if (console_y + GUI_ROWS * cell_height > fb_height)
                console_y = fb_height - GUI_ROWS * cell_height;
            console_w = GUI_COLS * cell_width;
            console_h = GUI_ROWS * cell_height;
            for (unsigned i = 0; i < 16; ++i) palette_native[i] = pack_color(palette[i]);
            panel_native = pack_color(0xFF202938);
            for (size_t i = 0; i < GUI_COLS * GUI_ROWS; ++i) shadow_cells[i] = CELL_UNKNOWN;
            fill_rect(0, 0, fb_width, fb_height, 0xFF111827);
            fill_rect(0, 0, fb_width, 42, 0xFF7F1D1D);
            draw_text(24, 5, "TAK", 0xFFFFFFFF, 0xFF7F1D1D, 2);
            draw_text(fb_width - 120, 13, "DESKTOP", 0xFFFFCCCC, 0xFF7F1D1D, 1);
            fill_rect(16, 64, fb_width - 32, fb_height - 80, 0xFF202938);
            fill_rect(16, 64, fb_width - 32, 34, 0xFFB91C1C);
            draw_text(32, 73, "TAK TERMINAL", 0xFFFFFFFF, 0xFFB91C1C, 1);
            fill_rect(16, fb_height - 24, fb_width - 32, 8, 0xFF7F1D1D);
            return 1;
        }
        offset = (offset + size + 7) & ~7u;
    }
    return 0;
}

/* Render one cell into the off-screen buffer only (no screen access). */
static void render_cell(size_t index, uint16_t cell) {
    const uint32_t col = (uint32_t)(index % GUI_COLS);
    const uint32_t row = (uint32_t)(index / GUI_COLS);
    const uint8_t ch = (uint8_t)cell;
    const uint8_t attributes = (uint8_t)(cell >> 8);
    const uint32_t fg = palette_native[attributes & 0x0F];
    const uint32_t bg = palette_native[attributes >> 4];
    const uint8_t *glyph = vga_font_8x16[ch];
    /* Like real VGA, the 9th column repeats the 8th for box/block characters. */
    const int line_graphics = ch >= 0xC0 && ch <= 0xDF;
    uint32_t *dst = console_back + (size_t)row * cell_height * console_w +
                    (size_t)col * cell_width;
    for (uint32_t py = 0; py < cell_height; ++py) {
        const uint8_t bits = glyph[py];
        uint32_t *line = dst + (size_t)py * console_w;
        for (uint32_t px = 0; px < cell_width; ++px) {
            const int on = px < 8 ? (bits & (0x80u >> px)) != 0
                                  : (line_graphics && (bits & 1u) != 0);
            line[px] = on ? fg : bg;
        }
    }
}

/* Copy a rectangle (console-relative pixels) from the back buffer to the screen. */
static void present_region(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    if (console_x + x >= fb_width) return;
    if (w > fb_width - console_x - x) w = fb_width - console_x - x;
    for (uint32_t r = 0; r < h; ++r) {
        if (console_y + y + r >= fb_height) break;
        const uint32_t *src = console_back + (size_t)(y + r) * console_w + x;
        uint32_t *dst = (uint32_t *)(fb + (size_t)(console_y + y + r) * fb_pitch) +
                        console_x + x;
        size_t count = w;
        __asm__ volatile ("rep movsl" : "+D"(dst), "+S"(src), "+c"(count) : : "memory");
    }
}

static int cell_overlaps_pointer(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    return pointer_drawn &&
        x < (uint32_t)pointer_x + POINTER_WIDTH && x + w > (uint32_t)pointer_x &&
        y < (uint32_t)pointer_y + POINTER_HEIGHT && y + h > (uint32_t)pointer_y;
}

void gui_clear_console(void) {
    if (fb == NULL) return;
    const int redraw_pointer = pointer_drawn;
    if (redraw_pointer) gui_mouse_erase();
    for (size_t i = 0; i < (size_t)console_w * console_h; ++i) console_back[i] = panel_native;
    for (size_t i = 0; i < GUI_COLS * GUI_ROWS; ++i) shadow_cells[i] = CELL_UNKNOWN;
    present_region(0, 0, console_w, console_h);
    if (redraw_pointer) draw_pointer();
}

void gui_draw_cell(size_t index, uint16_t cell) {
    if (fb == NULL || index >= GUI_COLS * GUI_ROWS) return;
    if (shadow_cells[index] == cell) return;
    const uint32_t cx = (uint32_t)(index % GUI_COLS) * cell_width;
    const uint32_t cy = (uint32_t)(index / GUI_COLS) * cell_height;
    const int overlaps_pointer =
        cell_overlaps_pointer(console_x + cx, console_y + cy, cell_width, cell_height);
    if (overlaps_pointer) gui_mouse_erase();
    render_cell(index, cell);
    shadow_cells[index] = cell;
    present_region(cx, cy, cell_width, cell_height);
    if (overlaps_pointer) draw_pointer();
}

void gui_redraw_console(const volatile uint16_t *cells, size_t count) {
    if (fb == NULL) return;
    uint32_t first[GUI_ROWS], last[GUI_ROWS];
    int any_dirty = 0;
    /* Pass 1: update only the cells that changed, in RAM. */
    for (uint32_t row = 0; row < GUI_ROWS; ++row) {
        first[row] = GUI_COLS;
        last[row] = 0;
        for (uint32_t col = 0; col < GUI_COLS; ++col) {
            const size_t i = (size_t)row * GUI_COLS + col;
            if (i >= count) break;
            const uint16_t cell = cells[i];
            if (shadow_cells[i] == cell) continue;
            render_cell(i, cell);
            shadow_cells[i] = cell;
            if (col < first[row]) first[row] = col;
            last[row] = col;
            any_dirty = 1;
        }
    }
    if (!any_dirty) return;
    /* Pass 2: copy the changed spans to the screen. No blanking step. */
    const int redraw_pointer = pointer_drawn;
    if (redraw_pointer) gui_mouse_erase();
    for (uint32_t row = 0; row < GUI_ROWS; ++row) {
        if (first[row] >= GUI_COLS) continue;
        present_region(first[row] * cell_width, row * cell_height,
                       (last[row] - first[row] + 1) * cell_width, cell_height);
    }
    if (redraw_pointer) draw_pointer();
}

void gui_mouse_draw(void) {
    if (fb == NULL) return;
    if (!pointer_initialized) {
        pointer_x = (int)(fb_width / 2);
        pointer_y = (int)(fb_height / 2);
        pointer_initialized = 1;
    }
    if (!pointer_drawn) draw_pointer();
}

void gui_mouse_erase(void) {
    if (!pointer_drawn || fb == NULL) return;
    for (uint32_t y = 0; y < POINTER_HEIGHT; ++y)
        for (uint32_t x = 0; x < POINTER_WIDTH; ++x)
            put_pixel((uint32_t)pointer_x + x, (uint32_t)pointer_y + y,
                      pointer_saved[y][x]);
    pointer_drawn = 0;
}

void gui_mouse_move(int dx, int dy) {
    if (fb == NULL) return;
    gui_mouse_erase();
    pointer_x += dx;
    pointer_y += dy;
    draw_pointer();
}
