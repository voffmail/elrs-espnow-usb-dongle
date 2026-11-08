#pragma once

#include <Arduino.h>

void ioBegin();
size_t ioWrite(const uint8_t *data, size_t len);
int ioRead();
size_t ioAvailable();

// True when a USB host has the data/control port open. For UART0/CP2102N
// the ESP cannot see the bridge's DTR line, so assume a host is present.
bool ioHostConnected();
