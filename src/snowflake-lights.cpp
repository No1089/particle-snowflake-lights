// snowflake-lights: Particle Snowflake (P2) + an external LED strip on the DATA pad.
//
// Hardware (from particle-iot/snowflake hw v1.0.1 schematic):
//   - 36 onboard WS2812B, data from P2 S0/MOSI (SPI) through a 74AHC1G125 buffer.
//   - The last onboard LED's DO goes through 200R to the DATA / DATA1 pads, so the
//     strip continues the same chain: pixels 0-35 = snowflake, 36.. = strip.
//   - D4 HIGH switches on +5V_EX (2N7002 -> IRLML6401) for the strip's power pad.
//   - Touch logo (AT42QT1010) on D10, active low.
//
// Controls:
//   - Tap the logo: next mode. Hold it ~1 s: lights off / on.
//   - Cloud functions: mode, brightness, strip, color, maxma, order (see README).
//   - Home Assistant: shows up as a light via the "particle" integration (ha/light).

#include "Particle.h"
#include "neopixel.h"

SYSTEM_THREAD(ENABLED);
SerialLogHandler logHandler(LOG_LEVEL_INFO);

// ---- hardware -----------------------------------------------------------------
static constexpr pin_t TOUCH_PIN = D10;
static constexpr pin_t STRIP_5V_EN = D4;
static constexpr uint16_t FLAKE_LEDS = 36;
static constexpr uint16_t MAX_STRIP_LEDS = 300;

// The snowflake: 6 arms of 6 LEDs, arm k = 6k..6k+5; 6k and 6k+5 sit on the
// inner ring.
static const uint8_t INNER_RING[] = {0, 5, 6, 11, 12, 17, 18, 23, 24, 29, 30, 35};

// ---- settings (EEPROM) --------------------------------------------------------
enum Mode : uint8_t {
    MODE_WARM,      // warm white, still
    MODE_RAINBOW,   // rainbow flowing along the strip, turning on the flake
    MODE_CANDY,     // red / white candy stripes marching along the strip
    MODE_TWINKLE,   // cool white + blue twinkles
    MODE_COMET,     // comets running down the strip, flake pulses as they leave
    MODE_FIRE,      // warm flicker
    MODE_SOLID,     // one colour (set with "color")
    MODE_TEST,      // strip count check: every 10th LED marked
    MODE_COUNT
};
static const char *MODE_NAMES[MODE_COUNT] = {
    "warm", "rainbow", "candy", "twinkle", "comet", "fire", "solid", "test"};

struct Settings {
    uint32_t magic;
    uint8_t mode;
    uint8_t brightness;  // 0-255 global
    uint16_t stripLeds;  // LEDs on the external strip
    uint16_t maxMilliamps;
    uint8_t on;
    uint8_t stripRgb;  // strip colour order, index into STRIP_ORDERS (0 = grb, 1 = rgb, 2 = bgr, ...)
    uint32_t solidColor;
    // Added 2026-11-22 after the fields above, so older saved settings keep
    // their values: magic2 marks whether this part has been written yet.
    uint32_t magic2;
    uint32_t fxColor[MODE_COUNT];  // per-pattern tint, 0 = the pattern's own colours
    // Added later still, same scheme: magic3 marks these as written.
    uint32_t magic3;
    uint8_t flakePct;  // star (onboard flake) brightness, % of the global brightness
    uint8_t stripPct;  // strip brightness, %
};
static constexpr uint32_t SETTINGS_MAGIC = 0x534E4F31;  // "SNO1"
static constexpr uint32_t SETTINGS_MAGIC2 = 0x534E4F32;  // "SNO2": fxColor valid
static constexpr uint32_t SETTINGS_MAGIC3 = 0x534E4F33;  // "SNO3": flakePct/stripPct valid
static Settings settings;

// Strip colour order: the byte order the strip expects on the wire. The onboard
// LEDs are WS2812B (grb); external strips vary. Check with solid red, green and
// blue (see README): a strip with red and blue swapped is bgr.
static const char *STRIP_ORDERS[] = {"grb", "rgb", "bgr", "brg", "rbg", "gbr"};
static constexpr uint8_t STRIP_ORDER_COUNT = 6;

