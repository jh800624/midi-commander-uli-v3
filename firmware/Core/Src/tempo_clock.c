#include "tempo_clock.h"

#include "main.h"
#include "midi_cmds.h"
#include "midi_defines.h"

#define TAP_MIN_INTERVAL_MS 250U
#define TAP_MAX_INTERVAL_MS 2000U
#define CLOCKS_PER_QUARTER 24U

static uint32_t last_tap_ms;
static uint32_t clock_interval_ms;
static uint32_t next_clock_ms;
static uint16_t current_bpm;
static uint8_t clock_running;

void tempoClock_init(void) {
    last_tap_ms = 0U;
    clock_interval_ms = 0U;
    next_clock_ms = 0U;
    current_bpm = 120U;
    clock_running = 0U;
}

void tempoClock_tap(void) {
    const uint32_t now = HAL_GetTick();

    if (last_tap_ms != 0U) {
        const uint32_t interval = now - last_tap_ms;
        if (interval >= TAP_MIN_INTERVAL_MS && interval <= TAP_MAX_INTERVAL_MS) {
            /* Round to the closest millisecond while maintaining 24 PPQN. */
            clock_interval_ms = (interval + (CLOCKS_PER_QUARTER / 2U)) / CLOCKS_PER_QUARTER;
            if (clock_interval_ms == 0U) {
                clock_interval_ms = 1U;
            }
            current_bpm = (uint16_t)(60000U / interval);
            next_clock_ms = now;
            clock_running = 1U;
        } else {
            /* Treat a long pause as a fresh first tap. */
            clock_running = 0U;
        }
    }

    last_tap_ms = now;
}

void tempoClock_task(void) {
    if (!clock_running) {
        return;
    }

    const uint32_t now = HAL_GetTick();
    if ((int32_t)(now - next_clock_ms) < 0) {
        return;
    }

    if (midiCmd_send_realtime(MIDI_REALTIME_CLOCK) == 0) {
        /* Advance from the planned tick to avoid tempo drift from loop jitter. */
        next_clock_ms += clock_interval_ms;
        if ((int32_t)(now - next_clock_ms) >= 0) {
            next_clock_ms = now + clock_interval_ms;
        }
    }
}

uint16_t tempoClock_bpm(void) {
    return current_bpm;
}

uint8_t tempoClock_is_running(void) {
    return clock_running;
}
