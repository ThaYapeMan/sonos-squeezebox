// Shared Sonos playback position: polled via UPnP in the main loop,
// consumed by the output thread to correct slimproto position reporting.

#include "sonos-position.h"

#include <atomic>
#include <cstdint>

static std::atomic<uint32_t> g_sonos_position_ms{0};

void set_sonos_position_ms(uint32_t ms)
{
    g_sonos_position_ms.store(ms, std::memory_order_relaxed);
}

uint32_t get_sonos_position_ms(void)
{
    return g_sonos_position_ms.load(std::memory_order_relaxed);
}
