#pragma once
#include <SoapySDR/Device.hpp>
#include <SoapySDR/Registry.hpp>
#include <memory>
#include <string>
#include <atomic>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "SmartSDRClient.hpp"
#include "DaxIQReceiver.hpp"
#include "RigCtldServer.hpp"

class FlexDevice : public SoapySDR::Device {
public:
    explicit FlexDevice(const SoapySDR::Kwargs& args);
    ~FlexDevice() override;

    // Identificación / Identification
    std::string getDriverKey()   const override { return "FlexSkyBridge"; }
    std::string getHardwareKey() const override { return "FLEX-6600";     }
    SoapySDR::Kwargs getHardwareInfo() const override;

    // Canales / Channels
    size_t getNumChannels(const int dir) const override;

    // Antenas / Antennas
    // Panadapter 1 (RX) arranca en modo V/U como RX en 70cm/UHF, así que su
    // selector de antena controla qué transverter se usa para la banda U.
    // Panadapter 2 (TX) arranca como TX en 2m/VHF, así que su selector
    // controla la banda V. Estos mismos valores son los que
    // SmartSDRClient::applyAntennaForBand usa automáticamente al detectar la
    // banda de la frecuencia sintonizada en cada slice.
    //
    // Panadapter 1 (RX) starts in V/U mode as RX on 70cm/UHF, so its antenna
    // selector controls which transverter is used for the U band.
    // Panadapter 2 (TX) starts as TX on 2m/VHF, so its selector controls the
    // V band. These same values are what SmartSDRClient::applyAntennaForBand
    // uses automatically when it detects the band of the frequency tuned on
    // each slice.
    std::vector<std::string> listAntennas(const int dir,
                                          const size_t ch) const override;
    void        setAntenna(const int dir, const size_t ch,
                           const std::string& name) override;
    std::string getAntenna(const int dir, const size_t ch) const override;

    // Sample rate
    void   setSampleRate(const int dir, const size_t ch,
                         const double rate) override;
    double getSampleRate(const int dir, const size_t ch) const override;
    std::vector<double> listSampleRates(const int dir,
                                        const size_t ch) const override;
    SoapySDR::RangeList getSampleRateRange(const int dir,
                                           const size_t ch) const override;

    // Frecuencia / Frequency
    void   setFrequency(const int dir, const size_t ch,
                        const std::string& name, const double freq,
                        const SoapySDR::Kwargs& args) override;
    double getFrequency(const int dir, const size_t ch,
                        const std::string& name) const override;
    std::vector<std::string> listFrequencies(const int dir,
                                             const size_t ch) const override;
    SoapySDR::RangeList getFrequencyRange(const int dir,
                                          const size_t ch,
                                          const std::string& name) const override;

    // Ganancia / Gain
    std::vector<std::string> listGains(const int dir,
                                       const size_t ch) const override;
    bool hasGainMode(const int dir, const size_t ch) const override { return false; }
    void setGainMode(const int dir, const size_t ch,
                     const bool automatic) override {}
    bool getGainMode(const int dir, const size_t ch) const override { return false; }
    void   setGain(const int dir, const size_t ch,
                   const double value) override {}
    double getGain(const int dir, const size_t ch) const override { return 0.0; }
    SoapySDR::Range getGainRange(const int dir,
                                 const size_t ch) const override;

    // Ancho de banda / Bandwidth
    void   setBandwidth(const int dir, const size_t ch,
                        const double bw) override {}
    double getBandwidth(const int dir, const size_t ch) const override;
    std::vector<double> listBandwidths(const int dir,
                                       const size_t ch) const override;
    SoapySDR::RangeList getBandwidthRange(const int dir,
                                          const size_t ch) const override;

    // Streaming IQ / IQ streaming
    SoapySDR::Stream* setupStream(const int dir,
                                  const std::string& format,
                                  const std::vector<size_t>& channels,
                                  const SoapySDR::Kwargs& args) override;

    int activateStream(SoapySDR::Stream* stream,
                       const int flags = 0,
                       const long long timeNs = 0,
                       const size_t numElems = 0) override;

    int deactivateStream(SoapySDR::Stream* stream,
                         const int flags = 0,
                         const long long timeNs = 0) override;

    void   closeStream(SoapySDR::Stream* stream) override;
    size_t getStreamMTU(SoapySDR::Stream* stream) const override;

    int readStream(SoapySDR::Stream* stream,
                   void* const* buffs,
                   const size_t numElems,
                   int& flags,
                   long long& timeNs,
                   const long timeoutUs) override;
                   
    // DC Offset Mode
    bool hasDCOffsetMode(const int dir, const size_t ch) const override { return true; }
    void setDCOffsetMode(const int dir, const size_t ch,
                     const bool automatic) override {}
    bool getDCOffsetMode(const int dir, const size_t ch) const override { return true; }

    // Settings persistidos en FlexSkyBridge_settings.ini (carpeta de datos de
    // SkyRoof). Los args del constructor tienen prioridad sobre el INI.
    // Settings persisted in FlexSkyBridge_settings.ini (SkyRoof data folder).
    // Constructor args take priority over the INI.
    SoapySDR::ArgInfoList getSettingInfo(void) const override;
    void        writeSetting(const std::string& key, const std::string& value) override;
    std::string readSetting(const std::string& key) const override;


private:
    void startRotctld();
    void stopRotctld();

    std::string radioIP_;
    int         daxChannel_{ 1 };
    uint16_t    udpPort_   { 7891 };

    double      currentFreqHz_    { 435e6 };
    double      currentSampleRate_{ 96000.0 };
    std::string currentAntenna_   { "ANT1" };
    std::string currentMode_      { "USB" };
    double      currentTxFreqHz_  { 145e6 };
    std::string currentTxMode_    { "USB" };

    // Antenas por banda (V/U) / Per-band antennas (V/U)
    // Punto de partida hardcoded para un transpondedor V/U: panadapter 1 (RX)
    // recibe en 70cm/UHF, panadapter 2 (TX) transmite en 2m/VHF.
    // Hardcoded starting point for a V/U transponder: panadapter 1 (RX)
    // receives on 70cm/UHF, panadapter 2 (TX) transmits on 2m/VHF.
    std::string vBandAntenna_{ "XVTA" };
    std::string uBandAntenna_{ "XVTB" };

    std::unique_ptr<SmartSDRClient> smartsdr_;
    std::unique_ptr<DaxIQReceiver>  daxReceiver_;
    std::unique_ptr<RigCtldServer>  rigctld_;
    // Servidor rigctld independiente para TX — SkyRoof (y la mayoría de
    // trackers satelitales) esperan una segunda conexión CAT simple para el
    // uplink en vez de split-VFO sobre la misma conexión.
    // Independent rigctld server for TX — SkyRoof (and most satellite
    // trackers) expect a second plain CAT connection for the uplink instead of
    // split-VFO over the same connection.
    std::unique_ptr<RigCtldServer>  rigctldTx_;

    uint16_t          rigctldPort_{ 4532 };
    uint16_t          rigctldTxPort_{ 4534 };
    std::atomic<bool> streaming_{ false };

    // rotctld (control de rotor via hamlib / rotator control via hamlib)
    std::string        rotctldExe_ { "C:\\hamlib\\bin\\rotctld.exe" };
    std::string        rotctldArgs_{ "-m 3 -r 127.0.0.1:4533" };
    PROCESS_INFORMATION rotctldProc_{ nullptr, nullptr, 0, 0 };
};
