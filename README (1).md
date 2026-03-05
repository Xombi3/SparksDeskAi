# 👁️ SPARKS — Robot Eyes for the Cheap Yellow Display

An animated robot face for the **ESP32-2432S028R** (Cheap Yellow Display). Two expressive eyes with a full personality system — autonomous moods, 15 idle animations, touch interactions, floating hearts and sparks, sleep ZZZs, a clock screen, and persistent pet memory. Zero flicker, all running on a single sprite push per frame.

---

![SPARKS robot face](preview.jpg)
> _Drop a photo of your build here_

---

## Hardware

| Part | Detail |
|------|--------|
| Board | ESP32-2432S028R (Cheap Yellow Display) |
| Display | 2.8" ILI9341, 320×240, SPI (VSPI) |
| Touch | XPT2046 resistive, SPI (HSPI) |
| RGB LED | Onboard active-low, pins 4 / 16 / 17 |
| Backlight | Pin 21 PWM |

Tested specifically on: **AITRIP 2 Pack ESP32-2432S028R** (Amazon).

---

## Dependencies

Install both via **Arduino Library Manager**:

| Library | Author |
|---------|--------|
| `TFT_eSPI` | Bodmer |
| `XPT2046_Touchscreen` | Paul Stoffregen |

These come with the ESP32 Arduino core — no separate install:
`WiFi.h` · `time.h` · `EEPROM.h` · `SPI.h` · `math.h`

---

## TFT_eSPI Setup

You must configure `TFT_eSPI` for the CYD before compiling. In your Arduino libraries folder, open `TFT_eSPI/User_Setup.h` and set:

```cpp
#define ILI9341_DRIVER
#define TFT_MISO 12
#define TFT_MOSI 13
#define TFT_SCLK 14
#define TFT_CS   15
#define TFT_DC    2
#define TFT_RST  -1
#define TOUCH_CS  33   // not used by TFT_eSPI — handled separately
#define SPI_FREQUENCY  55000000
```

The touch controller runs on **HSPI** completely separately — they never conflict.

---

## Configuration

Open `ai_robot_cyd.ino` and update the lines at the top:

```cpp
#define WIFI_SSID  "your_network_name"
#define WIFI_PASS  "your_password"
#define TZ_OFFSET  (-6 * 3600)    // UTC offset in seconds — see table below
```

**Timezone offsets:**

| Zone | Offset |
|------|--------|
| PST (UTC-8) | `(-8 * 3600)` |
| MST (UTC-7) | `(-7 * 3600)` |
| CST (UTC-6) | `(-6 * 3600)` |
| EST (UTC-5) | `(-5 * 3600)` |
| UTC | `0` |
| CET (UTC+1) | `(1 * 3600)` |

WiFi is only used for NTP time sync. If it fails, the robot runs fully offline — all animations work, only time-of-day mood changes are lost.

---

## Robot Name

The robot is named **SPARKS** by default. The name is stored in EEPROM and shown on the clock screen. To change it:

```cpp
char robotName[EEPROM_NAME_LEN] = "SPARKS";
```

Edit, reflash once, and it persists through power cycles.

---

## Screens

**Swipe left or right** anywhere on the screen to switch pages.

### Eyes Screen
The main face. All animations and touch interactions live here.

### Clock Screen
- Large 12-hour time displayed in the current eye colour
- Full date — day, month, date, year
- Robot name centred below the date
- Pets today and total lifetime pet count
- Refreshes every second

---

## Expressions

Seven expressions with unique eyelid shapes, eye colours, and RGB LED colours. Expression transitions use staggered per-lid lerping — inner corners move first, outer corners follow, giving a natural anatomical feel. A 90ms neutral pause fires between extreme expression changes.

| Expression | Eye Colour | LED | When |
|-----------|-----------|-----|------|
| NORMAL | Cyan | Teal | Default weekday daytime |
| HAPPY | Warm yellow-white | Green | Weekends |
| SAD | Soft blue | Blue | Random |
| ANGRY | Red | Red | Before 9am |
| SURPRISED | Bright yellow | Amber | Random |
| SUSPICIOUS | Purple | Purple | Random |
| SLEEPY | Dim grey-cyan | Dim blue | After 10pm / before 6am |

**Mood memory** stores the last 3 expressions and the picker avoids reusing any of them. 40% of changes follow the time-of-day mood, 60% are random from the full pool. Expressions cycle every 5–12 seconds.

---

## Idle Animations

15 animations fire randomly during idle. Each expression has a weighted pool so animations feel contextually appropriate. Behaviour chains link related animations — yawn tends to be followed by sleepy droop, think by side-eye or smug, and so on.

