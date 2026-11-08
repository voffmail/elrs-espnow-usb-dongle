#include "io_port.h"

#ifndef ELRS_UART_BAUD
#define ELRS_UART_BAUD 115200
#endif

static Stream *s_port = nullptr;

void ioBegin() {
#ifdef ELRS_DATA_USB
    Serial.begin(115200);
    s_port = &Serial;
#else
    Serial0.begin(ELRS_UART_BAUD, SERIAL_8N1, 20, 21);
    s_port = &Serial0;
#endif
}

size_t ioWrite(const uint8_t *data, size_t len) {
    if (!s_port || !data || !len) return 0;
    return s_port->write(data, len);
}

int ioRead() {
    if (!s_port) return -1;
    return s_port->read();
}

size_t ioAvailable() {
    if (!s_port) return 0;
    return (size_t)s_port->available();
}

bool ioHostConnected() {
#ifdef ELRS_DATA_USB
    return (bool)Serial;
#else
    return true;
#endif
}
