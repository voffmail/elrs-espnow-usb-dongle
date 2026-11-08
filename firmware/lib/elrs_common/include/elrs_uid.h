#pragma once

#include <Arduino.h>

// ELRS group address derived from the binding phrase:
// MD5("-DMY_BINDING_PHRASE=\"<phrase>\"")[0..5], first byte LSB cleared.
void elrsUidFromPhrase(const char *phrase, uint8_t uid[6]);

// Factory Wi-Fi station MAC with the multicast bit cleared.
void elrsUidFromFactory(uint8_t uid[6]);

// Phrase if non-empty, otherwise factory MAC.
void elrsUidCompute(const char *phrase, uint8_t uid[6]);

String elrsUidToString(const uint8_t uid[6]);
