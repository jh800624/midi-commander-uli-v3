#include "charge_diagnostic.h"

#include <stdio.h>

#include "main.h"
#include "safety.h"
#include "ssd1306.h"

#define CHARGE_ENABLE_PORT GPIOC
#define CHARGE_ENABLE_PIN  GPIO_PIN_10
#define CHARGE_STATUS_PORT GPIOC
#define CHARGE_STATUS_PIN  GPIO_PIN_0

/* Values recovered from the factory binary.  Voltages are centivolts using
 * 120 * ADC_sense / ADC_VREFINT, exactly as in the original state machine. */
#define CHARGE_USB_MIN_CV             (305U)
#define CHARGE_BATTERY_SHUTDOWN_CV    (180U)
#define CHARGE_PROGRESS_ZERO_CV       (235U)
#define CHARGE_PROGRESS_FULL_CV       (295U)
#define CHARGE_NONRECHARGEABLE_CV     (298U)
#define CHARGE_USB_SETTLE_MS          (3500U)
#define CHARGE_DETECT_PULSE_MS        (500U)
#define CHARGE_COMPLETE_DEBOUNCE_MS   (1000U)
#define CHARGE_NONRECHARGEABLE_MS     (1201000UL)
#define CHARGE_HARD_TIMEOUT_MS        (12UL * 60UL * 60UL * 1000UL)
#define CHARGE_SAMPLE_MS              (100U)
#define CHARGE_ADC_FILTER_SHIFT       (5U)
#define CHARGE_ADC_WARMUP_SAMPLES     (64U)

extern ADC_HandleTypeDef hadc1;

static uint32_t filtered_vref;
static uint32_t filtered_usb;
static uint32_t filtered_battery;
static uint8_t power_filter_valid;

static void charger_off(void)
{
    HAL_GPIO_WritePin(CHARGE_ENABLE_PORT, CHARGE_ENABLE_PIN, GPIO_PIN_SET);
}

static void charger_on(void)
{
    HAL_GPIO_WritePin(CHARGE_ENABLE_PORT, CHARGE_ENABLE_PIN, GPIO_PIN_RESET);
}

static void safe_delay(uint32_t milliseconds)
{
    const uint32_t end = HAL_GetTick() + milliseconds;
    while ((int32_t)(HAL_GetTick() - end) < 0) {
        safety_watchdog_refresh();
    }
}

static uint8_t read_adc_once(uint32_t channel, uint16_t *sample)
{
    ADC_ChannelConfTypeDef config = {0};
    uint32_t sum = 0U;
    config.Channel = channel;
    config.Rank = ADC_REGULAR_RANK_1;
    config.SamplingTime = ADC_SAMPLETIME_239CYCLES_5;
    if (HAL_ADC_ConfigChannel(&hadc1, &config) != HAL_OK) return 0U;
    /* Factory code continuously scans these channels.  With single-shot V3
     * reads, discard the first conversion after a mux change and average the
     * next four so the prior channel's sample capacitor cannot skew results. */
    for (uint8_t i = 0U; i < 5U; i++) {
        if (HAL_ADC_Start(&hadc1) != HAL_OK) return 0U;
        if (HAL_ADC_PollForConversion(&hadc1, 2U) != HAL_OK) {
            HAL_ADC_Stop(&hadc1);
            return 0U;
        }
        const uint16_t value = (uint16_t)HAL_ADC_GetValue(&hadc1);
        HAL_ADC_Stop(&hadc1);
        if (i != 0U) sum += value;
    }
    *sample = (uint16_t)((sum + 2U) / 4U);
    return 1U;
}

static uint32_t factory_filter(uint32_t previous, uint16_t sample)
{
    /* Exact IIR form recovered at factory addresses 0x08003A10, 0x08003A2A
     * and 0x08003A78: y = y - y/32 + x/32. */
    return previous - (previous >> CHARGE_ADC_FILTER_SHIFT) +
           ((uint32_t)sample >> CHARGE_ADC_FILTER_SHIFT);
}

