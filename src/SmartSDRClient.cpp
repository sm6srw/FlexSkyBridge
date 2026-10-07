#include "SmartSDRClient.hpp"
#include "SkyRoofPaths.hpp"

#include <stdexcept>
#include <sstream>
#include <iostream>
#include <iomanip>
#include <string>
#include <cstring>
#include <thread>
#include <chrono>

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>

namespace {
    struct WinsockInit {
        WinsockInit() {
            WSADATA wsa{};
            if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
                throw std::runtime_error("WSAStartup failed");
        }
        ~WinsockInit() { WSACleanup(); }
    };
    WinsockInit& winsockInstance() {
        static WinsockInit inst;
        return inst;
    }

    // Traducción de nombres de modo Hamlib ↔ Flex
    // Los clientes CAT (SkyRoof, rigctl, etc.) usan la nomenclatura de Hamlib
    // (p. ej. "FM_D" para FM digital, "USB_D"/"LSB_D" para USB/LSB digital),
    // mientras que el radio Flex espera sus propios nombres de modo de slice
    // (p. ej. "DFM", "DIGU", "DIGL"). Traducimos en ambas direcciones para que
    // el modo se refleje correctamente en ambos lados.
    // Hamlib <-> Flex mode name translation.
    // CAT clients (SkyRoof, rigctl, etc.) use Hamlib naming (e.g. "FM_D" for digital FM,
    // "USB_D"/"LSB_D" for digital USB/LSB), while the Flex radio expects its own
    // slice mode names (e.g. "DFM", "DIGU", "DIGL"). We translate in both directions
    // so the mode is reflected correctly on both sides.
    std::string hamlibToFlexMode(const std::string& mode) {
        if (mode == "FM_D")  return "DFM";
        if (mode == "USB_D") return "DIGU";
        if (mode == "LSB_D") return "DIGL";
        return mode;
    }

    std::string flexToHamlibMode(const std::string& mode) {
        if (mode == "DFM")  return "FM_D";
        if (mode == "DIGU") return "USB_D";
        if (mode == "DIGL") return "LSB_D";
        return mode;
    }
}

static void dbgSdr(const std::string& msg) {
    fsb::debugLog(msg);
}

SmartSDRClient::SmartSDRClient() {
    winsockInstance();
}

SmartSDRClient::~SmartSDRClient() {
    disconnect();
}

void SmartSDRClient::connect(const std::string& radioIP, uint16_t port) {
    if (connected_.load()) disconnect();

    radioIP_       = radioIP;
    firstClientId_ = "";
    firstPanId_    = "";
    clientHandle_  = 0;
    daxIQStreamId_ = 0;

    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET)
        throw std::runtime_error("socket() failed");

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    if (inet_pton(AF_INET, radioIP.c_str(), &addr.sin_addr) != 1) {
        closesocket(s);
        throw std::runtime_error("IP invalida: " + radioIP);
    }

    if (::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        closesocket(s);
        throw std::runtime_error("No se pudo conectar a " + radioIP + ":4992");
    }

    sock_      = static_cast<intptr_t>(s);
    connected_ = true;
    rxThread_  = std::thread(&SmartSDRClient::receiveLoop, this);

    // Esperar handle (hasta 2 segundos)
    // Wait for handle (up to 2 seconds)
    for (int i = 0; i < 200 && clientHandle_ == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    sendCommand("sub slice all");
    sendCommand("sub radio all");
    sendCommand("sub daxiq all");
    sendCommand("sub display all");
    sendCommand("sub client all");
    sendCommand("sub tx all");
    // El estado real de PTT (incluyendo MOX/mic local, no solo nuestro xmit)
    // se reporta via el mensaje de status "interlock", campo state=. El
    // mensaje "transmit" (sub tx all) no trae un campo transmit=0/1 util
    // para esto -- por eso necesitamos esta suscripcion adicional.
    // The real PTT state (including MOX/local mic, not just our xmit) is reported
    // via the "interlock" status message, field state=. The "transmit" message
    // (sub tx all) carries no useful transmit=0/1 field for this -- hence this
    // additional subscription.
    sendCommand("sub interlock all");
}

