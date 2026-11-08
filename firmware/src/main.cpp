#include <Arduino.h>
#include <esp_wifi.h>

#include "config_protocol.h"
#include "config_store.h"
#include "elrs_uid.h"
#include "espnow_rx.h"
#include "io_port.h"
#include "led_status.h"
#include "link_status.h"

#if __has_include("secrets.h")
#include "secrets.h"
#endif

#ifndef ELRS_BINDING_PHRASE
#define ELRS_BINDING_PHRASE ""
#endif

#ifndef ELRS_CHANNEL
#define ELRS_CHANNEL 1
#endif

#ifdef ELRS_DEBUG_SERIAL
#ifdef ELRS_DATA_USB
#define DBG_PORT Serial0
#else
#define DBG_PORT Serial
#endif
#define DBG(...) DBG_PORT.printf(__VA_ARGS__)
#else
#define DBG(...) do {} while (0)
#endif

static DongleConfig g_cfg;
static uint8_t g_uid[6];
static uint32_t g_lastRetry = 0;
static uint32_t g_lastStatus = 0;

static void put16(uint8_t *&p, uint16_t v) {
    *p++ = (uint8_t)(v & 0xFF);
    *p++ = (uint8_t)((v >> 8) & 0xFF);
}

static void put32(uint8_t *&p, uint32_t v) {
    *p++ = (uint8_t)(v & 0xFF);
    *p++ = (uint8_t)((v >> 8) & 0xFF);
    *p++ = (uint8_t)((v >> 16) & 0xFF);
    *p++ = (uint8_t)((v >> 24) & 0xFF);
}

static void sendResponse(uint8_t type, const uint8_t *payload, uint16_t len) {
    uint8_t out[ELRS_CFG_MAX_PAYLOAD + 10];
    size_t total = elrsCfgBuild(out, sizeof(out), (uint8_t)(type | ELRS_CFG_RESPONSE),
                                payload, len);
    if (total) ioWrite(out, total);
}

static void sendStatus(uint8_t type, uint8_t status) {
    uint8_t payload[1] = {status};
    sendResponse(type, payload, sizeof(payload));
}

static void updateUid() {
    if (g_cfg.uidMode == UID_MODE_MANUAL) {
        memcpy(g_uid, g_cfg.uid, sizeof(g_uid));
        configNormalizeUid(g_uid);
        if (!configHasUid(g_cfg)) {
            elrsUidFromFactory(g_uid);
        }
    } else {
        elrsUidCompute(g_cfg.phrase, g_uid);
    }
}

static bool applyRadio() {
    updateUid();
    return espnowRestart(g_uid, g_cfg.channel);
}

static void sendConfig() {
    uint8_t payload[ELRS_CFG_MAX_PAYLOAD];
    uint8_t *p = payload;
    size_t phraseLen = strlen(g_cfg.phrase);
    if (phraseLen > ELRS_CFG_MAX_PHRASE) phraseLen = ELRS_CFG_MAX_PHRASE;

    *p++ = ELRS_CFG_OK;
    *p++ = g_cfg.channel;
    memcpy(p, g_uid, 6);
    p += 6;
    *p++ = (uint8_t)phraseLen;
    if (phraseLen) {
        memcpy(p, g_cfg.phrase, phraseLen);
        p += phraseLen;
    }
    // Firmware version 0.1.0.
    *p++ = 0;
    *p++ = 1;
    *p++ = 0;
    // UID mode is appended for newer clients; older clients ignore it.
    *p++ = (uint8_t)g_cfg.uidMode;
    sendResponse(ELRS_CFG_GET_CONFIG, payload, (uint16_t)(p - payload));
}