static uint8_t read_power(uint16_t *usb_cv, uint16_t *battery_cv)
{
    uint16_t vref;
    uint16_t usb;
    uint16_t battery;
    if (!read_adc_once(ADC_CHANNEL_VREFINT, &vref) || vref == 0U ||
        !read_adc_once(ADC_CHANNEL_1, &usb) ||
        !read_adc_once(ADC_CHANNEL_15, &battery)) return 0U;

    if (!power_filter_valid) {
        filtered_vref = vref;
        filtered_usb = usb;
        filtered_battery = battery;
        power_filter_valid = 1U;
    } else {
        filtered_vref = factory_filter(filtered_vref, vref);
        filtered_usb = factory_filter(filtered_usb, usb);
        filtered_battery = factory_filter(filtered_battery, battery);
    }
    if (filtered_vref == 0U) return 0U;
    *usb_cv = (uint16_t)
        ((120U * filtered_usb + filtered_vref / 2U) / filtered_vref);
    *battery_cv = (uint16_t)
        ((120U * filtered_battery + filtered_vref / 2U) / filtered_vref);
    return 1U;
}

static void write_voltage(uint8_t y, const char *label, uint16_t value)
{
    char text[20];
    snprintf(text, sizeof(text), "%s %u.%02uV", label,
             value / 100U, value % 100U);
    ssd1306_SetCursor(2, y);
    ssd1306_WriteString(text, Font_6x8, White);
}

static void show_message(const char *title, const char *message,
                         uint16_t usb_cv, uint16_t battery_cv)
{
    ssd1306_Fill(Black);
    ssd1306_SetCursor(2, 2);
    ssd1306_WriteString((char *)title, Font_7x10, White);
    ssd1306_SetCursor(2, 16);
    ssd1306_WriteString((char *)message, Font_6x8, White);
    write_voltage(32, "USB", usb_cv);
    write_voltage(44, "BAT", battery_cv);
    ssd1306_UpdateScreen();
}

static uint8_t factory_progress(uint16_t battery_cv)
{
    if (battery_cv <= CHARGE_PROGRESS_ZERO_CV) return 0U;
    if (battery_cv >= CHARGE_PROGRESS_FULL_CV) return 100U;
    return (uint8_t)(((uint32_t)(battery_cv - CHARGE_PROGRESS_ZERO_CV) *
                      100U) /
                     (CHARGE_PROGRESS_FULL_CV - CHARGE_PROGRESS_ZERO_CV));
}

static void show_charging(uint16_t usb_cv, uint16_t battery_cv)
{
    char text[20];
    const uint8_t percent = factory_progress(battery_cv);
    ssd1306_Fill(Black);
    ssd1306_SetCursor(2, 2);
    ssd1306_WriteString("CHARGING", Font_7x10, White);
    snprintf(text, sizeof(text), "%u%%", percent);
    ssd1306_SetCursor(98, 3);
    ssd1306_WriteString(text, Font_6x8, White);
    ssd1306_DrawRectangle(3, 17, 119, 29, White);
    ssd1306_DrawRectangle(120, 20, 124, 26, White);
    if (percent != 0U) {
        const uint8_t fill_end =
            (uint8_t)(5U + (percent * 110U) / 100U);
        for (uint8_t x = 6U; x <= fill_end; x++)
            ssd1306_Line(x, 20, x, 26, White);
    }
    write_voltage(36, "USB", usb_cv);
    write_voltage(48, "BAT", battery_cv);
    ssd1306_UpdateScreen();
}

static void terminal_off(const char *title, const char *message,
                         uint16_t usb_cv, uint16_t battery_cv)
{
    charger_off();
    show_message(title, message, usb_cv, battery_cv);
    for (;;) safety_watchdog_refresh();
}

