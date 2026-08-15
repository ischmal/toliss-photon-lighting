// Machine load — CPU (whole machine, this process, and per core) and GPU —
// sampled on a background thread for the performance tool.
//
// ⚠ NOTHING IN HERE MAY CALL XPLM. The sampler runs on its own thread and XPLM is
// not thread-safe; that includes logging, which is why a failure is recorded as a
// NOTE the caller reads and logs from the simulator thread rather than being
// written to Log.txt where it happened. Same rule the Dev window's Build tab
// worker follows, for the same reason.
//
// ⚠ AND IT MAY NOT BE SAMPLED FROM THE FLIGHT LOOP. Collecting a wildcard PDH
// query walks every GPU engine instance on the machine — hundreds of them on a
// busy desktop — and it is not bounded in time. Doing that on the simulator
// thread would put a spike into the frame times this tool exists to measure, so
// the measurement would be reporting its own cost.
//
// Windows only for now: the numbers come from PDH, which has no counterpart worth
// emulating on the other two platforms. Elsewhere Start() succeeds, Read() returns
// an invalid sample and Note() says why, so the UI degrades to "FPS only" instead
// of disappearing.
#pragma once

#include <string>

namespace SysMetrics {

// Max cores reported. Anything wider is truncated and `cores` says how many are
// really there — a 128-thread machine should still show its first 64 rather than
// nothing.
const int kMaxCores = 64;

struct Sample {
    bool  cpuValid   = false;
    float systemCpu  = 0.0f;              // whole machine, 0..100
    float processCpu = 0.0f;              // X-Plane, 0..100 of the whole machine
    int   cores      = 0;                 // logical processors reported
    float core[kMaxCores] = {0.0f};       // per logical processor, 0..100

    bool  gpuValid = false;
    float gpu      = 0.0f;                // busiest 3D engine total, 0..100
};

// Idempotent. Called the first time a performance pane is drawn rather than at
// plugin start: a session that never opens one should not spin a thread or open a
// PDH query.
void Start();
void Stop();
bool Running();

// The most recent sample, copied under the lock. Cheap enough to call per frame.
Sample Read();

// "" while everything is working; otherwise one sentence saying what is missing.
// Copied under the same lock, so it is safe to hold and log.
std::string Note();

}  // namespace SysMetrics
