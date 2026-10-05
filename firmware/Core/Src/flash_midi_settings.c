#include "main.h"
#include "flash_midi_settings.h"
#include "safety.h"
#include "ssd1306.h"
#include <string.h>

/* The factory firmware stores configuration in the external 24C08 EEPROM.
 * Preserve its first 128 bytes and use two independent v3 records in the
 * erased area.  A 24C08 uses A8/A9 in the I2C device address and an 8-bit
 * memory address. */
#define V3_EEPROM_BASE_ADDRESS   (0x50U)
#define V3_EEPROM_SIZE           (1024U)
#define V3_EEPROM_PAGE_SIZE      (16U)
#define V3_EEPROM_SLOT0          (0x0080U)
#define V3_EEPROM_SLOT1          (0x0100U)
#define V3_EEPROM_IO_TIMEOUT_MS  (20U)
#define V3_EEPROM_READY_TRIALS   (10U)
#define V3_EEPROM_READY_TIMEOUT  (2U)
/* M24C08 specifies a write cycle of at most 5 ms.  The fitted 24C08 vendor
 * is unknown, so allow a conservative fixed settle interval before ACK
 * polling.  This also avoids interpreting the expected write-cycle NACK as
 * a failed save. */
#define V3_EEPROM_WRITE_CYCLE_MS (10U)
#define V3_MAGIC                 (0x33494C55UL) /* "ULI3" */
#define V3_FORMAT_VERSION        (1U)
#define V3_MAX_WRITE_RETRIES     (3U)

typedef struct __attribute__((packed)) {
  uint32_t magic;
  uint16_t version;
  uint16_t size;
  uint32_t generation;
  v3_profile_settings_t profile[V3_PROFILE_COUNT];
  uint32_t crc32;
} v3_settings_record_t;

_Static_assert(sizeof(v3_settings_record_t) == 80U,
               "unexpected settings record size");
_Static_assert(V3_EEPROM_SLOT0 >= 0x0080U,
               "v3 must not overwrite factory EEPROM settings");
_Static_assert(V3_EEPROM_SLOT0 + sizeof(v3_settings_record_t) <= V3_EEPROM_SLOT1,
               "EEPROM settings slots overlap");
_Static_assert(V3_EEPROM_SLOT1 + sizeof(v3_settings_record_t) <= V3_EEPROM_SIZE,
               "EEPROM settings exceed device capacity");

static v3_settings_record_t settings;
static uint16_t active_slot;
static uint8_t settings_dirty;
static uint8_t write_failures;
static uint8_t persistence_enabled;
static v3_save_status_t save_status;
static uint8_t save_error;
static uint32_t write_after_tick;

static uint16_t eeprom_device_address(uint16_t offset)
{
  return (uint16_t)((V3_EEPROM_BASE_ADDRESS | ((offset >> 8) & 0x03U)) << 1);
}

static uint8_t eeprom_ready(uint16_t offset)
{
  return HAL_I2C_IsDeviceReady(&hi2c1, eeprom_device_address(offset),
      V3_EEPROM_READY_TRIALS, V3_EEPROM_READY_TIMEOUT) == HAL_OK;
}

static uint8_t eeprom_read(uint16_t offset, void *destination, uint16_t length)
{
  if ((uint32_t)offset + length > V3_EEPROM_SIZE ||
      ((offset & 0xFFU) + length) > 0x100U) return 0U;
  return HAL_I2C_Mem_Read(&hi2c1, eeprom_device_address(offset),
      offset & 0xFFU, I2C_MEMADD_SIZE_8BIT, destination, length,
      V3_EEPROM_IO_TIMEOUT_MS) == HAL_OK;
}

/* 1=success, 2=write command failed, 3=device never became ready. */
static uint8_t eeprom_write_page(uint16_t offset, const uint8_t *source,
                                 uint16_t length)
{
  if (length == 0U || length > V3_EEPROM_PAGE_SIZE ||
      ((offset & (V3_EEPROM_PAGE_SIZE - 1U)) + length) > V3_EEPROM_PAGE_SIZE ||
      (uint32_t)offset + length > V3_EEPROM_SIZE) return 2U;
  if (HAL_I2C_Mem_Write(&hi2c1, eeprom_device_address(offset),
      offset & 0xFFU, I2C_MEMADD_SIZE_8BIT, (uint8_t *)source, length,
      V3_EEPROM_IO_TIMEOUT_MS) != HAL_OK) return 2U;
  safety_watchdog_refresh();
  HAL_Delay(V3_EEPROM_WRITE_CYCLE_MS);
  safety_watchdog_refresh();
  return eeprom_ready(offset) ? 1U : 3U;
}

