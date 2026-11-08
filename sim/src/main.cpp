#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <math.h>
#include <string.h>

#include "config_protocol.h"
#include "config_store.h"
#include "crsf.h"
#include "elrs_uid.h"
#include "msp_v2.h"

#if __has_include("secrets.h")
#include "secrets.h"
#endif

#ifndef ELRS_BINDING_PHRASE
#define ELRS_BINDING_PHRASE ""
#endif
#ifndef ELRS_CHANNEL
#define ELRS_CHANNEL 1
#endif

#define SIM_MSP_FLAG 0x00
#define SIM_MSP_FUNCTION 0x0011
#define SIM_CRSF_ADDRESS 0xC8
#define SIM_SEQ_TYPE 0x7F

#ifndef SIM_GPS_LAT
#define SIM_GPS_LAT 55.753930
#endif
#ifndef SIM_GPS_LON
#define SIM_GPS_LON 37.620795
#endif
#ifndef SIM_GPS_RADIUS_M
#define SIM_GPS_RADIUS_M 120.0
#endif
#ifndef SIM_GPS_SPEED_KMH
#define SIM_GPS_SPEED_KMH 18.0
#endif
#define SIM_PI 3.14159265358979323846

struct SimStats {
    uint32_t sentPackets;
    uint32_t sentBytes;
    uint32_t sendErrors;
    uint32_t lastSeq;
    uint16_t lastLen;
    uint32_t lastMs;
};

static DongleConfig g_cfg;
static uint8_t g_uid[6];
static bool g_ready = false;
static bool g_wifiStarted = false;
static SimStats g_stats = {};
static uint32_t g_lastLink = 0;
static uint32_t g_lastAtt = 0;
static uint32_t g_lastSlow = 0;
static uint32_t g_lastSeq = 0;
static uint32_t g_lastStatus = 0;
static uint32_t g_lastGps = 0;
static uint32_t g_lastVario = 0;
static uint32_t g_seq = 0;
static double g_theta = 0.0;
static double g_lat = SIM_GPS_LAT;
static double g_lon = SIM_GPS_LON;
static double g_alt = 120.0;
static double g_vario = 0.0;
static double g_headingDeg = 0.0;

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

static void putBE16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)((v >> 8) & 0xFF);
    p[1] = (uint8_t)(v & 0xFF);
}

static void putBEI16(uint8_t *p, int16_t v) {
    putBE16(p, (uint16_t)v);
}

static void putBE32(uint8_t *p, int32_t v) {
    uint32_t u = (uint32_t)v;
    p[0] = (uint8_t)((u >> 24) & 0xFF);
    p[1] = (uint8_t)((u >> 16) & 0xFF);
    p[2] = (uint8_t)((u >> 8) & 0xFF);
    p[3] = (uint8_t)(u & 0xFF);
}

static void sendResponse(uint8_t type, const uint8_t *payload, uint16_t len) {
    uint8_t out[ELRS_CFG_MAX_PAYLOAD + 10];
    size_t total = elrsCfgBuild(out, sizeof(out),
                                (uint8_t)(type | ELRS_CFG_RESPONSE), payload, len);
    if (total) Serial.write(out, total);
}

static void sendStatus(uint8_t type, uint8_t status) {
    uint8_t payload[1] = {status};
    sendResponse(type, payload, sizeof(payload));
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
    *p++ = 0;
    *p++ = 1;
    *p++ = 0;
    *p++ = (uint8_t)g_cfg.uidMode;
    sendResponse(ELRS_CFG_GET_CONFIG, payload, (uint16_t)(p - payload));
}

static void sendStats() {
    uint8_t payload[32];
    uint8_t *p = payload;
    *p++ = ELRS_CFG_OK;
    put32(p, g_stats.sentPackets);
    put32(p, g_stats.sentBytes);
    put32(p, 0);
    put16(p, g_stats.lastLen);
    put32(p, g_stats.lastMs);
    *p++ = g_ready ? 1 : 0;
    *p++ = g_cfg.channel;
    uint8_t wifiPrimary = 0;
    wifi_second_chan_t wifiSecond = WIFI_SECOND_CHAN_NONE;
    esp_wifi_get_channel(&wifiPrimary, &wifiSecond);
    *p++ = wifiPrimary;
    sendResponse(ELRS_CFG_GET_STATS, payload, (uint16_t)(p - payload));
}

