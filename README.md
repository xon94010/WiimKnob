# WiimKnob

![WiimKnob banner](docs/banner.webp)

Firmware for the [Waveshare ESP32-S3-Knob-Touch-LCD-1.8](https://www.waveshare.com/esp32-s3-knob-touch-lcd-1.8.htm)
that turns it into a small WiiM remote:

- Turn the knob to change volume, with a light haptic buzz on button presses (prev/play-pause/
  next) via the board's DRV2605L driver.
- Two swipeable screens: a full-screen "now playing" ambient view (just the album art, nothing
  else), and a controls screen with a smaller circular art inset, title/artist, a transparent
  pill holding the prev/play-pause/next controls, a plain-number volume badge, and a real
  battery-shaped gauge. Swipe left/right between them.
- A red ring around the edge of the controls screen shows the current volume, matching the
  knob; a thin red highlight marks the play/pause button. Everything else is neutral (dark
  buttons, white icons) so the volume/playback accent doesn't get lost in the noise.
- Tap the battery icon to flip it to a plain percentage (tap again to flip back); it pulses
  while charging is detected.
- Power-aware: the screen dims out after a short idle timeout (the knob still works while it's
  off, and LVGL itself stops redrawing to save CPU/SPI time), and the whole board deep-sleeps
  after a longer one, waking on a touch or knob turn.
- Handles accented/non-ASCII track and artist names correctly (see **Fonts** below) — the
  stock LVGL fonts only cover plain ASCII.

This is a first working version, not a finished product — see **Ideas for next steps** below.

## Screenshots

<table>
<tr>
<td><img src="docs/screenshot-controls.jpg" width="260" alt="Controls screen: album art, volume badge, battery icon, title/artist, and the prev/play-pause/next pill"></td>
<td><img src="docs/screenshot-angle.jpg" width="260" alt="Controls screen viewed at an angle, showing the physical knob and case"></td>
<td><img src="docs/screenshot-now-playing.jpg" width="260" alt="Now playing screen: full-screen album art with nothing else overlaid"></td>
</tr>
</table>

## Hardware background

Waveshare hasn't published a pinout or demo code for this board (its wiki page is a
placeholder as of writing). Everything in `include/Pins.h` and `src/display/PanelInit.h` comes
from a community reverse-engineering effort that measured the schematic and factory firmware
directly:
https://github.com/teetotum-rs/firmware/blob/main/docs/hardware/

Two things worth knowing going in:

1. **The board has two microcontrollers.** The ESP32-S3 (which this firmware runs on) owns the
   display, touch and one of the two physical encoders on the shaft. A second, classic ESP32
   owns the audio DAC, classic Bluetooth, and forwards knob turns to a *second* logical encoder.
   This project only uses the ESP32-S3 side — we don't need the board's own audio output since
   WiiM is doing the actual audio playback.
2. **The knob is not a quadrature encoder.** Each direction pulses its own GPIO low (one pulse
   per detent) rather than two lines in quadrature. `src/input/Knob.cpp` decodes it accordingly.

## WiiM API

Uses WiiM's local HTTP API (`https://<device-ip>/httpapi.asp?command=...`), documented at
https://cvdlinden.github.io/wiim-httpapi/ and in WiiM's own PDF. It's HTTPS with a self-signed
certificate and only reachable on your LAN, so the firmware skips certificate validation
(`WiFiClientSecure::setInsecure()`) rather than trying to pin a cert that isn't meant to be
verified against a public CA.

Commands used: `getPlayerStatus` (play state, volume), `getMetaInfo` (title/artist/album/cover
URL), `setPlayerCmd:vol:<0-100>`, `setPlayerCmd:onepause`, `setPlayerCmd:next`,
`setPlayerCmd:prev`.

