#include <stddef.h>
#include <stdint.h>
#include "gui.h"

#define VGA_WIDTH 80
#define VGA_HEIGHT 25
#define MOUSE_CELL_WIDTH 9
#define MOUSE_CELL_HEIGHT 16
#define VGA_MEMORY ((volatile uint16_t *)(uintptr_t)0xB8000)
#define VGA_COLOR(fg, bg) ((uint8_t)((bg) << 4 | (fg)))
#define VGA_ENTRY(ch, color) ((uint16_t)(uint8_t)(ch) | (uint16_t)(color) << 8)
#define COMMAND_MAX 128
static const char TAK_VERSION[] = "0.9.6b (0.9.6 Beta)";

_Static_assert(sizeof(void *) == 8, "TAK must be compiled for x86_64");

enum {
    COLOR_BLACK = 0,
    COLOR_LIGHT_GREY = 7,
    COLOR_CYAN = 3,
    COLOR_LIGHT_GREEN = 10,
    COLOR_LIGHT_RED = 12
};

static size_t cursor_x;
static size_t cursor_y;
static uint16_t terminal_cells[VGA_WIDTH * VGA_HEIGHT];
static uint8_t console_color = VGA_COLOR(COLOR_LIGHT_GREY, COLOR_BLACK);
static int gui_mode;

typedef enum {
    KEYBOARD_LAYOUT_US,
    KEYBOARD_LAYOUT_BR_ABNT2
} keyboard_layout_t;

static keyboard_layout_t active_keyboard_layout = KEYBOARD_LAYOUT_US;
static int caps_lock_on;
static int caps_lock_key_down;
static int shift_down;
static int mouse_enabled;
static int mouse_cursor_drawn;
static int mouse_x = VGA_WIDTH / 2;
static int mouse_y = VGA_HEIGHT / 2;
static int mouse_motion_x;
static int mouse_motion_y;
static uint16_t mouse_saved_cell;
static uint8_t mouse_packet[3];
static uint8_t mouse_packet_index;

enum {
    DEAD_KEY_ACUTE = 1,
    DEAD_KEY_GRAVE,
    DEAD_KEY_TILDE,
    DEAD_KEY_CIRCUMFLEX,
    DEAD_KEY_DIAERESIS
};

