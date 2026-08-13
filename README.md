# uLi MIDI Mod v3

這是 MeloAudio MIDI Commander 的 V3 穩定版韌體。V3 以未修改的上游 custom firmware commit `c03e265` 為基準，只保留 CUS1、CUS2、十顆可設定按鍵，以及本專案已確認需要的穩定性修正。

## 穩定版

- 版本：`v3.0.0-stable`
- 開機顯示：`uLi MIDI Mod v3`
- DFU：[uli-midi-mod-v3.0.0-stable.dfu](artifacts/releases/uli-midi-mod-v3.0.0-stable.dfu)
- DFU SHA-256：`8c96003dde3e9450509ecadd490e6e2f33110431afcd8f5f78ac186727a3045d`
- 韌體 payload SHA-256：`4de8903c475fdcecf0252e4269ed36b902deeda37a73b1b327f070f0b46256b9`
- 完整檢查紀錄：[STABLE-v3.0.0.md](docs/STABLE-v3.0.0.md)

這個 DFU 與 2026-08-14 已刷入機器、並由 Flash 讀回比對成功的 payload 相同。DFU 只包含從 `0x08003000` 開始的應用程式，不覆蓋原廠 bootloader。

## 操作

- 正常開機：CUS1
- 按住 `9` 開機：CUS2
- 按住 `0` 開機：設定模式
- 按住 `5` 開機：充電模式

實體按鍵固定為：

```text
1 2 3 4 5
6 7 8 9 0
```

設定模式：`2` 選 CUS1、`3` 選 CUS2、`5` 上移、`0` 下移、`7` 減、`8` 加。修改後約 300 ms 自動保存；畫面顯示 `WAIT`、`SAVED` 或 `E碼`。

每顆鍵可設定 CC、PC 或 MIDI Clock。TAG 關閉時，每次按下只送 CC 127，放開不送 0；TAG 開啟時，每次按下交替送 127／0。

## 主要功能

- CUS1、CUS2 各有十顆可設定按鍵。
- USB MIDI 與 DIN MIDI 輸出。
- USB Host 傳入的 MIDI realtime Clock、Start、Continue、Stop 轉送到 DIN MIDI。
- 兩組 expression pedal 輸入。
- 正常 UI 顯示 CUS、按鍵、MIDI 訊息與 expression meter。
- USB 連線時左上只顯示 `USB`；拔除後切回小型 `BAT`、電量百分比與電壓。
- USB 使用後拔線，電池／DIN 模式繼續運作，不會因 USB suspend 讓整台停住。
- watchdog 與 warm-reset crash telemetry；fault handler 不擦寫 Flash。

## 設定保存

V3 不把設定寫進 MCU 程式 Flash，也不使用舊版 CSV／GUI raw Flash 寫入流程。

- 設定保存在原廠 24C08 外部 EEPROM。
- 保留原廠 `0x000–0x07F`，V3 使用 `0x080` 與 `0x100` 雙槽。
- CUS1、CUS2 一起保存，含 generation、CRC32、掉電安全提交及寫後讀回。
- 保存失敗時不丟棄尚未保存的變更，會退避後重試。

請不要使用舊版 `CSV_to_Flash.py` 或 GUI 對 V3 寫入設定。

## 充電模式

充電模式沿用由原廠完整韌體交叉還原的腳位與判斷：PC10 active-low open-drain 控制、PC0 充電狀態、PA1 USB sense、PC5 電池 sense。安全路徑包含 USB 電壓門檻、低電池拒絕、不可充電電池判斷及 12 小時硬逾時；任何完成或錯誤狀態都先關閉 PC10。

目前已實機確認能進入充電、顯示穩定電壓與持續進度；「實際充滿後由 PC0 自動停止並顯示完成」尚未完成一次從低電量到滿電的長時間實機驗證。第一次完整充電仍應有人在場觀察，勿把未驗證項目當成保證。

## 安全燒錄與救援

建議只使用本頁列出的穩定版 DFU。安全上傳腳本會檢查：

- 原廠 512 KB 救援備份的固定 SHA-256。
- stack pointer 與 reset vector 必須落在合法範圍。
- DFU 只能有一個 target／element，起點必須是 `0x08003000`。
- payload、DfuSe CRC 與內部 Flash 保護邊界必須一致。
- 必須只找到一個 STM32 DFU alt 0 裝置。

建置及安全上傳：

```bash
python3 -m platformio run -e midi_dfu
python3 -m platformio run -e midi_dfu -t upload
```

DFU 模式仍由原廠 bootloader 提供，所以應用程式故障時仍可重新刷入。完整原廠 Flash 備份保存在 `backup/dumped_firmware.bin`，SHA-256 為 `c296af705132ce3997c896552281c66ac522df7a158ee198a0d6a1283a9d755f`。

## 開發驗證

```bash
python3 -m unittest discover -s python -p 'test_*.py' -v
python3 -m platformio run -e midi_debug
python3 -m platformio run -e midi_dfu
```

詳細功能邊界見 [V3 scope](docs/v3-scope.md)，充電逆向證據見 [factory-charge-reverse.md](docs/factory-charge-reverse.md)。

## 來源

本專案延續 `midi-commander-custom` 的 STM32、USB MIDI、DIN MIDI、OLED 與 expression pedal 基礎。原廠 binary 只用於行為比對與救援，不把不可維護的 binary 當原始碼直接修改。
