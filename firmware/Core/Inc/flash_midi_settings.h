/* v3 persistent CUS profile settings. */
#ifndef INC_FLASH_MIDI_SETTINGS_H_
#define INC_FLASH_MIDI_SETTINGS_H_

#include <stdint.h>

#define V3_PROFILE_COUNT          (2U)
#define V3_SWITCHES_PER_PROFILE   (10U)
#define MIDI_NUM_COMMANDS_PER_SWITCH (10U) /* legacy transport buffer sizing */
/* Coalesce fast repeated edits, then persist promptly once transports drain. */
#define V3_SETTINGS_WRITE_DELAY_MS (300U)
#define V3_SETTINGS_I2C_RETRY_MS   (100U)
#define V3_SETTINGS_RETRY_DELAY_MS (1500U)
#define V3_SETTINGS_LONG_RETRY_DELAY_MS (30000U)

typedef enum {
  V3_KEY_CC = 0,
  V3_KEY_PC = 1,
  V3_KEY_MIDI_CLOCK = 2
} v3_key_mode_t;

typedef struct {
  uint8_t mode;
  uint8_t number;
  uint8_t toggle;
} v3_key_setting_t;

typedef struct {
  uint8_t exp_cc[2];
  v3_key_setting_t key[V3_SWITCHES_PER_PROFILE];
} v3_profile_settings_t;

typedef enum {
  V3_SAVE_OFFLINE = 0,
  V3_SAVE_READY,
  V3_SAVE_PENDING,
  V3_SAVE_SAVED,
  V3_SAVE_ERROR
} v3_save_status_t;

/* Loads v3 data from two CRC-protected slots in the unused portion of the
 * factory 24C08 EEPROM. Returns 0 with usable RAM defaults when the EEPROM
 * cannot be verified. */
uint8_t v3_settings_init(void);
void v3_settings_task(void);
uint8_t v3_settings_persistence_enabled(void);
v3_save_status_t v3_settings_save_status(void);
uint8_t v3_settings_save_error(void);
const v3_profile_settings_t *v3_settings_profile(uint8_t profile);
void v3_settings_change(uint8_t profile, uint8_t row, int8_t delta);
void v3_settings_mark_dirty(void);

/* Factory SysEx flash programming is intentionally unsupported by v3. */
void flash_settings_erase(void);
void flash_settings_write(uint8_t *data, uint32_t offset);

#endif
