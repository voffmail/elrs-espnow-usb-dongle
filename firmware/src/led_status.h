#pragma once

#include <Arduino.h>

#include "espnow_rx.h"
#include "link_status.h"

void ledBegin();

// hostConnected: USB host/app has the serial port open.
// phraseConfigured: binding phrase is non-empty.
// espnowReady: Wi-Fi/ESP-NOW initialized.
// stats: raw ESP-NOW frame counters. link: protocol-independent link state
// (filled by the CRSF parser now, MAVLink later).
void ledUpdate(bool hostConnected, bool phraseConfigured, bool espnowReady,
               const EspNowStats &stats, const LinkStatus &link);