void SmartSDRClient::disconnect() {
    if (!connected_.exchange(false)) return;

    SOCKET s = static_cast<SOCKET>(sock_);
    shutdown(s, SD_BOTH);
    closesocket(s);
    sock_ = -1;

    if (rxThread_.joinable()) rxThread_.join();
}

// Control de frecuencia — mueve el centro del panadapter
// SkyRoof llama esto con la frecuencia Doppler corregida
// Frequency control - moves the panadapter center.
// SkyRoof calls this with the Doppler-corrected frequency.
void SmartSDRClient::setSliceFrequency(int sliceIdx, double freqHz) {
    double freqMHz = freqHz / 1e6;

    static int tuneCount = 0;
    ++tuneCount;

    applyAntennaForBand(sliceIdx, freqHz, false);

    // slice tune mueve el slice Y el pan, y funciona entre clientes
    // slice tune moves both the slice AND the pan, and works across clients
    std::ostringstream cmd;
    cmd << "slice tune " << sliceIdx
        << " " << std::fixed << std::setprecision(6) << freqMHz
        << " autopan=1";

    // Log completo solo las primeras 5 veces, luego cada 20
    // Full log only the first 5 times, then every 20th
    if (tuneCount <= 5 || tuneCount % 20 == 0)
        dbgSdr("slice tune #" + std::to_string(tuneCount) +
               ": " + cmd.str());

    sendCommand(cmd.str());
}

// Control de modo — USB/LSB/CW/AM/FM/DIGU/DIGL/etc.
// Mode control - USB/LSB/CW/AM/FM/DIGU/DIGL/etc.
void SmartSDRClient::setSliceMode(int sliceIdx, const std::string& mode) {
    std::ostringstream cmd;
    cmd << "slice set " << sliceIdx << " mode=" << hamlibToFlexMode(mode);

    dbgSdr("slice set mode: " + cmd.str());
    sendCommand(cmd.str());

    std::lock_guard<std::mutex> lock(modeMutex_);
    currentMode_ = mode;
}

std::string SmartSDRClient::getSliceMode() const {
    std::lock_guard<std::mutex> lock(modeMutex_);
    return currentMode_;
}

// Control de frecuencia TX — segundo slice/panadapter independiente del de RX
// TX frequency control - second slice/panadapter, independent from the RX one
void SmartSDRClient::setTxFrequency(double freqHz) {
    int idx = txSliceIdx_.load();
    if (idx < 0) {
        dbgSdr("WARN: setTxFrequency sin slice TX detectado, frecuencia descartada");
        return;
    }

    applyAntennaForBand(idx, freqHz, true);

    double freqMHz = freqHz / 1e6;
    std::ostringstream cmd;
    cmd << "slice tune " << idx
        << " " << std::fixed << std::setprecision(6) << freqMHz
        << " autopan=1";

    dbgSdr("slice tune (TX) idx=" + std::to_string(idx) + ": " + cmd.str());
    sendCommand(cmd.str());

    currentTxFreqHz_ = freqHz;
}

double SmartSDRClient::getTxFrequency() const {
    return currentTxFreqHz_.load();
}

// Control de modo TX — segundo slice/panadapter independiente del de RX
// TX mode control - second slice/panadapter, independent from the RX one
void SmartSDRClient::setTxMode(const std::string& mode) {
    int idx = txSliceIdx_.load();
    if (idx < 0) {
        dbgSdr("WARN: setTxMode sin slice TX detectado, modo descartado");
        return;
    }

    std::ostringstream cmd;
    cmd << "slice set " << idx << " mode=" << hamlibToFlexMode(mode);

    dbgSdr("slice set mode (TX): " + cmd.str());
    sendCommand(cmd.str());

    std::lock_guard<std::mutex> lock(txModeMutex_);
    currentTxMode_ = mode;
}

std::string SmartSDRClient::getTxMode() const {
    std::lock_guard<std::mutex> lock(txModeMutex_);
    return currentTxMode_;
}

