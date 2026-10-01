#pragma once
#include <string>
#include <thread>
#include <atomic>
#include <functional>
#include <vector>
#include <mutex>
#include <cstdint>

// Minimal Hamlib rigctld-compatible TCP server.
// SkyRoof (and any Hamlib client) connects here and sends frequency updates
// every ~250ms via the CAT Rx interface. We forward each F command directly
// to the Flex slice via the provided callback.
class RigCtldServer {
public:
    using FreqCallback = std::function<void(double freqHz)>;
    using ModeCallback = std::function<void(const std::string& mode, int passband)>;
    using PttCallback  = std::function<void(bool ptt)>;
    using CtcssCallback = std::function<void(double toneHz, bool enabled)>;

    RigCtldServer() = default;
    ~RigCtldServer() { stop(); }

    void start(uint16_t port, FreqCallback onSetFreq, ModeCallback onSetMode = nullptr,
               PttCallback onSetPtt = nullptr, CtcssCallback onSetCtcss = nullptr);
    void stop();
    void setCurrentFreq(double freqHz) { currentFreq_ = freqHz; }
    void setCurrentMode(const std::string& mode, int passband = 0);
    void setCurrentPtt(bool ptt) { currentPtt_ = ptt; }

private:
    void listenLoop();
    void handleClient(uintptr_t clientSock);

    uint16_t       port_{ 4532 };
    uintptr_t      listenSock_{ (uintptr_t)-1 };
    std::atomic<bool>   running_{ false };
    std::thread         listenThread_;
    FreqCallback        onSetFreq_;
    ModeCallback        onSetMode_;
    PttCallback         onSetPtt_;
    CtcssCallback       onSetCtcss_;
    std::atomic<int>    ctcssTenthsHz_{ 0 };
    std::atomic<bool>   ctcssEnabled_{ false };
    std::atomic<double> currentFreq_{ 145e6 };
    std::atomic<bool>   splitEnabled_{ false };
    std::atomic<bool>   currentPtt_{ false };

    std::mutex          modeMutex_;
    std::string         currentMode_{ "USB" };
    int                 currentPassband_{ 3000 };

    std::vector<std::thread> clientThreads_;
    std::mutex               clientMutex_;
};
