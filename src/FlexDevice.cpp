#include "FlexDevice.hpp"
#include "SkyRoofPaths.hpp"
#include "SettingsIni.hpp"
#include <SoapySDR/Logger.hpp>
#include <SoapySDR/Formats.hpp>
#include <SoapySDR/Types.hpp>
#include <stdexcept>
#include <string>
#include <iostream>
#include <sstream>
#include <map>

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>

static void dbg(const std::string& msg) {
    fsb::debugLog(msg);
}

static std::string resolveSetting(
    const SoapySDR::Kwargs& args,
    const std::map<std::string, std::string>& ini,
    std::initializer_list<const char*> keys,
    const std::string& fallback)
{
    for (auto k : keys) {
        auto it = args.find(k);
        if (it != args.end() && !it->second.empty())
            return it->second;
    }
    return fsb::settingFromMap(ini, keys, fallback);
}

static int parseIntSetting(const std::string& value, int fallback) {
    try {
        return std::stoi(value);
    } catch (...) {
        return fallback;
    }
}

static void persistSetting(const std::string& key, const std::string& value) {
    fsb::persistSetting(key, value);
}

static void persistAllSettings(const std::string& radio,
                               int channel, int udpPort,
                               int rigctld, int rigctldTx,
                               const std::string& rotctldExe,
                               const std::string& rotctldArgs,
                               const std::string& vAntenna,
                               const std::string& uAntenna) {
    auto settings = fsb::loadSettingsFile();
    settings["radio"]       = radio;
    settings["channel"]     = std::to_string(channel);
    settings["udpport"]     = std::to_string(udpPort);
    settings["rigctld"]     = std::to_string(rigctld);
    settings["rigctldtx"]   = std::to_string(rigctldTx);
    settings["rotctldexe"]  = rotctldExe;
    settings["rotctldargs"] = rotctldArgs;
    settings["v_antenna"]   = vAntenna;
    settings["u_antenna"]   = uAntenna;
    fsb::saveSettingsFile(settings);
}

FlexDevice::FlexDevice(const SoapySDR::Kwargs& args) {
    dbg("=== Constructor llamado ===");

    // Prioridad: args SoapySDR/SkyRoof > INI > valores por defecto.
    // Precedence: SoapySDR/SkyRoof args > INI > defaults.
    auto ini = fsb::loadSettingsFile();
    radioIP_       = resolveSetting(args, ini, {"radio"}, "192.168.0.208");
    daxChannel_    = parseIntSetting(resolveSetting(args, ini, {"channel"}, "1"), 1);
    udpPort_       = static_cast<uint16_t>(parseIntSetting(
        resolveSetting(args, ini, {"udpport"}, "7891"), 7891));
    rigctldPort_   = static_cast<uint16_t>(parseIntSetting(
        resolveSetting(args, ini, {"rigctld"}, "4532"), 4532));
    rigctldTxPort_ = static_cast<uint16_t>(parseIntSetting(
        resolveSetting(args, ini, {"rigctldtx"}, "4534"), 4534));
    rotctldExe_    = resolveSetting(args, ini, {"rotctldexe"}, rotctldExe_);
    rotctldArgs_   = resolveSetting(args, ini, {"rotctldargs"}, rotctldArgs_);
    vBandAntenna_  = resolveSetting(args, ini, {"vantenna", "v_antenna"}, vBandAntenna_);
    uBandAntenna_  = resolveSetting(args, ini, {"uantenna", "u_antenna"}, uBandAntenna_);

    persistAllSettings(radioIP_, daxChannel_, udpPort_, rigctldPort_, rigctldTxPort_,
                       rotctldExe_, rotctldArgs_, vBandAntenna_, uBandAntenna_);

    dbg("radioIP=" + radioIP_ + " canal=" + std::to_string(daxChannel_) +
        " udpPort=" + std::to_string(udpPort_) +
        " rigctldPort=" + std::to_string(rigctldPort_) +
        " rigctldTxPort=" + std::to_string(rigctldTxPort_) +
        " rotctldExe=" + rotctldExe_ +
        " rotctldArgs=" + rotctldArgs_ +
        " vAntenna=" + vBandAntenna_ +
        " uAntenna=" + uBandAntenna_);

    smartsdr_    = std::make_unique<SmartSDRClient>();
    daxReceiver_ = std::make_unique<DaxIQReceiver>();
    rigctld_     = std::make_unique<RigCtldServer>();
    rigctldTx_   = std::make_unique<RigCtldServer>();

    smartsdr_->setBandAntennas(vBandAntenna_, uBandAntenna_);

    smartsdr_->onFrequencyChanged([this](double hz) {
        currentFreqHz_ = hz;
        rigctld_->setCurrentFreq(hz);
    });
    smartsdr_->onTxFrequencyChanged([this](double hz) {
        currentTxFreqHz_ = hz;
        rigctldTx_->setCurrentFreq(hz);
    });
    smartsdr_->onTxModeChanged([this](const std::string& mode) {
        currentTxMode_ = mode;
        rigctldTx_->setCurrentMode(mode);
    });
    smartsdr_->onPttChanged([this](bool ptt) {
        // SkyRoof abre dos conexiones rigctld (RX 4532 y TX 4534) y puede
        // consultar get_ptt en cualquiera de las dos, asi que ambas deben
        // reflejar el mismo estado real de PTT del radio.
        // SkyRoof opens two rigctld connections (RX 4532 and TX 4534) and may
        // poll get_ptt on either one, so both must reflect the same real PTT
        // state of the radio.
        rigctld_->setCurrentPtt(ptt);
        rigctldTx_->setCurrentPtt(ptt);
    });
    dbg("Constructor completado OK");
}

