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

# --- dev tooling -------------------------------------------------------------
# ONE flag for everything that must not ship: the Dev menu and window, the panel
# probe, the FX compositor and its history, and the repo build commands. OFF by
# default, so a plain `cmake --build` is always a release build and
# make_release.py cannot bundle something that paints over the captain's PFD.
#
#   cmake -B build -A x64 -DPHOTON_DEV=ON        (or: deploy.ps1 -Dev)
option(PHOTON_DEV "Build the dev menu, Dev window and panel probe" OFF)

# --- Dear ImGui --------------------------------------------------------------
# Vendored at third_party/imgui/ (see its README for version and update steps).
# Compiled UNCONDITIONALLY, not behind the probe flag: it is the UI framework for
# tooling and for whatever windows get converted later, so its availability
# should not depend on which experiment is switched on. In a Release build the
# linker's /OPT:REF drops whatever ends up unreferenced, so a shipping plugin
# that uses no ImGui window pays almost nothing for it being compiled.
#
# The X-Plane backend is ours — src/imgui_xplm.cpp — because the stock backends
# assume they own the window, the GL context and the event loop. See that file.
set(PHOTON_IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui")

add_library(ToLissPhoton MODULE
    src/plugin.cpp
    src/imgui_xplm.cpp
    "${PHOTON_IMGUI_DIR}/imgui.cpp"
    "${PHOTON_IMGUI_DIR}/imgui_draw.cpp"
    "${PHOTON_IMGUI_DIR}/imgui_tables.cpp"
    "${PHOTON_IMGUI_DIR}/imgui_widgets.cpp"
    "${PHOTON_IMGUI_DIR}/imgui_demo.cpp")
set_target_properties(ToLissPhoton PROPERTIES PREFIX "" SUFFIX ".xpl" OUTPUT_NAME "ToLissPhoton")

target_include_directories(ToLissPhoton PRIVATE
    "${XPLANE_SDK}/CHeaders/XPLM"
    "${XPLANE_SDK}/CHeaders/Widgets"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${PHOTON_IMGUI_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb")

# Our settings live in src/photon_imconfig.h rather than in the vendored
# imconfig.h, so a version bump stays a straight file copy. Chief among them:
# IM_ASSERT logs instead of calling abort(), which inside X-Plane would take the
# simulator down with no message.
target_compile_definitions(ToLissPhoton PRIVATE
    "IMGUI_USER_CONFIG=\"photon_imconfig.h\"")

# SDK feature level. XPLM300/301 = X-Plane 11.10+, enough for the full port's
# flight loops + modern widgets. Bump to XPLM400=1 (XPLM410=1) for XP12-only APIs.
target_compile_definitions(ToLissPhoton PRIVATE XPLM200=1 XPLM210=1 XPLM300=1 XPLM301=1)

if(PHOTON_DEV)
    target_compile_definitions(ToLissPhoton PRIVATE PHOTON_DEV=1)
    # Linux links no GL of its own: like the XPLM symbols, glBegin & co. resolve
    # from the host process at load time.
    message(STATUS "ToLissPhoton: DEV build - Dev menu, panel probe and FX compositor are IN. Do not ship it.")
    find_package(Threads REQUIRED)          # the Build tab runs commands off-thread
    target_link_libraries(ToLissPhoton PRIVATE Threads::Threads)
    # stb_image is header-only and its implementation is compiled inside the same
    # #if PHOTON_DEV block in plugin.cpp, so nothing extra is listed here — the
    # include directory above is unconditional only because CMake has no reason
    # to hide it. It decodes the FX compositor's overlay images; the SDK has no
    # image loader of its own. See third_party/stb/README.md.
endif()

# --- per-platform: the SDK's required macro, linkage, and arch folder --------
# Exactly one of APL/IBM/LIN must be 1 (SDK convention).
if(WIN32)
    # NOMINMAX / WIN32_LEAN_AND_MEAN: the SDK pulls in windows.h on IBM, whose
    # min/max macros and CreateMenu/DestroyMenu symbols otherwise clash with ours.
    target_compile_definitions(ToLissPhoton PRIVATE IBM=1 APL=0 LIN=0
        _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
    # opengl32 is UNCONDITIONAL: the ImGui backend renders with GL and is always
    # compiled, so the link cannot be tied to PHOTON_DEV. (XPWidgets is still not
    # linked — every window is ImGui now, which needs no widget library.)
    target_link_libraries(ToLissPhoton PRIVATE
        "${XPLANE_SDK}/Libraries/Win/XPLM_64.lib"
        opengl32)
    set(PHOTON_ARCH win_x64)
elseif(APPLE)
    target_compile_definitions(ToLissPhoton PRIVATE APL=1 IBM=0 LIN=0)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64")   # universal 2 (Apple Silicon + Intel)
    target_link_libraries(ToLissPhoton PRIVATE
        "-F${XPLANE_SDK}/Libraries/Mac" "-framework XPLM"
        "-framework OpenGL")
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
