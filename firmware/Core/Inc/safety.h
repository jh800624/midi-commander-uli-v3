#ifndef INC_SAFETY_H_
#define INC_SAFETY_H_

#include <stdint.h>

typedef enum {
    SAFETY_FAULT_NONE = 0U,
    SAFETY_FAULT_HARD = 1U,
    SAFETY_FAULT_MEMORY = 2U,
    SAFETY_FAULT_BUS = 3U,
    SAFETY_FAULT_USAGE = 4U,
    SAFETY_FAULT_NMI = 5U,
    SAFETY_FAULT_SOFTWARE = 6U,
    SAFETY_FAULT_WATCHDOG = 7U
} safety_fault_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t pending;
    uint32_t boot_count;
    uint32_t fault;
    uint32_t reset_flags;
    uint32_t exception_return;
    uint32_t stacked_r0;
    uint32_t stacked_r1;
    uint32_t stacked_r2;
    uint32_t stacked_r3;
    uint32_t stacked_r12;
    uint32_t stacked_lr;
    uint32_t stacked_pc;
    uint32_t stacked_xpsr;
    uint32_t cfsr;
    uint32_t hfsr;
    uint32_t dfsr;
    uint32_t afsr;
    uint32_t mmfar;
    uint32_t bfar;
    uint32_t checksum;
} safety_telemetry_t;

void safety_boot_init(void);
void safety_watchdog_init(void);
void safety_watchdog_refresh(void);
uint8_t safety_telemetry_pending(void);
const safety_telemetry_t *safety_telemetry_get(void);
void safety_telemetry_clear(void);
void safety_record_fatal(safety_fault_t fault) __attribute__((noreturn));
void safety_fault_capture(const uint32_t *stack, safety_fault_t fault,
                          uint32_t exception_return) __attribute__((noreturn));

#endif /* INC_SAFETY_H_ */
