#include "config_protocol.h"

#include "elrs_crc.h"
#include <string.h>

static void dropFirst(uint8_t *buf, size_t &len) {
    if (len <= 1) {
        len = 0;
        return;
    }
    memmove(buf, buf + 1, len - 1);
    len--;
}

size_t elrsCfgBuild(uint8_t *out, size_t cap, uint8_t type,
                    const uint8_t *payload, uint16_t payloadLen) {
    if (!out || payloadLen > ELRS_CFG_MAX_PAYLOAD) return 0;
    size_t total = 6 + (size_t)payloadLen + 2;
    if (cap < total) return 0;

    out[0] = ELRS_CFG_MAGIC0;
    out[1] = ELRS_CFG_MAGIC1;
    out[2] = ELRS_CFG_VERSION;
    out[3] = type;
    out[4] = (uint8_t)(payloadLen & 0xFF);
    out[5] = (uint8_t)((payloadLen >> 8) & 0xFF);
    if (payloadLen && payload) {
        memcpy(out + 6, payload, payloadLen);
    }
    uint16_t crc = elrsCrc16(out + 2, 4 + (size_t)payloadLen);
    out[6 + payloadLen] = (uint8_t)(crc & 0xFF);
    out[7 + payloadLen] = (uint8_t)((crc >> 8) & 0xFF);
    return total;
}

ElrsCfgParser::ElrsCfgParser() : len_(0) {}

void ElrsCfgParser::reset() {
    len_ = 0;
}

bool ElrsCfgParser::feed(uint8_t byte, uint8_t &type,
                         const uint8_t *&payload, uint16_t &payloadLen) {
    if (len_ >= sizeof(buf_)) {
        dropFirst(buf_, len_);
    }
    buf_[len_++] = byte;

    while (len_ >= 2) {
        if (buf_[0] != ELRS_CFG_MAGIC0 || buf_[1] != ELRS_CFG_MAGIC1) {
            dropFirst(buf_, len_);
            continue;
        }
        if (len_ < 6) return false;
        if (buf_[2] != ELRS_CFG_VERSION) {
            dropFirst(buf_, len_);
            continue;
        }

        uint16_t size = (uint16_t)buf_[4] | ((uint16_t)buf_[5] << 8);
        if (size > ELRS_CFG_MAX_PAYLOAD) {
            dropFirst(buf_, len_);
            continue;
        }
        size_t total = 6 + (size_t)size + 2;
        if (len_ < total) return false;

        uint16_t expected = (uint16_t)buf_[6 + size] |
                            ((uint16_t)buf_[7 + size] << 8);
        uint16_t actual = elrsCrc16(buf_ + 2, 4 + (size_t)size);
        if (expected != actual) {
            dropFirst(buf_, len_);
            continue;
        }

        type = buf_[3];
        payload = buf_ + 6;
        payloadLen = size;

        if (len_ > total) {
            memmove(buf_, buf_ + total, len_ - total);
        }
        len_ -= total;
        return true;
    }
    return false;
}
