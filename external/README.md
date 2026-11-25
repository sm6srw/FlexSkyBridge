# External build dependencies

This directory contains the compile-time subset of the SoapySDR SDK used to
build `FlexSkyBridge.dll` without requiring a system-wide PothosSDR/SoapySDR
installation.

- `h/SoapySDR/`: SoapySDR public headers
- `lib/SoapySDR.lib`: 64-bit MSVC import library
- `licenses/SoapySDR-LICENSE_1_0.txt`: Boost Software License 1.0 for SoapySDR

## Provenance

The vendored files were copied from the locally installed PothosSDR distribution.
They correspond to SoapySDR module ABI `0.8` (see `SOAPY_SDR_ABI_VERSION` in
`h/SoapySDR/Version.h`). The import library targets 64-bit Windows/MSVC and is
used only at link time. The host application must provide the matching
`SoapySDR.dll` at runtime.

Do not update the headers and import library independently; replace both from
the same PothosSDR/SoapySDR release and retain the license file.
