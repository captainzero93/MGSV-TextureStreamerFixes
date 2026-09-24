// File and console logging
#pragma once
#include <cstdio>
#include <cstdarg>

void InitLog();
void Log(const char* fmt, ...);
void CrashLogf(const char* fmt, ...);
void CloseLog();

// Raw Win32 append to TextureStreamer_boot.log beside the exe. No CRT, safe in DllMain.
// Off until SetBootTrace(true), so DllMain steps are never written.
void BootTrace(const char* step, bool truncate = false);
void SetBootTrace(bool on);
