#include "RigCtldServer.hpp"
#include "SkyRoofPaths.hpp"

#include <sstream>
#include <cstring>
#include <chrono>

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>

static void dbgRig(const std::string& msg) {
    fsb::debugLog("[rigctld] " + msg);
}

void RigCtldServer::start(uint16_t port, FreqCallback onSetFreq, ModeCallback onSetMode,
                          PttCallback onSetPtt, CtcssCallback onSetCtcss) {
    if (running_.load()) return;

    port_        = port;
    onSetFreq_   = std::move(onSetFreq);
    onSetMode_   = std::move(onSetMode);
    onSetPtt_    = std::move(onSetPtt);
    onSetCtcss_  = std::move(onSetCtcss);
    running_     = true;

    listenThread_ = std::thread(&RigCtldServer::listenLoop, this);
    dbgRig("Servidor rigctld arrancado en puerto " + std::to_string(port_));
}

void RigCtldServer::stop() {
    if (!running_.exchange(false)) return;

    // Cerrar el socket de escucha para desbloquear accept()
    // Close the listening socket to unblock accept()
    if (listenSock_ != (uintptr_t)-1) {
        closesocket((SOCKET)listenSock_);
        listenSock_ = (uintptr_t)-1;
    }
    if (listenThread_.joinable()) listenThread_.join();

    std::lock_guard<std::mutex> lk(clientMutex_);
    for (auto& t : clientThreads_)
        if (t.joinable()) t.detach();
    clientThreads_.clear();

    dbgRig("Servidor rigctld detenido");
}

void RigCtldServer::setCurrentMode(const std::string& mode, int passband) {
    std::lock_guard<std::mutex> lk(modeMutex_);
    currentMode_ = mode;
    if (passband > 0) currentPassband_ = passband;
}

