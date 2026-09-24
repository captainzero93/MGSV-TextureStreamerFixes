// Code pattern scan over the game exe
#pragma once

#include <cstddef>
#include <cstdint>

// IDA-style pattern, "?" is a wildcard byte. Scans executable sections of the
// game exe. Returns the first match or nullptr; matchCount receives the total.
uint8_t* FindGamePattern(const char* pattern, size_t* matchCount);

// Resolves a rel32 at p to its absolute target (p + 4 + rel32).
uint8_t* ResolveRel32(uint8_t* p);

bool IsInGameImage(const void* p);