| Animation | What it does | Mood bias |
|-----------|-------------|-----------|
| **Squint** | One eye slowly closes to 70%, holds, pops open | ANGRY |
| **Startled** | Both eyes snap wide open with a held beat | SURPRISED |
| **Sneeze** | 3 rapid scrunches with screen shake | — |
| **Dizzy** | Eyes orbit in opposite directions, spiral out then back | — |
| **Yawn** | Lids droop heavily, mouth shape opens, slow recovery | SLEEPY ×3 |
| **Think** | Eyes drift up-right, one squints — "hmm" look | NORMAL, SUSPICIOUS |
| **Wink** | One eye closes fully, other stays perfectly still | HAPPY ×2 |
| **Eye roll** | Eyes drift up, sweep sideways, return | SAD, SUSPICIOUS |
| **Smug** | Eyes drift sideways slowly, one squints, long hold | SUSPICIOUS |
| **Exasperated blink** | 3× slower than normal blink — deliberate "I can't" | SAD, ANGRY |
| **Side-eye** | Hard snap to screen edge, long suspicious hold, snap back | SUSPICIOUS |
| **Excited bounce** | Fast vertical sinusoidal oscillation | HAPPY ×3 |
| **Sleepy droop** | Three rounds of drooping and jolting awake | SLEEPY ×3 |
| **Startled blink** | Reflex snap-close in ~30ms, immediately open | ANGRY, SURPRISED |
| **Pet joy** | Touch-only — eyes swell big, bottom lids crinkle, bounces | Touch only |

**Behaviour chains:**
- Yawn → sleepy droop more likely
- Startled blink → startled
- Sleepy droop → yawn
- Think → side-eye or smug
- Startled → startled blink

---

## Touch Interactions

All interactions only fire on the eyes screen.

| Gesture | What happens |
|---------|-------------|
| **Tap** | 2–4 pink hearts float up from tap point. Face goes HAPPY. Pet joy animation plays. Pet counter saves to EEPROM. |
| **Double-tap** | Two taps within 350ms. Eyes snap wide, SURPRISED expression, 6 hearts burst out. |
| **Tap while ANGRY** | Red-orange zigzag sparks shoot upward instead of hearts. Eyes narrow further. Robot stays irritable. |
| **Hold (up to 1.5s)** | Eyes widen progressively as you hold — up to 25% bigger. At 800ms face shifts to SURPRISED anticipation. |
| **Hold then release (1.5s+)** | Relief reaction — randomly sneezes or does a startled jump, then settles back to NORMAL. |
| **Drag** | Eyes track your finger as it moves. Autonomous drift is damped while touching. |
| **Swipe left / right** | Switch between eyes screen and clock screen. |

---

## Particle Effects

### Hearts
Pink hearts spawn at the tap position and float upward at randomised speeds (55–110 px/s). Each one shrinks slightly as it rises and fades from vivid pink to nothing over ~1.5 seconds. Up to 6 simultaneous. Drawn directly on the TFT so they layer above the sprite.

### Angry Sparks
8 red-orange particles with sinusoidal zigzag horizontal movement and gravity. Colour fades red → orange → yellow as they arc. Each particle erases its own trail per frame.

### Sleep ZZZs
After SLEEPY has held continuously for 5 seconds, small Z characters drift up from the right eye every 1.5–3 seconds. They sway gently side to side, fade from soft blue-green to nothing over ~2.5 seconds. Stop the instant the expression changes.

---

## Fluency System

The entire movement system is designed to feel organic rather than mechanical.

**Ease-out cubic lerp** — all position movement decelerates into targets. Eyes glide, not slide.

**Momentum + drag** — eyes have velocity and mass. A spring force pulls toward the drift target, drag bleeds energy each frame. Drag coefficient varies by mood: ANGRY snaps fast, SLEEPY crawls.

**Overshoot + settle** — when the eye passes close to its target while still moving fast, it overshoots slightly then springs back.

**Micro-jitter** — ±0.5px positional noise refreshed every 75ms removes the "too smooth" quality of pure lerp.

**Attention variance** — after 8+ seconds of relative stillness, the next drift target is 1.5–1.6× larger than normal. Long focus then a sudden shift.

**Per-lid staggered lerp** — inner eyelid corners move faster than outer corners during expression changes. Looks anatomically real.

**Transition neutral pause** — 90ms hold at lid-neutral when crossing between extreme expressions.

**Asymmetric blink** — closing and opening speeds are separate per mood. SLEEPY blinks slowly both ways. HAPPY and ANGRY blink crisply.

**Lid overshoot on open** — eyes go fractionally wider than normal for ~200ms after a blink opens, then settle.

