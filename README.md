# snowflake-lights

Replacement firmware for the Particle Snowflake (P2): drives its 36 onboard LEDs plus an external
WS2812B strip on the DATA pad, with eight patterns, colour tints, a power limit, cloud functions and
optional Home Assistant support. It does no audio and no voice detection, unlike the stock firmware
([particle-iot/snowflake](https://github.com/particle-iot/snowflake), used for the pinout).

## Hardware (from the Snowflake hw v1.0.1 schematic)

| What | Where |
|---|---|
| 36 onboard WS2812B | P2 S0/MOSI (`SPI`), through a 74AHC1G125 buffer |
| External strip data | `DATA`/`DATA1` pads, fed from the **last onboard LED's DO** via 200R, so the strip continues the chain: pixels 0-35 = flake, 36.. = strip |
| External strip 5V | `+5V_EX` pad, switched by **D4** (HIGH = on; 2N7002 -> IRLML6401) |
| Touch logo | D10 (AT42QT1010), active low |

## Patterns

`warm` (still warm white), `rainbow`, `candy` (red/white stripes), `twinkle` (white sparkles on blue),
`comet` (comets running down the strip, a new colour each lap), `fire`, `solid` (one colour) and `test`
(strip length check, below).

**Colours** depend on the pattern:

- **twinkle, comet and candy** take a colour as a tint: twinkle sparkles in it over a dim version of it,
  comets run in it, candy stripes become colour + white. Each pattern keeps its own tint. **White**
  resets a pattern to its own colours.
- **warm, fire and rainbow** are their colour: setting a colour switches to `solid` in that colour.

Pick the pattern first, then the colour.

## Controls

- **Touch logo**: tap for the next pattern; hold ~1 s for off/on.
- **Cloud functions** (`particle call <device> <function> <argument>`):

| Function | Argument |
|---|---|
| `mode` | a pattern name, `on` or `off` (or 0-7) |
| `brightness` | 0-255 (default 96) |
| `color` | `RRGGBB`: tints twinkle/comet/candy (returns 1; `FFFFFF` resets), otherwise switches to `solid` (returns 0) |
| `strip` | number of strip LEDs, 0-300 (default 100); `+N` / `-N` adjusts the current count |
| `order` | strip colour order: `grb` (WS2812B, default), `rgb`, `bgr`, `brg`, `rbg`, `gbr` |
| `balance` | star : strip brightness in %, e.g. `100/80`, `10:100`, or one number for the star (`80` = 80/100); on top of `brightness`. Returns star×1000 + strip, -1 if invalid |
| `maxma` | current budget in mA, 200-4000 (default 2000) |

- **Variables**: `mode`, `strip`, `brightness`, `milliamps` (estimated draw of the last frame).
- All settings persist in EEPROM (written 3 s after the last change).

## Setting up a strip

1. **Length**: `mode test` lights every 10th strip LED red, every 50th green and flashes the last ten
   blue (the rest, and the flake, dim white). If the flashing block isn't at the very end, fix `strip`.
2. **Colour order**: strips differ. Set `color FF0000`, then `00FF00`, then `0000FF`: the strip must
   show the same colour as the flake each time. If two colours are swapped, try the `order` that swaps
   them back (red and blue swapped: `bgr`; red and green: `rgb`).
3. **Balance**: the star is brighter than most strips at the same setting; `balance 30/100` or lower
   evens them out.

**Power**: frames are scaled so the estimate (20 mA per channel at full + 1 mA idle per LED) stays under
`maxma`. 136 LEDs at full white would be ~8 A; USB-C without PD gives 3 A at most, so keep `maxma`
≤ 2500 on USB or a power bank. A long strip also wants power injected at its far end from a 5 V supply.

## Build and flash

Device OS 5.6.0 or later, P2 platform; uses the `neopixel` library (in `project.properties`).

```sh
particle compile p2 . --target 5.6.0 --saveTo snowflake-lights.bin
particle flash <device> snowflake-lights.bin
```

If the Snowflake is in a product with released firmware, mark it as a development device first, or the
product firmware will replace this app.

## Home Assistant (optional)

With the [particle-ha](https://github.com/No1089/particle-ha) integration the Snowflake shows up in Home
Assistant as a light (on/off, brightness, colour, patterns as effects) plus a *Pattern* select, which
Home Assistant's HomeKit bridge turns into one switch per pattern. The firmware publishes its state as
`ha/light` on every change (touch logo included), every 15 minutes, and when Home Assistant publishes
`hass-sync`; Home Assistant drives it through the `mode`, `brightness` and `color` functions. Nothing to
configure: add the device in the integration.
