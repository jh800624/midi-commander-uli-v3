/*
 * display.c
 *
 * V3 OLED UI based on the validated V2 browser preview.  The editor remains
 * isolated behind the hold-0 boot gesture.
 */
#include "main.h"
#include "flash_midi_settings.h"
#include <stdio.h>
#include <string.h>
#include "ssd1306.h"
#include "usb_device.h"

static uint8_t performance_profile;
static uint8_t performance_key;
static uint8_t performance_mode;
static uint8_t performance_number;
static uint8_t performance_value;
static uint8_t performance_exp[2];
static uint8_t performance_battery_percent;
static uint16_t performance_battery_centivolts;
static uint8_t performance_battery_valid;
static uint8_t rendered_usb_connected;
static uint8_t showing_settings;
static uint8_t settings_profile;
static uint8_t settings_selected_row;
static v3_save_status_t rendered_save_status = V3_SAVE_OFFLINE;
static uint8_t display_dirty;
static uint32_t next_display_tick;

#define DISPLAY_REFRESH_INTERVAL_MS (33U)

static void fill_rect(uint8_t x, uint8_t y, uint8_t width, uint8_t height,
                      SSD1306_COLOR color)
{
    for (uint8_t iy = y; iy < y + height; iy++)
        for (uint8_t ix = x; ix < x + width; ix++)
            ssd1306_DrawPixel(ix, iy, color);
}

static void draw_performance(void)
{
    char text[18];
    const uint8_t usb_connected = usb_device_is_connected();

    ssd1306_Fill(Black);
    rendered_usb_connected = usb_connected;
    if (usb_connected) {
        ssd1306_SetCursor(2, 2);
        ssd1306_WriteString("USB", Font_6x8, White);
    } else {
        ssd1306_SetCursor(2, 2);
        ssd1306_WriteString("BAT", Font_6x8, White);
        if (performance_battery_valid) {
            snprintf(text, sizeof(text), "%3u%%", performance_battery_percent);
            ssd1306_SetCursor(2, 12);
            ssd1306_WriteString(text, Font_6x8, White);
            snprintf(text, sizeof(text), "%u.%02uV",
                     performance_battery_centivolts / 100U,
                     performance_battery_centivolts % 100U);
            ssd1306_SetCursor(2, 22);
            ssd1306_WriteString(text, Font_6x8, White);
        } else {
            ssd1306_SetCursor(2, 12);
            ssd1306_WriteString("--%", Font_6x8, White);
        }
    }

    fill_rect(43, 2, 43, 12, White);
    ssd1306_SetCursor(49, 4);
    ssd1306_WriteString(performance_profile ? "CUS-2" : "CUS-1",
                        Font_6x8, Black);

    text[0] = performance_key == 0U ? '0' :
        (char)('0' + performance_key);
    text[1] = '\0';
    ssd1306_SetCursor(58, 20);
    ssd1306_WriteString(text, Font_16x26, White);

    if (performance_mode == V3_KEY_CC)
        snprintf(text, sizeof(text), "CC %03u %03u",
                 performance_number, performance_value);
    else if (performance_mode == V3_KEY_PC)
        snprintf(text, sizeof(text), "PC %03u", performance_number);
    else if (performance_value)
        snprintf(text, sizeof(text), "CLK %uBPM", performance_number);
    else
        strcpy(text, "CLK TAP");
    ssd1306_SetCursor(2, 53);
    ssd1306_WriteString(text, Font_6x8, White);

    for (uint8_t exp = 0U; exp < 2U; exp++) {
        const uint8_t x = (uint8_t)(108U + exp * 11U);
        ssd1306_DrawRectangle(x, 4, x + 7U, 62, White);
        const uint8_t filled = (uint8_t)((uint16_t)performance_exp[exp] * 55U / 127U);
        if (filled) fill_rect(x + 2U, 60U - filled, 4U, filled, White);
    }
}

void display_init(void)
{
    ssd1306_Init();
    display_dirty = 0U;
    next_display_tick = 0U;

    ssd1306_Fill(Black);
    ssd1306_SetCursor(11, 27);
    ssd1306_WriteString(FIRMWARE_VERSION, Font_7x10, White);
    ssd1306_UpdateScreen();
}

void display_setConfigName(void)
{
    /* The V3 splash is complete in display_init(); retain this entry point so
     * the startup sequence does not need an unrelated structural change. */
}

void display_setProfileName(uint8_t profile)
{
    showing_settings = 0U;
    performance_profile = profile ? 1U : 0U;
    performance_key = 1U;
    performance_mode = V3_KEY_CC;
    performance_number = v3_settings_profile(performance_profile)->key[0].number;
    performance_value = 127U;
    performance_exp[0] = 0U;
    performance_exp[1] = 0U;
    display_dirty = 1U;
}

void display_performance_battery(uint8_t percent, uint16_t centivolts,
                                 uint8_t valid)
{
    if (showing_settings) return;
    if (performance_battery_percent == percent &&
        performance_battery_centivolts == centivolts &&
        performance_battery_valid == valid) return;
    performance_battery_percent = percent;
    performance_battery_centivolts = centivolts;
    performance_battery_valid = valid;
    display_dirty = 1U;
}