static bool simBegin() {
    if (g_ready) return true;
    if (!g_wifiStarted) {
        WiFi.persistent(false);
        WiFi.mode(WIFI_STA);
        delay(50);
        g_wifiStarted = true;
    }

    esp_wifi_stop();
    delay(20);
    esp_wifi_start();
    delay(50);
    esp_wifi_set_ps(WIFI_PS_NONE);
#ifdef ELRS_PHY_LR
    esp_wifi_set_protocol(WIFI_IF_STA,
                          WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G |
                          WIFI_PROTOCOL_11N | WIFI_PROTOCOL_LR);
#else
    esp_wifi_set_protocol(WIFI_IF_STA,
                          WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G |
                          WIFI_PROTOCOL_11N);
#endif
    esp_wifi_set_channel(g_cfg.channel, WIFI_SECOND_CHAN_NONE);

    if (esp_now_init() != ESP_OK) return false;

    // esp_now_init can leave the interface on its default channel; apply the
    // requested channel again after init.
    esp_wifi_set_channel(g_cfg.channel, WIFI_SECOND_CHAN_NONE);

    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, g_uid, 6);
    peer.channel = g_cfg.channel;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;
    if (esp_now_add_peer(&peer) != ESP_OK) {
        esp_now_deinit();
        return false;
    }

    g_ready = true;
    return true;
}

