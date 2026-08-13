# uLi MIDI Mod v3.0.0-stable 檢查紀錄

日期：2026-08-14

## 發布成品

- DFU：`artifacts/releases/uli-midi-mod-v3.0.0-stable.dfu`
- DFU SHA-256：`8c96003dde3e9450509ecadd490e6e2f33110431afcd8f5f78ac186727a3045d`
- payload：46,888 bytes，位址 `0x08003000–0x0800E727`
- payload SHA-256：`4de8903c475fdcecf0252e4269ed36b902deeda37a73b1b327f070f0b46256b9`
- 初始 SP：`0x20010000`
- reset vector：`0x08003225`
- 內部 Flash guard 起點：`0x0807F000`
- 原廠完整救援備份 SHA-256：`c296af705132ce3997c896552281c66ac522df7a158ee198a0d6a1283a9d755f`

穩定版 DFU、`platformio-latest.dfu` 與 2026-08-14 已刷入後讀回成功的版本三者相同。

## 檢查結果

| 區域 | 結果 | 證據邊界 |
|---|---|---|
| DFU／bootloader | 通過 | DFU 單一 element 從 `0x08003000` 開始；不包含 `0x08000000–0x08002FFF` bootloader |
| Flash 容量 | 通過 | DFU 結束於 `0x0800E728`，遠低於 `0x0807F000` guard |
| 救援映像 | 通過 | 512 KB 原廠 dump 長度、向量與固定 SHA-256 均通過 |
| 雙環境建置 | 通過 | `midi_debug` 與 `midi_dfu` 均成功；RAM 5,932 bytes，Flash 約 46 KB |
| 自動測試 | 通過 | 26 項測試全數通過 |
| CUS1／CUS2 | 通過 | 雙 profile 共存、300 ms 自動保存、讀回驗證與錯誤重試；已實機確認保存 |
| 按鍵編號 | 通過 | `1 2 3 4 5 / 6 7 8 9 0`，UI 最後一鍵顯示 0，不顯示 10 |
| TAG | 通過 | TAG OFF 按下送 127、不在放開送 0；TAG ON 按下交替 127／0 |
| USB MIDI | 通過 | 單一 class-compliant MIDI；裝置控制輸入已實機確認 |
| DIN／realtime | 通過 | USB→DIN Clock、Start、Continue、Stop 均有解析與轉送；傳送路徑無無限 busy wait |
| USB 拔除 | 通過 | suspend／disconnect 清除 USB busy，但不 suspend 主應用；電池模式已實機運作 |
| EEPROM | 通過 | 24C08 `0x080`／`0x100` 雙槽、CRC、generation、先失效後提交、寫後讀回；不呼叫 HAL Flash 擦寫 |
| UI／電量 | 通過 | USB/BAT 顯示切換、電壓與百分比版型已實機確認 |
| 充電進入／進度 | 通過 | 實機影片確認 USB 穩定檢查後進入 CHARGING，電壓與進度穩定更新 |
| 充電安全關閉 | 原始碼與逆向通過 | 完成、USB 掉電、ADC 錯誤、安全拒絕、12 小時逾時均先將 PC10 設為 off |
| watchdog／crash telemetry | 通過 | IWDG 啟動並在主迴圈與長啟動流程刷新；fault 資料只寫 `.noinit` SRAM |

## 審查發現及處理

1. 舊 README 混有 8 banks、CSV raw Flash、HID 與睡眠模式等不適用於 V3 的說明，已重寫，避免錯誤操作。
2. 安全上傳工具原本只檢查 reset vector 是否落在整顆 512 KB Flash；現在加強為必須落在本次 payload 內，避免接受截斷或不完整映像。
3. 原本把 MCU 尾端 4 KB 稱為 settings pages，但 V3 設定實際在外部 EEPROM；已改名為 internal Flash guard，避免維護誤判。
4. 額外 `-Wall -Wextra -Wformat=2 -Wundef -Wshadow` 掃描 25 個專案 translation units，只有既有 signed/unsigned 比較及未使用參數警告，沒有語法或連結錯誤。為維持已實機驗證 binary，本次不做無功能價值的韌體改寫。

## 尚未完成的實機驗證

- 尚未執行一次從低電量到充滿、由 PC0 判斷完成並自動停止的長時間完整充電。
- 尚未做 12 小時以上連續運轉 soak test。
- 尚未以邏輯分析儀量測 USB、DIN、expression 與 EEPROM 同時高負載時的最壞延遲。
- watchdog fault injection 與 crash telemetry 顯示路徑為程式／建置檢查，尚未刻意在實機製造 fault。

因此 `stable` 表示目前需求、已知 bug、建置與已執行實機流程已形成固定可回復版本，不表示所有硬體老化、電池品牌或極端時序都已被證明不可能出錯。第一次完整充電仍應有人在場觀察。

## 重現檢查

```bash
python3 -m unittest discover -s python -p 'test_*.py' -v
python3 -m platformio run -e midi_debug
python3 -m platformio run -e midi_dfu
(cd artifacts/releases && shasum -a 256 -c SHA256SUMS)
```
