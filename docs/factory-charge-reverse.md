# Factory charging reverse-engineering evidence

Source image: `backup/dumped_firmware.bin`, loaded at `0x08000000` as Thumb-2.

- GPIO init `0x08003868`: PC0 input pull-up; PC10 open-drain output.
- Charge state machine `0x08003bb4`.
- ADC conversion: `120 * ADC_sense / ADC_VREFINT`; PA1/ADC1 is USB sense and PC5/ADC15 is battery sense.
- ADC samples are not used raw. Functions at `0x08003A10`, `0x08003A2A`, and `0x08003A78` apply the IIR `y = y - y/32 + x/32` independently to scan channels. V3 warms these filters for 64 samples with charging disabled before evaluating any threshold.
- USB gate: `3.05 V` at `0x08003c78`, repeated throughout the state machine; stable timer is `3,500 ms`.
- PC10 is written low at `0x08003cda` for charger detection/activation. No-USB and terminal states write it high, including `0x08003d9c`, `0x08004352`, and `0x080043a8`.
- PC0 low enters the charging state; PC0 high during charging enters completion.
- Factory progress function `0x08007740`: 0% at or below `2.35 V`, linear to 100% at `2.95 V`.
- Non-rechargeable-cell guard: while charging, reaching `2.98 V` before elapsed second counter `1,201` enters the terminal warning state. Its original strings are `Batteries may be` and `NOT chargeable !`.
- Normal low-power shutdown path `0x0800704c` uses battery below `1.80 V` together with USB below `3.05 V`; the `Battary too Low` string is not the charger-entry minimum.
- Nine 96x48 battery-progress bitmaps are stored from `0x080137dc` through `0x080149dc`; full-screen rechargeable-cell and USB guidance frames are stored from `0x08012068` through `0x08013068`.

V3 adds one condition not observed in the factory state machine: a 12-hour hard timeout. It can only turn charging off.