// Patterns that take a colour from "color" (and HA's colour wheel). The others
// (warm, fire, rainbow, test) are their colour; a colour switches them to solid.
static bool tintable(uint8_t mode) {
    return mode == MODE_TWINKLE || mode == MODE_COMET || mode == MODE_CANDY;
}

static void defaultSettings() {
    settings = {};
    settings.magic = SETTINGS_MAGIC;
    settings.mode = MODE_RAINBOW;
    settings.brightness = 96;
    settings.stripLeds = 100;
    settings.maxMilliamps = 2000;  // a typical USB-C charger gives 2-3 A
    settings.on = 1;
    settings.stripRgb = 0;
    settings.solidColor = 0xFF6A1E;
    settings.magic2 = SETTINGS_MAGIC2;
    memset(settings.fxColor, 0, sizeof(settings.fxColor));
    settings.magic3 = SETTINGS_MAGIC3;
    settings.flakePct = 100;
    settings.stripPct = 100;
}

static void loadSettings() {
    EEPROM.get(0, settings);
    if (settings.magic != SETTINGS_MAGIC || settings.mode >= MODE_COUNT ||
        settings.stripLeds > MAX_STRIP_LEDS) {
        defaultSettings();
    }
    if (settings.magic2 != SETTINGS_MAGIC2) {  // saved before per-pattern colours existed
        settings.magic2 = SETTINGS_MAGIC2;
        memset(settings.fxColor, 0, sizeof(settings.fxColor));
    }
    if (settings.magic3 != SETTINGS_MAGIC3 || settings.flakePct > 100 || settings.stripPct > 100) {
        settings.magic3 = SETTINGS_MAGIC3;
        settings.flakePct = 100;
        settings.stripPct = 100;
    }
}

static bool settingsDirty = false;
static uint32_t settingsChangedAt = 0;
static bool haDirty = true;  // Home Assistant needs the new state (ha/light)
static void markDirty() {
    settingsDirty = true;
    settingsChangedAt = millis();
    haDirty = true;
}

// ---- frame buffer -------------------------------------------------------------
struct Px {
    uint8_t r, g, b;
};
static Px frame[FLAKE_LEDS + MAX_STRIP_LEDS];
static Adafruit_NeoPixel pixels(FLAKE_LEDS + 100, SPI, WS2812B);
static uint16_t totalLeds() { return FLAKE_LEDS + settings.stripLeds; }

