#pragma once

#include <Arduino.h>

struct EspNowStats {
    uint32_t rxPackets;
    uint32_t rxBytes;
    uint32_t queueDrops;
    uint16_t lastLen;
    uint32_t lastMs;
    uint8_t upLq;
    uint8_t downLq;
    bool linkStatsSeen;
    uint32_t lastLinkStatsMs;
};

extern EspNowStats espnowStats;

// Initializes Wi-Fi in STA mode, spoofs the ELRS group MAC, and starts ESP-NOW.
bool espnowBegin(const uint8_t uid[6], uint8_t channel);

// Stops ESP-NOW, then applies a new UID/channel.
bool espnowRestart(const uint8_t uid[6], uint8_t channel);

void espnowStop();

// Drains the ESP-NOW queue and writes raw payloads to the data port.
void espnowLoop();

bool espnowReady();