FlexDevice::~FlexDevice() {
    dbg("=== Destructor llamado ===");
    rigctld_->stop();
    rigctldTx_->stop();
    if (streaming_.load()) daxReceiver_->stop();
    if (smartsdr_->isConnected()) smartsdr_->disconnect();
    dbg("Destructor completado");
}

SoapySDR::Kwargs FlexDevice::getHardwareInfo() const {
    SoapySDR::Kwargs info;
    info["driver"]  = "FlexSkyBridge";
    info["hardware"]= "FLEX-6600";
    info["backend"] = "FlexSkyBridge-native";
    info["model"]   = "FLEX-6600";
    info["radio"]   = radioIP_;
    return info;
}

size_t FlexDevice::getNumChannels(const int dir) const {
    return (dir == SOAPY_SDR_RX || dir == SOAPY_SDR_TX) ? 1 : 0;
}

std::vector<std::string> FlexDevice::listAntennas(const int dir,
                                                   const size_t ch) const {
    return { "ANT1", "ANT2", "RX_A", "RX_B", "XVTA", "XVTB" };
}

// SkyRoof solo consulta/establece la antena del canal RX (confirmado en el
// log de depuración: nunca llega un setAntenna(SOAPY_SDR_TX, ...)). El canal
// TX no tiene stream SoapySDR real (setupStream rechaza TX), así que ese
// selector de UI nunca se dispara para TX. Por tanto usamos el único campo
// disponible (antena de RX) para editar la antena de la banda a la que esté
// sintonizado el RX en cada momento — V (2m) o U (70cm) — en vez de partir
// el ajuste en dos canales que SkyRoof no expone.
//
// SkyRoof only queries/sets the RX channel antenna (confirmed in the debug
// log: a setAntenna(SOAPY_SDR_TX, ...) never arrives). The TX channel has no
// real SoapySDR stream (setupStream rejects TX), so that UI selector is never
// triggered for TX. We therefore use the only available field (RX antenna)
// to edit the antenna of the band the RX is currently tuned to — V (2m) or
// U (70cm) — instead of splitting the setting across two channels that
// SkyRoof does not expose.
void FlexDevice::setAntenna(const int dir, const size_t ch, const std::string& name) {
    double freqMHz = currentFreqHz_ / 1e6;
    bool isVBand = (freqMHz >= 144.0 && freqMHz < 148.0);

    if (isVBand) {
        vBandAntenna_ = name;
        persistSetting("v_antenna", name);
        dbg("setAntenna(banda V, RX en 2m)=" + name);
    } else {
        uBandAntenna_ = name;
        persistSetting("u_antenna", name);
        dbg("setAntenna(banda U, RX en 70cm)=" + name);
    }
    smartsdr_->setBandAntennas(vBandAntenna_, uBandAntenna_);
}

std::string FlexDevice::getAntenna(const int dir, const size_t ch) const {
    double freqMHz = currentFreqHz_ / 1e6;
    bool isVBand = (freqMHz >= 144.0 && freqMHz < 148.0);
    return isVBand ? vBandAntenna_ : uBandAntenna_;
}

