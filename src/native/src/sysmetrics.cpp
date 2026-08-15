// See sysmetrics.h. The Windows half is PDH; everything else is the stub.

#include "sysmetrics.h"

#include <atomic>
#include <chrono>
#include <cwchar>
#include <mutex>
#include <thread>
#include <vector>

#if IBM
  #include <windows.h>
  #include <pdh.h>
  #include <pdhmsg.h>
  #pragma comment(lib, "pdh.lib")
#endif

namespace SysMetrics {
namespace {

std::mutex        gMutex;
Sample            gLatest;
std::string       gNote;
std::thread       gThread;
std::atomic<bool> gStop{false};
std::atomic<bool> gRunning{false};

void Publish(const Sample& s) {
    std::lock_guard<std::mutex> lock(gMutex);
    gLatest = s;
}

void SetNote(const std::string& n) {
    std::lock_guard<std::mutex> lock(gMutex);
    gNote = n;
}

// How often the thread wakes. PDH rate counters are differences between two
// collections, so this is also the window each number is averaged over: much
// below a quarter second the per-core figures turn into noise, much above it and
// a spike caused by toggling an effect has faded before it is read.
const int kPeriodMs = 500;

#if IBM

// ⚠ ENGLISH counter paths, via PdhAddEnglishCounter. The localized names are what
// PdhAddCounter wants, and on a German or Japanese Windows "\Processor
// Information(*)\% Processor Time" simply does not resolve — the tool would come
// up empty on someone else's machine and work perfectly on the developer's.
const wchar_t* const kCpuPath = L"\\Processor Information(*)\\% Processor Time";
const wchar_t* const kGpuPath = L"\\GPU Engine(*)\\Utilization Percentage";

double FileTimeToSeconds(const FILETIME& ft) {
    ULARGE_INTEGER u;
    u.LowPart  = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return (double)u.QuadPart * 1e-7;      // 100 ns units
}

bool NameHas(const wchar_t* s, const wchar_t* needle) {
    return s && needle && wcsstr(s, needle) != nullptr;
}

// One wildcard counter's instances, formatted. PDH wants the call made twice:
// once to size the buffer and once to fill it.
bool ReadArray(PDH_HCOUNTER counter, PDH_FMT_COUNTERVALUE_ITEM_W** out, DWORD* count,
               std::vector<unsigned char>* buf) {
    DWORD size = 0, n = 0;
    PDH_STATUS st = PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &size, &n, nullptr);
    if (st != PDH_MORE_DATA || size == 0) return false;
    buf->resize(size);
    st = PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &size, &n,
                                      (PDH_FMT_COUNTERVALUE_ITEM_W*)buf->data());
    if (st != ERROR_SUCCESS) return false;
    *out   = (PDH_FMT_COUNTERVALUE_ITEM_W*)buf->data();
    *count = n;
    return true;
}

