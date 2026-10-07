#pragma once

#include "SkyRoofPaths.hpp"

#include <fstream>
#include <initializer_list>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace fsb {

inline std::string trimCopy(const std::string& s) {
    auto a = s.find_first_not_of(" \t");
    if (a == std::string::npos) return {};
    auto b = s.find_last_not_of(" \t");
    return s.substr(a, b - a + 1);
}

inline std::map<std::string, std::string> loadSettingsFile() {
    std::map<std::string, std::string> result;
    std::ifstream in(settingsFilePath());
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        line = trimCopy(line);
        if (line.empty() || line[0] == '#' || line[0] == ';' || line[0] == '[')
            continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        auto key = trimCopy(line.substr(0, eq));
        auto val = trimCopy(line.substr(eq + 1));
        if (key.empty()) continue;
        result[key] = val;
    }
    if (result.count("vantenna") && !result.count("v_antenna"))
        result["v_antenna"] = result["vantenna"];
    if (result.count("uantenna") && !result.count("u_antenna"))
        result["u_antenna"] = result["uantenna"];
    return result;
}

inline std::string settingFromMap(
    const std::map<std::string, std::string>& ini,
    std::initializer_list<const char*> keys,
    const std::string& fallback)
{
    for (auto k : keys) {
        auto it = ini.find(k);
        if (it != ini.end() && !it->second.empty())
            return it->second;
    }
    return fallback;
}

inline void saveSettingsFile(const std::map<std::string, std::string>& settings) {
    static const std::vector<std::pair<const char*, const char*>> kKnown = {
        { "radio",       "FlexRadio IP" },
        { "channel",     "DAX IQ channel (1-8)" },
        { "udpport",     "UDP port for IQ" },
        { "rigctld",     "rigctld RX/downlink port" },
        { "rigctldtx",   "rigctld TX/uplink port" },
        { "rotctldexe",  "Path to rotctld.exe" },
        { "rotctldargs", "rotctld arguments" },
        { "v_antenna",   "V band antenna (2m/VHF)" },
        { "u_antenna",   "U band antenna (70cm/UHF)" },
    };

    std::ofstream out(settingsFilePath(), std::ios::trunc);
    out << "# FlexSkyBridge settings\n";
    out << "# SoapySDR/SkyRoof constructor args override these values when provided.\n";
    out << "#\n";

    std::map<std::string, bool> written;
    for (const auto& kv : kKnown) {
        auto it = settings.find(kv.first);
        out << "# " << kv.second << "\n";
        out << kv.first << "=" << (it != settings.end() ? it->second : "") << "\n";
        written[kv.first] = true;
    }

    bool extras = false;
    for (const auto& kv : settings) {
        if (written.count(kv.first)) continue;
        if (kv.first == "vantenna" || kv.first == "uantenna") continue;
        if (!extras) {
            out << "\n# Other\n";
            extras = true;
        }
        out << kv.first << "=" << kv.second << "\n";
    }
}

inline void persistSetting(const std::string& key, const std::string& value) {
    auto settings = loadSettingsFile();
    settings[key] = value;
    saveSettingsFile(settings);
}

} // namespace fsb