static inline uint8_t inb(uint16_t port) {
    uint8_t value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void outb(uint16_t port, uint8_t value) {
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint32_t inl(uint16_t port) {
    uint32_t value;
    __asm__ volatile ("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void outl(uint16_t port, uint32_t value) {
    __asm__ volatile ("outl %0, %1" : : "a"(value), "Nd"(port));
}

static void update_cursor(void) {
    if (gui_mode) return;
    const uint16_t position = (uint16_t)(cursor_y * VGA_WIDTH + cursor_x);
    outb(0x3D4, 0x0F);
    outb(0x3D5, (uint8_t)(position & 0xFF));
    outb(0x3D4, 0x0E);
    outb(0x3D5, (uint8_t)(position >> 8));
}

static size_t mouse_cell_index(void) {
    return (size_t)mouse_y * VGA_WIDTH + (size_t)mouse_x;
}

static void mouse_erase_cursor(void) {
    if (mouse_cursor_drawn) {
        if (gui_mode) {
            gui_mouse_erase();
        } else {
            const size_t index = mouse_cell_index();
            VGA_MEMORY[index] = mouse_saved_cell;
        }
        mouse_cursor_drawn = 0;
    }
}

static void mouse_draw_cursor(void) {
    if (!mouse_enabled || mouse_cursor_drawn) {
        return;
    }
    if (gui_mode) {
        gui_mouse_draw();
        mouse_cursor_drawn = 1;
        return;
    }
    const size_t index = mouse_cell_index();
    mouse_saved_cell = VGA_MEMORY[index];
    VGA_MEMORY[index] = VGA_ENTRY('X', VGA_COLOR(COLOR_BLACK, COLOR_LIGHT_GREY));
    mouse_cursor_drawn = 1;
}

static void vga_write_cell(size_t index, uint16_t value) {
    if (index >= VGA_WIDTH * VGA_HEIGHT) return;
    terminal_cells[index] = value;
    if (!gui_mode && mouse_cursor_drawn && index == mouse_cell_index()) {
        mouse_saved_cell = value;
        return;
    }
    VGA_MEMORY[index] = value;
    if (gui_mode) gui_draw_cell(index, value);
}

static void clear_screen(void) {
    mouse_erase_cursor();
    for (size_t i = 0; i < VGA_WIDTH * VGA_HEIGHT; ++i) {
        terminal_cells[i] = VGA_ENTRY(' ', console_color);
        VGA_MEMORY[i] = terminal_cells[i];
    }
    if (gui_mode) gui_redraw_console(terminal_cells, VGA_WIDTH * VGA_HEIGHT);
    cursor_x = 0;
    cursor_y = 0;
    mouse_draw_cursor();
    update_cursor();
}

static void scroll_screen(void) {
    mouse_erase_cursor();
    for (size_t row = 1; row < VGA_HEIGHT; ++row) {
        for (size_t col = 0; col < VGA_WIDTH; ++col) {
            terminal_cells[(row - 1) * VGA_WIDTH + col] =
                terminal_cells[row * VGA_WIDTH + col];
        }
    }
    for (size_t col = 0; col < VGA_WIDTH; ++col) {
        terminal_cells[(VGA_HEIGHT - 1) * VGA_WIDTH + col] =
            VGA_ENTRY(' ', console_color);
    }
    for (size_t i = 0; i < VGA_WIDTH * VGA_HEIGHT; ++i) {
        VGA_MEMORY[i] = terminal_cells[i];
    }
    if (gui_mode) gui_redraw_console(terminal_cells, VGA_WIDTH * VGA_HEIGHT);
    cursor_y = VGA_HEIGHT - 1;
    mouse_draw_cursor();
}

static void console_putc(char ch) {
    if (ch == '\n') {
        cursor_x = 0;
        ++cursor_y;
    } else if (ch == '\r') {
        cursor_x = 0;
    } else if (ch == '\b') {
        if (cursor_x > 0) {
            --cursor_x;
            vga_write_cell(cursor_y * VGA_WIDTH + cursor_x,
                           VGA_ENTRY(' ', console_color));
        }
    } else if (ch == '\t') {
        const size_t spaces = 8 - (cursor_x % 8);
        for (size_t i = 0; i < spaces; ++i) {
            vga_write_cell(cursor_y * VGA_WIDTH + cursor_x,
                           VGA_ENTRY(' ', console_color));
            ++cursor_x;
            if (cursor_x == VGA_WIDTH) {
                cursor_x = 0;
                ++cursor_y;
                if (cursor_y >= VGA_HEIGHT) {
                    scroll_screen();
                }
            }
        }
    } else {
        vga_write_cell(cursor_y * VGA_WIDTH + cursor_x, VGA_ENTRY(ch, console_color));
        ++cursor_x;
        if (cursor_x == VGA_WIDTH) {
            cursor_x = 0;
            ++cursor_y;
        }
    }

    if (cursor_y >= VGA_HEIGHT) {
        scroll_screen();
    }
    update_cursor();
}

static void console_write(const char *text) {
    for (size_t i = 0; text[i] != '\0'; ++i) {
        console_putc(text[i]);
    }
}

static int string_equal(const char *left, const char *right) {
    size_t i = 0;
    while (left[i] != '\0' && right[i] != '\0' && left[i] == right[i]) {
        ++i;
    }
    return left[i] == '\0' && right[i] == '\0';
}

static char ascii_lower(char ch) {
    if (ch >= 'A' && ch <= 'Z') {
        return (char)(ch - 'A' + 'a');
    }
    return ch;
}

static int string_equal_ignore_case(const char *left, const char *right) {
    size_t i = 0;
    while (left[i] != '\0' && right[i] != '\0' &&
           ascii_lower(left[i]) == ascii_lower(right[i])) {
        ++i;
    }
    return left[i] == '\0' && right[i] == '\0';
}

static int string_starts_with_ignore_case(const char *text, const char *prefix) {
    size_t i = 0;
    while (prefix[i] != '\0' && ascii_lower(text[i]) == ascii_lower(prefix[i])) {
        ++i;
    }
    return prefix[i] == '\0';
}

static char *skip_spaces(char *text) {
    while (*text == ' ' || *text == '\t') {
        ++text;
    }
    return text;
}

static uint32_t pci_read_config32(uint8_t bus, uint8_t device,
                                  uint8_t function, uint8_t offset) {
    const uint32_t address = UINT32_C(0x80000000) |
        ((uint32_t)bus << 16) | ((uint32_t)device << 11) |
        ((uint32_t)function << 8) | (offset & 0xFC);
    outl(0xCF8, address);
    return inl(0xCFC);
}

static inline void outw(uint16_t port, uint16_t value) {
    __asm__ volatile ("outw %0, %1" : : "a"(value), "Nd"(port));
}

static void pci_write_config16(uint8_t bus, uint8_t device,
                               uint8_t function, uint8_t offset,
                               uint16_t value) {
    const uint32_t address = UINT32_C(0x80000000) |
        ((uint32_t)bus << 16) | ((uint32_t)device << 11) |
        ((uint32_t)function << 8) | (offset & 0xFC);
    outl(0xCF8, address);
    outw((uint16_t)(0xCFC + (offset & 2)), value);
}
static uint8_t pci_read_config8(uint8_t bus, uint8_t device,
                                uint8_t function, uint8_t offset) {
    const uint32_t value = pci_read_config32(bus, device, function, offset);
    return (uint8_t)(value >> ((offset & 3) * 8));
}

static void print_hex8(uint8_t value) {
    static const char digits[] = "0123456789ABCDEF";
    console_putc(digits[value >> 4]);
    console_putc(digits[value & 0x0F]);
}

static void print_u64(uint64_t value);

static int map_xhci_mmio_uncached(uint64_t address) {
    extern uint64_t page_directory[];
    if (address >= UINT64_C(0x100000000)) return 0;
    const size_t entry = (size_t)(address >> 21);
    if (entry >= 2048) return 0;
    page_directory[entry] |= UINT64_C(0x18); /* PWT | PCD for device MMIO. */
    __asm__ volatile ("invlpg (%0)" : : "r"((uintptr_t)address) : "memory");
    return 1;
}

static void run_xhci_probe(uint8_t bus, uint8_t device, uint8_t function) {
    uint32_t bar_low = pci_read_config32(bus, device, function, 0x10);
    if ((bar_low & 1) != 0) {
        console_write("xHCI BAR is I/O space; cannot map it.\n");
        return;
    }

    uint64_t mmio = (uint64_t)(bar_low & UINT32_C(0xFFFFFFF0));
    if (((bar_low >> 1) & 3) == 2) {
        mmio |= (uint64_t)pci_read_config32(bus, device, function, 0x14) << 32;
    }
    if (mmio == 0 || mmio >= UINT64_C(0x100000000)) {
        console_write("xHCI MMIO BAR is outside TAK's current mapped range.\n");
        return;
    }

    const uint16_t command = (uint16_t)pci_read_config32(
        bus, device, function, 0x04);
    if ((command & 0x02) == 0) {
        pci_write_config16(bus, device, function, 0x04,
                           (uint16_t)(command | 0x02));
    }
    if (!map_xhci_mmio_uncached(mmio)) {
        console_write("Could not map xHCI MMIO.\n");
        return;
    }

    volatile uint8_t *const registers =
        (volatile uint8_t *)(uintptr_t)mmio;
    const uint8_t cap_length = registers[0x00];
    const uint16_t version = *(volatile uint16_t *)(registers + 0x02);
    const uint32_t hcsparams1 = *(volatile uint32_t *)(registers + 0x04);
    const unsigned port_count = (hcsparams1 >> 24) & 0xFF;
    if (cap_length < 0x20 || port_count == 0 || port_count > 255) {
        console_write("xHCI capability registers returned invalid values.\n");
        return;
    }

    const uint32_t usb_status =
        *(volatile uint32_t *)(registers + cap_length + 0x04);
    console_write("xHCI ");
    print_hex8((uint8_t)(version >> 8));
    print_hex8((uint8_t)version);
    console_write(" at MMIO ");
    print_hex8((uint8_t)(mmio >> 24));
    print_hex8((uint8_t)(mmio >> 16));
    print_hex8((uint8_t)(mmio >> 8));
    print_hex8((uint8_t)mmio);
    console_write("; ports: ");
    print_u64(port_count);
    console_write((usb_status & 1) ? "; halted\n" : "; running\n");

    unsigned connected = 0;
    for (unsigned port = 0; port < port_count; ++port) {
        const uintptr_t port_register = mmio + cap_length + 0x400 + port * 0x10;
        const uint32_t port_status =
            *(volatile uint32_t *)port_register;
        if ((port_status & 1) == 0) continue;
        ++connected;
        console_write("  connected USB port ");
        print_u64(port + 1);
        console_write("; speed code ");
        print_hex8((uint8_t)((port_status >> 10) & 0x0F));
        console_putc('\n');
    }
    if (connected == 0) {
        console_write("  no USB devices connected to xHCI ports\n");
    }
    console_write("xHCI MMIO/port probe only; USB transfers are not implemented yet.\n");
}

static void run_usb_info(void) {
    unsigned count = 0;
    for (unsigned bus = 0; bus < 256; ++bus) {
        for (unsigned device = 0; device < 32; ++device) {
            const uint16_t vendor = (uint16_t)pci_read_config32(
                (uint8_t)bus, (uint8_t)device, 0, 0);
            if (vendor == UINT16_C(0xFFFF)) continue;
            const uint8_t header = pci_read_config8(
                (uint8_t)bus, (uint8_t)device, 0, 0x0E);
            const unsigned functions = (header & 0x80) ? 8 : 1;
            for (unsigned function = 0; function < functions; ++function) {
                const uint32_t identity = pci_read_config32(
                    (uint8_t)bus, (uint8_t)device, (uint8_t)function, 0);
                if ((uint16_t)identity == UINT16_C(0xFFFF)) continue;
                const uint32_t class_info = pci_read_config32(
                    (uint8_t)bus, (uint8_t)device, (uint8_t)function, 0x08);
                const uint8_t class_code = (uint8_t)(class_info >> 24);
                const uint8_t subclass = (uint8_t)(class_info >> 16);
                const uint8_t prog_if = (uint8_t)(class_info >> 8);
                if (class_code != 0x0C || subclass != 0x03 || prog_if != 0x30) {
                    continue;
                }
                console_write("PCI ");
                print_hex8((uint8_t)bus);
                console_putc(':');
                print_hex8((uint8_t)device);
                console_putc('.');
                console_putc((char)('0' + function));
                console_write(" xHCI controller\n");
                ++count;
                run_xhci_probe((uint8_t)bus, (uint8_t)device,
                               (uint8_t)function);
            }
        }
    }
    if (count == 0) {
        console_write("No PCI xHCI USB host controllers found.\n");
    }
}
static void print_help(void) {
    console_write("Built-in commands:\n");
    console_write("  help       shows this list\n");
    console_write("  clear      clears the screen\n");
    console_write("  echo [OPTIONS] echoes it  to you (-n, -e, -E)\n");
    console_write("  print [TEXT]  prints text\n");
    console_write("  info-byte  stream binary data until Escape is pressed\n");
    console_write("  usb-info   inspect xHCI MMIO and connected ports\n");
    console_write("  Caps Lock   toggles uppercase letters.\n");
    console_write("\n");
    console_write("  uname [-a] shows kernel + system information\n");
    console_write("  version    shows kernel version\n");
    console_write("  halt       stop the CPU\n");
    console_write("  reboot     restarts the computer\n");
    console_write("  keyboard   show the active keyboard layout\n");
    console_write("  keyboard-US or keyboard-BR-ABNT2  change layout\n");
    console_write("file commands such as ls needs a filesystem, which TAK does not have yet.\n");
}

static int ps2_wait_input_empty(void) {
    for (uint32_t timeout = 0; timeout < 1000000; ++timeout) {
        if ((inb(0x64) & 0x02) == 0) return 1;
        __asm__ volatile ("pause");
    }
    return 0;
}

static int mouse_send_command(uint8_t command) {
    if (!ps2_wait_input_empty()) return 0;
    outb(0x64, 0xD4);
    if (!ps2_wait_input_empty()) return 0;
    outb(0x60, command);
    for (uint32_t timeout = 0; timeout < 1000000; ++timeout) {
        const uint8_t status = inb(0x64);
        if ((status & 0x01) != 0) {
            const uint8_t response = inb(0x60);
            if ((status & 0x20) != 0) return response == 0xFA;
        }
        __asm__ volatile ("pause");
    }
    return 0;
}

static int mouse_initialize(void) {
    if (!ps2_wait_input_empty()) return 0;
    outb(0x64, 0xA8); /* enable the auxiliary PS/2 port */
    if (!mouse_send_command(0xF4)) return 0; /* enable mouse reports */
    mouse_enabled = 1;
    mouse_draw_cursor();
    return 1;
}

static void mouse_process_byte(uint8_t value) {
    if (mouse_packet_index == 0 && (value & 0x08) == 0) return;
    mouse_packet[mouse_packet_index++] = value;
    if (mouse_packet_index < 3) return;
    mouse_packet_index = 0;
    if ((mouse_packet[0] & 0xC0) != 0) return;

    if (gui_mode) {
        gui_mouse_move((int)(int8_t)mouse_packet[1], -(int)(int8_t)mouse_packet[2]);
        return;
    }

    mouse_erase_cursor();
    mouse_motion_x += (int)(int8_t)mouse_packet[1];
    mouse_motion_y -= (int)(int8_t)mouse_packet[2];
    mouse_x += mouse_motion_x / MOUSE_CELL_WIDTH;
    mouse_y += mouse_motion_y / MOUSE_CELL_HEIGHT;
    mouse_motion_x %= MOUSE_CELL_WIDTH;
    mouse_motion_y %= MOUSE_CELL_HEIGHT;
    if (mouse_x < 0) mouse_x = 0;
    if (mouse_x >= VGA_WIDTH) mouse_x = VGA_WIDTH - 1;
    if (mouse_y < 0) mouse_y = 0;
    if (mouse_y >= VGA_HEIGHT) mouse_y = VGA_HEIGHT - 1;
    mouse_draw_cursor();
}

static void reboot_system(void) {
    while ((inb(0x64) & 0x02) != 0) {
        __asm__ volatile ("pause");
    }
    outb(0x64, 0xFE);
    for (;;) {
        __asm__ volatile ("hlt");
    }
}

static void run_echo(char *args) {
    int newline = 1;
    int interpret_escapes = 0;
    char *text = skip_spaces(args);

    for (;;) {
        if (text[0] != '-' || text[1] == '\0' || text[1] == ' ' || text[1] == '\t') {
            break;
        }

        char *end = text + 1;
        int valid_option = 1;
        while (*end != '\0' && *end != ' ' && *end != '\t') {
            if (*end != 'n' && *end != 'e' && *end != 'E') {
                valid_option = 0;
                break;
            }
            ++end;
        }
        if (!valid_option) {
            break;
        }

        for (char *option = text + 1; option < end; ++option) {
            if (*option == 'n') {
                newline = 0;
            } else if (*option == 'e') {
                interpret_escapes = 1;
            } else if (*option == 'E') {
                interpret_escapes = 0;
            }
        }
        text = skip_spaces(end);
    }

    for (size_t i = 0; text[i] != '\0'; ++i) {
        if (!interpret_escapes || text[i] != '\\' || text[i + 1] == '\0') {
            console_putc(text[i]);
            continue;
        }

        const char escaped = text[++i];
        switch (escaped) {
            case 'a': break; /* theres no speaker support yet, pc speaker */
            case 'b': console_putc('\b'); break;
            case 'c': return; /* suppress the newline and ignore remaining text */
            case 'n': console_putc('\n'); break;
            case 'r': console_putc('\r'); break;
            case 't': console_putc('\t'); break;
            case '\\': console_putc('\\'); break;
            default:
                console_putc('\\');
                console_putc(escaped);
                break;
        }
    }

    if (newline) {
        console_putc('\n');
    }
}

static void print_keyboard_layout(void) {
    if (active_keyboard_layout == KEYBOARD_LAYOUT_BR_ABNT2) {
        console_write("Active keyboard layout: BR-ABNT2\n");
    } else {
        console_write("Active keyboard layout: US\n");
    }
}

static void set_keyboard_layout(const char *name) {
    if (string_equal_ignore_case(name, "US")) {
        active_keyboard_layout = KEYBOARD_LAYOUT_US;
        console_write("Keyboard layout changed to US.\n");
    } else if (string_equal_ignore_case(name, "BR-ABNT2") ||
               string_equal_ignore_case(name, "BR")) {
        active_keyboard_layout = KEYBOARD_LAYOUT_BR_ABNT2;
        console_write("Keyboard layout changed to BR-ABNT2.\n");
    } else {
        console_write("Unknown layout, Choose US or BR-ABNT2.\n");
    }
}

static void run_info_byte(void);

static void run_command(char *line) {
    char *command = skip_spaces(line);
    char *args = command;

    while (*args != '\0' && *args != ' ' && *args != '\t') {
        ++args;
    }
    if (*args != '\0') {
        *args++ = '\0';
        args = skip_spaces(args);
    }

    if (*command == '\0') {
        return;
    }
    if (string_equal(command, "help")) {
        print_help();
    } else if (string_equal(command, "clear")) {
        clear_screen();
    } else if (string_equal(command, "echo")) {
        run_echo(args);
    } else if (string_equal(command, "usb-info")) {
        run_usb_info();
    } else if (string_equal(command, "info-byte")) {
        run_info_byte();
    } else if (string_equal(command, "print")) {
        console_write(args);
        console_putc('\n');
    } else if (string_equal(command, "uname")) {
        if (string_equal(args, "-a")) {
            console_write("TAK tak-kernel ");
            console_write(TAK_VERSION);
            console_write(" x86_64 runnning-racoon\n");
        } else if (string_equal(args, "-m")) {
            console_write("x86_64\n");
        } else if (string_equal(args, "-r")) {
            console_write(TAK_VERSION);
            console_putc('\n');
        } else {
            console_write("TAK\n");
        }
    } else if (string_equal(command, "version") || string_equal(command, "ver")) {
        console_write("TAK Kernel ");
        console_write(TAK_VERSION);
        console_write(" (x86_64)\n");
    } else if (string_equal(command, "halt")) {
        console_write("TAK halted. You can now restart or shutdown your machine\n");
        for (;;) {
            __asm__ volatile ("hlt");
        }
    } else if (string_equal(command, "reboot")) {
        console_write("rebooting...\n");
        reboot_system();
    } else if (string_equal_ignore_case(command, "keyboard")) {
        if (*args == '\0') {
            print_keyboard_layout();
        } else {
            set_keyboard_layout(args);
        }
    } else if (string_starts_with_ignore_case(command, "keyboard-")) {
        set_keyboard_layout(command + 9);
    } else {
        console_color = VGA_COLOR(COLOR_LIGHT_RED, COLOR_BLACK);
        console_write(command);
        console_write(": command not found\n");
        console_color = VGA_COLOR(COLOR_LIGHT_GREY, COLOR_BLACK);
    }
}

static uint8_t shifted_symbol(uint8_t scan_code, uint8_t ch) {
    if (ch >= 'a' && ch <= 'z') {
        return (uint8_t)(ch - 'a' + 'A');
    }
    switch (scan_code) {
        case 0x02: return '!';
        case 0x03: return '@';
        case 0x04: return '#';
        case 0x05: return '$';
        case 0x06: return '%';
        case 0x07: return '^';
        case 0x08: return '&';
        case 0x09: return '*';
        case 0x0A: return '(';
        case 0x0B: return ')';
        case 0x0C: return '_';
        case 0x0D: return '+';
        case 0x1A: return '{';
        case 0x1B: return '}';
        case 0x2B: return '|';
        case 0x27: return ':';
        case 0x28: return '"';
        case 0x33: return '<';
        case 0x34: return '>';
        case 0x35: return '?'; 
        case 0x29: return '~';
        default: return ch;
    }
}

static uint8_t translate_scan_code(uint8_t scan_code, int shift_down) {
    static const uint8_t us_keymap[128] = {
        [0x01] = 27,
        [0x02] = '1', [0x03] = '2', [0x04] = '3', [0x05] = '4',
        [0x06] = '5', [0x07] = '6', [0x08] = '7', [0x09] = '8',
        [0x0A] = '9', [0x0B] = '0', [0x0C] = '-', [0x0D] = '=',
        [0x0E] = '\b', [0x0F] = '\t',
        [0x10] = 'q', [0x11] = 'w', [0x12] = 'e', [0x13] = 'r',
        [0x14] = 't', [0x15] = 'y', [0x16] = 'u', [0x17] = 'i',
        [0x18] = 'o', [0x19] = 'p', [0x1A] = '[', [0x1B] = ']',
        [0x1C] = '\n', [0x1E] = 'a', [0x1F] = 's', [0x20] = 'd',
        [0x21] = 'f', [0x22] = 'g', [0x23] = 'h', [0x24] = 'j',
        [0x25] = 'k', [0x26] = 'l', [0x27] = ';', [0x28] = '\'',
        [0x29] = '`', [0x2B] = '\\', [0x2C] = 'z', [0x2D] = 'x',
        [0x2E] = 'c', [0x2F] = 'v', [0x30] = 'b', [0x31] = 'n',
        [0x32] = 'm', [0x33] = ',', [0x34] = '.', [0x35] = '/',
        [0x39] = ' '
    };

    if (active_keyboard_layout == KEYBOARD_LAYOUT_BR_ABNT2) {
        switch (scan_code) {
            case 0x07: return shift_down ? DEAD_KEY_DIAERESIS : '6';
            case 0x1A: return shift_down ? DEAD_KEY_GRAVE : DEAD_KEY_ACUTE;
            case 0x1B: return shift_down ? '{' : '[';
            case 0x27: return shift_down ? 128 : 135; /* Ç / ç, CP437 */
            case 0x28: return shift_down ? DEAD_KEY_CIRCUMFLEX : DEAD_KEY_TILDE; /* ok */
            case 0x29: return shift_down ? '"' : '\'';
            case 0x2B: return shift_down ? '}' : ']';
            case 0x35: return shift_down ? ':' : ';';
            case 0x56: return shift_down ? '|' : '\\';
            case 0x73: return shift_down ? '?' : '/';
            default: break;
        }
    }

    uint8_t ch = us_keymap[scan_code];
    if (shift_down && ch != '\0') {
        ch = shifted_symbol(scan_code, ch);
    }
    return ch;
}

static int keyboard_pollchar(uint8_t *character) {
    for (;;) {
        const uint8_t status = inb(0x64);
        if ((status & 0x01) == 0) {
            return 0;
        }
        const uint8_t scan_code = inb(0x60);
        if ((status & 0x20) != 0) {
            mouse_process_byte(scan_code);
            continue;
        }
        if (scan_code == 0x2A || scan_code == 0x36) {
            shift_down = 1;
            continue;
        }
        if (scan_code == 0xAA || scan_code == 0xB6) {
            shift_down = 0;
            continue;
        }
        if (scan_code == 0x3A) {
            if (!caps_lock_key_down) {
                caps_lock_on = !caps_lock_on;
            }
            caps_lock_key_down = 1;
            continue;
        }
        if (scan_code == 0xBA) {
            caps_lock_key_down = 0;
            continue;
        }
        if ((scan_code & 0x80) != 0) {
            continue;
        }

        uint8_t ch = translate_scan_code(scan_code, shift_down);
        if (((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')) &&
            (caps_lock_on != shift_down)) {
            if (ch >= 'a' && ch <= 'z') {
                ch = (uint8_t)(ch - 'a' + 'A');
            } else {
                ch = (uint8_t)(ch - 'A' + 'a');
            }
        }
        if (ch != 0) {
            *character = ch;
            return 1;
        }
    }
}

static uint8_t keyboard_getchar(void) {
    uint8_t character;
    while (!keyboard_pollchar(&character)) {
        __asm__ volatile ("pause");
    }
    return character;
}
static uint8_t compose_dead_key(uint8_t dead_key, uint8_t ch) {
    switch (dead_key) {
        case DEAD_KEY_ACUTE:
            switch (ch) {
                case 'a': return 160; case 'e': return 130; case 'i': return 161;
                case 'o': return 162; case 'u': return 163;
                case 'A': return 181; case 'E': return 144; case 'I': return 214;
                case 'O': return 224; case 'U': return 233;
                default: return 0;
            }
        case DEAD_KEY_GRAVE:
            switch (ch) {
                case 'a': return 133; case 'e': return 138; case 'i': return 141;
                case 'o': return 149; case 'u': return 151;
                case 'A': return 183; case 'E': return 212; case 'I': return 222;
                case 'O': return 227; case 'U': return 235;
                default: return 0;
            }
        case DEAD_KEY_TILDE:
            switch (ch) {
                case 'a': return 198; case 'o': return 228; case 'n': return 164;
                case 'A': return 199; case 'O': return 229; case 'N': return 165;
                default: return 0;
            }
        case DEAD_KEY_CIRCUMFLEX:
            switch (ch) {
                case 'a': return 131; case 'e': return 136; case 'i': return 140;
                case 'o': return 147; case 'u': return 150;
                case 'A': return 182; case 'E': return 210; case 'I': return 215;
                case 'O': return 226; case 'U': return 234;
                default: return 0;
            }
        case DEAD_KEY_DIAERESIS:
            switch (ch) {
                case 'a': return 132; case 'e': return 137; case 'i': return 139;
                case 'o': return 148; case 'u': return 129;
                case 'A': return 142; case 'O': return 153; case 'U': return 154;
                default: return 0;
            }
        default:
            return 0;
    }
}

static uint8_t dead_key_fallback(uint8_t dead_key) {
    switch (dead_key) {
        case DEAD_KEY_ACUTE: return '\'';
        case DEAD_KEY_GRAVE: return '`';
        case DEAD_KEY_TILDE: return '~';
        case DEAD_KEY_CIRCUMFLEX: return '^';
        case DEAD_KEY_DIAERESIS: return '"';
        default: return 0;
    }
}
static void print_u64(uint64_t value) {
    char digits[20];
    size_t count = 0;
    do {
        digits[count++] = (char)('0' + value % 10);
        value /= 10;
    } while (value != 0 && count < sizeof(digits));
    while (count > 0) {
        console_putc(digits[--count]);
    }
}

static void run_info_byte(void) {
    uint64_t state = UINT64_C(0x9E3779B97F4A7C15);
    uint64_t bytes_printed = 0;
    unsigned bytes_on_line = 0;
    int stop = 0;

    console_write("Streaming Data\n");
    while (!stop) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;

        for (unsigned byte_index = 0; byte_index < 8; ++byte_index) {
            const uint8_t byte = (uint8_t)(state >> (byte_index * 8));
            for (int bit = 7; bit >= 0; --bit) {
                console_putc((byte & (1u << bit)) ? '1' : '0');
            }
            console_putc(' ');
            ++bytes_printed;
            if (++bytes_on_line == 8) {
                console_putc('\n');
                bytes_on_line = 0;
            }

            if ((bytes_printed & 0x1F) == 0) {
                uint8_t key;
                while (keyboard_pollchar(&key)) {
                    if (key == 27) {
                        stop = 1;
                        break;
                    }
                }
                if (stop) break;
            }
        }
    }

    if (bytes_on_line != 0) {
        console_putc('\n');
    }
    console_write("Stopped after ");
    print_u64(bytes_printed);
    console_write(" bytes.\n");
}

static void shell_loop(void) {
    char line[COMMAND_MAX];

    for (;;) {
        size_t length = 0;
        console_color = VGA_COLOR(COLOR_LIGHT_GREEN, COLOR_BLACK);
        console_write("tak@kernel:~$ ");
        console_color = VGA_COLOR(COLOR_LIGHT_GREY, COLOR_BLACK);

        for (;;) {
            uint8_t ch = keyboard_getchar();
            static uint8_t pending_dead_key;
            if (ch >= DEAD_KEY_ACUTE && ch <= DEAD_KEY_DIAERESIS &&
                active_keyboard_layout == KEYBOARD_LAYOUT_BR_ABNT2) {
                pending_dead_key = ch;
                continue;
            }
            if (pending_dead_key != 0) {
                const uint8_t composed = compose_dead_key(pending_dead_key, ch);
                if (composed != 0) {
                    ch = composed;
                } else {
                    console_putc((char)dead_key_fallback(pending_dead_key));
                }
                pending_dead_key = 0;
            }
            if (ch == '\n') {
                console_putc('\n');
                line[length] = '\0';
                run_command(line);
                break;
            }
            if (ch == '\b') {
                if (length > 0) {
                    --length;
                    console_putc('\b');
                }
                continue;
            }
            if (ch >= ' ' && length < COMMAND_MAX - 1) {
                line[length++] = (char)ch;
                console_putc((char)ch);
            }
        }
    }
}

void kernel_main(uint64_t multiboot_magic, uint64_t multiboot_info) {
    gui_mode = gui_initialize((const uint32_t *)(uintptr_t)multiboot_info);
    clear_screen();
    console_color = VGA_COLOR(COLOR_LIGHT_RED, COLOR_BLACK);
    console_write("                            ***********************\n");
    console_write("                            **     ***        *****\n");
    console_write("                            **     *****     *** **\n");
    console_write("                            **    **   *******   **\n");
    console_write("                            **   **    ** ***    **\n");
    console_write("                            **  ***   **   ***** **\n");
    console_write("                            **  **  ***    **  ****\n");
    console_write("                            ** **  ***      **   **\n");
    console_write("                            **** ***        **   **\n");
    console_write("                            *******         **   **\n");
    console_write("                            ***********************\n\n");
    console_color = VGA_COLOR(COLOR_CYAN, COLOR_BLACK);
    console_write("TAK Kernel ");
    console_write(TAK_VERSION);
    console_putc('\n');
    console_color = VGA_COLOR(COLOR_LIGHT_GREY, COLOR_BLACK);

    if (multiboot_magic == UINT64_C(0x36D76289)) {
        console_write("64-bit long mode active; Multiboot2 handoff received.\n");
    } else {
        console_write("WARNING: unexpected bootloader handoff, expect errors.\n");
    }
    console_write("type 'help' to see available commands\n\n");
    if (mouse_initialize()) {
        console_write("Mouse cursor activated\n");
    }
    shell_loop();
}