void SampleLoop() {
    // --- process CPU: no PDH needed, and it is the one number that is exactly
    // about X-Plane rather than about the machine.
    SYSTEM_INFO si = {};
    GetSystemInfo(&si);
    const double ncpu = si.dwNumberOfProcessors > 0 ? (double)si.dwNumberOfProcessors : 1.0;

    FILETIME ftCreate, ftExit, ftKernel, ftUser;
    double prevProcess = 0.0;
    auto   prevWall    = std::chrono::steady_clock::now();
    bool   haveProcess = false;
    if (GetProcessTimes(GetCurrentProcess(), &ftCreate, &ftExit, &ftKernel, &ftUser)) {
        prevProcess = FileTimeToSeconds(ftKernel) + FileTimeToSeconds(ftUser);
        haveProcess = true;
    }

    // --- PDH. A failure here is not fatal: process CPU above still works, and a
    // machine with the GPU counter set missing (it needs Windows 10 1709+, and a
    // driver that publishes it) should still get its cores.
    PDH_HQUERY   query   = nullptr;
    PDH_HCOUNTER cpu     = nullptr;
    PDH_HCOUNTER gpu     = nullptr;
    std::string  note;

    if (PdhOpenQueryW(nullptr, 0, &query) != ERROR_SUCCESS) {
        query = nullptr;
        note  = "Windows performance counters (PDH) would not open, so only "
                "X-Plane's own CPU time is shown.";
    } else {
        if (PdhAddEnglishCounterW(query, kCpuPath, 0, &cpu) != ERROR_SUCCESS) cpu = nullptr;
        if (PdhAddEnglishCounterW(query, kGpuPath, 0, &gpu) != ERROR_SUCCESS) {
            gpu = nullptr;
            note = "No GPU utilization counter on this machine (it needs Windows 10 "
                   "1709 or newer and a driver that publishes one), so GPU load is "
                   "blank.";
        }
        // The first collection establishes the baseline every rate counter is a
        // difference from; its own values are meaningless and are thrown away.
        PdhCollectQueryData(query);
    }
    SetNote(note);

    std::vector<unsigned char> cpuBuf, gpuBuf;

    while (!gStop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(kPeriodMs));
        if (gStop.load()) break;

        Sample s;

        if (haveProcess &&
            GetProcessTimes(GetCurrentProcess(), &ftCreate, &ftExit, &ftKernel, &ftUser)) {
            const double now  = FileTimeToSeconds(ftKernel) + FileTimeToSeconds(ftUser);
            const auto   wall = std::chrono::steady_clock::now();
            const double dt   = std::chrono::duration<double>(wall - prevWall).count();
            if (dt > 0.0) {
                double pct = (now - prevProcess) / (dt * ncpu) * 100.0;
                if (pct < 0.0) pct = 0.0;
                if (pct > 100.0) pct = 100.0;
                s.processCpu = (float)pct;
                s.cpuValid   = true;
            }
            prevProcess = now;
            prevWall    = wall;
        }

        if (query && PdhCollectQueryData(query) == ERROR_SUCCESS) {
            PDH_FMT_COUNTERVALUE_ITEM_W* items = nullptr;
            DWORD n = 0;
            if (cpu && ReadArray(cpu, &items, &n, &cpuBuf)) {
                int c = 0;
                for (DWORD i = 0; i < n; ++i) {
                    const double v = items[i].FmtValue.doubleValue;
                    // Instances are "0,0", "0,1", ... plus a "_Total" per group and
                    // one overall. Anything with _Total in it is a summary, not a
                    // core, and counting it as one would add a phantom processor
                    // that is always the average of the others.
                    if (NameHas(items[i].szName, L"_Total")) {
                        if (wcscmp(items[i].szName, L"_Total") == 0)
                            s.systemCpu = (float)v;
                        continue;
                    }
                    if (c < kMaxCores) s.core[c] = (float)v;
                    ++c;
                }
                s.cores    = c;
                s.cpuValid = true;
            }
            if (gpu && ReadArray(gpu, &items, &n, &gpuBuf)) {
                // Every process's every engine is an instance. The 3D engines are
                // the ones a flight simulator's frame time lives on; copy, video
                // decode and encode are someone else's browser. Summed across
                // processes because the question is "is the GPU the limit", not
                // "whose GPU time is it".
                double total = 0.0;
                for (DWORD i = 0; i < n; ++i)
                    if (NameHas(items[i].szName, L"engtype_3D"))
                        total += items[i].FmtValue.doubleValue;
                if (total > 100.0) total = 100.0;
                s.gpu      = (float)total;
                s.gpuValid = true;
            }
        }

        Publish(s);
    }

    if (query) PdhCloseQuery(query);
}

#else   // not Windows

void SampleLoop() {
    SetNote("CPU and GPU load are only implemented on Windows. The frame-rate "
            "figures on this page are measured the same way everywhere.");
    while (!gStop.load())
        std::this_thread::sleep_for(std::chrono::milliseconds(kPeriodMs));
}

#endif

}  // namespace

void Start() {
    if (gRunning.exchange(true)) return;
    gStop.store(false);
    gThread = std::thread(SampleLoop);
}

void Stop() {
    if (!gRunning.exchange(false)) return;
    gStop.store(true);
    if (gThread.joinable()) gThread.join();
    // ⚠ The last sample is CLEARED on the way out. Leaving it would let a pane
    // that reopens after a stop show a frozen reading that looks live — the exact
    // failure this whole file is meant to help find.
    std::lock_guard<std::mutex> lock(gMutex);
    gLatest = Sample();
}

bool Running() { return gRunning.load(); }

Sample Read() {
    std::lock_guard<std::mutex> lock(gMutex);
    return gLatest;
}

std::string Note() {
    std::lock_guard<std::mutex> lock(gMutex);
    return gNote;
}

}  // namespace SysMetrics