// Settings — todos los parámetros persistidos en el INI /
// all parameters persisted in the INI
SoapySDR::ArgInfoList FlexDevice::getSettingInfo(void) const {
    SoapySDR::ArgInfoList infos;
    auto antennaOptions = listAntennas(SOAPY_SDR_RX, 0);

    auto addStr = [&](const char* key, const std::string& value,
                      const char* name, const char* description) {
        SoapySDR::ArgInfo info;
        info.key         = key;
        info.value       = value;
        info.name        = name;
        info.description = description;
        info.type        = SoapySDR::ArgInfo::STRING;
        infos.push_back(info);
    };
    auto addInt = [&](const char* key, int value,
                      const char* name, const char* description) {
        SoapySDR::ArgInfo info;
        info.key         = key;
        info.value       = std::to_string(value);
        info.name        = name;
        info.description = description;
        info.type        = SoapySDR::ArgInfo::INT;
        infos.push_back(info);
    };

    addStr("radio", radioIP_, "Radio IP",
           "FlexRadio IP address. Applied on the next stream start.");
    addInt("channel", daxChannel_, "DAX IQ channel",
           "DAX IQ channel (1-8). Applied on the next stream start.");
    addInt("udpport", udpPort_, "IQ UDP port",
           "UDP port for IQ. Applied on the next stream start.");
    addInt("rigctld", rigctldPort_, "rigctld RX port",
           "rigctld RX/downlink port. Applied on the next stream start.");
    addInt("rigctldtx", rigctldTxPort_, "rigctld TX port",
           "rigctld TX/uplink port. Applied on the next stream start.");
    addStr("rotctldexe", rotctldExe_, "rotctld executable",
           "Path to rotctld.exe. Applied on the next stream start.");
    addStr("rotctldargs", rotctldArgs_, "rotctld arguments",
           "Arguments passed to rotctld. Applied on the next stream start.");

    SoapySDR::ArgInfo vAnt;
    vAnt.key         = "v_antenna";
    vAnt.value       = vBandAntenna_;
    vAnt.name        = "V band antenna (2m)";
    vAnt.description = "Antenna/transverter used when the slice frequency is in the V (2m/VHF) band.";
    vAnt.type        = SoapySDR::ArgInfo::STRING;
    vAnt.options     = antennaOptions;
    infos.push_back(vAnt);

    SoapySDR::ArgInfo uAnt;
    uAnt.key         = "u_antenna";
    uAnt.value       = uBandAntenna_;
    uAnt.name        = "U band antenna (70cm)";
    uAnt.description = "Antenna/transverter used when the slice frequency is in the U (70cm/UHF) band.";
    uAnt.type        = SoapySDR::ArgInfo::STRING;
    uAnt.options     = antennaOptions;
    infos.push_back(uAnt);

    return infos;
}

void FlexDevice::writeSetting(const std::string& key, const std::string& value) {
    if (key == "radio") {
        radioIP_ = value;
        persistSetting("radio", value);
    } else if (key == "channel") {
        daxChannel_ = parseIntSetting(value, daxChannel_);
        persistSetting("channel", std::to_string(daxChannel_));
    } else if (key == "udpport") {
        udpPort_ = static_cast<uint16_t>(parseIntSetting(value, udpPort_));
        persistSetting("udpport", std::to_string(udpPort_));
    } else if (key == "rigctld") {
        rigctldPort_ = static_cast<uint16_t>(parseIntSetting(value, rigctldPort_));
        persistSetting("rigctld", std::to_string(rigctldPort_));
    } else if (key == "rigctldtx") {
        rigctldTxPort_ = static_cast<uint16_t>(parseIntSetting(value, rigctldTxPort_));
        persistSetting("rigctldtx", std::to_string(rigctldTxPort_));
    } else if (key == "rotctldexe") {
        rotctldExe_ = value;
        persistSetting("rotctldexe", value);
    } else if (key == "rotctldargs") {
        rotctldArgs_ = value;
        persistSetting("rotctldargs", value);
    } else if (key == "v_antenna" || key == "vantenna") {
        vBandAntenna_ = value;
        smartsdr_->setBandAntennas(vBandAntenna_, uBandAntenna_);
        persistSetting("v_antenna", value);
    } else if (key == "u_antenna" || key == "uantenna") {
        uBandAntenna_ = value;
        smartsdr_->setBandAntennas(vBandAntenna_, uBandAntenna_);
        persistSetting("u_antenna", value);
    } else {
        return;
    }
    dbg("writeSetting " + key + "=" + value);
}

