#pragma once

#include <stddef.h>
#include <stdint.h>

#define CRSF_MAX_PAYLOAD 60

// Builds a CRSF frame: address, length, type, payload, CRC8.
// length = payloadLen + 2 (type + CRC). Returns total bytes or 0.
size_t crsfBuild(uint8_t *out, size_t cap, uint8_t address, uint8_t type,
                 const uint8_t *payload, uint8_t payloadLen);