void display_performance_key(uint8_t key, uint8_t mode, uint8_t number,
                             uint8_t value, uint8_t profile)
{
    if (showing_settings) return;
    performance_key = key;
    performance_mode = mode;
    performance_number = number;
    performance_value = value;
    performance_profile = profile ? 1U : 0U;
    display_dirty = 1U;
}

void display_performance_expression(uint8_t exp1, uint8_t exp2)
{
    if (showing_settings ||
        (performance_exp[0] == exp1 && performance_exp[1] == exp2)) return;
    performance_exp[0] = exp1;
    performance_exp[1] = exp2;
    display_dirty = 1U;
}

static void draw_settings(void)
{
    char left[12], middle[14], right[8];
    const uint8_t profile = settings_profile;
    const uint8_t selected_row = settings_selected_row;
    const v3_profile_settings_t *p = v3_settings_profile(profile);

    ssd1306_Fill(Black);
    rendered_save_status = v3_settings_save_status();
    char error_text[5];
    const char *save_text;
    if (rendered_save_status == V3_SAVE_ERROR) {
        snprintf(error_text, sizeof(error_text), "E%u", v3_settings_save_error());
        save_text = error_text;
    } else {
        save_text = rendered_save_status == V3_SAVE_PENDING ? "WAIT" :
            rendered_save_status == V3_SAVE_SAVED ? "SAVED" :
            rendered_save_status == V3_SAVE_OFFLINE ? "OFF" : "";
    }
    ssd1306_SetCursor(0, 2);
    ssd1306_WriteString((char *)save_text, Font_6x8, White);

    fill_rect(43, 0, 43, 11, White);
    ssd1306_SetCursor(49, 2);
    ssd1306_WriteString(profile ? "CUS-2" : "CUS-1", Font_6x8, Black);
    ssd1306_SetCursor(3, 12);
    ssd1306_WriteString("NAME", Font_6x8, White);
    ssd1306_SetCursor(43, 12);
    ssd1306_WriteString("MOD", Font_6x8, White);
    ssd1306_SetCursor(91, 12);
    ssd1306_WriteString("VALUE", Font_6x8, White);

    uint8_t first_row = selected_row > 2U ? selected_row - 2U : 0U;
    if (first_row > 27U) first_row = 27U;
    for (uint8_t line = 0; line < 5; line++) {
        uint8_t row = first_row + line;
        if (row >= 32) break;

        if (row < 2) {
            snprintf(left, sizeof(left), "EXP%u", row + 1U);
            strcpy(middle, "CC");
            snprintf(right, sizeof(right), "%3u", p->exp_cc[row]);
        } else {
            uint8_t key = (row - 2U) / 3U;
            uint8_t field = (row - 2U) % 3U;
            snprintf(left, sizeof(left), "KEY %u", key == 9U ? 0U : key + 1U);
            if (field == 0U) {
                strcpy(middle, "MOD");
                strcpy(right, p->key[key].mode == V3_KEY_CC ? "CC" :
                    p->key[key].mode == V3_KEY_PC ? "PC" : "CLK");
            } else if (field == 1U) {
                strcpy(middle, "CC#/PC#");
                if (p->key[key].mode == V3_KEY_MIDI_CLOCK) strcpy(right, "--");
                else snprintf(right, sizeof(right), "%3u", p->key[key].number);
            } else {
                strcpy(middle, "TAG");
                strcpy(right, p->key[key].toggle ? "ON" : "OFF");
            }
        }

        const uint8_t selected = row == selected_row;
        uint8_t y = 21U + line * 8U;
        if (selected) fill_rect(0, y - 1U, 128, 8, White);
        ssd1306_SetCursor(3, y);
        ssd1306_WriteString(left, Font_6x8, selected ? Black : White);
        ssd1306_SetCursor(43, y);
        ssd1306_WriteString(middle, Font_6x8, selected ? Black : White);
        ssd1306_SetCursor(91, y);
        ssd1306_WriteString(right, Font_6x8, selected ? Black : White);
    }
}

void display_show_settings(uint8_t profile, uint8_t selected_row)
{
    showing_settings = 1U;
    settings_profile = profile;
    settings_selected_row = selected_row;
    display_dirty = 1U;
}

void display_task(void)
{
    const uint32_t now = HAL_GetTick();
    if (showing_settings && rendered_save_status != v3_settings_save_status())
        display_dirty = 1U;
    if (!showing_settings &&
        rendered_usb_connected != usb_device_is_connected()) display_dirty = 1U;
    if (!display_dirty || (int32_t)(now - next_display_tick) < 0 ||
        ssd1306_IsUpdateBusy()) return;

    if (showing_settings) draw_settings();
    else draw_performance();

    ssd1306_UpdateScreen();
    if (ssd1306_IsUpdateBusy()) display_dirty = 0U;
    next_display_tick = now + DISPLAY_REFRESH_INTERVAL_MS;
}