void charge_mode_run(void)
{
    uint16_t usb_cv = 0U;
    uint16_t battery_cv = 0U;

    charger_off();
    power_filter_valid = 0U;
    show_message("CHARGE MODE", "CHECKING POWER", 0U, 0U);

    /* In the factory firmware the ADC filter is already running before charge
     * mode opens.  Warm the same 1/32 filters here with the charger off so no
     * startup transient can qualify USB or animate the battery bar. */
    for (uint8_t i = 0U; i < CHARGE_ADC_WARMUP_SAMPLES; i++) {
        if (!read_power(&usb_cv, &battery_cv))
            terminal_off("CHARGE ERROR", "ADC FAILURE", usb_cv, battery_cv);
        safe_delay(5U);
    }

    /* Factory firmware requires USB >= 3.05 V continuously for 3.5 seconds. */
    uint32_t usb_good_since = 0U;
    for (;;) {
        if (!read_power(&usb_cv, &battery_cv))
            terminal_off("CHARGE ERROR", "ADC FAILURE", usb_cv, battery_cv);
        if (usb_cv < CHARGE_USB_MIN_CV) {
            charger_off();
            usb_good_since = 0U;
            show_message("NO USB POWER", "WAITING FOR USB", usb_cv, battery_cv);
            safe_delay(250U);
            continue;
        }
        if (battery_cv < CHARGE_BATTERY_SHUTDOWN_CV)
            terminal_off("BAT TOO LOW", "REPLACE BATTERY", usb_cv, battery_cv);
        if (usb_good_since == 0U) usb_good_since = HAL_GetTick();
        if ((uint32_t)(HAL_GetTick() - usb_good_since) >=
            CHARGE_USB_SETTLE_MS) break;
        show_message("USB DETECTED", "STABILITY CHECK", usb_cv, battery_cv);
        safe_delay(100U);
    }

    /* Factory detection stage asserts active-low PC10 for 500 ms.  End the
     * pulse before ADC/display operations so a later fault cannot extend it. */
    charger_on();
    safe_delay(CHARGE_DETECT_PULSE_MS);
    const GPIO_PinState status_after_pulse =
        HAL_GPIO_ReadPin(CHARGE_STATUS_PORT, CHARGE_STATUS_PIN);
    charger_off();
    if (!read_power(&usb_cv, &battery_cv))
        terminal_off("CHARGE ERROR", "ADC FAILURE", usb_cv, battery_cv);
    if (usb_cv < CHARGE_USB_MIN_CV)
        terminal_off("NO USB POWER", "CHARGER OFF", usb_cv, battery_cv);

    /* PC0 low is the factory charger's active/busy indication. */
    if (status_after_pulse != GPIO_PIN_RESET) {
        if (battery_cv >= CHARGE_PROGRESS_FULL_CV)
            terminal_off("CHARGE COMPLETE", "REMOVE BATTERY", usb_cv,
                         battery_cv);
        terminal_off("CHECK BATTERY", "USE NIMH AAA", usb_cv, battery_cv);
    }

    charger_on();
    const uint32_t charge_started = HAL_GetTick();
    uint32_t status_high_since = 0U;
    uint32_t next_display = 0U;
    for (;;) {
        safety_watchdog_refresh();
        if (!read_power(&usb_cv, &battery_cv))
            terminal_off("CHARGE ERROR", "ADC FAILURE", usb_cv, battery_cv);
        const uint32_t elapsed = HAL_GetTick() - charge_started;
        if (usb_cv < CHARGE_USB_MIN_CV)
            terminal_off("NO USB POWER", "CHARGER OFF", usb_cv, battery_cv);
        if (battery_cv < CHARGE_BATTERY_SHUTDOWN_CV)
            terminal_off("BAT TOO LOW", "CHARGER OFF", usb_cv, battery_cv);

        /* The factory image rejects a pack that reaches 2.98 V during its
         * first 1,201 seconds, showing "Batteries may be NOT chargeable". */
        if (battery_cv >= CHARGE_NONRECHARGEABLE_CV &&
            elapsed < CHARGE_NONRECHARGEABLE_MS)
            terminal_off("NOT CHARGEABLE", "REMOVE BATTERY", usb_cv,
                         battery_cv);

        if (HAL_GPIO_ReadPin(CHARGE_STATUS_PORT, CHARGE_STATUS_PIN) ==
            GPIO_PIN_SET) {
            if (status_high_since == 0U) status_high_since = HAL_GetTick();
            if ((uint32_t)(HAL_GetTick() - status_high_since) >=
                CHARGE_COMPLETE_DEBOUNCE_MS)
                terminal_off("CHARGE COMPLETE", "CHARGER OFF", usb_cv,
                             battery_cv);
        } else {
            status_high_since = 0U;
        }

        /* Extra fail-safe beyond the factory state machine. */
        if (elapsed >= CHARGE_HARD_TIMEOUT_MS)
            terminal_off("CHARGE TIMEOUT", "CHARGER OFF", usb_cv, battery_cv);
        if ((int32_t)(HAL_GetTick() - next_display) >= 0) {
            show_charging(usb_cv, battery_cv);
            next_display = HAL_GetTick() + 500U;
        }
        safe_delay(CHARGE_SAMPLE_MS);
    }
}
