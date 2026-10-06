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
static uint32_t performance_flash_until;
static uint8_t performance_flash_active;
static uint8_t performance_tag_active;
static uint32_t boot_profile_until;
static uint8_t boot_profile_visible;
static uint8_t boot_profile;

#define DISPLAY_REFRESH_INTERVAL_MS (33U)
#define SETTINGS_VISIBLE_ROWS       (5U)
#define SETTINGS_CURSOR_LINE        (2U)
#define PERFORMANCE_BOX_X           (32U)
#define PERFORMANCE_BOX_Y           (2U)
#define PERFORMANCE_BOX_WIDTH       (64U)
#define PERFORMANCE_BOX_HEIGHT      (60U)
#define PERFORMANCE_BOX_RADIUS      (6U)
#define PERFORMANCE_KEY_Y           (23U)
#define PERFORMANCE_FLASH_MS        (250U)
#define BOOT_PROFILE_MS              (750U)
#define LEFT_COLUMN_DIVIDER_X        (28U)
#define EXP_FILL_TOP                 (6U)
#define EXP_FILL_BOTTOM              (60U)
#define EXP_FILL_HEIGHT              (55U)

static void fill_rect(uint8_t x, uint8_t y, uint8_t width, uint8_t height,
                      SSD1306_COLOR color)
{
    /* Clip every primitive to the 128 x 64 logical framebuffer.  This keeps
     * a future layout change from wrapping at either display edge. */
    const uint16_t right = (uint16_t)x + width;
    const uint16_t bottom = (uint16_t)y + height;
    const uint8_t clipped_right = right > SSD1306_WIDTH ? SSD1306_WIDTH : (uint8_t)right;
    const uint8_t clipped_bottom = bottom > SSD1306_HEIGHT ? SSD1306_HEIGHT : (uint8_t)bottom;
    for (uint8_t iy = y; iy < clipped_bottom; iy++)
        for (uint8_t ix = x; ix < clipped_right; ix++)
            ssd1306_DrawPixel(ix, iy, color);
}

static void fill_rounded_rect(uint8_t x, uint8_t y, uint8_t width,
                              uint8_t height, uint8_t radius,
                              SSD1306_COLOR color)
{
    /* The box remains inside the central 56 x 60 area.  Its corner test is
     * deliberately integer-only for predictable render time on the STM32. */
    for (uint8_t iy = 0U; iy < height; iy++) {
        for (uint8_t ix = 0U; ix < width; ix++) {
            uint8_t dx = 0U;
            uint8_t dy = 0U;
            if (ix < radius) dx = radius - ix;
            else if (ix >= width - radius) dx = ix - (width - radius - 1U);
            if (iy < radius) dy = radius - iy;
            else if (iy >= height - radius) dy = iy - (height - radius - 1U);
            if (dx == 0U || dy == 0U ||
                (uint16_t)dx * dx + (uint16_t)dy * dy <=
                    (uint16_t)radius * radius)
                ssd1306_DrawPixel(x + ix, y + iy, color);
        }
    }
}

static void draw_display_frame(void)
{
    /* A 2px outer contour and 1px inner contour keep the frame visible
     * without the visual weight of a solid inverse background. */
    fill_rounded_rect(PERFORMANCE_BOX_X, PERFORMANCE_BOX_Y,
                      PERFORMANCE_BOX_WIDTH, PERFORMANCE_BOX_HEIGHT,
                      PERFORMANCE_BOX_RADIUS, White);
    fill_rounded_rect(PERFORMANCE_BOX_X + 2U, PERFORMANCE_BOX_Y + 2U,
                      PERFORMANCE_BOX_WIDTH - 4U, PERFORMANCE_BOX_HEIGHT - 4U,
                      PERFORMANCE_BOX_RADIUS - 2U, Black);
    fill_rounded_rect(PERFORMANCE_BOX_X + 3U, PERFORMANCE_BOX_Y + 3U,
                      PERFORMANCE_BOX_WIDTH - 6U, PERFORMANCE_BOX_HEIGHT - 6U,
                      PERFORMANCE_BOX_RADIUS - 3U, White);
    fill_rounded_rect(PERFORMANCE_BOX_X + 4U, PERFORMANCE_BOX_Y + 4U,
                      PERFORMANCE_BOX_WIDTH - 8U, PERFORMANCE_BOX_HEIGHT - 8U,
                      PERFORMANCE_BOX_RADIUS - 4U, Black);
}

static void draw_performance_detail(char *text)
{
    /* Keep command detail in the left column.  The full height is now free
     * for the central display frame, and no text can overdraw that frame. */
    if (performance_mode == V3_KEY_CC) {
        ssd1306_SetCursor(2, 36);
        ssd1306_WriteString("CC", Font_6x8, White);
        snprintf(text, 18, "%03u", performance_number);
        ssd1306_SetCursor(2, 44);
        ssd1306_WriteString(text, Font_6x8, White);
        snprintf(text, 18, "%03u", performance_value);
        ssd1306_SetCursor(2, 52);
        ssd1306_WriteString(text, Font_6x8, White);
    } else if (performance_mode == V3_KEY_PC) {
        ssd1306_SetCursor(2, 40);
        ssd1306_WriteString("PC", Font_6x8, White);
        snprintf(text, 18, "%03u", performance_number);
        ssd1306_SetCursor(2, 50);
        ssd1306_WriteString(text, Font_6x8, White);
    } else {
        ssd1306_SetCursor(2, 40);
        ssd1306_WriteString("CLK", Font_6x8, White);
        if (performance_value) snprintf(text, 18, "%u", performance_number);
        else strcpy(text, "TAP");
        ssd1306_SetCursor(2, 50);
        ssd1306_WriteString(text, Font_6x8, White);
    }
}