All WiiM networking runs on its own FreeRTOS task (`src/wiim/WiimTask.cpp`), pinned away from
the core that drives the display/touch/knob loop — a button press just drops a message in a
queue and returns immediately, so a slow or stalled network call can't freeze the UI. That
task also owns the one long-lived `WiFiClientSecure`/`HTTPClient` pair a `WiimClient` keeps
open across calls (see the big comment in `WiimClient.h`): **both** objects have to be
persistent, not just the socket. `HTTPClient`'s destructor unconditionally closes its
connection, so a fresh `HTTPClient` per call — even one that reuses the same
`WiFiClientSecure` — silently undid the keep-alive every time; every request paid a full TLS
handshake (measured at 400-700ms on this CPU) instead of the ~15-45ms a genuinely reused
connection costs. That was the actual cause of the multi-second lag on volume/prev/next/pause
in early versions of this firmware, not the network or the WiiM itself.

## Power management

Two independent timeouts, both configurable in `Config.h`, both reset by any touch or knob
turn:

- **`SCREEN_TIMEOUT_S`** (default 30s): turns the backlight off. Everything else keeps running
  normally — WiFi stays connected, polling continues, and turning the knob still changes volume
  (and turns the backlight back on so you can see the new level). This tier only saves the
  backlight's own draw.
- **`DEEP_SLEEP_TIMEOUT_MIN`** (default 10 min): puts the whole board into deep sleep, which cuts
  WiFi and the CPU too — the largest lever for battery life. A touch or knob turn wakes it by
  pulsing one of three GPIOs the chip watches for while asleep (see `src/power/Power.cpp`); no
  extra wiring needed, since the knob and touch controller already pulse those pins low as part
  of normal operation. Waking from deep sleep is a full reboot, so it takes a few seconds to
  reconnect to Wi-Fi and re-fetch state — "fairly easy" but not instant.

Deep sleep intentionally does not try to preserve any state (in-flight volume changes, current
track, etc.) — the display and WiiM state just repopulate over the first second or two after
wake, the same as a normal boot.

## Haptics

The board has a DRV2605L LRA haptic driver on the touch controller's I2C bus (address `0x5A`),
enabled via GPIO38. Per the community research this project relies on for its pinout, the chip
answers on I2C regardless of that pin's state but its output stage stays off — and everything
feels like no motor is attached — until GPIO38 is driven high. `src/haptics/Haptics.cpp` enables
it, configures the driver for LRA playback (not the ERM default), and fires a short "click"
effect on every prev/play-pause/next tap. It deliberately skips the DRV2605L's calibration
registers (rated voltage, overdrive clamp, auto-cal) — the ROM-library default felt effect
works fine on this board without them, and getting those wrong is how you end up silently
over-driving the actuator.

## Fonts

The stock LVGL `lv_font_montserrat_16`/`_20` only include ASCII plus the icon symbols — verified
directly from their compiled-in glyph range tables, not assumed. Any accented character (é, ö,
ñ, etc., e.g. in a track or artist name) had no glyph to draw at all. `src/ui/fonts/` has two
regenerated replacements (`lv_font_montserrat_16_latin`/`_20_latin`, used for the title/artist
labels only) built from the same Montserrat-Medium.ttf + FontAwesome5 symbol set LVGL's own
build uses, with the main range extended to cover Latin-1 Supplement and Latin Extended-A
(`0x20-0x7F,0xA0-0x17F,0x2022`) — enough for French, German, Spanish, Nordic, Polish, Czech,
and most other Western/Central European text. The exact `lv_font_conv` command is in each
file's own header comment if you need to extend the range further (e.g. for Cyrillic or Greek).

## Battery gauge

The controls screen shows a real battery-shaped icon (not just bars) with a proportional,
color-coded fill, next to the plain-number volume badge. This board has no fuel-gauge chip and
no confirmed charger-status pin, so both pieces of this are estimates, not hard readings — see
the comments in `src/power/Battery.h`/`.cpp`:

