cmake_minimum_required(VERSION 3.15)
project(FlexSkyBridge VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Localizar dependencias de SoapySDR / Locate the SoapySDR dependencies
# Usar el SDK local permite compilar sin instalar PothosSDR.
# Using the local SDK allows building without installing PothosSDR.
option(FLEXSKYBRIDGE_USE_VENDORED_SOAPYSDR
    "Use the SoapySDR headers and import library from external/" ON)

set(FLEXSKYBRIDGE_SOAPYSDR_INCLUDE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/external/h")
set(FLEXSKYBRIDGE_SOAPYSDR_LIBRARY
    "${CMAKE_CURRENT_SOURCE_DIR}/external/lib/SoapySDR.lib")

if(FLEXSKYBRIDGE_USE_VENDORED_SOAPYSDR)
    if(NOT EXISTS "${FLEXSKYBRIDGE_SOAPYSDR_INCLUDE_DIR}/SoapySDR/Device.hpp" OR
       NOT EXISTS "${FLEXSKYBRIDGE_SOAPYSDR_LIBRARY}")
        message(FATAL_ERROR
            "The vendored SoapySDR SDK is incomplete. "
            "Restore external/h and external/lib, or set "
            "FLEXSKYBRIDGE_USE_VENDORED_SOAPYSDR=OFF to use a system SDK.")
    endif()

    if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
        message(FATAL_ERROR
            "The vendored SoapySDR import library requires 64-bit Windows/MSVC. "
            "Set FLEXSKYBRIDGE_USE_VENDORED_SOAPYSDR=OFF to use a system SDK.")
    endif()

    add_library(SoapySDR UNKNOWN IMPORTED)
    set_target_properties(SoapySDR PROPERTIES
        IMPORTED_LOCATION "${FLEXSKYBRIDGE_SOAPYSDR_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${FLEXSKYBRIDGE_SOAPYSDR_INCLUDE_DIR}")
    message(STATUS "Using vendored SoapySDR SDK from external/")
else()
    list(APPEND CMAKE_PREFIX_PATH "C:/Program Files/PothosSDR")
    find_package(SoapySDR REQUIRED)
endif()

set(SOURCES
    src/Registration.cpp
    src/FlexDevice.cpp
    src/SmartSDRClient.cpp
    src/DaxIQReceiver.cpp
    src/RigCtldServer.cpp
)

add_library(FlexSkyBridge MODULE ${SOURCES})

target_include_directories(FlexSkyBridge PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(FlexSkyBridge PRIVATE SoapySDR)

# En Windows necesitamos Winsock2 para TCP/UDP.
# Windows requires Winsock2 for TCP/UDP.
if(WIN32)
    target_link_libraries(FlexSkyBridge PRIVATE ws2_32 ole32 uuid)
endif()

# Instalar en una carpeta relocatable / Install into a relocatable modules folder
install(TARGETS FlexSkyBridge
    DESTINATION "lib/SoapySDR/modules0.8"
)
