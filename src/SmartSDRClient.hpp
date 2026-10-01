#pragma once
#include <string>
#include <thread>
#include <atomic>
#include <functional>
#include <mutex>
#include <cstdint>

class SmartSDRClient {
public:
    using FreqCallback = std::function<void(double freqHz)>;
    using PortCallback = std::function<void(uint16_t port)>;
    using ModeCallback = std::function<void(const std::string& mode)>;
    using PttCallback  = std::function<void(bool ptt)>;

    SmartSDRClient();
    ~SmartSDRClient();

    void connect(const std::string& radioIP, uint16_t port = 4992);
    void disconnect();
    bool isConnected() const { return connected_.load(); }

    uint32_t clientHandle()  const { return clientHandle_; }
    uint32_t daxIQStreamId() const { return daxIQStreamId_.load(); }

    void setSliceFrequency(int sliceIdx, double freqHz);
    void setSliceMode(int sliceIdx, const std::string& mode);
    std::string getSliceMode() const;
    void startDaxIQStream(int channel, uint16_t udpPort, int sampleRate);
    void removeDaxIQStream(uint32_t streamID);

    // Antenas RX/TX
    // La antena se decide autom�ticamente seg�n la banda de la frecuencia
    // sintonizada en cada slice: banda VHF (2m) usa la antena configurada para
    // modo V, banda UHF (70cm) usa la configurada para modo U. As�, al cambiar
    // entre un transpondedor V/U y uno U/V, RX y TX intercambian de antena
    // solo con los cambios de frecuencia que ya llegan desde SkyRoof.
    // Los valores por defecto son XVTA (modo V) y XVTB (modo U); son
    // configurables desde los ajustes de SoapySDR (ver FlexDevice).
    //
    // The antenna is chosen automatically from the band of the frequency tuned
    // on each slice: the VHF band (2m) uses the antenna configured for V mode,
    // the UHF band (70cm) uses the one configured for U mode. So when switching
    // between a V/U and a U/V transponder, RX and TX swap antennas just from
    // the frequency changes already arriving from SkyRoof.
    // Defaults are XVTA (V mode) and XVTB (U mode); they are configurable from
    // the SoapySDR settings (see FlexDevice).
    void setBandAntennas(const std::string& vAntenna, const std::string& uAntenna);

    // TX � el slice/panadapter de TX es independiente del de RX
    // El slice/panadapter de TX es independiente del de RX.
    // El �ndice de slice TX se autodetecta del status "slice <n> ... tx=1"
    // The TX slice/panadapter is independent from the RX one.
    // The TX slice index is auto-detected from the "slice <n> ... tx=1" status.
    void   setTxFrequency(double freqHz);
    double getTxFrequency() const;
    void   setTxMode(const std::string& mode);
    std::string getTxMode() const;
    bool   hasTxSlice() const { return txSliceIdx_.load() >= 0; }

    // PTT
    // Activa/desactiva la transmisi�n del slice TX ("xmit" en el protocolo
    // SmartSDR). El estado real de PTT tambi�n puede cambiar por control local
    // en la radio, por lo que se refleja v�a onPttChanged() al recibir el
    // status "transmit ... transmit=<0|1>" del radio.
    //
    // Enables/disables transmission of the TX slice ("xmit" in the SmartSDR
    // protocol). The real PTT state can also change through local control on
    // the radio, so it is reflected via onPttChanged() when the radio's
    // "transmit ... transmit=<0|1>" status is received.
    void setPtt(bool ptt);

    // CTCSS en TX: fija fm_tone_mode/fm_tone_value en el slice TX.
    // TX CTCSS: sets fm_tone_mode/fm_tone_value on the TX slice.
    void setTxCtcss(double toneHz, bool enabled);
    bool getPtt() const { return pttActive_.load(); }

    void onFrequencyChanged(FreqCallback cb)   { freqCallback_ = std::move(cb); }
    void onDaxIQPort(PortCallback cb)          { daxIQPortCallback_ = std::move(cb); }
    void onModeChanged(ModeCallback cb)        { modeCallback_ = std::move(cb); }
    void onTxFrequencyChanged(FreqCallback cb) { txFreqCallback_ = std::move(cb); }
    void onTxModeChanged(ModeCallback cb)      { txModeCallback_ = std::move(cb); }
    void onPttChanged(PttCallback cb)          { pttCallback_ = std::move(cb); }

private:
    void sendCommand(const std::string& cmd);
    void receiveLoop();
    void parseLine(const std::string& line);

    // Selecci�n autom�tica de antena por banda
    // Banda VHF (2m, ~144-148 MHz) ? antena configurada para modo V.
    // Banda UHF (70cm, ~430-440 MHz) ? antena configurada para modo U.
    // Se aplica a cada slice cada vez que se sintoniza una frecuencia, de
    // forma que al cambiar entre un transpondedor V/U y uno U/V la antena de
    // RX y TX se intercambia autom�ticamente.
    //
    // VHF band (2m, ~144-148 MHz) -> antenna configured for V mode.
    // UHF band (70cm, ~430-440 MHz) -> antenna configured for U mode.
    // Applied to each slice every time a frequency is tuned, so when switching
    // between a V/U and a U/V transponder the RX and TX antennas are swapped
    // automatically.
    void applyAntennaForBand(int sliceIdx, double freqHz, bool isTx);

    std::string           vBandAntenna_{ "XVTA" };
    std::string           uBandAntenna_{ "XVTB" };

    intptr_t              sock_{ -1 };
    std::atomic<bool>     connected_{ false };
    std::thread           rxThread_;
    std::mutex            sendMutex_;

    uint32_t              clientHandle_{ 0 };
    std::atomic<uint32_t> seqNum_{ 1 };
    std::atomic<uint32_t> daxIQStreamId_{ 0 };
    std::atomic<uint32_t> existingDaxIQStreamId_{ 0 };

    std::string           firstClientId_;
    std::string           firstPanId_;

    FreqCallback          freqCallback_;
    PortCallback          daxIQPortCallback_;
    ModeCallback          modeCallback_;

    mutable std::mutex    modeMutex_;
    std::string           currentMode_{ "USB" };

    // Estado del slice TX (segundo panadapter)
    // TX slice state (second panadapter) / Estado del slice TX (segundo panadapter)
    std::atomic<int>      txSliceIdx_{ -1 };
    std::string           txPanId_;
    std::atomic<double>   currentTxFreqHz_{ 145e6 };
    FreqCallback          txFreqCallback_;

    // Antenas RX/TX aplicadas � evita reenviar el mismo comando repetido
    // Applied RX/TX antennas: avoids resending the same command repeatedly
    // (Antenas RX/TX aplicadas: evita reenviar el mismo comando)
    std::string           rxAntennaApplied_;
    std::string           txAntennaApplied_;

    mutable std::mutex    txModeMutex_;
    std::string           currentTxMode_{ "USB" };
    ModeCallback          txModeCallback_;

    // Estado de PTT
    // PTT state / Estado de PTT
    std::atomic<bool>     pttActive_{ false };
    std::string           lastCtcssCmd_;
    PttCallback           pttCallback_;

    std::string           radioIP_;
};
