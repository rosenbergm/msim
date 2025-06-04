/*
 * Copyright (c) 2002-2025 Martin Rosenberg
 * All rights reserved.
 *
 * Distributed under the terms of GPL.
 *
 *
 *  HD44780U LCD module device
 *
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../assert.h"
#include "../fault.h"
#include "../parser.h"
#include "../text.h"
#include "../utils.h"
#include "device.h"
#include "dlcd.h"

#define REGISTER_DATA 0 /**< Offset of data/command register */
#define REGISTER_CONTROL 1 /**< Offset of control register */
#define REGISTER_LIMIT 4 /**< Size of the register block */

#define LCD_MAX_DDRAM_SIZE 80 /* Maximum DDRAM characters supported by HD44780 */
#define LCD_MAX_ROWS 4 /* Maximum rows supported */
#define LCD_MAX_COLS 40 /* Maximum columns per row supported */

#define LCD_CMD_CLEAR 0x01 /**< Clear display command */
#define LCD_CMD_HOME 0x02 /**< Return home command */

#define LCD_SET_CURSOR_MASK 0x7F /**< Mask for setting cursor address */
#define LCD_SET_CURSOR_CMD 0x80 /**< Command to set cursor position */

#define LCD_CMD_ENTRY_MODE_BASE 0x04 /**< Entry mode set command base */
#define LCD_CMD_DISPLAY_CONTROL_BASE 0x08 /**< Display control command base */
#define LCD_CMD_FUNCTION_SET_BASE 0x20 /**< Function set command base */
#define LCD_CMD_SHIFT_BASE 0x10 /**< Cursor/display shift command base */

static uint8_t row_addr_map[LCD_MAX_ROWS] = {
    0x00, 0x40, 0x14, 0x54
}; /**< Row address map for HD44780 */

typedef union {
    uint32_t value;
    struct {
        bool rs : 1; /**< Register select */
        bool rw : 1; /**< Read/Write */
        bool e : 1; /**< Enable */
        int _ : 5;

        uint8_t data : 8; /**< Data/command register */
    } parts;
} lcd_reg_t;

typedef struct {
    int rows;
    int cols;

    int current_row;
    int current_col;

    lcd_reg_t reg; /**< Register value */
    lcd_reg_t reg_prev; /**< Previous register value */

    uint8_t *buffer;

    uint64_t addr; /**< Register address */

    bool display_on; /**< Display on/off state */
    bool increment_mode; /**< Increment mode for cursor */
    int display_shift_offset; /**< Display shift offset for scrolling */
    bool multi_line_mode; /**< Multi-line mode for display (1 line versus 2 and more lines) */
} lcd_data_t;

static bool dlcd_init(token_t *parm, device_t *dev)
{
    parm_next(&parm);
    uint cols = parm_uint(parm);

    if (cols > LCD_MAX_COLS) {
        error("Number of columns exceeds maximum (%d)", LCD_MAX_COLS);
        return false;
    }

    parm_next(&parm);
    uint rows = parm_uint(parm);

    if (rows > LCD_MAX_ROWS) {
        error("Number of rows exceeds maximum (%d)", LCD_MAX_ROWS);
        return false;
    }

    parm_next(&parm);
    uint64_t _addr = parm_uint(parm);

    if (!phys_range(_addr)) {
        error("Physical memory address of data register is out of range");
        return false;
    }

    if (!phys_range(_addr + (uint64_t) REGISTER_LIMIT)) {
        error("Invalid address, registers would exceed the physical "
              "memory range");
        return false;
    }

    uint64_t addr = _addr;

    lcd_data_t *data = safe_malloc_t(lcd_data_t);
    dev->data = data;

    data->rows = rows;
    data->cols = cols;
    data->current_row = 0;
    data->current_col = 0;

    data->buffer = safe_malloc(sizeof(uint8_t) * rows * cols);
    memset(data->buffer, 0, rows * cols);

    data->addr = addr;

    // defaults taken from HD44780 datasheet
    data->display_on = false;
    data->increment_mode = true;
    data->display_shift_offset = 0;
    data->multi_line_mode = false;

    return true;
}

static void lcd_done(device_t *dev)
{
    lcd_data_t *data = (lcd_data_t *) dev->data;

    safe_free(data->buffer);
    safe_free(data);
}

