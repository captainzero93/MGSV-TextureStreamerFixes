// Frame stall and streamer usage logging
#pragma once

#include <cstdint>

// Call around the original streamer update, on the update thread.
void StallMonitor_UpdateBegin(uint8_t* streamer);
void StallMonitor_UpdateEnd(uint8_t* streamer);

// Call once per Present, on the render thread.
void StallMonitor_OnPresent();