// CTCSS TX: "slice set <idx> fm_tone_mode=CTCSS_TX fm_tone_value=<Hz>" o
// fm_tone_mode=OFF. Solo tiene efecto con el slice TX en modo FM/NFM/DFM.
// TX CTCSS: "slice set <idx> fm_tone_mode=CTCSS_TX fm_tone_value=<Hz>" or
// fm_tone_mode=OFF. Only effective with the TX slice in FM/NFM/DFM mode.
void SmartSDRClient::setTxCtcss(double toneHz, bool enabled) {
    int idx = txSliceIdx_.load();
    if (idx < 0) {
        dbgSdr("WARN: setTxCtcss sin slice TX detectado, descartado");
        return;
    }

    std::ostringstream cmd;
    cmd << "slice set " << idx;
    if (enabled && toneHz > 0.0)
        cmd << " fm_tone_mode=CTCSS_TX fm_tone_value="
            << std::fixed << std::setprecision(1) << toneHz;
    else
        cmd << " fm_tone_mode=OFF";

    if (cmd.str() == lastCtcssCmd_) return;
    lastCtcssCmd_ = cmd.str();

    dbgSdr("CTCSS TX: " + cmd.str());
    sendCommand(cmd.str());
}

// Control de PTT - "xmit <0|1>" activa/desactiva la transmision en el radio.
// El comando xmit es global (no por slice); el radio transmite en el slice
// que tenga el flag "tx=1" marcado como activo, que por defecto suele ser el
// slice RX (A) y no nuestro slice TX (segundo panadapter). Por eso, antes de
// transmitir, forzamos "slice set <idx> tx=1" en nuestro slice TX para que el
// radio realmente transmita ahi - si no, transmite en el slice equivocado y
// SkyRoof ve el PTT caer casi al instante porque el estado no cuadra.
// PTT control - "xmit <0|1>" enables/disables transmission on the radio.
// The xmit command is global (not per slice); the radio transmits on the slice
// that has the "tx=1" flag active, which by default is usually the RX slice (A)
// and not our TX slice (second panadapter). Therefore, before transmitting, we
// force "slice set <idx> tx=1" on our TX slice so the radio really transmits
// there - otherwise it transmits on the wrong slice and SkyRoof sees PTT drop
// almost instantly because the state does not match.
void SmartSDRClient::setPtt(bool ptt) {
    int idx = txSliceIdx_.load();
    if (ptt && idx >= 0) {
        std::ostringstream txCmd;
        txCmd << "slice set " << idx << " tx=1";
        dbgSdr("slice set tx=1 (marcar slice TX activo): " + txCmd.str());
        sendCommand(txCmd.str());
    }

    std::ostringstream cmd;
    cmd << "xmit " << (ptt ? 1 : 0);

    dbgSdr(std::string("xmit: ") + cmd.str());
    sendCommand(cmd.str());

    pttActive_ = ptt;
}

// Configura qué antena/transverter usar para cada banda (modo V y modo U).
// Persistido/expuesto por FlexDevice a través de la settings API de SoapySDR.
// Configures which antenna/transverter to use for each band (V mode and U mode).
// Persisted/exposed by FlexDevice through the SoapySDR settings API.
void SmartSDRClient::setBandAntennas(const std::string& vAntenna, const std::string& uAntenna) {
    if (!vAntenna.empty()) vBandAntenna_ = vAntenna;
    if (!uAntenna.empty()) uBandAntenna_ = uAntenna;
    // Forzar reaplicación en la próxima sintonización, por si el mapeo cambió
    // en caliente mientras ya se había aplicado una antena a los slices.
    // Force re-application on the next tuning, in case the mapping changed
    // on the fly after an antenna had already been applied to the slices.
    rxAntennaApplied_.clear();
    txAntennaApplied_.clear();
    dbgSdr("setBandAntennas: V=" + vBandAntenna_ + " U=" + uBandAntenna_);
}