void RigCtldServer::listenLoop() {
    SOCKET srv = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (srv == INVALID_SOCKET) {
        dbgRig("ERROR: socket() falló");
        return;
    }

    // Permitir reutilizar el puerto inmediatamente
    // Allow immediate port reuse
    int opt = 1;
    ::setsockopt(srv, SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<const char*>(&opt), sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port_);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // solo localhost / localhost only

    if (::bind(srv, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        dbgRig("ERROR: bind() falló en puerto " + std::to_string(port_));
        closesocket(srv);
        return;
    }
    if (::listen(srv, 4) != 0) {
        dbgRig("ERROR: listen() falló");
        closesocket(srv);
        return;
    }

    listenSock_ = (uintptr_t)srv;
    dbgRig("Escuchando en 127.0.0.1:" + std::to_string(port_));

    while (running_.load()) {
        SOCKET client = ::accept(srv, nullptr, nullptr);
        if (client == INVALID_SOCKET) break;

        dbgRig("Cliente conectado");
        std::lock_guard<std::mutex> lk(clientMutex_);
        clientThreads_.emplace_back([this, client]() {
            handleClient((uintptr_t)client);
        });
    }
    closesocket(srv);
}

// Protocolo rigctld (Hamlib) — subconjunto mínimo para frecuencia y modo
void RigCtldServer::handleClient(uintptr_t clientSock) {
    SOCKET s = (SOCKET)clientSock;
    char   buf[1024];
    std::string buffer;

    // Timeout de recepción: 5 segundos
    // Receive timeout: 5 seconds
    DWORD tv = 5000;
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO,
                 reinterpret_cast<const char*>(&tv), sizeof(tv));

    static int cmdCount = 0;

    while (running_.load()) {
        int n = ::recv(s, buf, sizeof(buf) - 1, 0);
        if (n <= 0) break;
        buf[n] = '\0';
        buffer += buf;

        size_t pos;
        while ((pos = buffer.find('\n')) != std::string::npos) {
            std::string line = buffer.substr(0, pos);
            buffer.erase(0, pos + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;

            ++cmdCount;
            std::string response;

            // set_freq: "F 435804021" o "\set_freq 435804021"
            if ((line.size() > 2 && line[0] == 'F' && line[1] == ' ') ||
                line.rfind("\\set_freq ", 0) == 0)
            {
                auto sp = line.find(' ');
                try {
                    double freq = std::stod(line.substr(sp + 1));
                    currentFreq_ = freq;
                    if (onSetFreq_) onSetFreq_(freq);
                    if (cmdCount <= 5 || cmdCount % 40 == 0)
                        dbgRig("set_freq #" + std::to_string(cmdCount) +
                               " = " + std::to_string(freq / 1e6) + " MHz");
                } catch (...) {
                    dbgRig("ERROR parseo freq: " + line);
                }
                response = "RPRT 0\n";
            }
            // get_freq: "f" o "\get_freq"
            else if (line == "f" || line == "\\get_freq") {
                std::ostringstream oss;
                oss << (long long)currentFreq_.load() << "\n";
                response = oss.str();
            }
            // set_split_vfo: "S 1 VFOB" o "\set_split_vfo 1 VFOB"
            else if ((line.size() > 2 && line[0] == 'S' && line[1] == ' ') ||
                     line.rfind("\\set_split_vfo ", 0) == 0)
            {
                splitEnabled_ = (line.find('1') != std::string::npos);
                dbgRig("set_split_vfo: " + std::string(splitEnabled_.load() ? "ON" : "OFF"));
                response = "RPRT 0\n";
            }
            // get_split_vfo: "s" o "\get_split_vfo"
            else if (line == "s" || line == "\\get_split_vfo") {
                response = std::string(splitEnabled_.load() ? "1" : "0") + "\nVFOB\n";
            }
            // set_mode: "M USB 3000" o "\set_mode USB 3000"
            else if ((line.size() > 2 && line[0] == 'M' && line[1] == ' ') ||
                     line.rfind("\\set_mode ", 0) == 0)
            {
                auto sp1 = line.find(' ');
                std::string rest = (sp1 != std::string::npos) ? line.substr(sp1 + 1) : "";
                auto sp2 = rest.find(' ');
                std::string mode      = (sp2 != std::string::npos) ? rest.substr(0, sp2) : rest;
                int         passband  = 0;
                if (sp2 != std::string::npos) {
                    try { passband = std::stoi(rest.substr(sp2 + 1)); } catch (...) {}
                }

                if (!mode.empty()) {
                    {
                        std::lock_guard<std::mutex> lk(modeMutex_);
                        currentMode_ = mode;
                        if (passband > 0) currentPassband_ = passband;
                    }
                    if (onSetMode_) onSetMode_(mode, passband);
                    dbgRig("set_mode: " + mode + " pb=" + std::to_string(passband));
                }
                response = "RPRT 0\n";
            }
            // get_mode: "m" o "\get_mode"
            else if (line == "m" || line == "\\get_mode") {
                std::string mode;
                int passband;
                {
                    std::lock_guard<std::mutex> lk(modeMutex_);
                    mode     = currentMode_;
                    passband = currentPassband_;
                }
                response = mode + "\n" + std::to_string(passband) + "\n";
            }
            // dump_state — identificación completa para que Hamlib la acepte
            // dump_state - full identification so Hamlib accepts it
            else if (line == "dump_state" || line.rfind("\\dump_state", 0) == 0) {
                response =
                    "0\n"          // protocol version
                    "1\n"          // rig model (dummy)
                    "2\n"          // ITU region
                    // RX freq ranges: min max modes low_power high_power vfo ant
                    "100000 6000000000 0x1ff -1 -1 0x10000003 0x3\n"
                    "0 0 0 0 0 0 0\n"                              // terminador RX / RX terminator
                    // TX freq ranges — antes en 0, sin esto Hamlib rechaza split/TX
                    // TX freq ranges - previously 0; without this Hamlib rejects split/TX
                    "100000 6000000000 0x1ff 1 100 0x20000003 0x3\n"
                    "0 0 0 0 0 0 0\n"                              // terminador TX / TX terminator
                    // tuning steps
                    "0x1ff 1\n"
                    "0 0\n"                                        // terminador steps / steps terminator
                    // filtros / filters
                    "0x1ff 2400\n"
                    "0 0\n"                                        // terminador filtros / filters terminator
                    "0\n"          // max_rit
                    "0\n"          // max_xit
                    "0\n"          // max_ifshift
                    "0\n"          // announces
                    "\n"           // preamp list (ninguno / none)
                    "\n"           // attenuator list (ninguno)
                    "0\n"          // has_get_func
                    "0\n"          // has_set_func
                    "0\n"          // has_get_level
                    "0\n"          // has_set_level
                    "0\n"          // has_get_parm
                    "0\n"          // has_set_parm
                    "RPRT 0\n";
            }
            // get_vfo / set_vfo
            else if (line == "v" || line == "\\get_vfo") {
                response = "VFOA\n";
            }
            else if (line[0] == 'V' || line.rfind("\\set_vfo", 0) == 0) {
                response = "RPRT 0\n";
            }
            // PTT
            else if (line == "t" || line == "\\get_ptt") {
                // Respuesta estandar de Hamlib rigctld: solo el valor (0|1),
                // sin prefijo ni RPRT 0 final -- igual que get_freq (f) mas
                // abajo. Algunos parsers de CAT (incluido el de SkyRoof) leen
                // solo la primera linea como valor, pero un RPRT 0 extra
                // parece confundir a su parser de get_ptt especificamente.
                // Standard Hamlib rigctld response: only the value (0|1), with no prefix
                // or trailing RPRT 0 -- same as get_freq (f). Some CAT parsers (including
                // SkyRoof's) read only the first line as the value, but an extra RPRT 0
                // seems to confuse its get_ptt parser specifically.
                response = std::string(currentPtt_.load() ? "1" : "0") + "\n";
                dbgRig(std::string("get_ptt: ") + (currentPtt_.load() ? "ON" : "OFF"));
            }
            else if ((line.size() > 2 && line[0] == 'T' && line[1] == ' ') ||
                     line.rfind("\\set_ptt ", 0) == 0)
            {
                auto sp = line.find(' ');
                bool ptt = false;
                if (sp != std::string::npos) {
                    try { ptt = (std::stoi(line.substr(sp + 1)) != 0); } catch (...) {}
                }
                currentPtt_ = ptt;
                if (onSetPtt_) onSetPtt_(ptt);
                dbgRig(std::string("set_ptt: ") + (ptt ? "ON" : "OFF"));
                response = "RPRT 0\n";
            }
            // CTCSS TX: "C 670" (decimas de Hz) y "U TONE 1|0"
            // TX CTCSS: "C 670" (tenths of Hz) and "U TONE 1|0"
            else if ((line.size() > 2 && line[0] == 'C' && line[1] == ' ') ||
                     line.rfind("\\set_ctcss_tone ", 0) == 0)
            {
                auto sp = line.find(' ');
                try { ctcssTenthsHz_ = std::stoi(line.substr(sp + 1)); } catch (...) {}
                dbgRig("set_ctcss_tone: " + std::to_string(ctcssTenthsHz_.load()));
                if (onSetCtcss_) onSetCtcss_(ctcssTenthsHz_.load() / 10.0, ctcssEnabled_.load());
                response = "RPRT 0\n";
            }
            else if (line.rfind("U TONE", 0) == 0 || line.rfind("\\set_func TONE", 0) == 0) {
                auto sp = line.rfind(' ');
                bool on = false;
                try { on = (std::stoi(line.substr(sp + 1)) != 0); } catch (...) {}
                ctcssEnabled_ = on;
                dbgRig(std::string("set_func TONE: ") + (on ? "ON" : "OFF"));
                if (onSetCtcss_) onSetCtcss_(ctcssTenthsHz_.load() / 10.0, on);
                response = "RPRT 0\n";
            }
            else if (line == "c" || line == "\\get_ctcss_tone") {
                response = std::to_string(ctcssTenthsHz_.load()) + "\n";
            }
            else if (line.rfind("u TONE", 0) == 0 || line.rfind("\\get_func TONE", 0) == 0) {
                response = std::string(ctcssEnabled_.load() ? "1" : "0") + "\n";
            }
            // Comando "a" de SkyRoof: detecta si somos SkyCAT
            // Responder RPRT -18 indica "no soy SkyCAT, usa rigctld estándar"
            // Esto hace que SkyRoof use F <freq> para enviar la frecuencia Doppler
            // SkyRoof "a" command: detects whether we are SkyCAT.
            // Replying RPRT -18 means "I am not SkyCAT, use standard rigctld".
            // This makes SkyRoof use F <freq> to send the Doppler frequency.
            else if (line == "a") {
                response = "RPRT -18\n";
            }
            // Cualquier otro comando: ACK genérico
            // Any other command: generic ACK
            else {
                response = "RPRT 0\n";
            }

            ::send(s, response.c_str(), (int)response.size(), 0);
        }
    }
    ::closesocket(s);
    dbgRig("Cliente desconectado");
}