std::string FlexDevice::readSetting(const std::string& key) const {
    if (key == "radio") return radioIP_;
    if (key == "channel") return std::to_string(daxChannel_);
    if (key == "udpport") return std::to_string(udpPort_);
    if (key == "rigctld") return std::to_string(rigctldPort_);
    if (key == "rigctldtx") return std::to_string(rigctldTxPort_);
    if (key == "rotctldexe") return rotctldExe_;
    if (key == "rotctldargs") return rotctldArgs_;
    if (key == "v_antenna" || key == "vantenna") return vBandAntenna_;
    if (key == "u_antenna" || key == "uantenna") return uBandAntenna_;
    return "";
}

// Sample rates — soportamos los rates que ofrece el DAX IQ de FlexRadio / we support the rates offered by FlexRadio DAX IQ
std::vector<double> FlexDevice::listSampleRates(const int dir,
                                                 const size_t ch) const {
    return { 48000.0, 96000.0, 192000.0 };
}

SoapySDR::RangeList FlexDevice::getSampleRateRange(const int dir,
                                                    const size_t ch) const {
    return { SoapySDR::Range(48000.0, 192000.0) };
}

void FlexDevice::setSampleRate(const int dir, const size_t ch,
                                const double rate) {
    // Aceptar el rate que pide SkyRoof (48000, 96000 o 192000 Hz)
    // El radio y el dispositivo WASAPI se configuran en startDaxIQStream
    // Accept the rate SkyRoof asks for (48000, 96000 or 192000 Hz)
    // The radio and the WASAPI device are configured in startDaxIQStream
    if (rate == 48000.0 || rate == 96000.0 || rate == 192000.0)
        currentSampleRate_ = rate;
    else
        currentSampleRate_ = 192000.0;  // default si pide algo fuera de rango / default if it asks for something out of range
    dbg("setSampleRate=" + std::to_string(currentSampleRate_));
}

double FlexDevice::getSampleRate(const int dir, const size_t ch) const {
    return currentSampleRate_;  // siempre 48000 / always 48000
}

// Frecuencia / Frequency
void FlexDevice::setFrequency(const int dir, const size_t ch,
                               const std::string& name, const double freq,
                               const SoapySDR::Kwargs& args) {
    static int freqCallCount = 0;
    ++freqCallCount;

    if (dir == SOAPY_SDR_TX) {
        dbg("setFrequency(TX) #" + std::to_string(freqCallCount) +
            " freq=" + std::to_string(freq / 1e6) + " MHz" +
            " connected=" + (smartsdr_->isConnected() ? "yes" : "NO"));

        currentTxFreqHz_ = freq;
        if (smartsdr_->isConnected())
            smartsdr_->setTxFrequency(freq);
        else
            dbg("setFrequency(TX): radio NO conectado, frecuencia descartada");
        return;
    }

    dbg("setFrequency #" + std::to_string(freqCallCount) +
        " freq=" + std::to_string(freq / 1e6) + " MHz" +
        " connected=" + (smartsdr_->isConnected() ? "yes" : "NO"));

    currentFreqHz_ = freq;
    if (smartsdr_->isConnected())
        smartsdr_->setSliceFrequency(0, freq);
    else
        dbg("setFrequency: radio NO conectado, frecuencia descartada");
}

double FlexDevice::getFrequency(const int dir, const size_t ch,
                                 const std::string& name) const {
    if (dir == SOAPY_SDR_TX)
        return smartsdr_->isConnected() ? smartsdr_->getTxFrequency() : currentTxFreqHz_;
    return currentFreqHz_;
}

std::vector<std::string> FlexDevice::listFrequencies(const int dir,
                                                       const size_t ch) const {
    return { "RF" };
}

SoapySDR::RangeList FlexDevice::getFrequencyRange(const int dir,
                                                    const size_t ch,
                                                    const std::string& name) const {
    return { SoapySDR::Range(10000.0, 6000000000.0) };
}

// Ganancia / Gain
std::vector<std::string> FlexDevice::listGains(const int dir,
                                                const size_t ch) const {
    return { "RF" };
}


SoapySDR::Range FlexDevice::getGainRange(const int dir,
                                          const size_t ch) const {
    return SoapySDR::Range(0.0, 100.0, 1.0);
}

// Ancho de banda / Bandwidth
double FlexDevice::getBandwidth(const int dir, const size_t ch) const {
    return getSampleRate(dir, ch);
}

std::vector<double> FlexDevice::listBandwidths(const int dir,
                                                const size_t ch) const {
    return { 48000.0, 96000.0, 192000.0 };
}