static uint8_t settings_first_visible_row(uint8_t selected_row)
{
    /* Put the selected row on the centre (third) line while there are rows
     * on both sides.  At either end, clamp the list rather than leaving
     * blank lines on screen. */
    uint8_t first_row = selected_row > SETTINGS_CURSOR_LINE ?
        selected_row - SETTINGS_CURSOR_LINE : 0U;
    const uint8_t last_first_row = 32U - SETTINGS_VISIBLE_ROWS;
    if (first_row > last_first_row) first_row = last_first_row;
    return first_row;
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
        } else {
            ssd1306_SetCursor(2, 12);
            ssd1306_WriteString("--%", Font_6x8, White);
        }
    }

    /* Separate the left status/command column from the central display frame. */
    ssd1306_Line(LEFT_COLUMN_DIVIDER_X, 2, LEFT_COLUMN_DIVIDER_X, 62, White);

    if (boot_profile_visible) {
        /* CUS selection is a short boot confirmation, not persistent UI. */
        fill_rect(43, 2, 43, 12, White);
        ssd1306_SetCursor(49, 4);
        ssd1306_WriteString(boot_profile ? "CUS-2" : "CUS-1", Font_6x8, Black);
    }

    if (performance_flash_active || performance_tag_active) {
        /* Every key press flashes the central display frame once.  The box
         * is isolated from the battery, left command column and EXP bars. */
        draw_display_frame();
    }

    if (performance_mode == V3_KEY_MIDI_CLOCK) {
        /* Tempo is the active central value until another control is used;
         * only its frame fades after the short press flash. */
        snprintf(text, sizeof(text), "%u", performance_number);
        const uint8_t bpm_x = (uint8_t)(64U - strlen(text) * 8U);
        ssd1306_SetCursor(bpm_x, 14);
        ssd1306_WriteString(text, Font_16x26, White);
        ssd1306_SetCursor(55, 42);
        ssd1306_WriteString("BPM", Font_6x8, White);
    } else {
        text[0] = performance_key == 0U ? '0' :
            (char)('0' + performance_key);
        text[1] = '\0';
        ssd1306_SetCursor(56, PERFORMANCE_KEY_Y);
        ssd1306_WriteString(text, Font_16x26, White);
    }

    draw_performance_detail(text);

    for (uint8_t exp = 0U; exp < 2U; exp++) {
        const uint8_t x = (uint8_t)(108U + exp * 11U);
        ssd1306_DrawRectangle(x, 4, x + 7U, 62, White);
        for (uint8_t percent = 20U; percent < 100U; percent += 20U) {
            const uint8_t y = (uint8_t)(EXP_FILL_BOTTOM -
                (uint16_t)percent * (EXP_FILL_HEIGHT - 1U) / 100U);
            /* Long graduations stay inside the bar, matching a level gauge. */
            ssd1306_Line(x + 1U, y, x + 4U, y, White);
        }
        const uint8_t filled = (uint8_t)((uint16_t)performance_exp[exp] *
                                         EXP_FILL_HEIGHT / 127U);
        if (filled) fill_rect(x + 2U, EXP_FILL_BOTTOM + 1U - filled,
                              4U, filled, White);
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
    const uint8_t startup_profile = profile ? 1U : 0U;
    performance_key = 1U;
    performance_mode = V3_KEY_CC;
    performance_number = v3_settings_profile(startup_profile)->key[0].number;
    performance_value = 127U;
    performance_flash_active = 0U;
    performance_tag_active = 0U;
    boot_profile = startup_profile;
    boot_profile_visible = 1U;
    boot_profile_until = HAL_GetTick() + BOOT_PROFILE_MS;
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
                             uint8_t value, uint8_t profile, uint8_t is_tag,
                             uint8_t tag_on)
{
    if (showing_settings) return;
    performance_key = key;
    performance_mode = mode;
    performance_number = number;
    performance_value = value;
    (void)profile;
    performance_tag_active = tag_on ? 1U : 0U;
    /* A TAG which was just turned off clears its persistent frame now;
     * ordinary controls and TAG-on still receive the normal flash. */
    performance_flash_active = (is_tag && !tag_on) ? 0U : 1U;
    if (performance_flash_active)
        performance_flash_until = HAL_GetTick() + PERFORMANCE_FLASH_MS;
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
    ssd1306_Line(0, 20, 127, 20, White);

    const uint8_t first_row = settings_first_visible_row(selected_row);
    for (uint8_t line = 0; line < SETTINGS_VISIBLE_ROWS; line++) {
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
        uint8_t y = 22U + line * 8U;
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
    if (!showing_settings && performance_flash_active &&
        (int32_t)(now - performance_flash_until) >= 0) {
        performance_flash_active = 0U;
        display_dirty = 1U;
    }
    if (!showing_settings && boot_profile_visible &&
        (int32_t)(now - boot_profile_until) >= 0) {
        boot_profile_visible = 0U;
        display_dirty = 1U;
    }
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