// Selección automática de antena por banda — banda VHF/2m usa la antena
// configurada para modo V, banda UHF/70cm usa la configurada para modo U,
// según la frecuencia que se sintonice en cada slice. Se fija tanto rxant
// como txant del slice al mismo transverter, aunque uno de los dos lados no
// se use en ese slice — así cada panadapter queda consistentemente ligado a
// un único transverter.
// Automatic antenna selection by band - the VHF/2m band uses the antenna
// configured for V mode, the UHF/70cm band uses the one configured for U mode,
// according to the frequency tuned on each slice. Both rxant and txant of the
// slice are set to the same transverter, even if one of the two sides is unused
// on that slice - so each panadapter stays consistently tied to a single
// transverter.
void SmartSDRClient::applyAntennaForBand(int sliceIdx, double freqHz, bool isTx) {
    double freqMHz = freqHz / 1e6;

    std::string antenna;
    if (freqMHz >= 144.0 && freqMHz < 148.0)      antenna = vBandAntenna_; // 2m / VHF
    else if (freqMHz >= 430.0 && freqMHz < 440.0) antenna = uBandAntenna_; // 70cm / UHF
    else return; // fuera de las bandas conocidas, no tocar la antena / outside known bands, leave antenna untouched


    std::string& applied = isTx ? txAntennaApplied_ : rxAntennaApplied_;
    if (applied == antenna) return; // ya aplicada, evitar spam de comandos / already applied, avoid command spam

    std::ostringstream cmd;
    cmd << "slice set " << sliceIdx
        << " rxant=" << antenna
        << " txant=" << antenna;
    dbgSdr(std::string("slice set rxant/txant (banda auto, ") +
           (isTx ? "TX" : "RX") + "): " + cmd.str());
    sendCommand(cmd.str());
    applied = antenna;
}