static uint32_t crc32(const uint8_t *data, uint32_t length)
{
  uint32_t crc = 0xFFFFFFFFUL;
  while (length--) {
    crc ^= *data++;
    for (uint8_t bit = 0; bit < 8; bit++)
      crc = (crc & 1U) ? ((crc >> 1) ^ 0xEDB88320UL) : (crc >> 1);
  }
  return ~crc;
}

static uint8_t record_valid(const v3_settings_record_t *record)
{
  if (record->magic != V3_MAGIC || record->version != V3_FORMAT_VERSION ||
      record->size != sizeof(v3_settings_record_t)) return 0;
  return record->crc32 == crc32((const uint8_t *)record,
      sizeof(v3_settings_record_t) - sizeof(record->crc32));
}

static void set_defaults(void)
{
  memset(&settings, 0, sizeof(settings));
  settings.magic = V3_MAGIC;
  settings.version = V3_FORMAT_VERSION;
  settings.size = sizeof(settings);
  for (uint8_t profile = 0; profile < V3_PROFILE_COUNT; profile++) {
    settings.profile[profile].exp_cc[0] = 11;
    settings.profile[profile].exp_cc[1] = 4;
    for (uint8_t key = 0; key < V3_SWITCHES_PER_PROFILE; key++) {
      settings.profile[profile].key[key].mode = V3_KEY_CC;
      settings.profile[profile].key[key].number = key;
      settings.profile[profile].key[key].toggle = 0;
    }
  }
}

static uint8_t sanitize_settings(void)
{
  uint8_t changed = 0U;
  for (uint8_t profile = 0; profile < V3_PROFILE_COUNT; profile++) {
    v3_profile_settings_t *p = &settings.profile[profile];
    for (uint8_t exp = 0; exp < 2U; exp++) {
      if (p->exp_cc[exp] > 127U) { p->exp_cc[exp] = 0U; changed = 1U; }
    }
    for (uint8_t key = 0; key < V3_SWITCHES_PER_PROFILE; key++) {
      if (p->key[key].mode > V3_KEY_MIDI_CLOCK) {
        p->key[key].mode = V3_KEY_CC; changed = 1U;
      }
      if (p->key[key].number > 127U) {
        p->key[key].number = 0U; changed = 1U;
      }
      if (p->key[key].toggle > 1U) {
        p->key[key].toggle = 0U; changed = 1U;
      }
    }
  }
  return changed;
}

static uint8_t write_slot(uint16_t address)
{
  v3_settings_record_t verify;
  static const uint32_t invalid_magic = 0U;
  uint8_t result;
  save_error = 0U;
  settings.crc32 = crc32((const uint8_t *)&settings,
      sizeof(settings) - sizeof(settings.crc32));

  /* Invalidate the destination first, write its body, then commit the first
   * page containing magic/version/generation last.  A power loss therefore
   * leaves either the previous slot or the completed new slot valid. */
  result = eeprom_write_page(address, (const uint8_t *)&invalid_magic,
      sizeof(invalid_magic));
  if (result != 1U) { save_error = result == 2U ? 1U : 2U; return 0U; }

  const uint8_t *raw = (const uint8_t *)&settings;
  for (uint16_t offset = V3_EEPROM_PAGE_SIZE;
       offset < sizeof(settings); offset += V3_EEPROM_PAGE_SIZE) {
    result = eeprom_write_page(address + offset, raw + offset,
        V3_EEPROM_PAGE_SIZE);
    if (result != 1U) { save_error = result == 2U ? 3U : 4U; return 0U; }
  }
  result = eeprom_write_page(address, raw, V3_EEPROM_PAGE_SIZE);
  if (result != 1U) { save_error = result == 2U ? 5U : 6U; return 0U; }

  memset(&verify, 0, sizeof(verify));
  if (!eeprom_read(address, &verify, sizeof(verify))) {
    save_error = 7U;
    return 0U;
  }
  if (!record_valid(&verify)) {
    save_error = 8U;
    return 0U;
  }
  if (memcmp(&verify, &settings, sizeof(verify)) != 0) {
    save_error = 9U;
    return 0U;
  }
  return 1U;
}

