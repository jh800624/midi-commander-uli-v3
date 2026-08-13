import importlib.util
import struct
import tempfile
import unittest
from pathlib import Path


PROJECT = Path(__file__).resolve().parents[1]


def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


safe_upload = load_module("safe_dfu_upload", PROJECT / "scripts" / "safe_dfu_upload.py")
dfuse = load_module("bin_to_dfuse", PROJECT / "tools" / "bin_to_dfuse.py")


class FirmwareSafetyTests(unittest.TestCase):
    def test_recovery_backup_is_full_flash_with_valid_vector(self):
        safe_upload.validate_backup(PROJECT / "backup" / "dumped_firmware.bin")

    def test_dfuse_round_trip_and_crc(self):
        payload = struct.pack("<II", 0x20010000, 0x08003009) + bytes(range(64))
        image = dfuse.generate_dfuse(
            payload,
            load_address=safe_upload.APP_START,
            alt_setting=0,
            target_name="ST...",
            vendor=0x0483,
            product=0xDF11,
            device=0,
            dfu_version=0x011A,
        )
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "test.dfu"
            path.write_bytes(image)
            safe_upload.validate_dfuse(path, payload)
            path.write_bytes(image[:-1] + bytes([image[-1] ^ 1]))
            with self.assertRaisesRegex(RuntimeError, "CRC"):
                safe_upload.validate_dfuse(path, payload)

    def test_reset_vector_must_point_inside_payload(self):
        payload = struct.pack("<II", 0x20010000, 0x08004001) + bytes(range(64))
        with self.assertRaisesRegex(RuntimeError, "reset vector"):
            safe_upload.validate_vector(payload, safe_upload.APP_START)

    def test_current_dfu_matches_current_binary(self):
        binary = PROJECT / ".pio" / "build" / "midi_dfu" / "firmware.bin"
        image = PROJECT / "artifacts" / "dfu" / "platformio-latest.dfu"
        if not binary.exists() or not image.exists():
            self.skipTest("build artifacts have not been generated")
        payload = binary.read_bytes()
        safe_upload.validate_vector(payload, safe_upload.APP_START)
        safe_upload.validate_dfuse(image, payload)

    def test_linker_regions_reserve_settings_pages(self):
        debug_linker = (PROJECT / "firmware" / "STM32F103RETX_FLASH.ld").read_text()
        dfu_linker = (PROJECT / "firmware" / "STM32F103RETX_FLASH_DFU.ld").read_text()
        self.assertIn("LENGTH = (512K - 4K)", debug_linker)
        self.assertIn("LENGTH = (512K - 0x3000 - 4K)", dfu_linker)

    def test_built_vector_bases(self):
        for environment, base in (("midi_debug", 0x08000000), ("midi_dfu", 0x08003000)):
            binary = PROJECT / ".pio" / "build" / environment / "firmware.bin"
            if not binary.exists():
                self.skipTest(f"{environment} has not been built")
            stack, reset = struct.unpack_from("<II", binary.read_bytes())
            self.assertEqual(stack, 0x20010000)
            self.assertGreaterEqual(reset & ~1, base)
            self.assertLess(reset & ~1, safe_upload.FLASH_GUARD_START)

    def test_no_unbounded_usb_or_oled_waits(self):
        usb = (PROJECT / "firmware" / "Middlewares" / "ST" /
               "STM32_USB_Device_Library" / "Class" / "MIDI" / "Src" /
               "usbd_midi.c").read_text()
        oled = (PROJECT / "firmware" / "Middlewares" /
                "stm32-ssd1306-master" / "ssd1306" / "ssd1306.c").read_text()
        self.assertNotIn("while(USB_Tx_State)", usb)
        self.assertNotIn("while(HAL_I2C_GetState", oled)

    def test_failed_din_dma_start_is_retried_by_main_loop(self):
        main = (PROJECT / "firmware" / "Core" / "Src" / "main.c").read_text()
        midi = (PROJECT / "firmware" / "Core" / "Src" / "midi_cmds.c").read_text()
        self.assertIn("midiCmd_task();", main)
        self.assertIn("void midiCmd_task(void)", midi)
        self.assertNotIn("while(HAL_UART_Transmit_DMA", midi)

    def test_usb_enumerates_as_midi_only_for_embedded_host_compatibility(self):
        usb = (PROJECT / "firmware" / "USB_DEVICE" / "App" /
               "usb_device.c").read_text()
        self.assertIn("USBD_RegisterClass(&hUsbDeviceFS, &USBD_MIDI)", usb)
        self.assertNotIn("USBD_COMPOSITE_MIDI_HID", usb)
        self.assertIn("USBD_MIDI_RegisterInterface", usb)

    def test_battery_monitor_is_read_only_and_uses_stock_measurement_path(self):
        battery = (PROJECT / "firmware" / "Core" / "Src" /
                   "battery.c").read_text()
        main = (PROJECT / "firmware" / "Core" / "Src" / "main.c").read_text()
        self.assertIn("ADC_CHANNEL_VREFINT", battery)
        self.assertIn("ADC_CHANNEL_15", battery)
        self.assertIn("120U * (uint32_t)sense", battery)
        self.assertIn("BATTERY_SAMPLE_INTERVAL_MS (1000U)", battery)
        self.assertIn("BATTERY_MIN_CV             (275U)", battery)
        self.assertIn("BATTERY_MAX_CV             (310U)", battery)
        self.assertIn("usb_device_is_connected()", battery)
        self.assertIn("battery_task();", main)
        self.assertIn("GPIO_PIN_1", main)
        self.assertNotIn("HAL_GPIO_WritePin", battery)
        self.assertNotIn("GPIO_PIN_10", battery)

    def test_charger_mode_is_factory_pinned_and_fails_off(self):
        diagnostic = (PROJECT / "firmware" / "Core" / "Src" /
                      "charge_diagnostic.c").read_text()
        main = (PROJECT / "firmware" / "Core" / "Src" / "main.c").read_text()
        self.assertIn("CHARGE_ENABLE_PIN  GPIO_PIN_10", diagnostic)
        self.assertIn("CHARGE_STATUS_PIN  GPIO_PIN_0", diagnostic)
        self.assertIn("ADC_CHANNEL_1", diagnostic)
        self.assertIn("ADC_CHANNEL_15", diagnostic)
        self.assertIn("CHARGE_USB_MIN_CV             (305U)", diagnostic)
        self.assertIn("CHARGE_BATTERY_SHUTDOWN_CV    (180U)", diagnostic)
        self.assertIn("CHARGE_NONRECHARGEABLE_CV     (298U)", diagnostic)
        self.assertIn("CHARGE_USB_SETTLE_MS          (3500U)", diagnostic)
        self.assertIn("CHARGE_DETECT_PULSE_MS        (500U)", diagnostic)
        self.assertIn("CHARGE_NONRECHARGEABLE_MS     (1201000UL)", diagnostic)
        self.assertIn("CHARGE_HARD_TIMEOUT_MS", diagnostic)
        self.assertIn("CHARGE_ADC_FILTER_SHIFT       (5U)", diagnostic)
        self.assertIn("CHARGE_ADC_WARMUP_SAMPLES     (64U)", diagnostic)
        self.assertIn("previous - (previous >> CHARGE_ADC_FILTER_SHIFT)",
                      diagnostic)
        self.assertIn("GPIO_MODE_OUTPUT_OD", main)
        self.assertIn(
            "HAL_GPIO_WritePin(GPIOC, GPIO_PIN_10, GPIO_PIN_SET);", main)
        self.assertIn("!HAL_GPIO_ReadPin(SW_5_GPIO_Port, SW_5_Pin)", main)
        pulse = diagnostic.split("charger_on();", 1)[1]
        pulse = pulse.split("charger_on();", 1)[0]
        self.assertIn("safe_delay(CHARGE_DETECT_PULSE_MS);", pulse)
        self.assertIn("charger_off();", pulse)
        self.assertLess(pulse.index("charger_off();"),
                        pulse.index("read_power"))
        terminal = diagnostic.split("static void terminal_off", 1)[1]
        terminal = terminal.split("void charge_mode_run", 1)[0]
        self.assertLess(terminal.index("charger_off();"),
                        terminal.index("show_message"))

    def test_normal_ui_uses_v2_preview_layout_without_fake_battery_percent(self):
        display = (PROJECT / "firmware" / "Core" / "Src" / "display.c").read_text()
        performance = display.split("static void draw_performance", 1)[1]
        performance = performance.split("void display_init", 1)[0]
        self.assertIn('"CUS-2" : "CUS-1"', performance)
        self.assertIn("Font_16x26", performance)
        self.assertIn("performance_exp[exp]", performance)
        self.assertIn('ssd1306_WriteString("USB", Font_6x8', performance)
        self.assertIn('ssd1306_WriteString("BAT", Font_6x8', performance)
        self.assertNotIn("Font_11x18", performance)
        self.assertNotIn("ssd1306_DrawRectangle(2, 12, 31, 21", performance)
        self.assertIn('"CC %03u %03u"', performance)
        self.assertIn("performance_battery_percent", performance)
        self.assertIn("performance_battery_centivolts", performance)
        self.assertNotIn('"BAT %', performance)

    def test_boot_screen_is_clean_single_line_v3_brand(self):
        header = (PROJECT / "firmware" / "Core" / "Inc" / "main.h").read_text()
        display = (PROJECT / "firmware" / "Core" / "Src" / "display.c").read_text()
        splash = display.split("void display_init", 1)[1]
        splash = splash.split("void display_setProfileName", 1)[0]
        self.assertIn('FIRMWARE_VERSION\t"uLi MIDI Mod v3"', header)
        self.assertIn("ssd1306_SetCursor(11, 27)", splash)
        self.assertEqual(splash.count("ssd1306_UpdateScreen();"), 1)
        self.assertNotIn('ssd1306_WriteString("#"', splash)
        self.assertNotIn('ssd1306_WriteString("ULI V3"', splash)

    def test_settings_use_factory_eeprom_free_area_without_midi_silence(self):
        settings = (PROJECT / "firmware" / "Core" / "Src" /
                    "flash_midi_settings.c").read_text()
        self.assertIn("V3_EEPROM_SLOT0          (0x0080U)", settings)
        self.assertIn("V3_EEPROM_SLOT1          (0x0100U)", settings)
        self.assertIn("v3 must not overwrite factory EEPROM settings", settings)
        self.assertIn("persistence_enabled = 0U", settings)
        self.assertIn("ssd1306_IsUpdateBusy", settings)
        self.assertIn("HAL_I2C_IsDeviceReady", settings)
        self.assertIn("HAL_I2C_Mem_Write", settings)
        self.assertIn("V3_EEPROM_WRITE_CYCLE_MS (10U)", settings)
        write_page = settings.split("static uint8_t eeprom_write_page", 1)[1]
        write_page = write_page.split("static uint32_t crc32", 1)[0]
        self.assertLess(write_page.index("HAL_I2C_Mem_Write"),
                        write_page.index("HAL_Delay(V3_EEPROM_WRITE_CYCLE_MS)"))
        self.assertLess(write_page.index("HAL_Delay(V3_EEPROM_WRITE_CYCLE_MS)"),
                        write_page.index("eeprom_ready(offset)"))
        self.assertIn("memcmp(&verify, &settings", settings)
        self.assertNotIn("HAL_FLASH_", settings)
        self.assertNotIn("midiCmd_transport_idle_for", settings)
        self.assertNotIn("FLASHSIZE_BASE", settings)
        self.assertNotIn("DBGMCU", settings)

    def test_realtime_pass_through_is_complete(self):
        interface = (PROJECT / "firmware" / "USB_DEVICE" / "App" /
                     "usbd_midi_if.c").read_text()
        for symbol in ("MIDI_REALTIME_CLOCK", "MIDI_REALTIME_START",
                       "MIDI_REALTIME_CONTINUE", "MIDI_REALTIME_STOP"):
            self.assertIn(symbol, interface)

    def test_crash_telemetry_uses_noinit_not_flash(self):
        safety = (PROJECT / "firmware" / "Core" / "Src" / "safety.c").read_text()
        interrupts = (PROJECT / "firmware" / "Core" / "Src" /
                      "stm32f1xx_it.c").read_text()
        self.assertIn('section(".noinit")', safety)
        self.assertIn("stacked_pc", safety)
        self.assertIn("safety_fault_capture", interrupts)
        self.assertNotIn("HAL_FLASH", safety)

    def test_watchdog_is_refreshed_around_long_startup_delays(self):
        main = (PROJECT / "firmware" / "Core" / "Src" / "main.c").read_text()
        startup = main.split("/* Infinite loop */", 1)[0]
        self.assertGreaterEqual(startup.count("safety_watchdog_refresh();"), 5)

    def test_usb_loss_keeps_standalone_application_running(self):
        usb = (PROJECT / "firmware" / "USB_DEVICE" / "Target" /
               "usbd_conf.c").read_text()
        suspend = usb.split("void HAL_PCD_SuspendCallback", 1)[1]
        suspend = suspend.split("void HAL_PCD_ResumeCallback", 1)[0]
        disconnect = usb.split("void HAL_PCD_DisconnectCallback", 1)[1]
        disconnect = disconnect.split("USBD_StatusTypeDef USBD_LL_Init", 1)[0]
        for callback in (suspend, disconnect):
            self.assertIn("USBD_MIDI_NotifyLinkDown", callback)
            self.assertIn("setIsSuspended(0)", callback)
            self.assertNotIn("setIsSuspended(1)", callback)
        self.assertNotIn("ssd1306_SetDisplayOn(0)", suspend)

    def test_settings_mode_reaches_prompt_two_profile_save_window(self):
        header = (PROJECT / "firmware" / "Core" / "Inc" /
                  "flash_midi_settings.h").read_text()
        main = (PROJECT / "firmware" / "Core" / "Src" / "main.c").read_text()
        settings = (PROJECT / "firmware" / "Core" / "Src" /
                    "flash_midi_settings.c").read_text()
        self.assertIn("V3_SETTINGS_WRITE_DELAY_MS (300U)", header)
        self.assertIn("V3_SETTINGS_I2C_RETRY_MS   (100U)", header)
        self.assertIn("V3_SETTINGS_RETRY_DELAY_MS (1500U)", header)
        self.assertIn("V3_SETTINGS_LONG_RETRY_DELAY_MS (30000U)", header)
        self.assertIn("!switch_router_is_settings_mode()", main)
        self.assertIn("v3_profile_settings_t profile[V3_PROFILE_COUNT]", settings)

    def test_unsaved_settings_are_not_silently_discarded(self):
        settings = (PROJECT / "firmware" / "Core" / "Src" /
                    "flash_midi_settings.c").read_text()
        retry_branch = settings.split("++write_failures >= V3_MAX_WRITE_RETRIES", 1)[1]
        retry_branch = retry_branch.split("} else {", 1)[0]
        self.assertIn("V3_SETTINGS_LONG_RETRY_DELAY_MS", retry_branch)
        self.assertNotIn("settings_dirty = 0", retry_branch)

    def test_physical_button_zero_is_not_displayed_as_ten(self):
        header = (PROJECT / "firmware" / "Core" / "Inc" /
                  "switch_router.h").read_text()
        router = (PROJECT / "firmware" / "Core" / "Src" /
                  "switch_router.c").read_text()
        self.assertIn("V3_SW_0", header)
        self.assertNotIn("V3_SW_10", header)
        self.assertIn("sw == V3_SW_0 ? 0U : sw + 1U", router)

    def test_tag_off_is_one_shot_cc_127_without_release_zero(self):
        router = (PROJECT / "firmware" / "Core" / "Src" /
                  "switch_router.c").read_text()
        self.assertIn("key->toggle ? (toggle_state ? 127U : 0U) : 127U", router)
        self.assertNotIn("handle_v3_key_up", router)

    def test_settings_screen_reports_save_result(self):
        display = (PROJECT / "firmware" / "Core" / "Src" /
                   "display.c").read_text()
        self.assertIn('"WAIT"', display)
        self.assertIn('"SAVED"', display)
        self.assertIn('"E%u"', display)

    def test_eeprom_save_failure_has_stage_code(self):
        settings = (PROJECT / "firmware" / "Core" / "Src" /
                    "flash_midi_settings.c").read_text()
        display = (PROJECT / "firmware" / "Core" / "Src" /
                   "display.c").read_text()
        for code in range(7, 10):
            self.assertIn(f"save_error = {code}U", settings)
        for pair in ("? 1U : 2U", "? 3U : 4U", "? 5U : 6U"):
            self.assertIn(pair, settings)
        self.assertIn('"E%u"', display)


if __name__ == "__main__":
    unittest.main()
