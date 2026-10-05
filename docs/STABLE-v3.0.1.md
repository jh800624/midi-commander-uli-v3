# uLi MIDI Mod v3.0.1-stable 檢查紀錄

日期：2026-08-14

## 修正內容

V3.0.0 錯把充電時的高端電壓範圍 `2.75–3.10V` 用於電池放電百分比，因此外部充電器剛充滿、裝機帶載讀到 `2.57V` 時仍顯示 0%。V3.0.1 只修正正常電池模式的百分比換算，不修改 ADC 電壓量測、充電控制、USB、MIDI、設定保存或 UI 配置。

- `2.55V` 以上：100%
- `2.00V` 以下：0%
- 中間範圍：線性估計並四捨五入

這是兩顆 NiMH AAA 在機器帶載時的電壓估計，不是電池容量計；NiMH 放電曲線平坦，百分比只能作相對參考，畫面上的實際電壓更具判斷價值。

## 發布成品

- DFU：`artifacts/releases/uli-midi-mod-v3.0.1-stable.dfu`
- DFU SHA-256：`f1eb0bc1261bca12a183bbf1994ab5c135c1434d685fc99785dd6ab4b4b96a92`
- payload：46,884 bytes，位址 `0x08003000–0x0800E723`
- payload SHA-256：`169157eefe9164c86e0bf296b2a70d426e2634a2516e8ebdda04905d5e6ee6b9`
- 初始 SP：`0x20010000`
- reset vector：`0x08003225`
- 內部 Flash guard 起點：`0x0807F000`

## 驗證

- 26 項自動測試全部通過。
- `midi_debug` 與 `midi_dfu` 雙環境建置成功。
- 原廠 512 KB 救援備份、向量、DFU 單一 element、payload、DfuSe CRC 與 Flash guard 均通過安全檢查。
- 安全上傳成功，寫入起點為 `0x08003000`，未包含 bootloader `0x08000000–0x08002FFF`。
- 實機拔除 USB、使用外部充滿的兩顆 NiMH AAA 開機後，顯示 `2.61V / 100%`，修正目標通過。

除上述百分比換算外，完整 V3 安全檢查與尚未完成的長時間充電／soak test 邊界沿用 [STABLE-v3.0.0.md](STABLE-v3.0.0.md)。

## 重現檢查

```bash
python3 -m unittest discover -s python -p 'test_*.py' -v
python3 -m platformio run -e midi_debug
python3 -m platformio run -e midi_dfu
(cd artifacts/releases && shasum -a 256 -c SHA256SUMS)
```
