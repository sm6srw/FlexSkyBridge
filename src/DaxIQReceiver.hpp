#pragma once
#include <cstdint>
#include <thread>
#include <atomic>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <string>

// DaxIQReceiver — recibe paquetes VITA-49 UDP directamente del radio FlexRadio
//
// El radio envía float32 LE interleaved I,Q en paquetes VITA-49 (cabecera 28
// bytes fija para streams DAX IQ). No se usa WASAPI ni el driver DAX de Windows.
//
// DaxIQReceiver — receives VITA-49 UDP packets directly from the FlexRadio.
//
// The radio sends float32 LE interleaved I,Q in VITA-49 packets (fixed 28-byte
// header for DAX IQ streams). Neither WASAPI nor the Windows DAX driver is used.

class DaxIQReceiver {
public:
    static constexpr int RING_CAPACITY = 262144;  // 256k muestras IQ / 256k IQ samples

    DaxIQReceiver();
    ~DaxIQReceiver();

    // udpPort: puerto UDP donde el radio enviará los paquetes VITA-49
    // udpPort: UDP port where the radio will send the VITA-49 packets
    void start(uint16_t udpPort, const std::string& unused = "");
    void stop();

    bool isRunning() const { return running_.load(); }

    uint32_t sampleRate() const { return sampleRate_.load(); }

    // Espera hasta tener exactamente maxSamples muestras (como SoapyFlexRadio)
    // Waits until exactly maxSamples samples are available (like SoapyFlexRadio)
    int  read(float* dest, int maxSamples, int timeoutMs = 500);
    int  available() const;
    void flush();

private:
    void captureLoop();

    uint16_t                  udpPort_{ 7891 };
    std::atomic<bool>         running_{ false };
    std::thread               captureThread_;

    // Ring buffer CF32 (I,Q intercalados — 2 floats por muestra)
    // CF32 ring buffer (interleaved I,Q — 2 floats per sample)
    std::vector<float>        ring_;
    size_t                    writePos_{ 0 };
    size_t                    readPos_{ 0 };
    std::atomic<int>          count_{ 0 };

    mutable std::mutex        ringMutex_;
    std::condition_variable   dataReady_;

    std::atomic<uint32_t>     sampleRate_{ 192000 };
};