static void sendStats() {
    uint8_t payload[32];
    uint8_t *p = payload;
    *p++ = ELRS_CFG_OK;
    put32(p, espnowStats.rxPackets);
    put32(p, espnowStats.rxBytes);
    put32(p, espnowStats.queueDrops);
    put16(p, espnowStats.lastLen);
    put32(p, espnowStats.lastMs);
    *p++ = espnowReady() ? 1 : 0;
    *p++ = g_cfg.channel;
    uint8_t wifiPrimary = 0;
    wifi_second_chan_t wifiSecond = WIFI_SECOND_CHAN_NONE;
    esp_wifi_get_channel(&wifiPrimary, &wifiSecond);
    *p++ = wifiPrimary;
    *p++ = ioHostConnected() ? 1 : 0;
    *p++ = espnowStats.upLq;
    *p++ = espnowStats.downLq;
    *p++ = espnowStats.linkStatsSeen ? 1 : 0;
    sendResponse(ELRS_CFG_GET_STATS, payload, (uint16_t)(p - payload));
}

static void handleSetConfig(const uint8_t *payload, uint16_t len) {
    if (len < 2) {
        sendStatus(ELRS_CFG_SET_CONFIG, ELRS_CFG_ERR_BAD_REQUEST);
        return;
    }
    uint8_t channel = payload[0];
    uint8_t phraseLen = payload[1];
    if (phraseLen > ELRS_CFG_MAX_PHRASE || len < (uint16_t)(2 + phraseLen)) {
        sendStatus(ELRS_CFG_SET_CONFIG, ELRS_CFG_ERR_TOO_LONG);
        return;
    }
    DongleConfig next = g_cfg;
    next.channel = channel;
    next.uidMode = UID_MODE_PHRASE;
    memset(next.uid, 0, sizeof(next.uid));
    memset(next.phrase, 0, sizeof(next.phrase));
    memcpy(next.phrase, payload + 2, phraseLen);
    next.phrase[ELRS_CFG_MAX_PHRASE] = '\0';

    if (!configValidate(next)) {
        sendStatus(ELRS_CFG_SET_CONFIG, ELRS_CFG_ERR_INVALID);
        return;
    }

    DongleConfig old = g_cfg;
    g_cfg = next;
    if (!configSave(g_cfg)) {
        g_cfg = old;
        sendStatus(ELRS_CFG_SET_CONFIG, ELRS_CFG_ERR_BUSY);
        return;
    }

    bool ok = applyRadio();
    uint8_t resp[8] = {(uint8_t)(ok ? ELRS_CFG_OK : ELRS_CFG_ERR_BUSY),
                       g_cfg.channel};
    memcpy(resp + 2, g_uid, 6);
    sendResponse(ELRS_CFG_SET_CONFIG, resp, sizeof(resp));
}

static void handleSetUid(const uint8_t *payload, uint16_t len) {
    if (len != 7) {
        sendStatus(ELRS_CFG_SET_UID, ELRS_CFG_ERR_BAD_REQUEST);
        return;
    }
    uint8_t channel = payload[0];
    if (channel < 1 || channel > 13) {
        sendStatus(ELRS_CFG_SET_UID, ELRS_CFG_ERR_INVALID);
        return;
    }

    uint8_t uid[6];
    memcpy(uid, payload + 1, sizeof(uid));
    configNormalizeUid(uid);

    DongleConfig next = g_cfg;
    next.channel = channel;
    next.uidMode = UID_MODE_MANUAL;
    memcpy(next.uid, uid, sizeof(next.uid));
    if (!configValidate(next)) {
        sendStatus(ELRS_CFG_SET_UID, ELRS_CFG_ERR_INVALID);
        return;
    }

    DongleConfig old = g_cfg;
    g_cfg = next;
    if (!configSave(g_cfg)) {
        g_cfg = old;
        sendStatus(ELRS_CFG_SET_UID, ELRS_CFG_ERR_BUSY);
        return;
    }

    bool ok = applyRadio();
    uint8_t resp[8] = {(uint8_t)(ok ? ELRS_CFG_OK : ELRS_CFG_ERR_BUSY),
                       g_cfg.channel};
    memcpy(resp + 2, g_uid, 6);
    sendResponse(ELRS_CFG_SET_UID, resp, sizeof(resp));
}