uint8_t v3_settings_init(void)
{
  v3_settings_record_t slot0;
  v3_settings_record_t slot1;
  set_defaults();
  active_slot = 0U;
  settings_dirty = 0U;
  write_failures = 0U;
  persistence_enabled = 0U;
  save_status = V3_SAVE_OFFLINE;
  save_error = 0U;

  if (ssd1306_IsUpdateBusy() || HAL_I2C_GetState(&hi2c1) != HAL_I2C_STATE_READY ||
      !eeprom_ready(V3_EEPROM_SLOT0) ||
      !eeprom_read(V3_EEPROM_SLOT0, &slot0, sizeof(slot0)) ||
      !eeprom_read(V3_EEPROM_SLOT1, &slot1, sizeof(slot1))) return 0U;
  persistence_enabled = 1U;
  save_status = V3_SAVE_READY;

  uint8_t valid0 = record_valid(&slot0);
  uint8_t valid1 = record_valid(&slot1);
  if (valid0 && (!valid1 || (int32_t)(slot0.generation - slot1.generation) > 0)) {
    memcpy(&settings, &slot0, sizeof(settings)); active_slot = V3_EEPROM_SLOT0;
  } else if (valid1) {
    memcpy(&settings, &slot1, sizeof(settings)); active_slot = V3_EEPROM_SLOT1;
  } else {
    active_slot = V3_EEPROM_SLOT1; v3_settings_mark_dirty();
  }
  if (sanitize_settings()) v3_settings_mark_dirty();
  return 1U;
}

uint8_t v3_settings_persistence_enabled(void)
{
  return persistence_enabled;
}

v3_save_status_t v3_settings_save_status(void)
{
  return save_status;
}

uint8_t v3_settings_save_error(void)
{
  return save_error;
}

const v3_profile_settings_t *v3_settings_profile(uint8_t profile)
{
  return &settings.profile[(profile < V3_PROFILE_COUNT) ? profile : 0];
}

void v3_settings_mark_dirty(void)
{
  if (!persistence_enabled) return;
  settings_dirty = 1;
  save_status = V3_SAVE_PENDING;
  save_error = 0U;
  write_failures = 0U;
  write_after_tick = HAL_GetTick() + V3_SETTINGS_WRITE_DELAY_MS;
}

void v3_settings_change(uint8_t profile, uint8_t row, int8_t delta)
{
  if (profile >= V3_PROFILE_COUNT || row >= 32 || delta == 0) return;
  v3_profile_settings_t *p = &settings.profile[profile];
  if (row < 2) {
    p->exp_cc[row] = (uint8_t)((p->exp_cc[row] + 128 + delta) & 0x7F);
  } else {
    uint8_t key = (row - 2) / 3;
    uint8_t field = (row - 2) % 3;
    v3_key_setting_t *setting = &p->key[key];
    if (field == 0) setting->mode = (uint8_t)((setting->mode + 3 + delta) % 3);
    else if (field == 1 && setting->mode != V3_KEY_MIDI_CLOCK)
      setting->number = (uint8_t)((setting->number + 128 + delta) & 0x7F);
    else if (field == 2) setting->toggle ^= 1U;
  }
  v3_settings_mark_dirty();
}

void v3_settings_task(void)
{
  if (!persistence_enabled || !settings_dirty ||
      (int32_t)(HAL_GetTick() - write_after_tick) < 0) return;

  /* EEPROM writes do not stall CPU instruction fetch.  Do not wait for MIDI
   * silence: an external clock may run continuously while settings must still
   * auto-save.  Only serialize access to the shared OLED/EEPROM I2C bus. */
  if (ssd1306_IsUpdateBusy() || HAL_I2C_GetState(&hi2c1) != HAL_I2C_STATE_READY) {
    write_after_tick = HAL_GetTick() + V3_SETTINGS_I2C_RETRY_MS;
    return;
  }

  settings.generation++;
  uint16_t next_slot = active_slot == V3_EEPROM_SLOT0 ?
      V3_EEPROM_SLOT1 : V3_EEPROM_SLOT0;
  safety_watchdog_refresh();
  if (write_slot(next_slot)) {
    active_slot = next_slot;
    settings_dirty = 0;
    write_failures = 0U;
    save_status = V3_SAVE_SAVED;
  } else if (++write_failures >= V3_MAX_WRITE_RETRIES) {
    /* Never silently discard an unsaved edit.  Back off after the prompt
     * attempts, retain dirty state, and try again later. */
    write_failures = 0U;
    save_status = V3_SAVE_ERROR;
    write_after_tick = HAL_GetTick() + V3_SETTINGS_LONG_RETRY_DELAY_MS;
  } else {
    save_status = V3_SAVE_ERROR;
    write_after_tick = HAL_GetTick() + V3_SETTINGS_RETRY_DELAY_MS;
  }
  safety_watchdog_refresh();
}

void flash_settings_erase(void) { }
void flash_settings_write(uint8_t *data, uint32_t offset) { (void)data; (void)offset; }
