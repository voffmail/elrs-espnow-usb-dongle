#pragma once

#include <stdint.h>

enum class LinkSource : uint8_t {
    None = 0,
    Crsf = 1,
    Mavlink = 2,
};

// Protocol-independent link state for the status LED. A CRSF parser fills it
// today; a MAVLink parser can fill the same structure later without touching
// the LED logic.
struct LinkStatus {
    bool known;       // the payload protocol provides link state
    bool up;          // vehicle/telemetry link is alive
    uint8_t quality;  // 0..100, if available
    uint32_t lastMs;  // when the state was last updated
    LinkSource source;
};

extern LinkStatus g_linkStatus;
