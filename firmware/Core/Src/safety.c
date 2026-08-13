#include "safety.h"

#include "stm32f1xx_hal.h"

/* STM32F1 IWDG key values.  Prescaler 64 and the maximum reload give roughly
 * 4.4-8.7 seconds across the specified 30-60 kHz LSI tolerance: enough for flash
 * erase and USB setup, but bounded if a peripheral driver gets stuck. */
#define IWDG_WRITE_ACCESS_KEY (0x5555U)
#define IWDG_RELOAD_KEY       (0xAAAAU)
#define IWDG_START_KEY        (0xCCCCU)
#define IWDG_PRESCALER_64     (0x04U)
#define IWDG_MAX_RELOAD       (0x0FFFU)
#define IWDG_UPDATE_TIMEOUT_MS (10U)

#define SAFETY_TELEMETRY_MAGIC   (0x33465343UL) /* "CSF3" */
#define SAFETY_TELEMETRY_VERSION (1UL)

/* NOLOAD/.noinit survives a warm reset and is never touched by the startup BSS
 * clear.  Fault handlers must not erase or program single-bank Flash. */
__attribute__((section(".noinit"), aligned(4)))
static volatile safety_telemetry_t telemetry;

static uint32_t telemetry_checksum(void)
{
    const volatile uint32_t *word = (const volatile uint32_t *)&telemetry;
    uint32_t checksum = 0x811C9DC5UL;
    for (uint32_t i = 0U;
         i < (sizeof(safety_telemetry_t) / sizeof(uint32_t)) - 1U; i++) {
        checksum ^= word[i];
        checksum *= 16777619UL;
    }
    return checksum;
}

static uint8_t telemetry_valid(void)
{
    return telemetry.magic == SAFETY_TELEMETRY_MAGIC &&
           telemetry.version == SAFETY_TELEMETRY_VERSION &&
           telemetry.checksum == telemetry_checksum();
}

static void telemetry_reset(void)
{
    volatile uint32_t *word = (volatile uint32_t *)&telemetry;
    for (uint32_t i = 0U; i < sizeof(telemetry) / sizeof(uint32_t); i++)
        word[i] = 0U;
    telemetry.magic = SAFETY_TELEMETRY_MAGIC;
    telemetry.version = SAFETY_TELEMETRY_VERSION;
}

void safety_boot_init(void)
{
    const uint32_t reset_flags = RCC->CSR &
        (RCC_CSR_LPWRRSTF | RCC_CSR_WWDGRSTF | RCC_CSR_IWDGRSTF |
         RCC_CSR_SFTRSTF | RCC_CSR_PORRSTF | RCC_CSR_PINRSTF);
    if (!telemetry_valid()) telemetry_reset();

    telemetry.boot_count++;
    telemetry.reset_flags = reset_flags;
    if ((reset_flags & RCC_CSR_IWDGRSTF) != 0U && telemetry.pending == 0U) {
        telemetry.pending = 1U;
        telemetry.fault = SAFETY_FAULT_WATCHDOG;
    }
    telemetry.checksum = telemetry_checksum();
    RCC->CSR |= RCC_CSR_RMVF;
}

void safety_watchdog_init(void)
{
    /* Follow ST's HAL sequence: start first so the LSI/update domain is live,
     * then program PR/RLR, wait with a finite timeout, and reload. */
    IWDG->KR = IWDG_START_KEY;
    IWDG->KR = IWDG_WRITE_ACCESS_KEY;
    IWDG->PR = IWDG_PRESCALER_64;
    IWDG->RLR = IWDG_MAX_RELOAD;
    const uint32_t started = HAL_GetTick();
    while (IWDG->SR != 0U &&
           (uint32_t)(HAL_GetTick() - started) < IWDG_UPDATE_TIMEOUT_MS) {
    }
    IWDG->KR = IWDG_RELOAD_KEY;
}

void safety_watchdog_refresh(void)
{
    IWDG->KR = IWDG_RELOAD_KEY;
}

uint8_t safety_telemetry_pending(void)
{
    return telemetry_valid() && telemetry.pending != 0U;
}

const safety_telemetry_t *safety_telemetry_get(void)
{
    return (const safety_telemetry_t *)&telemetry;
}

void safety_telemetry_clear(void)
{
    telemetry.pending = 0U;
    telemetry.checksum = telemetry_checksum();
}

void safety_fault_capture(const uint32_t *stack, safety_fault_t fault,
                          uint32_t exception_return)
{
    telemetry_reset();
    telemetry.pending = 1U;
    telemetry.fault = (uint32_t)fault;
    telemetry.exception_return = exception_return;
    if (stack != NULL) {
        telemetry.stacked_r0 = stack[0];
        telemetry.stacked_r1 = stack[1];
        telemetry.stacked_r2 = stack[2];
        telemetry.stacked_r3 = stack[3];
        telemetry.stacked_r12 = stack[4];
        telemetry.stacked_lr = stack[5];
        telemetry.stacked_pc = stack[6];
        telemetry.stacked_xpsr = stack[7];
    }
    telemetry.cfsr = SCB->CFSR;
    telemetry.hfsr = SCB->HFSR;
    telemetry.dfsr = SCB->DFSR;
    telemetry.afsr = SCB->AFSR;
    telemetry.mmfar = SCB->MMFAR;
    telemetry.bfar = SCB->BFAR;
    telemetry.checksum = telemetry_checksum();
    __DSB();
    NVIC_SystemReset();
    while (1) { }
}

void safety_record_fatal(safety_fault_t fault)
{
    safety_fault_capture(NULL, fault, 0U);
}
