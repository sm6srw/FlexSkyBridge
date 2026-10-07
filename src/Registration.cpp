#include <SoapySDR/Registry.hpp>
#include <SoapySDR/Device.hpp>
#include "FlexDevice.hpp"
#include "SkyRoofPaths.hpp"
#include "SettingsIni.hpp"

static void dbgReg(const std::string& msg) {
    fsb::debugLog(msg);
}

static std::vector<SoapySDR::Kwargs> findFlex(const SoapySDR::Kwargs& args) {
    dbgReg("=== findFlex llamado ===");

    std::vector<SoapySDR::Kwargs> results;

    auto ini = fsb::loadSettingsFile();
    auto radio   = fsb::settingFromMap(ini, {"radio"}, "192.168.0.208");
    auto channel = fsb::settingFromMap(ini, {"channel"}, "1");
    auto udpport = fsb::settingFromMap(ini, {"udpport"}, "7891");

    if (args.count("radio")) {
        SoapySDR::Kwargs dev;
        dev["driver"]  = "FlexSkyBridge";
        dev["label"]   = "Flex 6600 via FlexSkyBridge (" + args.at("radio") + ")";
        dev["radio"]   = args.at("radio");
        dev["channel"] = args.count("channel") ? args.at("channel") : channel;
        dev["udpport"] = args.count("udpport") ? args.at("udpport") : udpport;
        results.push_back(dev);
        return results;
    }

    SoapySDR::Kwargs dev;
    dev["driver"]  = "FlexSkyBridge";
    dev["label"]   = "Flex 6600 via FlexSkyBridge (" + radio + ")";
    dev["radio"]   = radio;
    dev["channel"] = channel;
    dev["udpport"] = udpport;
    results.push_back(dev);

    return results;
}

static SoapySDR::Device* makeFlex(const SoapySDR::Kwargs& args) {
    return new FlexDevice(args);
}

static SoapySDR::Registry registerFlex(
    "FlexSkyBridge",
    &findFlex,
    &makeFlex,
    SOAPY_SDR_ABI_VERSION
);
