#pragma once

#include <cstdlib>
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

inline void debugLog(const std::string& msg) {
    std::ofstream log(debugLogPath(), std::ios::app);
    log << msg << "\n";
}

} // namespace fsb