static void handleReset() {
    configReset();
    configDefaults(g_cfg);
    bool ok = applyRadio();
    uint8_t resp[8] = {(uint8_t)(ok ? ELRS_CFG_OK : ELRS_CFG_ERR_BUSY),
                       g_cfg.channel};
    memcpy(resp + 2, g_uid, 6);
    sendResponse(ELRS_CFG_RESET, resp, sizeof(resp));
}

static void handleCommand(uint8_t type, const uint8_t *payload, uint16_t len) {
    switch (type) {
        case ELRS_CFG_GET_CONFIG:
            sendConfig();
            break;
        case ELRS_CFG_SET_CONFIG:
            handleSetConfig(payload, len);
            break;
        case ELRS_CFG_SET_UID:
            handleSetUid(payload, len);
            break;
        case ELRS_CFG_GET_STATS:
            sendStats();
            break;
        case ELRS_CFG_RESET:
            handleReset();
            break;
        case ELRS_CFG_PING:
            sendStatus(ELRS_CFG_PING, ELRS_CFG_OK);
            break;
        default:
            sendStatus(type, ELRS_CFG_ERR_BAD_REQUEST);
            break;
    }
}

static void handleControl() {
    static ElrsCfgParser parser;
    while (ioAvailable() > 0) {
        int b = ioRead();
        if (b < 0) break;
        uint8_t type = 0;
        const uint8_t *payload = nullptr;
        uint16_t len = 0;
        if (parser.feed((uint8_t)b, type, payload, len)) {
            handleCommand(type, payload, len);
        }
    }
}

void setup() {
    ioBegin();
    ledBegin();

#ifdef ELRS_DEBUG_SERIAL
#ifdef ELRS_DATA_USB
    Serial0.begin(115200, SERIAL_8N1, 20, 21);
#else
    Serial.begin(115200);
#endif
    delay(200);
#endif

    configLoad(g_cfg);
    if (g_cfg.uidMode == UID_MODE_PHRASE && g_cfg.phrase[0] == '\0' &&
        ELRS_BINDING_PHRASE[0] != '\0') {
        strncpy(g_cfg.phrase, ELRS_BINDING_PHRASE, ELRS_CFG_MAX_PHRASE);
        g_cfg.phrase[ELRS_CFG_MAX_PHRASE] = '\0';
    }
    if (g_cfg.channel < 1 || g_cfg.channel > 13) {
        g_cfg.channel = ELRS_CHANNEL;
    }

    updateUid();
    DBG("\n[ELRS dongle] boot, UID %s, channel %u, mode %u, phrase_len %u\n",
        elrsUidToString(g_uid).c_str(), (unsigned)g_cfg.channel,
        (unsigned)g_cfg.uidMode, (unsigned)strlen(g_cfg.phrase));

    if (!configHasUid(g_cfg)) {
        DBG("[ELRS dongle] WARNING: no UID configured, using factory MAC\n");
    }

    if (!espnowBegin(g_uid, g_cfg.channel)) {
        DBG("[ELRS dongle] ESP-NOW init failed, will retry\n");
    } else {
        DBG("[ELRS dongle] ESP-NOW ready\n");
    }
}

void loop() {
    if (!espnowReady()) {
        if (millis() - g_lastRetry >= 2000) {
            g_lastRetry = millis();
            if (espnowBegin(g_uid, g_cfg.channel)) {
                DBG("[ELRS dongle] ESP-NOW ready after retry\n");
            }
        }
    } else {
        espnowLoop();
    }

    handleControl();

    ledUpdate(ioHostConnected(), configHasUid(g_cfg), espnowReady(),
              espnowStats, g_linkStatus);

#ifdef ELRS_DEBUG_SERIAL
    if (millis() - g_lastStatus >= 5000) {
        g_lastStatus = millis();
        DBG("[status] rx=%lu bytes=%lu drops=%lu last=%u heap=%lu ready=%u\n",
            (unsigned long)espnowStats.rxPackets,
            (unsigned long)espnowStats.rxBytes,
            (unsigned long)espnowStats.queueDrops,
            (unsigned)espnowStats.lastLen,
            (unsigned long)ESP.getFreeHeap(),
            espnowReady() ? 1U : 0U);
    }
#endif
}
