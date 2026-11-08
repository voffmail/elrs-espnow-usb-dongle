#include "msp_v2.h"

#include "elrs_crc.h"
#include <string.h>

size_t mspV2Build(uint8_t *out, size_t cap, uint8_t flag, uint16_t function,
                  const uint8_t *payload, uint16_t payloadLen) {
    if (!out || payloadLen > MSP_V2_MAX_PAYLOAD) return 0;
    size_t total = MSP_V2_HEADER_LEN + (size_t)payloadLen + 1;
    if (cap < total) return 0;

    out[0] = MSP_V2_MAGIC0;
    out[1] = MSP_V2_MAGIC1;
    out[2] = MSP_V2_MAGIC2;
    out[3] = flag;
    out[4] = (uint8_t)(function & 0xFF);
    out[5] = (uint8_t)((function >> 8) & 0xFF);
    out[6] = (uint8_t)(payloadLen & 0xFF);
    out[7] = (uint8_t)((payloadLen >> 8) & 0xFF);
    if (payloadLen && payload) {
        memcpy(out + MSP_V2_HEADER_LEN, payload, payloadLen);
    }
    out[MSP_V2_HEADER_LEN + payloadLen] =
        elrsCrc8(out + 3, 5 + (size_t)payloadLen);
    return total;
}

bool mspV2Parse(const uint8_t *data, size_t len, uint8_t &flag,
                uint16_t &function, const uint8_t *&payload,
                uint16_t &payloadLen) {
    if (!data || len < MSP_V2_HEADER_LEN + 1) return false;
    if (data[0] != MSP_V2_MAGIC0 || data[1] != MSP_V2_MAGIC1 ||
        data[2] != MSP_V2_MAGIC2) {
        return false;
    }

    uint16_t size = (uint16_t)data[6] | ((uint16_t)data[7] << 8);
    if (size > MSP_V2_MAX_PAYLOAD) return false;
    size_t total = MSP_V2_HEADER_LEN + (size_t)size + 1;
    if (len < total) return false;

    uint8_t expected = elrsCrc8(data + 3, 5 + (size_t)size);
    if (expected != data[MSP_V2_HEADER_LEN + size]) return false;

    flag = data[3];
    function = (uint16_t)data[4] | ((uint16_t)data[5] << 8);
    payload = data + MSP_V2_HEADER_LEN;
    payloadLen = size;
    return true;
}