- **Level**: read from `BATTERY_ADC_PIN` (GPIO1, a community-documented but Waveshare-unverified
  candidate for the battery sense pin) and converted through `BATTERY_DIVIDER_RATIO` and a
  generic LiPo voltage curve. Both the pin and the ratio need calibrating against a multimeter
  reading of your actual battery — the raw voltage is logged over serial every poll
  (`[battery] ...`) specifically so you can do that.
- **Charging**: there's no status pin, so this is inferred by watching for a sustained rising
  voltage trend across consecutive readings. It can occasionally misfire (e.g. right after a
  load spike recovers) — good enough for "does this look like it's charging", not a precise
  signal.

An earlier version of this firmware had a separate battery detail screen (percentage, time
estimate, raw voltage) — it's been folded into the single gauge on the controls screen instead.

## Setup

1. Copy `include/Config.h.example` to `include/Config.h` and fill in your Wi-Fi credentials and
   your WiiM's IP address (find it in the WiiM Home app under Device Info; a static DHCP
   reservation on your router means you won't need to update this later).
2. Install [PlatformIO](https://platformio.org/). **This needs Python 3.10 or newer** — the
   platform this project uses (see below) refuses to run under older Python. If your default
   `python3` is older (macOS ships 3.9 by default), install a newer one (e.g. `brew install
   python@3.11`) and create a venv for PlatformIO with it:

   ```bash
   python3.11 -m venv ~/.platformio-venv
   ~/.platformio-venv/bin/pip install platformio
   ```

   Use `~/.platformio-venv/bin/pio` in place of `pio` below (or add it to your `PATH`).
3. Build and upload:

   ```bash
   pio run -e esp32-s3-knob -t upload
   ```

   To flash: hold the **BOOT** button on the ESP32-S3R8 side, plug in via USB-C, then release.
   The Type-C port routes to either the ESP32-S3's native USB or the USB-UART bridge depending
   on cable orientation (per Waveshare's product page) — if upload doesn't find the port, try
   flipping the cable.

4. Open the serial monitor (`pio device monitor`) to watch it connect to Wi-Fi and start
   polling.

If the screen stays dark or PSRAM isn't detected at boot, check `platformio.ini`'s
`board_build.arduino.memory_type` / `board_build.psram_type` against what `esptool.py
flash_id` reports for your specific board revision — these straps are inferred from the
product page spec (16MB quad flash, 8MB octal PSRAM on the ESP32-S3R8 module) but Waveshare
could ship a revision with different wiring.

## Project layout

```
include/
  Pins.h            board pinout
  Config.h.example  Wi-Fi + WiiM settings template (copy to Config.h)
  lv_conf.h         LVGL configuration
src/
  main.cpp          setup/loop: Wi-Fi, polling, input wiring
  display/          QSPI bus + ST77916 panel bring-up, LVGL display driver glue
  haptics/          DRV2605L LRA haptic feedback
  input/            knob (pulse decoding) and touch (CST816D) drivers
  power/            screen-timeout + deep-sleep idle management, battery estimate
  wiim/             WiiM HTTP API client + album art JPEG fetch/decode
  ui/                the two-screen LVGL layout (ambient + controls) and gesture navigation
    fonts/            regenerated fonts with accented-character support, see Fonts above
```

## Ideas for next steps

- **Auto-discovery**: currently the WiiM's IP is hardcoded in `Config.h`. mDNS/SSDP discovery
  would remove that step.
- **Backlight dimming**: `display::setBacklight()` is on/off only; the backlight pin (GPIO47)
  is PWM-capable if you want to add a brightness setting or auto-dim.
- **Multiroom awareness**: the WiiM API exposes multiroom group info; the UI doesn't show or
  control it yet.
- **Charging detection is a voltage-trend guess** (see the Battery gauge section) because this
  board has no confirmed charger-status pin. If you find one by probing the charge IC directly,
  wiring it to a GPIO and reading it would be far more reliable than the current heuristic.
- **Wider Unicode coverage**: the regenerated fonts (see Fonts above) cover Western/Central
  European accents but not Cyrillic, Greek, or CJK — extend the `lv_font_conv` range if you
  need those.
