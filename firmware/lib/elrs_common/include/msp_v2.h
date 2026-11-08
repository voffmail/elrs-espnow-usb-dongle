#pragma once

#include <stddef.h>
#include <stdint.h>

#define MSP_V2_MAGIC0 0x24
#define MSP_V2_MAGIC1 0x58
#define MSP_V2_MAGIC2 0x3C
#define MSP_V2_HEADER_LEN 8
#define MSP_V2_MAX_PAYLOAD 248

size_t mspV2Build(uint8_t *out, size_t cap, uint8_t flag, uint16_t function,
                  const uint8_t *payload, uint16_t payloadLen);

bool mspV2Parse(const uint8_t *data, size_t len, uint8_t &flag,
                uint16_t &function, const uint8_t *&payload,
                uint16_t &payloadLen);
