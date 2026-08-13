#include "battery.h"

#include "display.h"
#include "main.h"
#include "usb_device.h"

/* Stock MA1010 firmware measures ADC1 channel 1 against VREFINT and uses
 * centivolts = 120 * CH15 / VREFINT.  ADC1 channel 1 is the USB supply sense;
 * channel 15 on PC5 is the battery sense.  This product uses two series NiMH AAA
 * cells.  The stock state machine treats about 2.75 V as battery-low; the
 * observed charged pack is about 3.10 V. */
#define BATTERY_SAMPLE_INTERVAL_MS (1000U)
#define BATTERY_MIN_CV             (275U)
#define BATTERY_MAX_CV             (310U)
#define BATTERY_PLAUSIBLE_MIN_CV   (200U)
#define BATTERY_PLAUSIBLE_MAX_CV   (350U)

extern ADC_HandleTypeDef hadc1;

static uint16_t battery_centivolts;
static uint32_t next_sample_tick;
static uint8_t battery_valid;

static uint8_t read_adc_once(uint32_t channel, uint16_t *sample)
{
    ADC_ChannelConfTypeDef config = {0};
    uint32_t sum = 0U;
    config.Channel = channel;
    config.Rank = ADC_REGULAR_RANK_1;
    config.SamplingTime = ADC_SAMPLETIME_239CYCLES_5;
    if (HAL_ADC_ConfigChannel(&hadc1, &config) != HAL_OK) return 0U;
    /* Match the settling behavior of the factory continuous scan: discard the
     * first sample after changing channels, then average four conversions. */
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

static void sample_battery(void)
{
    uint16_t vref;
    uint16_t sense;
    if (!read_adc_once(ADC_CHANNEL_VREFINT, &vref) || vref == 0U ||
        !read_adc_once(ADC_CHANNEL_15, &sense)) return;

    const uint16_t measured =
        (uint16_t)((120U * (uint32_t)sense + vref / 2U) / vref);
    if (measured < BATTERY_PLAUSIBLE_MIN_CV ||
        measured > BATTERY_PLAUSIBLE_MAX_CV) return;

    if (!battery_valid) battery_centivolts = measured;
    else battery_centivolts = (uint16_t)
        ((7U * (uint32_t)battery_centivolts + measured + 4U) / 8U);
    battery_valid = 1U;
    display_performance_battery(battery_get_percent(), battery_centivolts,
                                battery_valid);
}

void battery_init(void)
{
    battery_centivolts = 0U;
    battery_valid = 0U;
    next_sample_tick = HAL_GetTick();
}

void battery_task(void)
{
    const uint32_t now = HAL_GetTick();
    if ((int32_t)(now - next_sample_tick) < 0) return;
    next_sample_tick = now + BATTERY_SAMPLE_INTERVAL_MS;
    /* PC1 is configured as analog by the factory image and is not a reliable
     * digital VBUS signal.  Pause battery UI sampling while a USB host has
     * actually enumerated the MIDI interface. */
    if (usb_device_is_connected()) return;
    sample_battery();
}

uint8_t battery_is_valid(void)
{
    return battery_valid;
}

uint16_t battery_get_centivolts(void)
{
    return battery_centivolts;
}

uint8_t battery_get_percent(void)
{
    if (!battery_valid || battery_centivolts <= BATTERY_MIN_CV) return 0U;
    if (battery_centivolts >= BATTERY_MAX_CV) return 100U;
    return (uint8_t)(((uint32_t)(battery_centivolts - BATTERY_MIN_CV) * 100U) /
                     (BATTERY_MAX_CV - BATTERY_MIN_CV));
}
