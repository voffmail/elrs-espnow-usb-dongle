#pragma once

#include <stddef.h>
#include <stdint.h>

#define ELRS_CFG_MAGIC0 0xAA
#define ELRS_CFG_MAGIC1 0x55
#define ELRS_CFG_VERSION 1
#define ELRS_CFG_MAX_PAYLOAD 128
#define ELRS_CFG_MAX_PHRASE 64

enum : uint8_t {
    ELRS_CFG_GET_CONFIG = 0x01,
    ELRS_CFG_SET_CONFIG = 0x02,
    ELRS_CFG_GET_STATS = 0x03,
    ELRS_CFG_RESET = 0x04,
    ELRS_CFG_PING = 0x05,
    ELRS_CFG_SET_UID = 0x06,
    ELRS_CFG_RESPONSE = 0x80,
};

enum : uint8_t {
    ELRS_CFG_OK = 0,
    ELRS_CFG_ERR_BAD_REQUEST = 1,
    ELRS_CFG_ERR_TOO_LONG = 2,
    ELRS_CFG_ERR_INVALID = 3,
    ELRS_CFG_ERR_BUSY = 4,
};

size_t elrsCfgBuild(uint8_t *out, size_t cap, uint8_t type,
                    const uint8_t *payload, uint16_t payloadLen);

// Incremental parser for the AA 55 framed config protocol.
class ElrsCfgParser {
public:
    ElrsCfgParser();
    void reset();

    // Returns true when a complete valid frame is available. The payload
    // pointer is valid until the next feed()/reset() call.
    bool feed(uint8_t byte, uint8_t &type, const uint8_t *&payload,
              uint16_t &payloadLen);

private:
    uint8_t buf_[ELRS_CFG_MAX_PAYLOAD + 10];
    size_t len_;
};