static void lcd_print(lcd_data_t *data)
{
    printf("┌");

    for (int i = 0; i < data->cols; i++) {
        printf("─");
    }

    printf("┐\n");

    if (!data->display_on) {
        for (int row = 0; row < data->rows; row++) {
            printf("│");
            for (int col = 0; col < data->cols; col++) {
                printf(" ");
            }
            printf("│\n");
        }
    } else {
        int active_rows = data->multi_line_mode ? data->rows : 1;

        for (int row = 0; row < active_rows; row++) {
            printf("│");

            for (int col = 0; col < data->cols; col++) {
                int source_col = col + data->display_shift_offset;
                char c = ' ';

                if (source_col >= 0 && source_col < data->cols) {
                    c = data->buffer[row * data->cols + source_col];
                    if (c == 0) {
                        c = ' ';
                    }
                }

                printf("%c", c);
            }

            printf("│\n");
        }

        for (int row = active_rows; row < data->rows; row++) {
            printf("│");
            for (int col = 0; col < data->cols; col++) {
                printf(" ");
            }
            printf("│\n");
        }
    }
    printf("└");

    for (int i = 0; i < data->cols; i++) {
        printf("─");
    }

    printf("┘\n");

    fflush(stdout);
}

static bool ddram_addr_to_position(lcd_data_t *data, uint8_t addr, int *row, int *col)
{
    for (int i = 0; i < data->rows; i++) {
        uint8_t base = row_addr_map[i];

        if (addr >= base && addr < base + LCD_MAX_COLS) {
            *row = i;
            *col = addr - base;

            // ensure column is within bounds
            if (*col >= data->cols) {
                *col = data->cols - 1;
            }

            return true;
        }
    }

    return false;
}

static void lcd_set_cursor(lcd_data_t *data, uint8_t addr)
{
    int row, col;

    if (ddram_addr_to_position(data, addr, &row, &col)) {
        data->current_row = row;
        data->current_col = col;
    }
}

static void lcd_advance_cursor(lcd_data_t *lcd)
{
    if (lcd->increment_mode) {
        // increment mode
        lcd->current_col++;

        // automatically wrap to next line if needed
        if (!lcd->multi_line_mode) {
            if (lcd->current_col >= lcd->cols) {
                lcd->current_col = 0; // stay on line 0
            }
        } else {
            // multi-line mode: wrap to next line
            if (lcd->current_col >= lcd->cols) {
                lcd->current_col = 0;
                lcd->current_row = (lcd->current_row + 1) % lcd->rows;
            }
        }
    } else {
        // decrement mode
        lcd->current_col--;

        if (lcd->current_col < 0) {
            if (!lcd->multi_line_mode) {
                lcd->current_col = lcd->cols - 1; // stay on line 0
            } else {
                lcd->current_col = lcd->cols - 1;
                lcd->current_row = (lcd->current_row - 1 + lcd->rows) % lcd->rows;
            }
        }
    }
}

static void lcd_handle_clear_command(lcd_data_t *lcd)
{
    memset(lcd->buffer, 0, lcd->rows * lcd->cols);
    lcd->current_row = 0;
    lcd->current_col = 0;
    lcd->display_shift_offset = 0;
}

static void lcd_handle_home_command(lcd_data_t *lcd)
{
    lcd->current_row = 0;
    lcd->current_col = 0;
    lcd->display_shift_offset = 0;
}

static void lcd_handle_entry_mode_command(lcd_data_t *lcd, uint8_t cmd)
{
    lcd->increment_mode = (cmd & 0x02) != 0;
}

static void lcd_handle_display_control_command(lcd_data_t *lcd, uint8_t cmd)
{
    lcd->display_on = (cmd & 0x04) != 0;
}

static void lcd_handle_function_set_command(lcd_data_t *lcd, uint8_t cmd)
{
    lcd->multi_line_mode = (cmd & 0x08) != 0;
}

static void lcd_handle_shift_command(lcd_data_t *lcd, uint8_t cmd)
{
    bool shift_display = (cmd & 0x08) != 0;
    bool shift_right = (cmd & 0x04) != 0;

    if (shift_display) {
        // shift entire display
        if (shift_right) {
            lcd->display_shift_offset++;
        } else {
            lcd->display_shift_offset--;
        }

        if (lcd->display_shift_offset < -lcd->cols) {
            lcd->display_shift_offset = -lcd->cols;
        }

        if (lcd->display_shift_offset > lcd->cols) {
            lcd->display_shift_offset = lcd->cols;
        }
    } else {
        // move cursor only
        if (shift_right) {
            lcd->current_col++;

            if (lcd->current_col >= lcd->cols) {
                lcd->current_col = 0;
                lcd->current_row = (lcd->current_row + 1) % lcd->rows;
            }
        } else {
            lcd->current_col--;

            if (lcd->current_col < 0) {
                lcd->current_col = lcd->cols - 1;
                lcd->current_row = (lcd->current_row - 1 + lcd->rows) % lcd->rows;
            }
        }
    }
}