static inline Px rgb(uint8_t r, uint8_t g, uint8_t b) { return {r, g, b}; }
static inline Px fromHex(uint32_t c) { return {(uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c}; }
static inline Px scale(Px c, uint8_t s) {
    return {(uint8_t)(c.r * s / 255), (uint8_t)(c.g * s / 255), (uint8_t)(c.b * s / 255)};
}
static inline Px blend(Px a, Px b, uint8_t t) {  // t=0 -> a, 255 -> b
    return {(uint8_t)(a.r + (b.r - a.r) * t / 255), (uint8_t)(a.g + (b.g - a.g) * t / 255),
            (uint8_t)(a.b + (b.b - a.b) * t / 255)};
}
static Px wheel(uint8_t pos) {
    pos = 255 - pos;
    if (pos < 85) return rgb(255 - pos * 3, 0, pos * 3);
    if (pos < 170) {
        pos -= 85;
        return rgb(0, pos * 3, 255 - pos * 3);
    }
    pos -= 170;
    return rgb(pos * 3, 255 - pos * 3, 0);
}
static void fillAll(Px c) {
    for (uint16_t i = 0; i < totalLeds(); i++) frame[i] = c;
}
static Px &flake(uint16_t i) { return frame[i]; }
static Px &strip(uint16_t i) { return frame[FLAKE_LEDS + i]; }

// ---- effects ------------------------------------------------------------------
// Per-pixel twinkle state for the whole chain.
static uint8_t sparkle[FLAKE_LEDS + MAX_STRIP_LEDS];

static void renderWarm(uint32_t) { fillAll(rgb(255, 140, 50)); }

static void renderRainbow(uint32_t t) {
    uint8_t base = t / 20;
    for (uint16_t i = 0; i < FLAKE_LEDS; i++) flake(i) = wheel(base + (i / 6) * 42 + (i % 6) * 4);
    for (uint16_t i = 0; i < settings.stripLeds; i++) strip(i) = wheel(base - i * 4);
}

static void renderCandy(uint32_t t) {
    uint32_t tint = settings.fxColor[MODE_CANDY];
    const Px red = tint ? fromHex(tint) : rgb(255, 0, 0), white = rgb(255, 255, 255);
    uint32_t offset = t / 120;
    for (uint16_t i = 0; i < settings.stripLeds; i++) strip(i) = ((i + offset) / 4) % 2 ? white : red;
    bool flip = (t / 1000) % 2;
    for (uint16_t i = 0; i < FLAKE_LEDS; i++) flake(i) = ((i / 6) % 2) ^ flip ? white : red;
    for (uint8_t i : INNER_RING) flake(i) = rgb(0, 160, 0);
}

static void renderTwinkle(uint32_t) {
    // Default: white sparkles on blue. Tinted: sparkles in the colour over a dim version of it.
    uint32_t tint = settings.fxColor[MODE_TWINKLE];
    // Base raised 2026-11-22 (was 10,20,60 / 40): the strip looked too dark between sparkles.
    const Px base = tint ? scale(fromHex(tint), 90) : rgb(25, 50, 150);
    const Px peak = tint ? fromHex(tint) : rgb(255, 255, 255);
    uint16_t n = totalLeds();
    for (uint16_t i = 0; i < n; i++) {
        if (sparkle[i] == 0 && random(1000) < 6) sparkle[i] = 255;
        frame[i] = blend(base, peak, sparkle[i]);
        sparkle[i] = sparkle[i] > 12 ? sparkle[i] - 12 : 0;
    }
}

static void renderComet(uint32_t t) {
    const uint16_t n = settings.stripLeds;
    const uint16_t tail = 12;
    uint16_t period = n + tail + 10;
    uint16_t head = (t / 15) % (period ? period : 1);
    uint8_t hue = (t / (15 * period)) * 40;  // new colour each lap
    uint32_t tint = settings.fxColor[MODE_COMET];
    Px c = tint ? fromHex(tint) : wheel(hue);  // default: a new colour each lap
    for (uint16_t i = 0; i < n; i++) {
        int d = (int)head - (int)i;
        strip(i) = (d >= 0 && d < tail) ? scale(c, 255 - d * 255 / tail) : rgb(0, 0, 0);
    }
    // The flake glows in the comet's colour, flashing as each lap starts.
    uint8_t glow = head < 20 ? 255 - head * 8 : 80;
    for (uint16_t i = 0; i < FLAKE_LEDS; i++) flake(i) = scale(c, glow);
}

static void renderFire(uint32_t) {
    uint16_t n = totalLeds();
    for (uint16_t i = 0; i < n; i++) {
        uint8_t heat = 120 + random(136);
        frame[i] = blend(frame[i], rgb(heat, heat * 2 / 5, heat / 16), 90);
    }
}

static void renderSolid(uint32_t) { fillAll(fromHex(settings.solidColor)); }

// Strip count check (counting from 1 at the board): every 10th LED red, every
// 50th green, the rest dim white; the last ten flash blue/off twice a second.
// The flake shows dim white. If the flashing block isn't at the very end of the
// strip, adjust "strip".
static void renderTest(uint32_t t) {
    bool flashOn = (t / 500) % 2;
    for (uint16_t i = 0; i < FLAKE_LEDS; i++) flake(i) = rgb(30, 30, 30);
    for (uint16_t i = 0; i < settings.stripLeds; i++) {
        uint16_t num = i + 1;
        if (num + 10 > settings.stripLeds) strip(i) = flashOn ? rgb(0, 0, 255) : rgb(0, 0, 0);
        else if (num % 50 == 0) strip(i) = rgb(0, 255, 0);
        else if (num % 10 == 0) strip(i) = rgb(255, 0, 0);
        else strip(i) = rgb(30, 30, 30);
    }
}

typedef void (*Renderer)(uint32_t);
static const Renderer RENDERERS[MODE_COUNT] = {renderWarm,  renderRainbow, renderCandy, renderTwinkle,
                                               renderComet, renderFire,    renderSolid, renderTest};

// ---- output with a power limit ------------------------------------------------
// WS2812B: ~20 mA per channel at full, ~1 mA idle per LED. Scale the frame so
// the estimate stays under maxMilliamps; brightness is applied first.
static uint16_t lastMilliamps = 0;
// Star/strip balance, applied on output only (frame keeps the pattern's colours,
// which fire needs) and before the power estimate so the limit stays right.
static inline Px balanced(uint16_t i) {
    uint8_t pct = i < FLAKE_LEDS ? settings.flakePct : settings.stripPct;
    return pct >= 100 ? frame[i] : scale(frame[i], pct * 255 / 100);
}

static void show() {
    uint16_t n = totalLeds();
    uint32_t sum = 0;  // sum of channel values after brightness
    for (uint16_t i = 0; i < n; i++) {
        Px p = balanced(i);
        sum += p.r + p.g + p.b;
    }
    sum = sum * settings.brightness / 255;
    uint32_t idle = n;  // mA
    uint32_t mA = idle + sum * 20 / 255;
    uint32_t scaleNum = settings.brightness;  // final scale out of 255
    if (mA > settings.maxMilliamps && sum > 0) {
        uint32_t budget = settings.maxMilliamps > idle ? settings.maxMilliamps - idle : 0;
        scaleNum = scaleNum * (budget * 255 / 20) / sum;
        mA = idle + budget;
    }
    lastMilliamps = mA;
    for (uint16_t i = 0; i < n; i++) {
        Px c = scale(balanced(i), scaleNum);
        if (i >= FLAKE_LEDS && settings.stripRgb) {
            // The library sends (r, g, b) as bytes g, r, b. Pick its arguments so the
            // bytes on the wire come out in the strip's order.
            const char *o = STRIP_ORDERS[settings.stripRgb < STRIP_ORDER_COUNT ? settings.stripRgb : 0];
            uint8_t wire[3];
            for (int k = 0; k < 3; k++) wire[k] = o[k] == 'r' ? c.r : o[k] == 'g' ? c.g : c.b;
            pixels.setPixelColor(i, wire[1], wire[0], wire[2]);
        } else {
            pixels.setPixelColor(i, c.r, c.g, c.b);
        }
    }
    pixels.show();
}

static void applyLength() {
    // updateLength() clears the buffer; send zeros past the end once so any
    // LEDs beyond the new length go dark.
    pixels.updateLength(FLAKE_LEDS + MAX_STRIP_LEDS);
    pixels.show();
    pixels.updateLength(totalLeds());
    memset(frame, 0, sizeof(frame));
    memset(sparkle, 0, sizeof(sparkle));
}

static void allOff() {
    pixels.clear();
    pixels.show();
}

static void setPower(bool on) {
    digitalWrite(STRIP_5V_EN, on && settings.stripLeds > 0 ? HIGH : LOW);
}

// ---- touch logo ---------------------------------------------------------------
static void handleTouch() {
    static bool down = false;
    static uint32_t downAt = 0;
    static bool longDone = false;
    bool pressed = digitalRead(TOUCH_PIN) == LOW;
    uint32_t now = millis();
    if (pressed && !down) {
        down = true;
        downAt = now;
        longDone = false;
    } else if (pressed && down && !longDone && now - downAt > 1000) {
        longDone = true;
        settings.on = !settings.on;
        markDirty();
        Log.info("touch: %s", settings.on ? "on" : "off");
    } else if (!pressed && down) {
        down = false;
        if (!longDone && now - downAt > 40) {
            if (!settings.on) settings.on = 1;
            else settings.mode = (settings.mode + 1) % MODE_COUNT;
            markDirty();
            Log.info("touch: mode %s", MODE_NAMES[settings.mode]);
        }
    }
}

// ---- cloud --------------------------------------------------------------------
static String modeVar;

static int fnMode(String arg) {
    arg.trim();
    arg.toLowerCase();
    if (arg == "off") {
        settings.on = 0;
        markDirty();
        return 0;
    }
    if (arg == "on") {
        settings.on = 1;
        markDirty();
        return settings.mode;
    }
    for (int i = 0; i < MODE_COUNT; i++) {
        if (arg == MODE_NAMES[i] || arg == String(i)) {
            settings.mode = i;
            settings.on = 1;
            markDirty();
            return i;
        }
    }
    return -1;
}

static int fnBrightness(String arg) {
    int v = arg.toInt();
    if (v < 0 || v > 255) return -1;
    settings.brightness = v;
    markDirty();
    return v;
}

static volatile bool lengthChanged = false;
static int fnStrip(String arg) {
    // "N" sets the count; "+N" / "-N" adjusts it.
    arg.trim();
    bool relative = arg.startsWith("+") || arg.startsWith("-");
    String digits = relative ? arg.substring(1) : arg;
    if (digits.length() == 0) return -1;
    for (unsigned i = 0; i < digits.length(); i++)
        if (!isdigit((unsigned char)digits.charAt(i))) return -1;
    int v = digits.toInt();
    if (relative) v = arg.startsWith("+") ? settings.stripLeds + v : settings.stripLeds - v;
    if (v < 0 || v > MAX_STRIP_LEDS) return -1;
    settings.stripLeds = v;
    lengthChanged = true;
    markDirty();
    return v;
}

static int fnColor(String arg) {
    arg.trim();
    if (arg.startsWith("#")) arg = arg.substring(1);
    if (arg.length() != 6) return -1;
    char *end = nullptr;
    uint32_t c = strtoul(arg.c_str(), &end, 16);
    if (!end || *end) return -1;
    settings.on = 1;
    if (tintable(settings.mode)) {
        // Tint the running pattern; white brings back its own colours.
        settings.fxColor[settings.mode] = (c == 0xFFFFFF) ? 0 : c;
        markDirty();
        return 1;
    }
    settings.solidColor = c;
    settings.mode = MODE_SOLID;
    markDirty();
    return 0;
}

static int fnMaxMa(String arg) {
    int v = arg.toInt();
    if (v < 200 || v > 4000) return -1;
    settings.maxMilliamps = v;
    markDirty();
    return v;
}

static int fnOrder(String arg) {
    arg.trim();
    arg.toLowerCase();
    for (uint8_t k = 0; k < STRIP_ORDER_COUNT; k++) {
        if (arg == STRIP_ORDERS[k]) {
            settings.stripRgb = k;
            markDirty();
            return k;
        }
    }
    return -1;
}

// ---- Home Assistant -----------------------------------------------------------
// The HA "particle" integration (No1089/particle-ha) turns the event ha/light into
// a light entity: on/off, brightness, colour and the modes as effects. It sends
// commands to the mode / brightness / color functions above. State goes out on
// every change (touch logo included), every 15 min, and when HA asks (hass-sync).
static void haSync(const char *, const char *) { haDirty = true; }

static void publishHaState() {
    static uint32_t lastSent = 0;
    uint32_t now = millis();
    bool due = haDirty || lastSent == 0 || now - lastSent > 15 * 60 * 1000UL;
    if (!due || now - lastSent < 1000 || !Particle.connected()) return;
    char buf[256];
    JSONBufferWriter w(buf, sizeof(buf) - 1);
    // The colour HA shows: what the current pattern is using. White = a pattern's
    // own colours (picking white resets a tint); warm/fire/rainbow show their look.
    uint32_t shown;
    switch (settings.mode) {
        case MODE_SOLID: shown = settings.solidColor; break;
        case MODE_WARM: shown = 0xFF8C32; break;
        case MODE_FIRE: shown = 0xFF6614; break;
        default: shown = tintable(settings.mode) && settings.fxColor[settings.mode]
                             ? settings.fxColor[settings.mode] : 0xFFFFFF;
    }
    char hex[7];
    snprintf(hex, sizeof(hex), "%06lx", (unsigned long)(shown & 0xFFFFFF));
    w.beginObject();
    w.name("on").value(settings.on ? 1 : 0);
    w.name("bri").value((int)settings.brightness);
    w.name("fx").value(MODE_NAMES[settings.mode]);
    w.name("rgb").value(hex);
    w.name("fxs").beginArray();
    for (int i = 0; i < MODE_COUNT; i++)
        if (i != MODE_TEST) w.value(MODE_NAMES[i]);
    w.endArray();
    w.endObject();
    buf[std::min(w.dataSize(), sizeof(buf) - 1)] = 0;
    lastSent = now ? now : 1;
    haDirty = false;
    // NO_ACK: don't stall the animation waiting for the cloud's acknowledgement.
    Particle.publish("ha/light", buf, PRIVATE | NO_ACK);
}

// "balance": star/strip brightness in %, e.g. "100/80", "75:100", or one number
// for the star alone ("80" = star 80 %, strip 100 %). Returns star*1000 + strip.
// Function only, no variable: set once per installation.
static int fnBalance(String arg) {
    arg.trim();
    int sep = arg.indexOf('/');
    if (sep < 0) sep = arg.indexOf(':');
    String a = sep < 0 ? arg : arg.substring(0, sep);
    String b = sep < 0 ? String("100") : arg.substring(sep + 1);
    a.trim();
    b.trim();
    auto pct = [](const String &v) -> int {
        if (v.length() == 0 || v.length() > 3) return -1;
        for (unsigned k = 0; k < v.length(); k++)
            if (!isdigit((unsigned char)v.charAt(k))) return -1;
        int n = v.toInt();
        return n <= 100 ? n : -1;
    };
    int star = pct(a), strip = pct(b);
    if (star < 0 || strip < 0) return -1;
    settings.flakePct = star;
    settings.stripPct = strip;
    markDirty();
    return star * 1000 + strip;
}

static int stripVar() { return settings.stripLeds; }
static int brightnessVar() { return settings.brightness; }
static int milliampsVar() { return lastMilliamps; }

// ---- main ---------------------------------------------------------------------
void setup() {
    pinMode(STRIP_5V_EN, OUTPUT);
    digitalWrite(STRIP_5V_EN, LOW);
    pinMode(TOUCH_PIN, INPUT_PULLUP);

    loadSettings();
    pixels.begin();
    applyLength();

    Particle.function("mode", fnMode);
    Particle.function("brightness", fnBrightness);
    Particle.function("strip", fnStrip);
    Particle.function("color", fnColor);
    Particle.function("maxma", fnMaxMa);
    Particle.function("order", fnOrder);
    Particle.function("balance", fnBalance);
    Particle.variable("mode", modeVar);
    Particle.variable("strip", stripVar);
    Particle.variable("brightness", brightnessVar);
    Particle.variable("milliamps", milliampsVar);
    Particle.subscribe("hass-sync", haSync);

    Log.info("snowflake-lights: %u flake + %u strip LEDs, mode %s", FLAKE_LEDS, settings.stripLeds,
             MODE_NAMES[settings.mode]);
}

void loop() {
    static uint32_t lastFrame = 0;
    static bool wasOn = true;
    uint32_t now = millis();

    handleTouch();

    if (lengthChanged) {
        lengthChanged = false;
        applyLength();
    }

    modeVar = settings.on ? MODE_NAMES[settings.mode] : "off";
    publishHaState();

    if (settingsDirty && now - settingsChangedAt > 3000) {
        EEPROM.put(0, settings);  // debounced, to spare the flash
        settingsDirty = false;
    }

    if (now - lastFrame < 20) return;  // ~50 fps
    lastFrame = now;

    if (!settings.on) {
        if (wasOn) {
            allOff();
            setPower(false);
            lastMilliamps = 0;
            wasOn = false;
        }
        return;
    }
    if (!wasOn) wasOn = true;
    setPower(true);
    RENDERERS[settings.mode](now);
    show();
}