SoapySDR::RangeList FlexDevice::getBandwidthRange(const int dir,
                                                   const size_t ch) const {
    return { SoapySDR::Range(48000.0, 192000.0) };
}

// rotctld (proceso externo hamlib / external hamlib process)
void FlexDevice::startRotctld() {
    if (rotctldExe_.empty()) return;

    // Matar instancia previa si quedara huérfana / Kill previous instance if left orphaned
    stopRotctld();

    std::string cmdLine = "\"" + rotctldExe_ + "\" " + rotctldArgs_;
    dbg("Arrancando rotctld: " + cmdLine);

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;   // sin ventana visible / no visible window

    if (!CreateProcessA(nullptr,
                        const_cast<char*>(cmdLine.c_str()),
                        nullptr, nullptr, FALSE,
                        CREATE_NEW_PROCESS_GROUP,
                        nullptr, nullptr,
                        &si, &rotctldProc_)) {
        dbg("WARN: no se pudo arrancar rotctld (error " +
            std::to_string(GetLastError()) + ")");
        rotctldProc_ = { nullptr, nullptr, 0, 0 };
    } else {
        dbg("rotctld arrancado PID=" + std::to_string(rotctldProc_.dwProcessId));
    }
}

void FlexDevice::stopRotctld() {
    if (rotctldProc_.hProcess == nullptr) return;
    dbg("Deteniendo rotctld PID=" + std::to_string(rotctldProc_.dwProcessId));
    TerminateProcess(rotctldProc_.hProcess, 0);
    WaitForSingleObject(rotctldProc_.hProcess, 2000);
    CloseHandle(rotctldProc_.hProcess);
    CloseHandle(rotctldProc_.hThread);
    rotctldProc_ = { nullptr, nullptr, 0, 0 };
    dbg("rotctld detenido");
}

// Streaming IQ / IQ streaming
SoapySDR::Stream* FlexDevice::setupStream(const int dir,
                                           const std::string& format,
                                           const std::vector<size_t>& channels,
                                           const SoapySDR::Kwargs& args) {
    dbg("setupStream llamado, formato=" + format +
        " dir=" + std::to_string(dir));
    if (dir != SOAPY_SDR_RX) {
        dbg("ERROR: solo RX soportado");
        throw std::runtime_error("FlexSkyBridge: solo RX soportado");
    }
    if (format != SOAPY_SDR_CF32) {
        dbg("ERROR: formato no soportado: " + format);
        throw std::runtime_error("FlexSkyBridge: usa CF32, recibido: " + format);
    }
    dbg("setupStream OK");
    return reinterpret_cast<SoapySDR::Stream*>(this);
}