static void lcd_handle_set_cursor_command(lcd_data_t *lcd, uint8_t cmd)
{
    uint8_t addr = cmd & LCD_SET_CURSOR_MASK;

    if (!lcd->multi_line_mode) {
        // force all cursor positions to line 0
        int row, col;

        if (ddram_addr_to_position(lcd, addr, &row, &col)) {
            lcd->current_row = 0;
            lcd->current_col = col;
        }
    } else {
        lcd_set_cursor(lcd, addr);
    }
}

static bool lcd_handle_command_write(lcd_data_t *lcd)
{
    if (lcd->reg.parts.rw) {
        return false;
    }

    uint8_t cmd = lcd->reg.parts.data;
    bool display_updated = false;

    if (cmd == LCD_CMD_CLEAR) {
        lcd_handle_clear_command(lcd);
        display_updated = true;

    } else if (cmd == LCD_CMD_HOME) {
        lcd_handle_home_command(lcd);

    } else if ((cmd & 0xFC) == LCD_CMD_ENTRY_MODE_BASE) {
        lcd_handle_entry_mode_command(lcd, cmd);

    } else if ((cmd & 0xF8) == LCD_CMD_DISPLAY_CONTROL_BASE) {
        lcd_handle_display_control_command(lcd, cmd);
        display_updated = true;

    } else if ((cmd & 0xE0) == LCD_CMD_FUNCTION_SET_BASE) {
        lcd_handle_function_set_command(lcd, cmd);
        display_updated = true;

    } else if ((cmd & 0xFC) == LCD_CMD_SHIFT_BASE) {
        lcd_handle_shift_command(lcd, cmd);
        display_updated = true;

    } else if (cmd & LCD_SET_CURSOR_CMD) {
        lcd_handle_set_cursor_command(lcd, cmd);
    }

    return display_updated;
}

static bool lcd_handle_data_write(lcd_data_t *lcd)
{
    if (lcd->reg.parts.rw) {
        return false;
    }

    if (lcd->current_col < lcd->cols) {
        lcd->buffer[lcd->current_row * lcd->cols + lcd->current_col] = lcd->reg.parts.data;
        lcd_advance_cursor(lcd);

        return true;
    }

    return false;
}

static void lcd_execute_command(lcd_data_t *lcd)
{
    if ((lcd->reg_prev.parts.e) && !(lcd->reg.parts.e)) {
        bool display_updated = false;

        if (lcd->reg.parts.rs) {
            display_updated = lcd_handle_data_write(lcd);
        } else {
            display_updated = lcd_handle_command_write(lcd);
        }

        if (display_updated) {
            lcd_print(lcd);
        }
    }
}

static void lcd_write32(unsigned int procno, device_t *dev, ptr36_t addr, uint32_t val)
{
    ASSERT(dev != NULL);

    lcd_data_t *data = (lcd_data_t *) dev->data;

    switch (addr - data->addr) {
    case REGISTER_DATA:
        data->reg.parts.data = (uint8_t) val;
        break;

    case REGISTER_CONTROL:
        data->reg_prev.value = data->reg.value;

        lcd_reg_t value = { .value = val };

        data->reg.parts.rs = value.parts.rs;
        data->reg.parts.rw = value.parts.rw;
        data->reg.parts.e = value.parts.e;

        lcd_execute_command(data);

        break;

    default:
        break;
    }
}

static bool dlcd_info(token_t *parm, device_t *dev)
{
    lcd_data_t *data = (lcd_data_t *) dev->data;

    printf("[data register]\n");
    printf("%#11" PRIx64 "\n", data->addr);
    printf("[control register]\n");
    printf("%#11" PRIx64 "\n", data->addr + 1);

    return true;
}

static cmd_t lcd_cmds[] = {
    { "init",
            (fcmd_t) dlcd_init,
            DEFAULT,
            DEFAULT,
            "Initialization",
            "Initialization",
            REQ STR "name/lcd name" NEXT
                    REQ INT "rows/number of rows" NEXT
                            REQ INT "columns/number of columns" NEXT
                                    REQ INT "register/address of the register" END },
    { "help",
            (fcmd_t) dev_generic_help,
            DEFAULT,
            DEFAULT,
            "Display this help text",
            "Display this help text",
            OPT STR "cmd/command name" END },
    { "info",
            (fcmd_t) dlcd_info,
            DEFAULT,
            DEFAULT,
            "Display LCD state and configuration",
            "Display LCD state and configuration",
            NOCMD },
    LAST_CMD
};

device_type_t dlcd = {
    /* LCD is a deterministic device */
    .nondet = false,

    .name = "dlcd",
    .brief = "LCD and shift register module simulation",
    .full = "LCD and shift register module simulation",

    .done = lcd_done,
    .write32 = lcd_write32,

    .cmds = lcd_cmds
};
