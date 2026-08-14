# ULI V3 最小修改範圍

V3 以未修改的上游 custom firmware commit `c03e265` 為基準，保留既有 STM32、GPIO、ADC、USB、UART、OLED 與 DFU bootloader 架構，只更動下列產品需求與已確認安全問題。原廠完整 Flash/EEPROM dump 僅用於救援與行為比對。

## 操作模式

- 只保留 `CUS1` 與 `CUS2`。
- 十顆實體鍵為 `1 2 3 4 5 / 6 7 8 9 0`，正常使用時全部可設定。
- 正常開機進入 CUS1；按住 9 開機進入 CUS2；按住 0 開機進入設定。
- 設定模式按鍵：2 選 CUS1、3 選 CUS2、5 上移、0 下移、7 減、8 加。
- 每顆鍵一個動作：CC、PC 或 MIDI Clock。
- TAG 關閉：按下送 CC 127，放開不送 0。TAG 開啟：每次按下交替送 127/0。

## UI 邊界

- 正常模式以 V2 網頁測試 UI 為基底，顯示 `CUS-1`／`CUS-2`、最後踩下的實體鍵號、MIDI 訊息與兩支 Expression meter。
- 實體鍵號固定為 `1 2 3 4 5 / 6 7 8 9 0`；UI 不把最後一鍵寫成 10。
- 設定頁只在按住 0 開機時出現，採五列置中捲動表格，並保留 `WAIT`、`SAVED`、`E碼` 存檔狀態。
- 電池監控使用原廠逆向確認的 ADC1 CH15/VREFINT 量測路徑，每秒取一組樣本並以 EMA 穩定顯示。百分比只代表帶載電壓估計，依實機外部充電後的 2.57 V 校正：2.55 V 以上顯示 100%，2.00 V 以下顯示 0%。充電中的較高端電壓不拿來當放電百分比門檻。
- 原廠將 PC1 設成類比腳位，不能當成可靠的數位 VBUS 訊號。V3 依 USB MIDI 是否完成連線切換電源 UI：連線時左上只顯示小型 `USB`；斷線後顯示小型 `BAT`、百分比與電壓。
- 原廠充電腳位、狀態機、門檻與 UI 圖資已由完整 binary 交叉還原：PC10 是 active-low open-drain 充電開關（低電位充電，高電位關閉）、PC0 是上拉狀態輸入、PA1/ADC1 是 USB supply sense、PC5/ADC15 是電池 sense。按住實體 5 開機進入獨立充電模式；沿用原廠 USB 3.05 V、低電池 1.80 V、進度 2.35–2.95 V、前 1,201 秒達 2.98 V 判定不可充電等條件，並額外加入 12 小時硬逾時。任何完成、USB 中斷、ADC 錯誤或安全拒絕路徑都先將 PC10 釋放為高電位，再更新畫面。

## 設定保存

- 使用原廠 24C08 EEPROM，不擦寫 MCU 程式 Flash。
- 保留原廠 `0x000–0x07F`；V3 雙槽位於 `0x080` 與 `0x100`。
- CUS1、CUS2 一起保存，含 generation、CRC32、掉電安全提交與寫後讀回驗證。
- 每個 EEPROM page 寫入後預留 10 ms 寫入週期（24C08 規格上限 5 ms，再留相容餘裕），才做 ready 與讀回驗證。
- 最後一次修改 300 ms 後，只要 I²C 空閒就嘗試保存；持續進來的 MIDI Clock 不會阻擋存檔。失敗不丟棄 dirty 狀態，退避後繼續重試。
- 設定畫面顯示 `WAIT`、`SAVED` 或 `ERR`，讓實機可直接確認保存狀態。

## 已知 bug 修正

- USB 接著使用後拔線，電池/DIN 模式繼續運作，不把整個應用程式設為 suspended。
- USB MIDI 封包固定以 4 bytes 前進，SysEx 有長度邊界，避免 RAM 越界。
- USB 與 DIN 傳送改為有限狀態處理，不使用無限 busy wait。
- OLED DMA 更新不阻塞主迴圈；EEPROM 寫入只在 I2C idle 時開始。
- USB→DIN realtime pass-through 包含 Clock、Start、Continue、Stop。
- USB 枚舉為單一 class-compliant MIDI 裝置，不附加未使用的 HID 鍵盤介面，以維持效果器等 embedded USB Host 的原廠相容性。
- Watchdog 與 warm-reset crash telemetry 實作在 `.noinit` SRAM，不在 fault handler 擦寫 Flash。
- DFU image 固定從 `0x08003000` 開始，保留原廠 bootloader；安全 uploader驗證備份、向量、payload 與邊界。
