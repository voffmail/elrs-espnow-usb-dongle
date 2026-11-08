#include "led_status.h"

#ifdef ELRS_DATA_USB
#define LED_PIN 8
#endif

enum class LedMode { Off, Solid, Telemetry, Error };

#define LED_RX_TIMEOUT_MS 3000
#define LED_LINK_TIMEOUT_MS 500

static LedMode s_mode = LedMode::Off;
static uint32_t s_modeStart = 0;
static bool s_on = false;
static uint32_t s_errorUntil = 0;
static uint32_t s_lastDrops = 0;

static void writeLed(bool on) {
    if (on == s_on) return;
    s_on = on;
#ifdef ELRS_DATA_USB
    // Native-USB C3 boards used here have a plain blue LED on GPIO8,
    // active low. Do not use neopixelWrite(): it holds the pin and the LED
    // stays solid.
    digitalWrite(LED_PIN, on ? LOW : HIGH);
#else
    // DevKitM-1 has an RGB WS2812 LED on GPIO8. Use blue for all states.
    neopixelWrite(RGB_BUILTIN, 0, 0, on ? 64 : 0);
#endif
}

static void setMode(LedMode mode, uint32_t now) {
    if (s_mode == mode) return;
    s_mode = mode;
    s_modeStart = now;
}

void ledBegin() {
#ifdef ELRS_DATA_USB
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, HIGH);  // off for active-low LED
#else
    neopixelWrite(RGB_BUILTIN, 0, 0, 0);
#endif
    s_on = false;
    s_mode = LedMode::Off;
    s_modeStart = millis();
}

void ledUpdate(bool hostConnected, bool phraseConfigured, bool espnowReady,
               const EspNowStats &stats, const LinkStatus &link) {
    const uint32_t now = millis();

    if (stats.queueDrops != s_lastDrops) {
        s_lastDrops = stats.queueDrops;
        s_errorUntil = now + 2000;
    }

    // No binding phrase or no app/PC on USB: LED is off.
    if (!phraseConfigured || !hostConnected) {
        setMode(LedMode::Off, now);
        writeLed(false);
        return;
    }

    // ESP-NOW failed to start, or the dongle queue recently overflowed:
    // three short pulses every 2 seconds.
    if (!espnowReady || now < s_errorUntil) {
        setMode(LedMode::Error, now);
        const uint32_t phase = (now - s_modeStart) % 2000;
        const bool on = phase < 100 || (phase >= 200 && phase < 300) ||
                        (phase >= 400 && phase < 500);
        writeLed(on);
        return;
    }

    // No ESP-NOW frames at all for a few seconds: the TX backpack/radio is
    // off or the ESP-NOW link is gone. Show off instead of the "ready, no
    // telemetry" solid state.
    const bool anyRawRecent = stats.rxPackets > 0 &&
                              (now - stats.lastMs) < LED_RX_TIMEOUT_MS;
    if (!anyRawRecent) {
        setMode(LedMode::Off, now);
        writeLed(false);
        return;
    }

    // Telemetry is flowing: 200 ms on / 200 ms off. If link statistics
    // (CRSF 0x14) are present, require a live uplink/downlink LQ; a backpack
    // keeps sending 0x14 with LQ=0 after the drone is switched off, which is
    // not telemetry and must show solid, not blink.
    const bool freshFrames = (now - stats.lastMs) < 500;
    bool flowing = freshFrames;
    if (link.known) {
        const bool freshLink = (now - link.lastMs) < LED_LINK_TIMEOUT_MS;
        flowing = freshFrames && freshLink && link.up;
    }
    if (flowing) {
        setMode(LedMode::Telemetry, now);
        const bool on = ((now - s_modeStart) / 200) % 2 == 0;
        writeLed(on);
        return;
    }

    // Ready, app connected, but no telemetry: steady on.
    setMode(LedMode::Solid, now);
    writeLed(true);
}
