
#include "vga.h"

#include <stdarg.h>

void Vga::scroll_one_line() {
    u16 value = VGA_COLOR(Color::WHITE, Color::BLACK);
    for (s32 i = 1; i < VGA_HEGIHT; ++i) {
        for (u64 x = 0; x < VGA_WIDTH; ++x) {
            u64 index = x + VGA_WIDTH * (i-1);
            buffer[index] = value;
        }
        u16 *dst = &buffer[((i-1) * VGA_WIDTH)];
        u16 *src = &buffer[(i * VGA_WIDTH)];
        memcpy(dst, src, sizeof(u16) * VGA_WIDTH);
    }
    
    for (u64 x = 0; x < VGA_WIDTH; ++x) {
        u64 index = x + VGA_WIDTH * (VGA_HEGIHT-1);
        buffer[index] = value;
    }
    
    buffer_cursor_pos_x = 0;
    buffer_cursor_pos_y--;
}

void Vga::write(u8 c) {
    if (c == '\n') {
        buffer_cursor_pos_x = 0;
        buffer_cursor_pos_y++;
        
        if (buffer_cursor_pos_y >= VGA_HEGIHT) {
            scroll_one_line();
        }
        
        set_cursor_coordinates(buffer_cursor_pos_x, buffer_cursor_pos_y);
        return;
    }
    u64 index = buffer_cursor_pos_x + VGA_WIDTH * buffer_cursor_pos_y;
    buffer[index] = color | c;
    
    buffer_cursor_pos_x++;
    
    if (buffer_cursor_pos_x >= VGA_WIDTH) {
        buffer_cursor_pos_x = 0;
        buffer_cursor_pos_y++;
        
        if (buffer_cursor_pos_y >= VGA_HEGIHT) {
            scroll_one_line();
        }
    }
    
    set_cursor_coordinates(buffer_cursor_pos_x, buffer_cursor_pos_y);
}


// @TODO error codes
int vga_putchar(void *payload, u8 c) {
    Vga *vga = reinterpret_cast<Vga *>(payload);
    vga->write(c);
    return 0;
}

void Vga::write(String s) {
    for (s64 i = 0; i < s.length; ++i) {
        u8 c = s.data[i];
        write(c);
    }
}

void Vga::clear_screen() {
    u16 value = VGA_COLOR(Color::WHITE, Color::BLACK);
    for (u64 y = 0; y < VGA_HEGIHT; ++y) {
        for (u64 x = 0; x < VGA_WIDTH; ++x) {
            u64 index = x + VGA_WIDTH * y;
            buffer[index] = value;
        }
    }
    
    buffer_cursor_pos_x = 0;
    buffer_cursor_pos_y = 0;
}


// TODO(josh): cleanup these hardcoded-values
void Vga::enable_cursor(bool enable) {
    if (enable) {
        _port_io_write_u8(0x3D4, 0x0A);
        u8 cursor_start = 11;
        u8 cursor_end = 13;
        _port_io_write_u8(0x3D5, (_port_io_read_u8(0x3D5) & 0xC0) | cursor_start);
        
        _port_io_write_u8(0x3D4, 0x0B);
        _port_io_write_u8(0x3D5, (_port_io_read_u8(0x3E0) & 0xE0) | cursor_end);
        
        set_cursor_coordinates(0, 0);
    } else {
        _port_io_write_u8(0x3D4, 0x0A);
        _port_io_write_u8(0x3D5, 0x20);
    }
}

void Vga::set_cursor_coordinates(u16 x, u16 y) {
    u16 pos = y * VGA_WIDTH + x;
    _port_io_write_u8(0x3D4, 0x0F);
    _port_io_write_u8(0x3D5, pos & 0xFF);
    _port_io_write_u8(0x3D4, 0x0E);
    _port_io_write_u8(0x3D5, (pos >> 8) & 0xFF);
}