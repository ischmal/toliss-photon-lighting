cmake_minimum_required(VERSION 3.16)
project(ToLissPhotonNative CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- binary hygiene: static CRT on Windows -----------------------------------
# ⚠ GLOBAL, and it has to be: photoncore is linked into BOTH the `.xpl` and
# `photon-installer`, and mixing /MT and /MD across a static library and the module
# that consumes it is a link-time warning at best and two heaps at worst.
#
# Static is the right side to land on. `photon-installer` is a file an end user
# downloads and double-clicks; a missing-VC++-redistributable dialog on a lighting
# mod reads as malware, and there is no installer-for-the-installer to fix it with.
# The `.xpl` gains the same property (it previously took the default /MD and so
# quietly depended on the redistributable being present). Nothing crosses the module
# boundary that cares — the plugin talks to X-Plane through the XPLM C API and keeps
# every CRT resource inside itself.
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# --- photoncore: the shared static library -----------------------------------
# Constants, the version, the file patchers, detection, the manifest — everything
# the plugin, the installer and the tests all need to agree on. See
# docs/installer_cpp_plan.md §3.
#
# ⚠ photoncore MUST NOT INCLUDE ANY XPLM HEADER OR LINK THE SDK. The installer and
# the core tests build on machines and CI runners with no X-Plane SDK, and that
# independence is what keeps the release matrix simple. Anything needing XPLM stays
# in plugin.cpp and calls INTO core, never the reverse. The SDK check below sits
# under this block deliberately: `-DPHOTON_CORE_ONLY=ON` builds photoncore and its
# tests without an SDK present at all.
option(PHOTON_CORE_ONLY
       "Build only photoncore + its tests (no X-Plane SDK required)" OFF)

set(PHOTON_CORE_SOURCES
    src/core/constants.cpp
    src/core/fsutil.cpp
    src/core/marker.cpp
    src/core/acf.cpp
    src/core/manifest.cpp
    src/core/patch_acf.cpp
    src/core/patch_acf_screens.cpp
    src/core/patch_glow.cpp
    src/core/patch_realwings.cpp
    src/core/config.cpp
    src/core/detect.cpp
    src/core/payload.cpp
    src/core/actions.cpp)

add_library(photoncore STATIC ${PHOTON_CORE_SOURCES})
# `third_party/` is on the include path so the JSON header is reached as
# `third_party/nlohmann/json.hpp` — the same shape stb and imgui use.
target_include_directories(photoncore PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(photoncore PUBLIC cxx_std_17)

if(WIN32)
    target_compile_definitions(photoncore PRIVATE
        _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
    # ⚠ Source files hold UTF-8 string literals (the ⚠ in log lines, and every
    # comment). MSVC otherwise reads them in the system code page and mangles them.
    target_compile_options(photoncore PUBLIC /utf-8)
endif()

find_package(Threads REQUIRED)
target_link_libraries(photoncore PUBLIC Threads::Threads)
# GCC before 9 keeps std::filesystem in a separate library.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9.0)
    target_link_libraries(photoncore PUBLIC stdc++fs)
endif()

# --- photoncore_tests: plain-main assert exe ---------------------------------
# No framework, matching the offline ImGui reproduction harness that found the
# CmdListsCount bug in a minute after two sim restarts found nothing. Runs on all
# three OSes in CI; also the place to reproduce a patcher bug without X-Plane.
add_executable(photoncore_tests tests/core_tests.cpp)
target_link_libraries(photoncore_tests PRIVATE photoncore)
enable_testing()
add_test(NAME photoncore_tests COMMAND photoncore_tests)

# --- photon-installer: the compiled end-user installer -----------------------
# ⚠ Links photoncore and NOTHING X-Plane. It builds on a runner with no SDK, which
# is what keeps the CI matrix simple — and is why the SDK check below this block
# never runs for it.
add_executable(photon-installer
    src/installer/main.cpp
    src/installer/cli.cpp)
target_link_libraries(photon-installer PRIVATE photoncore)

if(APPLE)
    set_target_properties(photon-installer PROPERTIES
        OSX_ARCHITECTURES "arm64;x86_64")   # universal 2, same as the .xpl
elseif(NOT MSVC)
    # The same flags the `.xpl` already uses: a distro's libstdc++ is older than the
    # one this was built against more often than not, and the installer has to run
    # before anything else does.
    target_link_options(photon-installer PRIVATE
        -static-libgcc -static-libstdc++)
endif()

if(PHOTON_CORE_ONLY)
    message(STATUS "ToLissPhoton: CORE-ONLY build - photoncore + photoncore_tests, no SDK needed.")
    return()
endif()

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
# PROBE, the FX compositor's EDITOR and history, the live light editor, and the
# repo build commands. OFF by default, so a plain `cmake --build` is always a
# release build and make_release.py cannot bundle something that paints over the
# captain's PFD.
#
# ⚠ The FX COMPOSITOR ITSELF IS NOT BEHIND THIS FLAG any more — it ships, driven
# by the panelfx.txt the installer puts in the plugin folder and controlled from
# the Displays tab. What stays here is the tooling that AUTHORS that file.
#
#   cmake -B build -A x64 -DPHOTON_DEV=ON        (or: deploy.ps1 -Dev)
option(PHOTON_DEV "Build the dev menu, Dev window, panel probe and FX editor" OFF)

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
    src/sysmetrics.cpp
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

# Threads are UNCONDITIONAL. They were a dev-only dependency (the Build tab shells
# out off-thread) until the performance tool shipped: src/sysmetrics.cpp samples
# CPU and GPU load on its own thread, because collecting a wildcard PDH query on
# the simulator thread would put a spike into the very frame times it measures.
find_package(Threads REQUIRED)
target_link_libraries(ToLissPhoton PRIVATE Threads::Threads photoncore)

if(PHOTON_DEV)
    target_compile_definitions(ToLissPhoton PRIVATE PHOTON_DEV=1)
    # Linux links no GL of its own: like the XPLM symbols, glBegin & co. resolve
    # from the host process at load time.
    message(STATUS "ToLissPhoton: DEV build - Dev menu, panel probe and FX editor are IN. Do not ship it.")
endif()

# stb_image is header-only and its implementation is compiled into plugin.cpp, so
# nothing is listed as a source — only the include directory above, which is
# unconditional because the FX compositor (which decodes the overlay images with
# it) now ships. The SDK has no image loader of its own; see third_party/stb/README.md.

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