**Breath-linked blink rate** — slower breathing (SLEEPY) means rarer blinks. Faster breathing (ANGRY, SURPRISED) means more frequent ones.

**Eye breathing** — a continuous sine wave adds ±2px to eye height. Speed varies by mood.

---

## Time-of-Day Moods

Requires WiFi to be connected for NTP sync.

| Time | Mood |
|------|------|
| 10pm – 6am | SLEEPY |
| 6am – 9am | ANGRY |
| Saturday / Sunday | HAPPY |
| All other times | NORMAL |

---

## Pet Counter

Every tap-pet increments two counters saved in EEPROM:

- **petsToday** — resets automatically at midnight (checked via `tm_yday` on boot)
- **petsTotal** — lifetime count, never resets

Both survive power-off and display on the clock screen.

**EEPROM layout:**

| Address | Data | Size |
|---------|------|------|
| 0 | petsToday | int (4 bytes) |
| 4 | petsTotal | int (4 bytes) |
| 8 | lastDay (tm_yday) | int (4 bytes) |
| 12 | robotName | char[20] |

Total used: 32 bytes of the 64-byte EEPROM allocation.

---

## Wake Animation

On every boot, before entering the main loop:

1. Screen fades in from black over ~600ms while eyes stay fully shut
2. Three REM-style flickers — lids half-open and close like dreaming
3. Groggy half-open at ~45%, holds 500ms
4. Closes again — resisting waking up
5. Opens fully with a small lid overshoot that settles
6. RGB LED fades in to the correct expression colour over 800ms

---

## Rendering Architecture

A single `320×114` sprite covers the full eye region (~73KB RAM). Both eyes are drawn into the sprite every frame, then pushed to the display in one atomic `pushSprite()` call. The display never sees a partially-drawn frame — zero flicker guaranteed.

Particle effects (hearts, sparks, ZZZs) draw directly on the TFT outside the sprite band so they never interfere with the sprite renderer.

---

## Touch Wiring

The CYD's XPT2046 runs on **HSPI** — completely separate SPI bus from the display's VSPI.

| Signal | GPIO |
|--------|------|
| CLK | 25 |
| MISO | 39 |
| MOSI | 32 |
| CS | 33 |
| IRQ | 36 |

Touch is polled directly (`ts.touched()`) rather than using the IRQ pin, which is unreliable on some ESP32-2432S028R board revisions. A 40ms debounce prevents spurious reads.

---

## Serial Monitor

Baud rate: **115200**

Successful boot:
```
WiFi.... OK
Heap: 187432
```

WiFi failed (offline mode):
```
WiFi FAILED
Heap: 195120
```

---

## File Structure

```
ai_robot_cyd/
├── ai_robot_cyd.ino   — main sketch (~1440 lines)
└── README.md          — this file
```

You'll also need a configured `User_Setup.h` in your TFT_eSPI library folder as described above.

---

## Troubleshooting

**Blank screen on boot**
Free heap below ~90KB means `createSprite()` failed. Check Serial Monitor for the heap value. Try closing other apps, reducing WiFi connection timeout, or using a board with PSRAM.

**Touch not responding**
Confirm `XPT2046_Touchscreen` by Paul Stoffregen is installed — not a fork. Also confirm your board is specifically the ESP32-2432S028R. Other CYD variants use different touch pins.

**Wrong time on clock**
Check `TZ_OFFSET`. NTP sync takes a few seconds after WiFi connects, so the very first expression after boot might be off. Reconnect WiFi or wait for the next expression cycle.

**Name not showing on clock**
The name is written to EEPROM on first flash. If it shows garbage, the EEPROM address contains invalid data from a previous sketch. Reflash — it validates the first character is A–Z before trusting the stored value.

**Pet count stuck at 0**
`petsToday` resets by comparing `tm_yday`, which requires a valid NTP sync. Without WiFi, today's count won't reset at midnight but `petsTotal` increments correctly regardless.

**Eyes too fast / slow**
Drift speeds are mood-dependent. SLEEPY is intentionally very slow. ANGRY is intentionally snappy. For global speed changes, find the momentum spring constant in the loop:
```cpp
float targetVX = (dTX - driftX) * 2.5f;
```
Increase `2.5f` to make all movement faster, decrease for slower.

---

## Contributing

Pull requests welcome. Good areas to expand:

- LDR brightness control (CYD has a light sensor on pin 34)
- Additional expressions
- Custom wake sequence per time-of-day
- Bluetooth serial name change without reflashing

---

## License

MIT — do whatever you want, credit appreciated.

---

*Built for the Cheap Yellow Display community. If you build one, share a photo.*
