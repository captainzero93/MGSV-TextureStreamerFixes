// Settings from plugins\TextureStreamer.lua
#include "pch.h"

#include <fstream>

#include "Settings.h"
#include "log.h"

static Settings g_Settings;

const Settings& GetSettings()
{
    return g_Settings;
}

static std::string Trim(const std::string& s)
{
    const size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos)
        return {};
    const size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

static bool ParseBool(const std::string& v, bool& out)
{
    if (v == "true")
        out = true;
    else if (v == "false")
        out = false;
    else
        return false;
    return true;
}

static bool ParseNumber(const std::string& v, double& out)
{
    char* end = nullptr;
    out = strtod(v.c_str(), &end);
    return end && end != v.c_str() && *end == '\0';
}

static void ApplyValue(const std::string& key, const std::string& value)
{
    Settings& s = g_Settings;
    double num = 0.0;
    bool ok = false;

    if (key == "debugWindow")
        ok = ParseBool(value, s.debugWindow);
    else if (key == "debugLog")
        ok = ParseBool(value, s.debugLog);
    else if (key == "patchDispatch")
        ok = ParseBool(value, s.patchDispatch);
    else if (key == "raiseBudget")
        ok = ParseBool(value, s.raiseBudget);
    else if (key == "patchUpgrade")
        ok = ParseBool(value, s.patchUpgrade);
    else if (key == "smallPoolFallback")
        ok = ParseBool(value, s.smallPoolFallback);
    else if (key == "hookPresent")
        ok = ParseBool(value, s.hookPresent);
    else if (key == "hookUpdate")
        ok = ParseBool(value, s.hookUpdate);
    else if (key == "hookGetAvail")
        ok = ParseBool(value, s.hookGetAvail);
    else if (key == "hookRequestConfig")
        ok = ParseBool(value, s.hookRequestConfig);
    else if (key == "lockVramMax")
        ok = ParseBool(value, s.lockVramMax);
    else if (key == "upgradePerFrame" && (ok = ParseNumber(value, num)))
        s.upgradePerFrame = static_cast<int>(num);

    if (!ok)
        Log("[Settings] Ignored %s = %s\n", key.c_str(), value.c_str());
}

void LoadSettings()
{
    char path[MAX_PATH] = {};
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(&LoadSettings), &self);
    GetModuleFileNameA(self, path, MAX_PATH);
    char* slash = strrchr(path, '\\');
    if (slash)
        strcpy_s(slash + 1, MAX_PATH - (slash + 1 - path), "TextureStreamer.lua");

    std::ifstream file(path);
    if (!file)
    {
        Log("[Settings] %s not found, using defaults\n", path);
    }
    else
    {
        std::string line;
        while (std::getline(file, line))
        {
            const size_t comment = line.find("--");
            if (comment != std::string::npos)
                line.erase(comment);
            const size_t eq = line.find('=');
            if (eq == std::string::npos)
                continue;
            const std::string key = Trim(line.substr(0, eq));
            std::string value = Trim(line.substr(eq + 1));
            if (!value.empty() && value.back() == ',')
                value = Trim(value.substr(0, value.size() - 1));
            // skip "local this = {" and similar
            if (key.empty() || key.find_first_of(" \t") != std::string::npos || value == "{")
                continue;
            ApplyValue(key, value);
        }
        Log("[Settings] Loaded %s\n", path);
    }

    Settings& s = g_Settings;
    // original CMP takes imm8, 127 max
    if (s.upgradePerFrame < 16)
        s.upgradePerFrame = 16;
    if (s.upgradePerFrame > 127)
        s.upgradePerFrame = 127;

    Log("[Settings] debugWindow=%d debugLog=%d raiseBudget=%d patchDispatch=%d patchUpgrade=%d smallPoolFallback=%d lockVramMax=%d upgradePerFrame=%d "
        "hookPresent=%d hookUpdate=%d hookGetAvail=%d hookRequestConfig=%d\n",
        (int)s.debugWindow,
        (int)s.debugLog,
        (int)s.raiseBudget,
        (int)s.patchDispatch,
        (int)s.patchUpgrade,
        (int)s.smallPoolFallback,
        (int)s.lockVramMax,
        s.upgradePerFrame,
        (int)s.hookPresent,
        (int)s.hookUpdate,
        (int)s.hookGetAvail,
        (int)s.hookRequestConfig);
}
