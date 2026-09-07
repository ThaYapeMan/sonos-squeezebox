#ifndef SONOS_POSITION_H
#define SONOS_POSITION_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Actual Sonos playback position polled via UPnP AVTransport GetPositionInfo.
// Written by the main loop (C++), read by the output thread (C).
// Returns 0 when unknown (before first successful poll or at stream start).
uint32_t get_sonos_position_ms(void);
void set_sonos_position_ms(uint32_t ms);

#ifdef __cplusplus
}
#endif

#endif /* SONOS_POSITION_H */
