#ifndef TAK_GUI_H
#define TAK_GUI_H
#include <stddef.h>
#include <stdint.h>
int gui_initialize(const uint32_t *multiboot_info);
void gui_clear_console(void);
void gui_draw_cell(size_t index, uint16_t cell);
void gui_redraw_console(const volatile uint16_t *cells, size_t count);
void gui_mouse_draw(void);
void gui_mouse_move(int dx, int dy);
void gui_mouse_erase(void);
#endif
