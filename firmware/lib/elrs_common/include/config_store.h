#pragma once

#include <Arduino.h>

#include "config_protocol.h"

enum UidMode : uint8_t {
    UID_MODE_PHRASE = 0,
    UID_MODE_MANUAL = 1,
};

struct DongleConfig {
    char phrase[ELRS_CFG_MAX_PHRASE + 1];
    uint8_t channel;
    UidMode uidMode;
    uint8_t uid[6];
};

void configDefaults(DongleConfig &cfg);
bool configValidate(const DongleConfig &cfg);
bool configHasUid(const DongleConfig &cfg);
void configNormalizeUid(uint8_t uid[6]);
bool configLoad(DongleConfig &cfg);
bool configSave(const DongleConfig &cfg);
void configReset();