int FlexDevice::activateStream(SoapySDR::Stream* stream,
                                const int flags,
                                const long long timeNs,
                                const size_t numElems) {
    dbg("activateStream llamado");
    if (streaming_.exchange(true)) {
        dbg("activateStream: ya activo");
        return 0;
    }

    try {
        if (!smartsdr_->isConnected()) {
            dbg("Conectando a " + radioIP_);
            smartsdr_->connect(radioIP_);
            std::ostringstream hs;
            hs << std::hex << smartsdr_->clientHandle();
            dbg("Conectado OK. Handle=0x" + hs.str());
        }

        // Arrancar rotctld (hamlib) para control de rotor / Start rotctld (hamlib) for rotator control
        startRotctld();

        // Arrancar servidor rigctld para recibir frecuencias Doppler de SkyRoof
        // Start rigctld server to receive Doppler frequencies from SkyRoof
        dbg("Arrancando rigctld en puerto " + std::to_string(rigctldPort_));
        rigctld_->start(rigctldPort_,
            [this](double freqHz) {
                dbg("rigctld set_freq: " + std::to_string(freqHz / 1e6) + " MHz");
                currentFreqHz_ = freqHz;
                rigctld_->setCurrentFreq(freqHz);
                if (smartsdr_->isConnected())
                    smartsdr_->setSliceFrequency(0, freqHz);
            },
            [this](const std::string& mode, int /*passband*/) {
                dbg("rigctld set_mode: " + mode);
                currentMode_ = mode;
                if (smartsdr_->isConnected())
                    smartsdr_->setSliceMode(0, mode);           
            });

        // Arrancar un segundo servidor rigctld INDEPENDIENTE dedicado solo al
        // uplink (TX). SkyRoof (y la mayoría de trackers satelitales) esperan
        // una segunda conexión CAT "normal" (F/f) para el TX, no split-VFO
        // sobre la misma conexión de RX.
        // Start a second, INDEPENDENT rigctld server dedicated to the uplink
        // (TX). SkyRoof (and most satellite trackers) expect a second "normal"
        // CAT connection (F/f) for TX, not split-VFO over the same RX connection.
        dbg("Arrancando rigctld TX en puerto " + std::to_string(rigctldTxPort_));
        rigctldTx_->start(rigctldTxPort_,
            [this](double freqHz) {
                dbg("rigctld TX set_freq: " + std::to_string(freqHz / 1e6) + " MHz");
                currentTxFreqHz_ = freqHz;
                rigctldTx_->setCurrentFreq(freqHz);
                if (smartsdr_->isConnected())
                    smartsdr_->setTxFrequency(freqHz);
            },
            [this](const std::string& mode, int /*passband*/) {
                dbg("rigctld TX set_mode: " + mode);
                currentTxMode_ = mode;
                if (smartsdr_->isConnected())
                    smartsdr_->setTxMode(mode);
            },
            [this](bool ptt) {
                dbg(std::string("rigctld TX set_ptt: ") + (ptt ? "ON" : "OFF"));
                if (smartsdr_->isConnected())
                    smartsdr_->setPtt(ptt);
            },
            [this](double toneHz, bool enabled) {
                dbg("rigctld TX ctcss: " + std::to_string(toneHz) + " Hz " +
                    (enabled ? "ON" : "OFF"));
                if (smartsdr_->isConnected())
                    smartsdr_->setTxCtcss(toneHz, enabled);
            });

        // Arrancar receptor UDP primero / Start the UDP receiver first
        dbg("Arrancando DAX receiver en puerto " + std::to_string(udpPort_));
        daxReceiver_->start(udpPort_, "DAX IQ RX 1");
        dbg("DAX receiver OK en puerto " + std::to_string(udpPort_));

        // Secuencia completa de flexlib-go con rate correcto
        // Full flexlib-go sequence with the correct rate
        smartsdr_->startDaxIQStream(daxChannel_, udpPort_,
                                     static_cast<int>(currentSampleRate_));
        dbg("startDaxIQStream completado");

        // Aplicar modo actual al radio una vez conectado / Apply current mode to the radio once connected
        if (smartsdr_->isConnected()) {
            smartsdr_->setSliceMode(0, currentMode_);
            smartsdr_->setTxMode(currentTxMode_);
        }

    } catch (const std::exception& e) {
        dbg("EXCEPCION en activateStream: " + std::string(e.what()));
        streaming_ = false;
        return SOAPY_SDR_STREAM_ERROR;
    }

    dbg("activateStream OK");
    SoapySDR::log(SOAPY_SDR_INFO, "[FlexSkyBridge] Stream activado");
    return 0;
}

int FlexDevice::deactivateStream(SoapySDR::Stream* stream,
                                  const int flags,
                                  const long long timeNs) {
    dbg("deactivateStream llamado");
    if (!streaming_.exchange(false)) return 0;
    stopRotctld();
    rigctld_->stop();
    rigctldTx_->stop();
    daxReceiver_->stop();
    if (smartsdr_->isConnected()) smartsdr_->disconnect();
    dbg("deactivateStream OK");
    return 0;
}

void FlexDevice::closeStream(SoapySDR::Stream* stream) {
    dbg("closeStream llamado");
    deactivateStream(stream);
}

size_t FlexDevice::getStreamMTU(SoapySDR::Stream* stream) const {
    return 1024;
}

int FlexDevice::readStream(SoapySDR::Stream* stream,
                            void* const* buffs,
                            const size_t numElems,
                            int& flags,
                            long long& timeNs,
                            const long timeoutUs) {
    if (!streaming_.load()) return SOAPY_SDR_NOT_SUPPORTED;

    float* out       = static_cast<float*>(buffs[0]);
    int    timeoutMs = static_cast<int>(timeoutUs / 1000);
    if (timeoutMs < 1) timeoutMs = 1;

    static int rCount = 0;
    if (rCount++ < 5)
        dbg("readStream: pedidas=" + std::to_string(numElems) +
            " disponibles=" + std::to_string(daxReceiver_->available()));

    int read = daxReceiver_->read(out, static_cast<int>(numElems), timeoutMs);
    if (read == 0) return SOAPY_SDR_TIMEOUT;

    flags  = 0;
    timeNs = 0;
    return read;
}