static void simStop() {
    if (!g_ready) return;
    esp_now_deinit();
    esp_wifi_stop();
    g_ready = false;
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

static bool applyConfig() {
    updateUid();
    simStop();
    return simBegin();
}

static bool sendCrsf(uint8_t type, const uint8_t *payload, uint8_t payloadLen) {
    if (!g_ready) return false;

    uint8_t crsf[CRSF_MAX_PAYLOAD + 4];
    size_t crsfLen = crsfBuild(crsf, sizeof(crsf), SIM_CRSF_ADDRESS, type,
                               payload, payloadLen);
    if (!crsfLen) return false;

    uint8_t msp[MSP_V2_HEADER_LEN + sizeof(crsf) + 1];
    size_t mspLen = mspV2Build(msp, sizeof(msp), SIM_MSP_FLAG,
                               SIM_MSP_FUNCTION, crsf, (uint16_t)crsfLen);
    if (!mspLen) return false;

    esp_err_t err = esp_now_send(g_uid, msp, mspLen);
    if (err != ESP_OK) {
        g_stats.sendErrors++;
        return false;
    }

    g_stats.sentPackets++;
    g_stats.sentBytes += (uint32_t)mspLen;
    g_stats.lastLen = (uint16_t)mspLen;
    g_stats.lastMs = millis();
    return true;
}

static void updateGps() {
    uint32_t now = millis();
    double dt = g_lastGps ? (now - g_lastGps) / 1000.0 : 0.1;
    g_lastGps = now;
    if (dt <= 0.0 || dt > 0.5) dt = 0.1;

    const double radius = SIM_GPS_RADIUS_M;
    const double speedMps = SIM_GPS_SPEED_KMH / 3.6;
    const double omega = speedMps / radius;

    g_theta += omega * dt;
    if (g_theta >= 2.0 * SIM_PI) g_theta -= 2.0 * SIM_PI;

    const double north = radius * cos(g_theta);
    const double east = radius * sin(g_theta);
    const double latRad = SIM_GPS_LAT * SIM_PI / 180.0;

    g_lat = SIM_GPS_LAT + north / 111320.0;
    g_lon = SIM_GPS_LON + east / (111320.0 * cos(latRad));

    const double vn = -radius * omega * sin(g_theta);
    const double ve = radius * omega * cos(g_theta);
    g_headingDeg = atan2(ve, vn) * 180.0 / SIM_PI;
    if (g_headingDeg < 0.0) g_headingDeg += 360.0;

    g_alt = 120.0 + 10.0 * sin(2.0 * g_theta);
    g_vario = 20.0 * cos(2.0 * g_theta) * omega;
}

static void sendLink() {
    uint8_t payload[10] = {0xC2, 0x00, 100, 12, 0, 5, 1, 0xDC, 100, 11};
    payload[2] = (uint8_t)(100 - (g_seq % 3));
    payload[8] = (uint8_t)(100 - (g_seq % 2));
    sendCrsf(0x14, payload, sizeof(payload));
}

static void sendAttitude() {
    double yawRad = g_headingDeg * SIM_PI / 180.0;
    if (yawRad > SIM_PI) yawRad -= 2.0 * SIM_PI;
    double rollDeg = 18.0 + 6.0 * sin(g_theta * 2.0);
    double pitchDeg = 4.0 + 2.0 * sin(g_theta * 3.0);
    uint8_t p[6];
    putBEI16(p, (int16_t)llround(pitchDeg * SIM_PI / 180.0 * 10000.0));
    putBEI16(p + 2, (int16_t)llround(rollDeg * SIM_PI / 180.0 * 10000.0));
    putBEI16(p + 4, (int16_t)llround(yawRad * 10000.0));
    sendCrsf(0x1E, p, sizeof(p));
}

static void sendBattery() {
    uint16_t voltage = (uint16_t)(168 - fabs(sin(g_theta)) * 12.0);  // 15.6..16.8 V
    uint16_t current = 150;                                       // 15.0 A
    uint32_t capacity = 1200;                                     // mAh
    uint8_t p[8];
    putBE16(p, voltage);
    putBE16(p + 2, current);
    p[4] = (uint8_t)((capacity >> 16) & 0xFF);
    p[5] = (uint8_t)((capacity >> 8) & 0xFF);
    p[6] = (uint8_t)(capacity & 0xFF);
    p[7] = 80;
    sendCrsf(0x08, p, sizeof(p));
}

static void sendGps() {
    int32_t lat = (int32_t)llround(g_lat * 1e7);
    int32_t lon = (int32_t)llround(g_lon * 1e7);
    uint16_t speed = (uint16_t)llround(SIM_GPS_SPEED_KMH * 10.0);
    uint16_t heading = (uint16_t)llround(g_headingDeg * 100.0) % 36000;
    uint16_t alt = (uint16_t)llround(g_alt + 1000.0);
    uint8_t sats = (uint8_t)(10 + (millis() / 1000) % 3);
    uint8_t p[15];
    putBE32(p, lat);
    putBE32(p + 4, lon);
    putBE16(p + 8, speed);
    putBE16(p + 10, heading);
    putBE16(p + 12, alt);
    p[14] = sats;
    sendCrsf(0x02, p, sizeof(p));
}

static void sendGpsExt() {
    const double radius = SIM_GPS_RADIUS_M;
    const double speedMps = SIM_GPS_SPEED_KMH / 3.6;
    const double omega = speedMps / radius;
    const double vn = -radius * omega * sin(g_theta);
    const double ve = radius * omega * cos(g_theta);
    uint8_t p[20];
    p[0] = 3;  // 3D fix
    putBEI16(p + 1, (int16_t)llround(vn * 100.0));
    putBEI16(p + 3, (int16_t)llround(ve * 100.0));
    putBEI16(p + 5, (int16_t)llround(g_vario * 100.0));
    putBEI16(p + 7, 50);    // speed accuracy 0.5 m/s
    putBEI16(p + 9, 100);   // track accuracy
    putBEI16(p + 11, (int16_t)llround(g_alt));
    putBEI16(p + 13, 150);  // horizontal accuracy 1.5 m
    putBEI16(p + 15, 250);  // vertical accuracy 2.5 m
    p[17] = 0;
    p[18] = 8;              // hdop 0.8
    p[19] = 11;             // vdop 1.1
    sendCrsf(0x06, p, sizeof(p));
}

static void sendVario() {
    uint8_t p[2];
    putBEI16(p, (int16_t)llround(g_vario * 100.0));
    sendCrsf(0x07, p, sizeof(p));
}

static void sendBaro() {
    uint16_t raw = 0x8000 | (uint16_t)llround(g_alt);
    uint8_t p[3] = {(uint8_t)((raw >> 8) & 0xFF), (uint8_t)(raw & 0xFF), 0};
    sendCrsf(0x09, p, sizeof(p));
}

static void sendGpsTime() {
    uint32_t seconds = millis() / 1000;
    uint32_t total = 12u * 3600u + seconds;
    uint8_t p[9];
    putBE16(p, 2026);
    p[2] = 5;
    p[3] = 1;
    p[4] = (uint8_t)((total / 3600) % 24);
    p[5] = (uint8_t)((total / 60) % 60);
    p[6] = (uint8_t)(total % 60);
    putBE16(p + 7, (uint16_t)(millis() % 1000));
    sendCrsf(0x03, p, sizeof(p));
}

static void sendDevice() {
    static const uint8_t name[] = {'B', 'e', 't', 'a', 'f', 'l', 'i', 'g', 'h', 't'};
    uint8_t p[2 + sizeof(name)];
    p[0] = 0;
    p[1] = 0;
    memcpy(p + 2, name, sizeof(name));
    sendCrsf(0x29, p, sizeof(p));
}

static void sendMode() {
    static const uint8_t mode[] = {'A', 'N', 'G', 'L', 'E'};
    sendCrsf(0x21, mode, sizeof(mode));
}

static void sendSequence() {
    uint8_t payload[4];
    payload[0] = (uint8_t)(g_seq & 0xFF);
    payload[1] = (uint8_t)((g_seq >> 8) & 0xFF);
    payload[2] = (uint8_t)((g_seq >> 16) & 0xFF);
    payload[3] = (uint8_t)((g_seq >> 24) & 0xFF);
    if (sendCrsf(SIM_SEQ_TYPE, payload, sizeof(payload))) {
        g_stats.lastSeq = g_seq;
        g_seq++;
    }
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

    bool ok = applyConfig();
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

    bool ok = applyConfig();
    uint8_t resp[8] = {(uint8_t)(ok ? ELRS_CFG_OK : ELRS_CFG_ERR_BUSY),
                       g_cfg.channel};
    memcpy(resp + 2, g_uid, 6);
    sendResponse(ELRS_CFG_SET_UID, resp, sizeof(resp));
}

static void handleReset() {
    configReset();
    configDefaults(g_cfg);
    bool ok = applyConfig();
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
    while (Serial.available() > 0) {
        int b = Serial.read();
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
    Serial.begin(115200);
    delay(200);

    configLoad(g_cfg);
    if (g_cfg.uidMode == UID_MODE_PHRASE && g_cfg.phrase[0] == '\0' &&
        ELRS_BINDING_PHRASE[0] != '\0') {
        strncpy(g_cfg.phrase, ELRS_BINDING_PHRASE, ELRS_CFG_MAX_PHRASE);
        g_cfg.phrase[ELRS_CFG_MAX_PHRASE] = '\0';
    }
    if (g_cfg.channel < 1 || g_cfg.channel > 13) g_cfg.channel = ELRS_CHANNEL;

    updateUid();
    Serial.printf("\n[ELRS sim] UID %s, channel %u, mode %u, phrase_len %u\n",
                  elrsUidToString(g_uid).c_str(), (unsigned)g_cfg.channel,
                  (unsigned)g_cfg.uidMode, (unsigned)strlen(g_cfg.phrase));
    if (!configHasUid(g_cfg)) {
        Serial.println("[ELRS sim] no UID configured; using factory MAC");
    }
    if (!simBegin()) {
        Serial.println("[ELRS sim] ESP-NOW init failed");
    }
}

void loop() {
    if (!g_ready) {
        static uint32_t lastRetry = 0;
        if (millis() - lastRetry >= 2000) {
            lastRetry = millis();
            if (simBegin()) Serial.println("[ELRS sim] ESP-NOW ready after retry");
        }
    } else {
        uint32_t now = millis();
        if (now - g_lastLink >= 100) {
            g_lastLink = now;
            sendLink();
            sendSequence();
        }
        if (now - g_lastGps >= 100) {
            g_lastGps = now;
            updateGps();
            sendGps();
        }
        if (now - g_lastAtt >= 200) {
            g_lastAtt = now;
            sendAttitude();
        }
        if (now - g_lastVario >= 200) {
            g_lastVario = now;
            sendVario();
        }
        if (now - g_lastSlow >= 1000) {
            g_lastSlow = now;
            sendBattery();
            sendGpsExt();
            sendBaro();
            sendMode();
            sendGpsTime();
            sendDevice();
        }
    }

    handleControl();

    if (millis() - g_lastStatus >= 10000) {
        g_lastStatus = millis();
        uint8_t wifiPrimary = 0;
        wifi_second_chan_t wifiSecond = WIFI_SECOND_CHAN_NONE;
        esp_wifi_get_channel(&wifiPrimary, &wifiSecond);
        Serial.printf("[ELRS sim] sent=%lu err=%lu seq=%lu heap=%lu ready=%u ch=%u\n",
                      (unsigned long)g_stats.sentPackets,
                      (unsigned long)g_stats.sendErrors,
                      (unsigned long)g_stats.lastSeq,
                      (unsigned long)ESP.getFreeHeap(),
                      g_ready ? 1U : 0U, (unsigned)wifiPrimary);
    }
}
