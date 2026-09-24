// File and console logging
#include "pch.h"

#include "log.h"
#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>

static FILE* g_LogFile = nullptr;

// TextureStreamer.log beside the game exe, previous run kept as TextureStreamer.prev.log
void InitLog()
{
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    char* lastSlash = strrchr(path, '\\');
    if (lastSlash)
        *(lastSlash + 1) = '\0';

    char prevPath[MAX_PATH];
    strcpy_s(prevPath, path);
    strcat_s(prevPath, "TextureStreamer.prev.log");
    strcat_s(path, "TextureStreamer.log");
    MoveFileExA(path, prevPath, MOVEFILE_REPLACE_EXISTING);

    fopen_s(&g_LogFile, path, "w");
    if (g_LogFile)
        fprintf(g_LogFile, "[LOG] Log file created successfully.\n");
}

void Log(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);

    vprintf(fmt, args);

    if (g_LogFile)
    {
        va_list args2;
        va_start(args2, fmt);
        vfprintf(g_LogFile, fmt, args2);
        va_end(args2);
        fflush(g_LogFile);
    }

    va_end(args);
}

void CrashLogf(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);

    if (g_LogFile)
    {
        va_list args2;
        va_start(args2, fmt);
        vfprintf(g_LogFile, fmt, args2);
        va_end(args2);
        fflush(g_LogFile);
    }
}

void CloseLog()
{
    if (g_LogFile)
    {
        fprintf(g_LogFile, "[LOG] Closing log.\n");
        fclose(g_LogFile);
        g_LogFile = nullptr;
    }
}

static volatile bool g_BootTraceOn = false;

void SetBootTrace(bool on)
{
    g_BootTraceOn = on;
}

void BootTrace(const char* step, bool truncate)
{
    if (!g_BootTraceOn)
        return;
    char path[MAX_PATH];
    const DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
    DWORD cut = n;
    while (cut > 0 && path[cut - 1] != '\\')
        --cut;
    const char name[] = "TextureStreamer_boot.log";
    if (cut + sizeof(name) > MAX_PATH)
        return;
    memcpy(path + cut, name, sizeof(name));

    HANDLE f = CreateFileA(
        path,
        truncate ? GENERIC_WRITE : FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        truncate ? CREATE_ALWAYS : OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return;

    // tick, thread id, step; no wsprintf/CRT so it is safe under loader lock
    char line[256];
    int len = 0;
    auto putNum = [&](unsigned long v)
    {
        char tmp[16];
        int t = 0;
        do
        {
            tmp[t++] = static_cast<char>('0' + v % 10);
            v /= 10;
        } while (v && t < 16);
        while (t && len < 200)
            line[len++] = tmp[--t];
    };
    putNum(GetTickCount());
    for (const char* s = " tid="; *s; ++s)
        line[len++] = *s;
    putNum(GetCurrentThreadId());
    line[len++] = ' ';
    for (const char* s = step; *s && len < 250; ++s)
        line[len++] = *s;
    line[len++] = '\r';
    line[len++] = '\n';
    DWORD written = 0;
    WriteFile(f, line, (DWORD)len, &written, nullptr);
    CloseHandle(f);
}
