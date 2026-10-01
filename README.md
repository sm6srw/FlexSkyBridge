# FlexSkyBridge

<a name="espanol"></a>
🇪🇸 Español · [🇬🇧 English](#english)

Plugin SoapySDR para Windows que conecta

<img width="1142" height="677" alt="screenshot" src="https://github.com/user-attachments/assets/14a70581-7b19-4978-a5a0-d79159751b89" />

## Características

- Habla directamente con el FlexRadio 6600 vía protocolo SmartSDR (TCP/4992) — sin necesidad de SmartSDR DAX ni smartsdr-iqtransfer
- Recibe IQ a **192.000 Hz** vía UDP directo (paquetes VITA-49), sin latencia de driver de audio
- Corrección Doppler en tiempo real via rigctld integrado (puerto 4532)
- Control de rotor automático — lanza y cierra `rotctld` (hamlib) junto con el stream, enviando el tracking a PstRotator
- Mueve automáticamente el slice y el panadapter de SmartSDR al cambiar de frecuencia
- Compatible con AetherSDR u otros clientes SmartSDR funcionando simultáneamente

## Flujo de datos

```
                    ┌─────────────────────────────────────────┐
                    │           FlexSkyBridge.dll             │
SkyRoof ──SoapySDR──►  rigctld :4532   →  slice tune TCP/4992 ──► FlexRadio 6600
        ──rotctld──►  rotctld  :4533   →  PstRotator :4533        UDP VITA-49 ◄──┘
                    └─────────────────────────────────────────┘
```

- **TCP/4992** — protocolo SmartSDR: crea el stream DAX IQ, mueve el slice con Doppler
- **UDP/7891** — paquetes VITA-49 a 192 kHz directamente desde el radio
- **Puerto 4532** — rigctld embebido: recibe correcciones Doppler de SkyRoof
- **Puerto 4533** — rotctld (hamlib): recibe Az/El de SkyRoof y los reenvía a PstRotator

## Requisitos

- Windows 10/11 x64
- [PothosSDR](https://github.com/pothosware/PothosSDR/releases) (incluye SoapySDR)
- FlexRadio 6600 con SmartSDR v1.4 o superior
- [Hamlib](https://hamlib.github.io/) instalado en `C:\hamlib\` (para control de rotor)
- [PstRotator](http://www.qsl.net/yo3dmu/index_Page346.htm) configurado como servidor rotctld en puerto 4533
- SkyRoof 1.33 o superior (ver sección [Instalación con SkyRoof](#instalación-con-skyroof--ruta-correcta-del-módulo))
- CMake 3.15+ y Visual Studio 2019/2022

## Compilación

```powershell
git clone https://github.com/ea5wa/FlexSkyBridge.git
cd FlexSkyBridge

cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

El DLL resultante queda en `build/Release/FlexSkyBridge.dll`.

## Instalación

SkyRoof **no usa** el directorio estándar de PothosSDR para cargar plugins SoapySDR. Usa su propia carpeta interna. Debes copiar la DLL en **dos sitios**:

### 1. Módulos de PothosSDR (para que SoapySDRUtil lo detecte)

```powershell
copy build\Release\FlexSkyBridge.dll "C:\Program Files\PothosSDR\lib\SoapySDR\modules0.8\"
```

### 2. Módulos internos de SkyRoof (imprescindible para que funcione)

La ruta depende de dónde esté instalado SkyRoof. Encuéntrala abriendo el log de SkyRoof (`%APPDATA%\Afreet\Products\SkyRoof\Logs\SkyRoof_YYYYMMDD.txt`) y buscando la línea:

```
Setting SoapySDR plugin path: C:\...
```

Copia la DLL a esa ruta:

```powershell
# Ajusta según lo que indique el log de SkyRoof
copy build\Release\FlexSkyBridge.dll "C:\RADIO\SkyRoof\SkyRoof\lib\SoapySDR\modules0.8\"
```

Rutas conocidas según versión de SkyRoof:

| Versión SkyRoof | Ruta del módulo |
|----------------|-----------------|
| 1.34 | `C:\RADIO\SkyRoof\lib\SoapySDR\modules0.8\` |

### Verificación

Comprueba que SkyRoof carga el plugin correctamente en su log:

```
SDR started: Flex 6600 via FlexSkyBridge
[SOAPY_SDR_INFO]: [FlexSkyBridge] Stream activado
```

Si aparece `Device Flex 6600 via FlexSkyBridge is no longer available`, la DLL no está en la carpeta correcta de SkyRoof.

## Configuración en SkyRoof

En SkyRoof, selecciona como SDR device:

```
<img width="981" height="789" alt="image" src="https://github.com/user-attachments/assets/a6c36d86-c654-4750-8d9a-6c0b857cb4a0" />


```

| Parámetro | Descripción | Valor por defecto |
|-----------|-------------|-------------------|
| `radio` | IP del FlexRadio | `192.168.0.208` |
| `channel` | Canal DAX IQ (1-8) | `1` |
| `udpport` | Puerto UDP para recibir IQ | `7891` |
| `rigctld` | Puerto rigctld Doppler (RX/downlink) | `4532` |
| `rigctldtx` | Puerto rigctld dedicado a TX/uplink | `4534` |
| `rotctldexe` | Ruta a rotctld.exe | `C:\hamlib\bin\rotctld.exe` |
| `rotctldargs` | Argumentos de rotctld | `-m 3 -r 127.0.0.1:4533` |
| `vantenna` | Antena/transverter para banda V (2m/VHF) | `XVTA` |
| `uantenna` | Antena/transverter para banda U (70cm/UHF) | `XVTB` |

`vantenna`/`uantenna` también pueden cambiarse en caliente desde el panel de
**Settings** de SoapySDR (SkyRoof lo expone en la configuración del dispositivo),
con las opciones disponibles en la lista de antenas del dispositivo. El valor
elegido se persiste en `C:\RADIO\FlexSkyBridge_settings.ini` y sobrevive a
reinicios. Al conmutar entre un transpondedor V/U y uno U/V, el driver detecta
automáticamente la banda de la frecuencia sintonizada en cada slice (RX/TX) y
asigna la antena configurada para esa banda.

En SkyRoof configura también:
- **CAT / Rig control (RX)** → rigctld en `127.0.0.1:4532`
- **CAT / Rig control (TX / uplink)** → rigctld en `127.0.0.1:4534` (conexión independiente,
  no split-VFO — SkyRoof espera una segunda conexión CAT simple para el uplink)
- **Rotor** → rotctld en `127.0.0.1:4533`
- **Sample rate**: 192000 Hz · **Format**: CF32

## Control de TX (PTT y CTCSS)

El servidor rigctld TX (puerto 4534) gestiona la frecuencia, el modo, el PTT y el tono CTCSS del uplink en el segundo slice del radio. El tono CTCSS se aplica solo si SkyRoof lo envía activado: habilita el tono en la configuración del transmisor en SkyRoof (el log de SkyRoof debe mostrar `SetCtcssTone 67 on`).

## Control de rotor

FlexSkyBridge lanza

El comando que ejecuta internamente es:

```
C:\hamlib\bin\rotctld.exe -m 3 -r 127.0.0.1:4533
```

PstRotator debe estar configurado en modo **rotctld server** escuchando en el puerto 4533.

Si necesitas apuntar a un PstRotator en otra máquina, pasa el parámetro en el device string:

```
...,rotctldargs=-m 3 -r 192.168.0.X:4533
```

## Notas de uso

- No es necesario activar el DAX IQ 1 en SmartSDR DAX — el plugin crea su propio stream independiente
- El plugin convive con AetherSDR / SmartSDR-Win abierto simultáneamente
- El log de depuración se escribe en `C:\RADIO\FlexSkyBridge_debug.log`


## Arquitectura interna

| Fichero | Responsabilidad |
|---------|----------------|
| `FlexDevice.cpp` | Interfaz SoapySDR · gestión del proceso rotctld |
| `SmartSDRClient.cpp` | Protocolo SmartSDR TCP: crea stream DAX IQ, slice tune Doppler |
| `DaxIQReceiver.cpp` | Recepción UDP de paquetes VITA-49 y ring buffer CF32 |
| `RigCtldServer.cpp` | Servidor rigctld embebido para correcciones Doppler |
| `Registration.cpp` | Registro del plugin en SoapySDR |

## Licencia

MIT — ver [LICENSE](LICENSE)

## Autor

EA5WA — [@ea5wa](https://github.com/ea5wa)

---

<a name="english"></a>
# English

[🇪🇸 Español](#espanol) · 🇬🇧 English

SoapySDR plugin for Windows that connects [SkyRoof](https://ve3nea.github.io/SkyRoof/) to the **FlexRadio 6600** transceiver for satellite tracking with automatic Doppler correction and rotator control. Made with the help of Claude Code.

## Features

- Talks directly to the FlexRadio 6600 via the SmartSDR protocol (TCP/4992) — no SmartSDR DAX or smartsdr-iqtransfer needed
- Receives IQ at **192,000 Hz** over direct UDP (VITA-49 packets), with no audio driver latency
- Real-time Doppler correction via built-in rigctld (port 4532)
- Dedicated TX/uplink rigctld (port 4534): frequency, mode, PTT and CTCSS tone
- Automatic rotator control — launches and closes `rotctld` (hamlib) with the stream, sending tracking to PstRotator
- Automatically moves the SmartSDR slice and panadapter when the frequency changes
- Works alongside AetherSDR or other SmartSDR clients running simultaneously

## Data flow

```
                    ┌─────────────────────────────────────────┐
                    │           FlexSkyBridge.dll             │
SkyRoof ──SoapySDR──►  rigctld :4532   →  slice tune TCP/4992 ──► FlexRadio 6600
        ──rotctld──►  rotctld  :4533   →  PstRotator :4533        UDP VITA-49 ◄──┘
                    └─────────────────────────────────────────┘
```

- **TCP/4992** — SmartSDR protocol: creates the DAX IQ stream, moves the slice with Doppler
- **UDP/7891** — VITA-49 packets at 192 kHz straight from the radio
- **Port 4532** — embedded rigctld: receives Doppler corrections from SkyRoof
- **Port 4534** — embedded TX rigctld: uplink frequency, mode, PTT, CTCSS
- **Port 4533** — rotctld (hamlib): receives Az/El from SkyRoof and forwards them to PstRotator

## Requirements

- Windows 10/11 x64
- [PothosSDR](https://github.com/pothosware/PothosSDR/releases) (includes SoapySDR)
- FlexRadio 6600 with SmartSDR v1.4 or later
- [Hamlib](https://hamlib.github.io/) installed in `C:\hamlib\` (for rotator control)
- [PstRotator](http://www.qsl.net/yo3dmu/index_Page346.htm) configured as a rotctld server on port 4533
- SkyRoof 1.33 or later
- CMake 3.15+ and Visual Studio 2019/2022

## Build

```powershell
git clone https://github.com/ea5wa/FlexSkyBridge.git
cd FlexSkyBridge

cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

The resulting DLL is `build/Release/FlexSkyBridge.dll`.

## Installation

SkyRoof **does not use** the standard PothosSDR directory to load SoapySDR plugins; it uses its own internal folder. Copy the DLL to **two places**:

### 1. PothosSDR modules (so SoapySDRUtil detects it)

```powershell
copy build\Release\FlexSkyBridge.dll "C:\Program Files\PothosSDR\lib\SoapySDR\modules0.8\"
```

### 2. SkyRoof internal modules (required for it to work)

The path depends on where SkyRoof is installed. Find it in the SkyRoof log (`%APPDATA%\Afreet\Products\SkyRoof\Logs\SkyRoof_YYYYMMDD.txt`) by looking for the line:

```
Setting SoapySDR plugin path: C:\...
```

Copy the DLL to that path:

```powershell
# Adjust to what the SkyRoof log says
copy build\Release\FlexSkyBridge.dll "C:\RADIO\SkyRoof\SkyRoof\lib\SoapySDR\modules0.8\"
```

| SkyRoof version | Module path |
|-----------------|-------------|
| 1.34 | `C:\RADIO\SkyRoof\lib\SoapySDR\modules0.8\` |

### Verification

Check that SkyRoof loads the plugin in its log:

```
SDR started: Flex 6600 via FlexSkyBridge
[SOAPY_SDR_INFO]: [FlexSkyBridge] Stream activado
```

If you see `Device Flex 6600 via FlexSkyBridge is no longer available`, the DLL is not in SkyRoof's correct folder.

## SkyRoof configuration

In SkyRoof, select the SDR device and set these parameters:

| Parameter | Description | Default |
|-----------|-------------|---------|
| `radio` | FlexRadio IP | `192.168.0.208` |
| `channel` | DAX IQ channel (1-8) | `1` |
| `udpport` | UDP port for receiving IQ | `7891` |
| `rigctld` | Doppler rigctld port (RX/downlink) | `4532` |
| `rigctldtx` | Dedicated TX/uplink rigctld port | `4534` |
| `rotctldexe` | Path to rotctld.exe | `C:\hamlib\bin\rotctld.exe` |
| `rotctldargs` | rotctld arguments | `-m 3 -r 127.0.0.1:4533` |
| `vantenna` | Antenna/transverter for V band (2m/VHF) | `XVTA` |
| `uantenna` | Antenna/transverter for U band (70cm/UHF) | `XVTB` |

`vantenna`/`uantenna` can also be changed on the fly from the SoapySDR **Settings** panel
(SkyRoof exposes it in the device configuration), using the options from the device's
antenna list. The chosen value is persisted in `C:\RADIO\FlexSkyBridge_settings.ini` and
survives restarts. When switching between a V/U and a U/V transponder, the driver detects
the band of the tuned frequency on each slice (RX/TX) and assigns the antenna configured
for that band.

Also configure in SkyRoof:
- **CAT / Rig control (RX)** → rigctld at `127.0.0.1:4532`
- **CAT / Rig control (TX / uplink)** → rigctld at `127.0.0.1:4534` (independent connection,
  not split-VFO — SkyRoof expects a second plain CAT connection for the uplink)
- **Rotator** → rotctld at `127.0.0.1:4533`
- **Sample rate**: 192000 Hz · **Format**: CF32

## TX control (PTT and CTCSS)

The TX rigctld server (port 4534) handles the uplink frequency, mode, PTT and CTCSS tone on the radio's second slice. The CTCSS tone is applied only if SkyRoof sends it enabled: enable the tone in the transmitter configuration in SkyRoof (the SkyRoof log should show `SetCtcssTone 67 on`).

## Rotator control

FlexSkyBridge automatically launches `rotctld.exe` when the stream starts and closes it when it ends. You don't need to start rotctld manually.

The command it runs internally is:

```
C:\hamlib\bin\rotctld.exe -m 3 -r 127.0.0.1:4533
```

PstRotator must be configured in **rotctld server** mode listening on port 4533.

To point to a PstRotator on another machine, pass the parameter in the device string:

```
...,rotctldargs=-m 3 -r 192.168.0.X:4533
```

## Usage notes

- You don't need to enable DAX IQ 1 in SmartSDR DAX — the plugin creates its own independent stream
- The plugin coexists with AetherSDR / SmartSDR-Win open at the same time
- The debug log is written to `C:\RADIO\FlexSkyBridge_debug.log`

## Internal architecture

| File | Responsibility |
|------|----------------|
| `FlexDevice.cpp` | SoapySDR interface · rotctld process management |
| `SmartSDRClient.cpp` | SmartSDR TCP protocol: creates DAX IQ stream, Doppler slice tune |
| `DaxIQReceiver.cpp` | UDP reception of VITA-49 packets and CF32 ring buffer |
| `RigCtldServer.cpp` | Embedded rigctld server for Doppler corrections |
| `Registration.cpp` | Plugin registration in SoapySDR |

## License

MIT — see [LICENSE](LICENSE)

## Author

EA5WA — [@ea5wa](https://github.com/ea5wa)
