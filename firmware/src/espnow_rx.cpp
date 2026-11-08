#include "espnow_rx.h"

#include <WiFi.h>
#include <esp_idf_version.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <string.h>

#include "io_port.h"
#include "link_status.h"
#include "elrs_crc.h"
#include "msp_v2.h"

#define ESPNOW_FRAME_MAX 250
#define ESPNOW_QUEUE_LEN 16

struct RawFrame {
    uint16_t len;
    uint8_t data[ESPNOW_FRAME_MAX];
};

EspNowStats espnowStats = {};

static QueueHandle_t frameQueue = nullptr;
static bool ready = false;
static bool wifiStarted = false;

static bool parseLinkStats(const uint8_t *data, int len, uint8_t &upLq,
                           uint8_t &downLq) {
    uint8_t flag = 0;
    uint16_t function = 0;
    uint16_t payloadLen = 0;
    const uint8_t *payload = nullptr;
    if (!mspV2Parse(data, (size_t)len, flag, function, payload, payloadLen)) {
        return false;
    }
    if (function != 0x0011 || payloadLen < 4) return false;

    const uint8_t *crsf = payload;
    uint8_t address = crsf[0];
    if (address != 0xC8 && address != 0xEA && address != 0xEE) return false;
    uint8_t frameLen = crsf[1];
    if (frameLen < 3 || (int)frameLen + 2 > (int)payloadLen) return false;
    if (crsf[2] != 0x14) return false;              // link statistics
    if (frameLen - 2 < 10) return false;            // 10-byte payload
    int crcIndex = 1 + frameLen;
    if (crcIndex >= (int)payloadLen) return false;
    if (elrsCrc8(crsf + 2, (size_t)frameLen - 1) != crsf[crcIndex]) return false;

    upLq = crsf[5];
    downLq = crsf[11];
    return true;
}

static void enqueueFrame(const uint8_t *data, int len) {
    if (!frameQueue || len <= 0 || len > ESPNOW_FRAME_MAX) return;

    RawFrame frame;
    frame.len = (uint16_t)len;
    memcpy(frame.data, data, (size_t)len);

    if (xQueueSend(frameQueue, &frame, 0) != pdTRUE) {
        RawFrame dropped;
        if (xQueueReceive(frameQueue, &dropped, 0) == pdTRUE) {
            espnowStats.queueDrops++;
        }
        xQueueSend(frameQueue, &frame, 0);
    }
}

#if ESP_IDF_VERSION_MAJOR >= 5
static void onEspNowRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
    (void)info;
    enqueueFrame(data, len);
}
#else
static void onEspNowRecv(const uint8_t *mac, const uint8_t *data, int len) {
    (void)mac;
    enqueueFrame(data, len);
}
#endif

bool espnowBegin(const uint8_t uid[6], uint8_t channel) {
    if (ready) return true;

    if (!frameQueue) {
        frameQueue = xQueueCreate(ESPNOW_QUEUE_LEN, sizeof(RawFrame));
        if (!frameQueue) return false;
    }

    if (!wifiStarted) {
        WiFi.persistent(false);
        WiFi.mode(WIFI_STA);
        delay(50);
        wifiStarted = true;
    }

    esp_wifi_stop();
    delay(20);

    esp_err_t macErr = esp_wifi_set_mac(WIFI_IF_STA, uid);
    esp_wifi_start();
    delay(50);

    if (macErr != ESP_OK) {
        return false;
    }

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
    esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);

    if (esp_now_init() != ESP_OK) {
        return false;
    }

    // esp_now_init can leave the interface on its default channel; apply the
    // requested channel again after init.
    esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);

    if (esp_now_register_recv_cb(onEspNowRecv) != ESP_OK) {
        return false;
    }

    // Not required for receiving unencrypted unicast, but harmless and keeps
    // the door open for broadcast bind packets.
    esp_now_peer_info_t peer = {};
    memset(peer.peer_addr, 0xFF, 6);
    peer.channel = channel;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;
    esp_now_add_peer(&peer);

    ready = true;
    return true;
}

void espnowStop() {
    if (!ready) return;
    esp_now_deinit();
    esp_wifi_stop();
    ready = false;
}

bool espnowRestart(const uint8_t uid[6], uint8_t channel) {
    espnowStop();
    if (frameQueue) xQueueReset(frameQueue);
    return espnowBegin(uid, channel);
}

void espnowLoop() {
    if (!ready) return;

    RawFrame frame;
    while (xQueueReceive(frameQueue, &frame, 0) == pdTRUE) {
        uint8_t upLq = 0;
        uint8_t downLq = 0;
        if (parseLinkStats(frame.data, frame.len, upLq, downLq)) {
            espnowStats.upLq = upLq;
            espnowStats.downLq = downLq;
            espnowStats.linkStatsSeen = true;
            espnowStats.lastLinkStatsMs = millis();

            g_linkStatus.known = true;
            g_linkStatus.up = upLq > 0 && downLq > 0;
            g_linkStatus.quality = upLq < downLq ? upLq : downLq;
            g_linkStatus.lastMs = espnowStats.lastLinkStatsMs;
            g_linkStatus.source = LinkSource::Crsf;
        }
        ioWrite(frame.data, frame.len);
        espnowStats.rxPackets++;
        espnowStats.rxBytes += frame.len;
        espnowStats.lastLen = frame.len;
        espnowStats.lastMs = millis();
    }
}

bool espnowReady() {
    return ready;
}
