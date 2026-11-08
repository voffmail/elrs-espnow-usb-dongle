#include "elrs_uid.h"

#include <esp_mac.h>
#include <mbedtls/md5.h>
#include <stdio.h>
#include <string.h>

static const char BIND_PREFIX[] = "-DMY_BINDING_PHRASE=\"";

void elrsUidFromPhrase(const char *phrase, uint8_t uid[6]) {
    mbedtls_md5_context ctx;
    uint8_t digest[16];

    mbedtls_md5_init(&ctx);
    mbedtls_md5_update_ret(&ctx, (const unsigned char *)BIND_PREFIX,
                           sizeof(BIND_PREFIX) - 1);
    if (phrase && phrase[0]) {
        mbedtls_md5_update_ret(&ctx, (const unsigned char *)phrase,
                               strlen(phrase));
    }
    mbedtls_md5_update_ret(&ctx, (const unsigned char *)"\"", 1);
    mbedtls_md5_finish_ret(&ctx, digest);
    mbedtls_md5_free(&ctx);

    memcpy(uid, digest, 6);
    uid[0] &= (uint8_t)~0x01;
}

void elrsUidFromFactory(uint8_t uid[6]) {
    esp_read_mac(uid, ESP_MAC_WIFI_STA);
    uid[0] &= (uint8_t)~0x01;
}

void elrsUidCompute(const char *phrase, uint8_t uid[6]) {
    if (phrase && phrase[0]) {
        elrsUidFromPhrase(phrase, uid);
    } else {
        elrsUidFromFactory(uid);
    }
}

String elrsUidToString(const uint8_t uid[6]) {
    char buf[18];
    snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
             uid[0], uid[1], uid[2], uid[3], uid[4], uid[5]);
    return String(buf);
}
