#include "config_store.h"

#include <Preferences.h>
#include <string.h>

#ifndef ELRS_CHANNEL
#define ELRS_CHANNEL 1
#endif

static Preferences prefs;

void configDefaults(DongleConfig &cfg) {
    memset(&cfg, 0, sizeof(cfg));
    cfg.channel = ELRS_CHANNEL;
    cfg.uidMode = UID_MODE_PHRASE;
}

void configNormalizeUid(uint8_t uid[6]) {
    // ELRS group addresses are unicast: the multicast bit must be clear.
    uid[0] &= (uint8_t)~0x01;
}

bool configHasUid(const DongleConfig &cfg) {
    if (cfg.uidMode == UID_MODE_MANUAL) {
        uint8_t uid[6];
        memcpy(uid, cfg.uid, sizeof(uid));
        configNormalizeUid(uid);
        for (size_t i = 0; i < sizeof(uid); i++) {
            if (uid[i]) return true;
        }
        return false;
    }
    return cfg.phrase[0] != '\0';
}

bool configValidate(const DongleConfig &cfg) {
    if (cfg.channel < 1 || cfg.channel > 13) return false;
    if (cfg.uidMode != UID_MODE_PHRASE && cfg.uidMode != UID_MODE_MANUAL) {
        return false;
    }
    if (cfg.uidMode == UID_MODE_PHRASE) {
        // An empty phrase is valid: it means "use the factory MAC" (unbound).
        return strlen(cfg.phrase) <= ELRS_CFG_MAX_PHRASE;
    }
    return configHasUid(cfg);
}

bool configLoad(DongleConfig &cfg) {
    configDefaults(cfg);
    if (!prefs.begin("elrs", true)) return false;

    uint8_t channel = prefs.getUChar("channel", cfg.channel);
    uint8_t mode = prefs.getUChar("uid_mode", (uint8_t)UID_MODE_PHRASE);
    String phrase = prefs.getString("phrase", "");
    uint8_t uid[6] = {0};
    size_t uidLen = prefs.getBytes("uid", uid, sizeof(uid));
    prefs.end();

    cfg.channel = channel;
    cfg.uidMode = (UidMode)mode;
    if (phrase.length() > ELRS_CFG_MAX_PHRASE) {
        configDefaults(cfg);
        return false;
    }
    strncpy(cfg.phrase, phrase.c_str(), ELRS_CFG_MAX_PHRASE);
    cfg.phrase[ELRS_CFG_MAX_PHRASE] = '\0';
    if (uidLen == sizeof(cfg.uid)) {
        memcpy(cfg.uid, uid, sizeof(cfg.uid));
    }
    configNormalizeUid(cfg.uid);

    if (!configValidate(cfg)) {
        configDefaults(cfg);
        return false;
    }
    return true;
}

bool configSave(const DongleConfig &cfg) {
    DongleConfig normalized = cfg;
    configNormalizeUid(normalized.uid);
    if (!configValidate(normalized)) return false;
    if (!prefs.begin("elrs", false)) return false;
    size_t written = prefs.putString("phrase", normalized.phrase);
    prefs.putUChar("channel", normalized.channel);
    prefs.putUChar("uid_mode", (uint8_t)normalized.uidMode);
    prefs.putBytes("uid", normalized.uid, sizeof(normalized.uid));
    prefs.end();
    return written == strlen(normalized.phrase);
}

void configReset() {
    if (prefs.begin("elrs", false)) {
        prefs.clear();
        prefs.end();
    }
}
