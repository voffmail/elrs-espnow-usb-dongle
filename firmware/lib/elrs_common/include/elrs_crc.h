#pragma once

#include <stddef.h>
#include <stdint.h>

// CRC8 DVB-S2 (poly 0xD5, init 0), used by CRSF and MSP v2.
uint8_t elrsCrc8(const uint8_t *data, size_t len);

// CRC16-CCITT-FALSE (poly 0x1021, init 0xFFFF), used by the config protocol.
uint16_t elrsCrc16(const uint8_t *data, size_t len);
