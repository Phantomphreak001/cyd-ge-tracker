# CYD GE Tracker

A mini **Old School RuneScape Grand Exchange tracker** for the ESP32 "Cheap Yellow Display" (CYD). It's a pocket-sized companion to the desktop app [osrs-ge-tracker](https://github.com/Phantomphreak001/osrs-ge-tracker): plug it in, join it to your WiFi, and it shows live GE prices and flip ideas on its 2.8" screen.

## Features

- **High-roller ticker.** The top bar scrolls through the 12 items with the biggest after-tax margins, showing price, margin and % change against the 1-hour average (green up, red down).
- **Top 5 flips for new players.** Items are ranked by trades in the last hour. To make the list an item needs:
  - an after-tax margin above zero
  - ROI of at least 1%
  - a buy price of 2M or less
  - at least 20 trades in the last hour

  Each row shows buy, sell, margin and volume, with ROI and buy limit underneath.
- **Accurate GE tax.** The tax is 2%, rounded down (so items under 50gp pay nothing) and capped at 5M per item. Items on the wiki's [tax-exempt list](https://oldschool.runescape.wiki/w/Grand_Exchange#Exempt_from_tax), such as Old school bonds, pay no tax.
- **60-second refresh.** Prices update every minute, and tapping the screen refreshes straight away.
- **No pause during refresh.** Downloads run on one ESP32 core and the screen on the other, so the ticker keeps scrolling while new prices load.
- **Phone WiFi setup.** WiFi is entered from your phone through a setup page. No passwords are stored in the code.
- **Dark theme, status bar.** The status bar shows WiFi signal, last update time, and the failing step and HTTP code if a download fails.

## Hardware

- **ESP32-2432S028R** "Cheap Yellow Display": an ESP32-WROOM with a 2.8" 320×240 ILI9341 screen and XPT2046 touch.
- **USB cable:** micro-USB, or USB-C on the CYD2USB variant.

## Quick flash (no coding needed)

1. Download `cyd-ge-tracker-v1.0.0-merged.bin` from the [latest release](https://github.com/Phantomphreak001/cyd-ge-tracker/releases/latest).
2. Install Python, then run: `pip install esptool`
3. Plug in the CYD and find its port.
   - **Windows:** Device Manager → Ports, listed as "USB-SERIAL CH340 (COMx)".
   - **Mac/Linux:** `/dev/ttyUSB0` or `/dev/cu.usbserial-*`.
4. Flash it:
   ```
   esptool --port COMx write_flash 0 cyd-ge-tracker-v1.0.0-merged.bin
   ```
   Older esptool versions use `esptool.py` instead of `esptool`. If it won't connect, hold the **BOOT** button while it says "Connecting...".

## WiFi setup

1. After flashing, the screen shows **WiFi setup** instructions.
2. On your phone, join the WiFi network **`GE-Tracker-Setup`**.
3. A setup page should open by itself. If not, browse to **http://192.168.4.1**.
4. Tap **Configure WiFi**, pick your network, enter the password and save.
5. The tracker connects and loads prices.

The board saves your WiFi. To change networks, **hold your finger on the screen while powering on** and the setup page comes back.

## Build from source

You need [PlatformIO](https://platformio.org/) (`pip install platformio`) or VS Code with the PlatformIO extension.

```
git clone https://github.com/Phantomphreak001/cyd-ge-tracker
cd cyd-ge-tracker
pio run -t upload --upload-port COMx
pio device monitor -b 115200
```

All libraries download automatically: TFT_eSPI, ArduinoJson 7, WiFiManager and XPT2046_Touchscreen.

## Customizing

**`include/items.h`** holds the baked-in item list (`{id, name, buy limit, taxFree}`). The full item mapping is several MB, too big to download on the device, so the list is built into the firmware. Look up item IDs in the [mapping endpoint](https://prices.runescape.wiki/api/v1/osrs/mapping).

**Constants at the top of `src/main.cpp`:**

| Constant | Default | Meaning |
|---|---|---|
| `REFRESH_MS` | 60000 | Refresh interval (ms) |
| `VOL_FLOOR` | 20 | Minimum trades in the last hour for a flip |
| `ROI_FLOOR` | 1.0 | Minimum ROI % for a flip |
| `MAX_BUY` | 2000000 | Highest buy price for a flip (gp) |
| `HIGH_ROLLERS` | 12 | Number of items in the ticker |
| `TICKER_STEP_PX` / `TICKER_FRAME_MS` | 1 / 25 | Ticker speed (40 px/s) |

## Display troubleshooting

The screen is set up in `platformio.ini` with these pins: MISO 12, MOSI 13, SCLK 14, CS 15, DC 2, backlight 21.

- **Colours inverted** (black shows as white): add `-DTFT_INVERSION_ON` to `build_flags`.
- **Garbage or blank screen:** replace `-DILI9341_2_DRIVER=1` with `-DILI9341_DRIVER=1`. On the CYD2USB variant, use `-DST7789_DRIVER=1` together with `-DTFT_INVERSION_ON`.
- **Backlight off:** check that `TFT_BL=21` is set.

## How it works

- Prices come from `/latest`. Buy and sell volume and the 1-hour average come from `/1h`.
- Both downloads are read piece by piece and only the baked-in items are kept, so the full multi-MB response never has to fit in memory.
- The Wiki rejects old-style HTTP/1.0 requests, so the firmware uses HTTP/1.1 and unpacks the chunked response itself.

## Credits

Price data comes from the [OSRS Wiki Real-time Prices API](https://prices.runescape.wiki/), with thanks to the OSRS Wiki and RuneLite. Requests send the User-Agent `cyd-ge-ticker - Phantomphreak001 GitHub`.

This project is not affiliated with or endorsed by Jagex Ltd. Old School RuneScape is a trademark of Jagex Ltd.

## License

[MIT](LICENSE) © 2026 Phantomphreak001
