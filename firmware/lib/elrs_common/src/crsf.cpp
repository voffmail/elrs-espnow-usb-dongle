#include "crsf.h"

#include "elrs_crc.h"
#include <string.h>

size_t crsfBuild(uint8_t *out, size_t cap, uint8_t address, uint8_t type,
                 const uint8_t *payload, uint8_t payloadLen) {
    if (!out || payloadLen > CRSF_MAX_PAYLOAD) return 0;
    size_t total = (size_t)payloadLen + 4;
    if (cap < total) return 0;

    out[0] = address;
    out[1] = (uint8_t)(payloadLen + 2);
    out[2] = type;
    if (payloadLen && payload) {
        memcpy(out + 3, payload, payloadLen);
    }
    out[3 + payloadLen] = elrsCrc8(out + 2, (size_t)payloadLen + 1);
    return total;
}