// Secuencia exacta de flexlib-go smartsdr-iqtransfer
// Exact sequence from flexlib-go smartsdr-iqtransfer
void SmartSDRClient::startDaxIQStream(int channel,
                                       uint16_t udpPort,
                                       int sampleRate)
{
    // Esperar hasta tener cliente Y panadapter (hasta 5 segundos)
    // Wait until we have a client AND a panadapter (up to 5 seconds)
    for (int i = 0; i < 500 && (firstClientId_.empty() || firstPanId_.empty()); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    dbgSdr("startDaxIQStream: clientId=" + firstClientId_ +
           " panId=" + firstPanId_ +
           " rate=" + std::to_string(sampleRate));

    if (firstClientId_.empty()) {
        dbgSdr("WARN: clientId no disponible, continuando sin bind");
    }
    if (firstPanId_.empty()) {
        dbgSdr("WARN: panId no disponible, el stream no tendrá panadapter");
    }

    // Esperar a que lleguen los status de streams preexistentes (sub daxiq all)
    // Wait for the status of pre-existing streams to arrive (sub daxiq all)
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // 1. Bind al primer cliente GUI
    // 1. Bind to the first GUI client
    if (!firstClientId_.empty())
        sendCommand("client bind client_id=" + firstClientId_);

    // 2. Registrar nuestro puerto UDP
    // 2. Register our UDP port
    sendCommand("client udpport " + std::to_string(udpPort));

    // 3. Eliminar stream preexistente en este canal (evita estado "Busy")
    // 3. Remove pre-existing stream on this channel (avoids "Busy" state)
    uint32_t prevId = existingDaxIQStreamId_.load();
    if (prevId != 0) {
        std::ostringstream prevHex;
        prevHex << std::hex << prevId;
        dbgSdr("Eliminando stream DAX IQ preexistente 0x" + prevHex.str());
        removeDaxIQStream(prevId);
        existingDaxIQStreamId_ = 0;
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }

    // Crear stream DAX IQ — sintaxis exacta de flexlib-go
    // Create DAX IQ stream - exact flexlib-go syntax
    sendCommand("stream create type=dax_iq daxiq_channel=" +
                std::to_string(channel));

    // 4. Asignar al panadapter con sample rate
    // 4. Assign to the panadapter with sample rate
    if (!firstPanId_.empty()) {
        sendCommand("dax iq set " + std::to_string(channel) +
                    " pan=" + firstPanId_ +
                    " rate=" + std::to_string(sampleRate));
    }

    // 5. Esperar stream ID (hasta 1 segundo)
    // 5. Wait for stream ID (up to 1 second)
    for (int i = 0; i < 100 && daxIQStreamId_.load() == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    // 6. Ajustar rate del stream al valor solicitado
    // 6. Set the stream rate to the requested value
    if (daxIQStreamId_.load() != 0) {
        std::ostringstream cmd;
        cmd << "stream set 0x" << std::hex << daxIQStreamId_.load()
            << " daxiq_rate=" << std::dec << sampleRate;
        sendCommand(cmd.str());
        std::ostringstream sid;
        sid << std::hex << daxIQStreamId_.load();
        dbgSdr("stream set rate=" + std::to_string(sampleRate) +
               " streamId=0x" + sid.str());
    } else {
        dbgSdr("WARN: stream ID no capturado, no se pudo ajustar rate");
    }

    dbgSdr("startDaxIQStream completado");
}

void SmartSDRClient::removeDaxIQStream(uint32_t streamID) {
    if (streamID == 0) return;
    std::ostringstream cmd;
    cmd << "stream remove 0x" << std::hex << streamID;
    sendCommand(cmd.str());
}

void SmartSDRClient::sendCommand(const std::string& cmd) {
    if (!connected_.load()) return;

    uint32_t seq  = seqNum_.fetch_add(1);
    std::string line = "C" + std::to_string(seq) + "|" + cmd + "\n";

    std::lock_guard<std::mutex> lock(sendMutex_);
    SOCKET s = static_cast<SOCKET>(sock_);
    ::send(s, line.c_str(), static_cast<int>(line.size()), 0);
}

void SmartSDRClient::receiveLoop() {
    SOCKET s = static_cast<SOCKET>(sock_);
    std::string buffer;
    char chunk[4096];

    while (connected_.load()) {
        int n = ::recv(s, chunk, sizeof(chunk) - 1, 0);
        if (n <= 0) break;
        chunk[n] = '\0';
        buffer  += chunk;

        size_t pos;
        while ((pos = buffer.find('\n')) != std::string::npos) {
            std::string line = buffer.substr(0, pos);
            buffer.erase(0, pos + 1);
            if (!line.empty()) parseLine(line);
        }
    }
    connected_ = false;
}

// Parser de mensajes SmartSDR
// SmartSDR message parser
void SmartSDRClient::parseLine(const std::string& line) {
    if (line.empty()) return;

    // Log de las primeras líneas
    // Log of the first lines
    static int lineCount = 0;
    if (lineCount++ < 50)
        dbgSdr("RADIO>> " + line);

    char type = line[0];

    // Handle: "H<hex>"
    if (type == 'H') {
        try {
            clientHandle_ = static_cast<uint32_t>(
                std::stoul(line.substr(1), nullptr, 16));
        } catch (...) {}
        return;
    }

    // Estado: "S<handle>|<payload>"
    // Status: "S<handle>|<payload>"
    if (type == 'S') {
        auto pipePos = line.find('|');
        if (pipePos == std::string::npos) return;
        std::string payload = line.substr(pipePos + 1);

        // Slice — capturar frecuencia, modo y pan ID
        // Slice - capture frequency, mode and pan ID
        if (payload.rfind("slice ", 0) == 0) {
            // Índice de slice: "slice <n> ..."
            // Slice index: "slice <n> ..."
            int sliceIdx = -1;
            {
                auto idxStart = 6u;
                auto idxEnd   = payload.find(' ', idxStart);
                try {
                    sliceIdx = std::stoi(payload.substr(idxStart,
                        idxEnd == std::string::npos ? idxEnd : idxEnd - idxStart));
                } catch (...) {}
            }

            // El slice TX vive en su propio panadapter (segundo slice, índice
            // distinto de 0/RX). No usamos el flag "tx=1": ese flag marca cuál
            // slice está actualmente transmitiendo y por defecto apunta al
            // slice A (índice 0), que es el mismo que usamos para RX — usarlo
            // provocaba que setTxFrequency() sintonizara siempre el slice A.
            // The TX slice lives on its own panadapter (second slice, index different
            // from 0/RX). We do not use the "tx=1" flag: that flag marks which slice is
            // currently transmitting and by default points to slice A (index 0), the same
            // one we use for RX - using it made setTxFrequency() always tune slice A.
            if (sliceIdx > 0 && txSliceIdx_.load() < 0) {
                txSliceIdx_ = sliceIdx;
                dbgSdr("Slice TX detectado (2┬║ panadapter): idx=" + std::to_string(sliceIdx));
            }

            bool isKnownTxSlice = (sliceIdx >= 0 && sliceIdx == txSliceIdx_.load());

            // Frecuencia Doppler / RF
            // Doppler / RF frequency
            auto freqPos = payload.find("RF_frequency=");
            if (freqPos != std::string::npos) {
                try {
                    double freqMHz = std::stod(payload.substr(freqPos + 13));
                    if (isKnownTxSlice) {
                        currentTxFreqHz_ = freqMHz * 1e6;
                        if (txFreqCallback_) txFreqCallback_(freqMHz * 1e6);
                    } else {
                        if (freqCallback_) freqCallback_(freqMHz * 1e6);
                    }
                } catch (...) {}
            }

            // Modo (USB/LSB/CW/AM/FM/DIGU/DIGL/DFM/...)
            // Mode (USB/LSB/CW/AM/FM/DIGU/DIGL/DFM/...)
            auto modePos = payload.find("mode=");
            if (modePos != std::string::npos) {
                auto start = modePos + 5;
                auto end   = payload.find(' ', start);
                std::string mode = flexToHamlibMode(payload.substr(start,
                    end == std::string::npos ? end : end - start));
                if (!mode.empty()) {
                    if (isKnownTxSlice) {
                        {
                            std::lock_guard<std::mutex> lock(txModeMutex_);
                            currentTxMode_ = mode;
                        }
                        if (txModeCallback_) txModeCallback_(mode);
                    } else {
                        {
                            std::lock_guard<std::mutex> lock(modeMutex_);
                            currentMode_ = mode;
                        }
                        if (modeCallback_) modeCallback_(mode);
                    }
                }
            }

            // Pan ID — extraído del mensaje de slice
            // Pan ID - extracted from the slice message
            auto panPos = payload.find(" pan=");
            if (panPos != std::string::npos) {
                auto start = panPos + 5;
                auto end   = payload.find(' ', start);
                std::string panCandidate = payload.substr(start,
                    end == std::string::npos ? end : end - start);
                // Solo aceptar si es un handle válido (0x40000000 etc)
                // Only accept if it is a valid handle (0x40000000 etc.)
                if (panCandidate.rfind("0x", 0) == 0 && panCandidate != "0x0") {
                    if (isKnownTxSlice) {
                        if (txPanId_.empty()) {
                            txPanId_ = panCandidate;
                            dbgSdr("txPanId capturado desde slice TX: " + txPanId_);
                        }
                    } else if (firstPanId_.empty()) {
                        firstPanId_ = panCandidate;
                        dbgSdr("firstPanId capturado desde slice: " + firstPanId_);
                    }
                }
            }
        }

        // -- Interlock - estado real de PTT reportado por el radio ----------
        // "interlock state=<...>" refleja el PTT real sea cual sea el origen
        // (nuestro xmit, mic local, MOX del panel frontal, footswitch, etc.).
        // Estados posibles incluyen RECEIVE, READY, PTT_REQUESTED,
        // TRANSMITTING, TUNE, TX_INHIBIT, UNKEY_REQUESTED, TRANSMIT_DELAY...
        // Consideramos "activo" solo TRANSMITTING y TUNE.
        // Interlock - real PTT state reported by the radio.
        // "interlock state=<...>" reflects the real PTT whatever its origin (our xmit,
        // local mic, front-panel MOX, footswitch, etc.). Possible states include
        // RECEIVE, READY, PTT_REQUESTED, TRANSMITTING, TUNE, TX_INHIBIT,
        // UNKEY_REQUESTED, TRANSMIT_DELAY... We consider only TRANSMITTING and TUNE
        // as "active".
        if (payload.rfind("interlock ", 0) == 0) {
            auto statePos = payload.find("state=");
            if (statePos != std::string::npos) {
                auto start = statePos + 6;
                auto end   = payload.find(' ', start);
                std::string state = payload.substr(start,
                    end == std::string::npos ? end : end - start);
                bool newPtt = (state == "TRANSMITTING" || state == "TUNE");
                dbgSdr("interlock state=" + state);
                if (newPtt != pttActive_.load()) {
                    pttActive_ = newPtt;
                    dbgSdr(std::string("PTT actualizado desde radio (interlock): ") + (newPtt ? "ON" : "OFF"));
                    if (pttCallback_) pttCallback_(newPtt);
                }
            }
        }

        // Primer cliente GUI
        // First GUI client
        if (payload.rfind("client ", 0) == 0 && firstClientId_.empty()) {
            auto cidPos = payload.find("client_id=");
            if (cidPos != std::string::npos) {
                auto start = cidPos + 10;
                auto end   = payload.find(' ', start);
                firstClientId_ = payload.substr(start,
                    end == std::string::npos ? end : end - start);
                dbgSdr("firstClientId capturado: " + firstClientId_);
            }
        }

        // Panadapter desde display pan
        // Panadapter from display pan
        if (payload.rfind("display pan ", 0) == 0 && firstPanId_.empty()) {
            auto rest     = payload.substr(12);
            auto spacePos = rest.find(' ');
            std::string panCandidate = rest.substr(0, spacePos);
            if (panCandidate != "0x0" && !panCandidate.empty()) {
                firstPanId_ = panCandidate;
                dbgSdr("firstPanId capturado desde display pan: " + firstPanId_);
            }
        }

        // Stream DAX IQ
        // DAX IQ stream
        if (payload.find("type=dax_iq") != std::string::npos) {
            dbgSdr("STREAM DAX IQ: " + payload);

            // Extraer stream ID del payload ("stream 0xNNNNNNNN ...")
            // Extract stream ID from the payload ("stream 0xNNNNNNNN ...")
            uint32_t payloadStreamId = 0;
            if (payload.rfind("stream 0x", 0) == 0) {
                try {
                    auto idStart = 7u;
                    auto idEnd   = payload.find(' ', idStart);
                    std::string idStr = payload.substr(idStart,
                        idEnd == std::string::npos ? idEnd : idEnd - idStart);
                    payloadStreamId = std::stoul(idStr, nullptr, 16);
                } catch (...) {}
            }

            // Capturar stream preexistente al arrancar (solo si aún no tenemos uno)
            // Capture pre-existing stream at startup (only if we do not have one yet)
            if (payloadStreamId != 0 && existingDaxIQStreamId_.load() == 0
                && daxIQStreamId_.load() == 0) {
                existingDaxIQStreamId_ = payloadStreamId;
                std::ostringstream ss; ss << std::hex << payloadStreamId;
                dbgSdr("Stream DAX IQ preexistente capturado: 0x" + ss.str());
            }

            // Detectar si nuestro stream activo pierde el pan (endpoint=Not Assigned)
            // y reasignarlo automáticamente
            // Detect if our active stream loses its pan (endpoint=Not Assigned)
            // and reassign it automatically
            if (payloadStreamId != 0
                && payloadStreamId == daxIQStreamId_.load()
                && payload.find("endpoint_type=Not Assigned") != std::string::npos
                && !firstPanId_.empty()) {
                dbgSdr("WARN: stream perdio pan, reasignando a " + firstPanId_);
                // Extraer el canal del payload
                // Extract the channel from the payload
                auto chPos = payload.find("daxiq_channel=");
                int ch = 1;
                if (chPos != std::string::npos) {
                    try { ch = std::stoi(payload.substr(chPos + 14)); } catch (...) {}
                }
                sendCommand("dax iq set " + std::to_string(ch) +
                            " pan=" + firstPanId_);
            }
        }

        return;
    }

    // Respuesta: "R<seq>|<status>|<data>"
    // Response: "R<seq>|<status>|<data>"
    if (type == 'R') {
        auto p1 = line.find('|');
        auto p2 = (p1 != std::string::npos) ? line.find('|', p1 + 1)
                                             : std::string::npos;
        if (p1 != std::string::npos && p2 != std::string::npos) {
            std::string status = line.substr(p1 + 1, p2 - p1 - 1);
            std::string data   = line.substr(p2 + 1);

            if (status == "0") {
                dbgSdr("Radio OK: " + line);

                // Capturar stream ID de la respuesta al stream create
                // Capture stream ID from the stream create response
                if (!data.empty() && data.size() <= 8 &&
                    data.find_first_not_of("0123456789abcdefABCDEF")
                        == std::string::npos) {
                    try {
                        uint32_t sid = std::stoul(data, nullptr, 16);
                        if (sid >= 0x20000000) {
                            daxIQStreamId_ = sid;
                            std::ostringstream ss;
                            ss << std::hex << sid;
                            dbgSdr("Stream ID capturado: 0x" + ss.str());
                        }
                    } catch (...) {}
                }
            } else {
                dbgSdr("Radio error: " + line);
            }
        }
        return;
    }
}
