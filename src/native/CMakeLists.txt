cmake_minimum_required(VERSION 3.16)

# --- binary hygiene: universal binaries on macOS -----------------------------
# ⚠ BEFORE project(), and as cache variables — CMake reads both during platform
# initialization, and CMAKE_OSX_ARCHITECTURES only initializes a target's
# OSX_ARCHITECTURES property at add_library/add_executable time. This used to be
# set inside the .xpl's per-platform block, AFTER photoncore and photon-installer
# already existed: the static lib came out single-arch and the universal installer
# exe could not link its x86_64 slice on an arm64 runner. Ignored everywhere but
# macOS, so it is unconditional (APPLE is not defined until project() runs).
#
# The deployment target pins the oldest macOS the binaries run on. Without it the
# floor is whatever SDK the build machine carries, and an installer built on a new
# CI runner refuses to launch on an older Intel Mac it would otherwise support.
# 11.0 = X-Plane 12's own floor; the arm64 slice cannot go lower anyway.
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "macOS: universal 2")
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "macOS: oldest supported OS")

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
    src/core/progress.cpp
    src/core/actions.cpp)

add_library(photoncore STATIC ${PHOTON_CORE_SOURCES})
# `third_party/` is on the include path so the JSON header is reached as
# `third_party/nlohmann/json.hpp` — the same shape stb and imgui use.
target_include_directories(photoncore PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(photoncore PUBLIC cxx_std_17)

if(WIN32)
    # ⚠ PUBLIC, not PRIVATE. Every consumer of this library that also includes
    # windows.h needs the same guards, and NOMINMAX is the one that bites: without
    # it windows.h defines min/max as MACROS, and the first `std::min(` in a
    # consumer fails with "illegal token on right side of '::'" — an error that
    # names neither windows.h nor the macro. Setting it here rather than repeating
    # it per target is what stops the next target from rediscovering that.
    target_compile_definitions(photoncore PUBLIC
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
#
# The TUI's string engine is tested here too. It lives in the installer target
# rather than photoncore (nothing else draws a terminal), so its two files are
# compiled into the test binary directly — `WrapAnsi`/`Clip`/`VisLen` are pure
# functions with an exhaustive-case shape, and they are exactly the layer where a
# 1:1 port of somebody else's index arithmetic goes subtly wrong.
add_executable(photoncore_tests
    tests/core_tests.cpp
    tests/install_tests.cpp
    src/installer/tui.cpp
    src/installer/platform.cpp)
target_link_libraries(photoncore_tests PRIVATE photoncore)
if(WIN32)
    target_link_libraries(photoncore_tests PRIVATE shell32)
endif()
enable_testing()
add_test(NAME photoncore_tests COMMAND photoncore_tests)

# --- photon-installer: the compiled end-user installer -----------------------
# ⚠ Links photoncore and NOTHING X-Plane. It builds on a runner with no SDK, which
# is what keeps the CI matrix simple — and is why the SDK check below this block
# never runs for it.
# ⚠ THE GUI IS OPT-IN AND OFF BY DEFAULT, and must stay that way until the Slint
# front-end is finished. Everything below it — CI's three runners, the core-only
# tree, a contributor's checkout — builds photon-installer with no Rust toolchain
# present, and switching this ON is what adds that dependency. Flipping the
# default before CI installs Rust turns every job red at once.
#
# See docs/installer_gui_plan.md. Build it with:
#   cmake -B build-gui -A x64 -DPHOTON_GUI=ON
option(PHOTON_GUI "Build photon-installer's Slint GUI front-end (requires Rust)" OFF)

set(PHOTON_INSTALLER_SOURCES
    src/installer/main.cpp
    src/installer/cli.cpp
    src/installer/platform.cpp
    src/installer/log.cpp
    src/installer/tui.cpp
    src/installer/screens.cpp)
if(PHOTON_GUI)
    list(APPEND PHOTON_INSTALLER_SOURCES src/installer/gui.cpp)
endif()

add_executable(photon-installer ${PHOTON_INSTALLER_SOURCES})
target_link_libraries(photon-installer PRIVATE photoncore)

# --- the Slint GUI front-end -------------------------------------------------
# ⚠ SLINT IS GPLv3 HERE, AND THAT IS A DELIBERATE CHOICE (2026-08-12). Slint's
# alternative — its royalty-free license — was rejected because it carries
# attribution conditions. The whole repository was relicensed from MIT to GPLv3 to
# match (see LICENSE and README.md §Usage), so there is no longer a per-target
# licensing split to keep track of: everything here is GPLv3, and the third-party
# components under `third_party/` and `reference/` keep their own compatible terms.
if(PHOTON_GUI)
    include(FetchContent)

    # ⚠ STATIC, NOT SHARED — `build_bundle` ships exactly ONE executable plus
    # `data/` and README.txt and cannot express a second file, so a slint_cpp DLL
    # beside the binary has nowhere to go. It would also break the "runs from
    # Downloads, from a USB stick" property the payload resolution depends on.
    # Saved and restored rather than set outright: FetchContent_MakeAvailable is an
    # add_subdirectory, so a bare `set` here would silently retarget anything
    # declared after this block too.
    set(PHOTON_SAVED_BUILD_SHARED_LIBS ${BUILD_SHARED_LIBS})
    set(BUILD_SHARED_LIBS OFF)

    if(MSVC)
        # ⚠ RUST DEFAULTS TO THE DYNAMIC CRT (/MD) AND THIS PROJECT IS GLOBALLY /MT.
        # Without +crt-static the Rust staticlib drags in msvcrt while every C++
        # object here uses libcmt: duplicate-symbol spew at best, two heaps at
        # worst — and a bad free across that boundary is a crash with no stack
        # pointing anywhere near the cause.
        set(ENV{RUSTFLAGS} "$ENV{RUSTFLAGS} -C target-feature=+crt-static")
    endif()

    # ⚠ macOS UNIVERSAL BINARIES NEED BOTH RUST TARGETS INSTALLED. This file asks
    # for `arm64;x86_64` at the top, but cargo builds ONE architecture at a time —
    # Corrosion drives it once per arch and lipos the results, and it can only do
    # that if rustc has both std libraries. Without them the failure is a cargo
    # error about a missing target deep in a Slint build log, which reads as Slint
    # being broken rather than as two missing toolchain components.
    #
    # This is the ONLY extra step the GUI adds on macOS, and it is checked rather
    # than assumed because the release matrix already ships a universal installer.
    if(APPLE AND CMAKE_OSX_ARCHITECTURES MATCHES ";")
        find_program(PHOTON_RUSTUP rustup)
        if(PHOTON_RUSTUP)
            execute_process(COMMAND "${PHOTON_RUSTUP}" target list --installed
                            OUTPUT_VARIABLE PHOTON_RUST_TARGETS
                            ERROR_QUIET OUTPUT_STRIP_TRAILING_WHITESPACE)
            foreach(arch IN LISTS CMAKE_OSX_ARCHITECTURES)
                if(arch STREQUAL "arm64")
                    set(want "aarch64-apple-darwin")
                else()
                    set(want "${arch}-apple-darwin")
                endif()
                if(NOT PHOTON_RUST_TARGETS MATCHES "${want}")
                    message(FATAL_ERROR
                        "Rust target '${want}' is not installed, so the universal "
                        "(${CMAKE_OSX_ARCHITECTURES}) installer cannot be built.\n"
                        "  rustup target add ${want}\n"
                        "Or configure a single-architecture build with "
                        "-DCMAKE_OSX_ARCHITECTURES=${CMAKE_HOST_SYSTEM_PROCESSOR}.")
                endif()
            endforeach()
        else()
            message(WARNING
                "rustup not found - cannot verify the macOS Rust targets for a "
                "universal build. If the Slint build fails with a missing target, "
                "run: rustup target add aarch64-apple-darwin x86_64-apple-darwin")
        endif()
    endif()

    FetchContent_Declare(
        Slint
        GIT_REPOSITORY https://github.com/slint-ui/slint.git
        # ⚠ PIN THIS TO AN EXACT TAG BEFORE THE FIRST RELEASE BUILD. `release/1` is
        # a MOVING tag: it is the right thing while the UI is being written and the
        # wrong thing for a reproducible bundle, because two CI runs a month apart
        # would compile different Slint versions into the same Photon version.
        GIT_TAG release/1
        SOURCE_SUBDIR api/cpp)
    FetchContent_MakeAvailable(Slint)

    set(BUILD_SHARED_LIBS ${PHOTON_SAVED_BUILD_SHARED_LIBS})

    target_link_libraries(photon-installer PRIVATE Slint::Slint)
    slint_target_sources(photon-installer ui/app.slint)
    target_compile_definitions(photon-installer PRIVATE PHOTON_GUI=1)

    # ⚠ EMBED THE IMAGES IN THE BINARY. Slint's default for `@image-url` is
    # `as-absolute-path`, which bakes THIS MACHINE'S path to ui/assets/*.svg into
    # the executable. It works perfectly on the build machine and shows nothing —
    # no error, just missing artwork — on every user's, which is the worst
    # possible failure mode for a single-file download that also travels from a
    # USB stick. The payload rule (resolve from the executable, never the working
    # directory) applies to the UI's own assets too; embedding is how.
    set_property(TARGET photon-installer PROPERTY SLINT_EMBED_RESOURCES embed-files)

    if(MSVC)
        # ⚠ THE NON-DEBUG STATIC CRT IN EVERY CONFIG, overriding this file's global
        # `MultiThreaded$<$<CONFIG:Debug>:Debug>`. A Rust DEBUG build still links
        # the RELEASE CRT even under +crt-static, so a CMake Debug build would pair
        # our /MTd objects with Slint's /MT ones and fail to link. Pinning the
        # binary to /MT everywhere is the smaller loss: the installer is not a
        # thing anyone steps through in a debugger, and photoncore_tests — which is
        # — does not link Slint and keeps the global setting.
        set_target_properties(photon-installer PROPERTIES
            MSVC_RUNTIME_LIBRARY "MultiThreaded")
    endif()

    # ⚠ DO NOT SWITCH THIS BUILD TO THE SKIA RENDERER. It is the only one that
    # draws Slint's `inner-shadow-*` properties, so eleven of the design's
    # thirteen inset shadows are approximated by hand (ui/widgets.slint). Tried
    # 2026-08-12 and it does not link, for a reason that is not fixable here:
    # skia-bindings ships a PREBUILT Skia built against the DYNAMIC CRT, and this
    # binary is /MT everywhere on purpose (see the RUSTFLAGS note above) — the
    # link ends in `LNK2038: 'MD_DynamicRelease' doesn't match 'MT_StaticRelease'`
    # once the archives are supplied. Building Skia from source with /MT would
    # need Python + GN + ninja on every CI runner, and 42 MB of static archive
    # against a 14.8 MB binary. Full write-up: docs/installer_gui_plan.md §6.
    message(STATUS "ToLissPhoton: GUI build - photon-installer links Slint (GPLv3).")
endif()

if(WIN32)
    # ShellExecuteW, for opening the .org/GitHub pages from the Complete screen;
    # ole32 for the GUI's IFileOpenDialog folder picker (harmless in a TUI build,
    # where PickFolder is compiled but never called).
    target_link_libraries(photon-installer PRIVATE shell32 ole32)
    if(PHOTON_GUI)
        # ⚠ WINDOWS SUBSYSTEM, so double-clicking the installer does NOT flash a
        # console window behind the GUI. The cost is that the CLI half has no
        # stdout to write to when run from an existing terminal — `main` fixes that
        # by re-attaching the parent console before RunCli. See main.cpp.
        set_target_properties(photon-installer PROPERTIES WIN32_EXECUTABLE ON)

        # ⚠ THE GUI SUBSYSTEM LOOKS FOR `WinMain`, AND WE HAVE `main`. Without this
        # the link fails on an unresolved WinMain coming out of libcmt's
        # exe_winmain.obj — an error that names the CRT and not the subsystem
        # switch that caused it. Redirecting the entry point keeps ONE `main()`
        # serving the GUI, the TUI and the CLI, which is what stops the three modes
        # from drifting apart at startup.
        target_link_options(photon-installer PRIVATE /ENTRY:mainCRTStartup)

        # ⚠ SLINT'S OWN TRANSITIVE WINDOWS LIBRARIES, WHICH CORROSION DOES NOT
        # PROPAGATE. Its `Required static libs` line reports only
        # kernel32/ntdll/userenv/ws2_32/dbghelp; the winit backend additionally
        # pulls in WGL (glutin) and the IME API (winit), and both surface as a wall
        # of unresolved externals with Rust-mangled names that look like a broken
        # Slint build rather than a missing `-l`.
        #   opengl32 -> wglCreateContext, wglMakeCurrent, wglGetProcAddress, ...
        #   imm32    -> ImmGetContext, ImmSetCompositionWindow, ...
        target_link_libraries(photon-installer PRIVATE opengl32 imm32)
    else()
        # A CONSOLE subsystem app: the TUI is a terminal UI, and a WIN32 subsystem
        # build would open with no console for it to draw in.
        set_target_properties(photon-installer PROPERTIES WIN32_EXECUTABLE OFF)
    endif()
endif()

if(APPLE)
    # Universal 2 + deployment target come from the cache variables at the top of
    # this file — set there, before any target, so photoncore gets them too.
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
    # Universal 2 comes from CMAKE_OSX_ARCHITECTURES at the top of this file —
    # setting it HERE never worked: the property snapshots the variable when the
    # target is created, and this line ran after add_library(ToLissPhoton).
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
