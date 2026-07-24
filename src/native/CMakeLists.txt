cmake_minimum_required(VERSION 3.16)
project(ToLissPhotonNative CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- locate the X-Plane SDK --------------------------------------------------
# Download from https://developer.x-plane.com/sdk/ and unzip so that
#   <XPLANE_SDK>/CHeaders/XPLM/XPLMDefs.h   exists.
# Either drop the SDK/ folder at src/native/SDK/ (gitignored) or configure with
#   cmake -B build -DXPLANE_SDK=/path/to/SDK
if(NOT DEFINED XPLANE_SDK)
    set(XPLANE_SDK "${CMAKE_CURRENT_SOURCE_DIR}/SDK")
endif()
if(NOT EXISTS "${XPLANE_SDK}/CHeaders/XPLM/XPLMDefs.h")
    message(FATAL_ERROR
        "X-Plane SDK not found at '${XPLANE_SDK}'.\n"
        "Download it from https://developer.x-plane.com/sdk/ and unzip so that\n"
        "  ${XPLANE_SDK}/CHeaders/XPLM/XPLMDefs.h\n"
        "exists (place SDK/ at src/native/SDK/, or pass -DXPLANE_SDK=/path/to/SDK).")
endif()

add_library(ToLissPhoton MODULE src/plugin.cpp)
set_target_properties(ToLissPhoton PROPERTIES PREFIX "" SUFFIX ".xpl" OUTPUT_NAME "ToLissPhoton")

target_include_directories(ToLissPhoton PRIVATE
    "${XPLANE_SDK}/CHeaders/XPLM"
    "${XPLANE_SDK}/CHeaders/Widgets")

# SDK feature level. XPLM300/301 = X-Plane 11.10+, enough for the full port's
# flight loops + modern widgets. Bump to XPLM400=1 (XPLM410=1) for XP12-only APIs.
target_compile_definitions(ToLissPhoton PRIVATE XPLM200=1 XPLM210=1 XPLM300=1 XPLM301=1)

# --- per-platform: the SDK's required macro, linkage, and arch folder --------
# Exactly one of APL/IBM/LIN must be 1 (SDK convention).
if(WIN32)
    # NOMINMAX / WIN32_LEAN_AND_MEAN: the SDK pulls in windows.h on IBM, whose
    # min/max macros and CreateMenu/DestroyMenu symbols otherwise clash with ours.
    target_compile_definitions(ToLissPhoton PRIVATE IBM=1 APL=0 LIN=0
        _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
    # Only XPLM: the Custom window draws via XPLMDrawTranslucentDarkBox + XPLMDrawString
    # (no raw GL, no XPWidgets), so neither opengl32 nor the widgets lib is linked.
    target_link_libraries(ToLissPhoton PRIVATE
        "${XPLANE_SDK}/Libraries/Win/XPLM_64.lib")
    set(PHOTON_ARCH win_x64)
elseif(APPLE)
    target_compile_definitions(ToLissPhoton PRIVATE APL=1 IBM=0 LIN=0)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64")   # universal 2 (Apple Silicon + Intel)
    target_link_libraries(ToLissPhoton PRIVATE
        "-F${XPLANE_SDK}/Libraries/Mac" "-framework XPLM")
    set(PHOTON_ARCH mac_x64)
else()  # Linux: XPLM symbols are resolved by the host at load time (don't link them)
    target_compile_definitions(ToLissPhoton PRIVATE LIN=1 APL=0 IBM=0)
    target_compile_options(ToLissPhoton PRIVATE -fPIC -fvisibility=hidden)
    target_link_options(ToLissPhoton PRIVATE -shared -static-libgcc -static-libstdc++)
    set(PHOTON_ARCH lin_x64)
endif()

# Emit into the fat-plugin folder layout X-Plane expects:
#   Resources/plugins/ToLissPhoton/<arch>/ToLissPhoton.xpl
# (per-config properties keep multi-config generators like MSVC from inserting a
# Debug/ or Release/ subfolder).
set_target_properties(ToLissPhoton PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY         "${CMAKE_BINARY_DIR}/ToLissPhoton/${PHOTON_ARCH}"
    LIBRARY_OUTPUT_DIRECTORY_DEBUG   "${CMAKE_BINARY_DIR}/ToLissPhoton/${PHOTON_ARCH}"
    LIBRARY_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/ToLissPhoton/${PHOTON_ARCH}")
