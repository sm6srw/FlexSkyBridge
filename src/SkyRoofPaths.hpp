#pragma once

#include <cstdlib>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <string>

// Rutas en la carpeta de datos de SkyRoof:
//   %APPDATA%/Afreet/Products/SkyRoof
// Paths in SkyRoof's data folder:
//   %APPDATA%/Afreet/Products/SkyRoof
namespace fsb {

inline std::filesystem::path skyRoofDataDir() {
    static std::filesystem::path cached;
    if (!cached.empty()) return cached;

    const char* appdata = std::getenv("APPDATA");
    if (appdata && *appdata) {
        cached = std::filesystem::path(appdata) / "Afreet" / "Products" / "SkyRoof";
    } else {
        cached = "C:\\RADIO";
    }

    std::error_code ec;
    std::filesystem::create_directories(cached, ec);
    return cached;
}

inline std::filesystem::path settingsFilePath() {
    auto path = skyRoofDataDir() / "FlexSkyBridge_settings.ini";
    static const bool migrated = [] {
        std::error_code ec;
        const char* legacy = "C:\\RADIO\\FlexSkyBridge_settings.ini";
        auto dest = skyRoofDataDir() / "FlexSkyBridge_settings.ini";
        if (!std::filesystem::exists(dest, ec)
            && std::filesystem::exists(legacy, ec)) {
            std::filesystem::copy_file(legacy, dest, ec);
        }
        return true;
    }();
    (void)migrated;
    return path;
}

inline std::filesystem::path debugLogPath() {
    return skyRoofDataDir() / "FlexSkyBridge_debug.log";
}

inline bool logToDiskEnabled() {
#ifdef FLEXSKYBRIDGE_LOG_TO_DISK
    std::ifstream in(settingsFilePath());
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        auto key = line.substr(0, eq);
        if (key == "logtodisk" || key == "log_to_disk" || key == "enablelogtodisk") {
            auto value = line.substr(eq + 1);
            for (char& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            value.erase(0, value.find_first_not_of(" \t"));
            value.erase(value.find_last_not_of(" \t") + 1);
            return value == "1" || value == "true" || value == "yes" || value == "on" || value == "enabled";
        }
    }
#endif
    return false;
}

inline void debugLog(const std::string& msg) {
    if (!logToDiskEnabled()) return;
    std::ofstream log(debugLogPath(), std::ios::app);
    log << msg << "\n";
}

} // namespace fsb
