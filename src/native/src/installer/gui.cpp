#include "installer/gui.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/actions.h"
#include "core/constants.h"
#include "core/detect.h"
#include "core/fsutil.h"
#include "core/manifest.h"
#include "core/version.h"
#include "core/wingmod.h"
#include "installer/log.h"
#include "installer/platform.h"

// Generated from ui/app.slint by slint_target_sources(). The header is named for
// the .slint file, not for the component inside it.
#include "app.h"

#ifdef _WIN32
#    include <windows.h>

#    include "../../res/resource.h"
#endif

namespace photon {
namespace installer {
namespace {

#ifdef _WIN32
// ~1 s of 50 ms retries. The window normally exists on the very first tick; this
// is only a bound on how long we keep looking if it never does.
constexpr int kIconAttempts = 20;

// ⚠ THE TITLE BAR DOES NOT USE THE .rc ICON UNLESS WE PUT IT THERE.
//
// Measured 2026-08-14 on a live window: WM_GETICON returned a 256x256 icon and
// GetClassLongPtr(GCLP_HICONSM) returned NOTHING. Slint's `Window.icon` is one
// image, so winit hands Windows that single 256x256 bitmap as the window's SMALL
// icon and the frame squashes it to SM_CXSMICON (16 px) with a crude stretch --
// the authored 16 px entry sits in the .ico completely unused, and the result is
// visibly harsher than the artwork. Explorer and the taskbar were always fine,
// which is what makes this look like an artwork problem rather than a wiring one.
//
// LoadImage() with an explicit size picks the BEST-MATCHING entry out of the icon
// group, so this is what actually gets the hand-tuned 16 in front of the user.
//
// ⚠ THE .slint MUST NOT ALSO BIND `Window.icon`, and that is why it does not.
// Tried both ways: with the binding in place winit re-applies its own 256x256
// icon as it creates the window, AFTER this has run, and the override is silently
// replaced -- the instrumentation showed a clean hit=1 while the title bar kept
// the wrong icon. app.slint leaves the property empty and C++ fills it on the
// platforms that need it.
//
// ⚠ IT ALSO CANNOT RUN AT show() TIME -- there is no HWND until winit's first
// event-loop iteration, so it is armed on a timer and retries. A window this
// never finds keeps the GENERIC WINDOWS APPLICATION ICON: measured 2026-08-14,
// the shell does NOT fall back to the executable's own resource for the title
// bar, so "set nothing" is not a safe default here.
bool ApplyWindowIcon() {
    HMODULE self = GetModuleHandleW(nullptr);
    auto load = [self](int metricCx, int metricCy) {
        return static_cast<HICON>(LoadImageW(
            self, MAKEINTRESOURCEW(PHOTON_ICON_ID), IMAGE_ICON,
            GetSystemMetrics(metricCx), GetSystemMetrics(metricCy), LR_DEFAULTCOLOR));
    };
    HICON small = load(SM_CXSMICON, SM_CYSMICON);   // title bar, Alt-Tab strip
    HICON big = load(SM_CXICON, SM_CYICON);         // taskbar, task view

    // Every visible, unowned, titled top-level window on this thread. Found by
    // thread rather than by title text: the title is a user-visible string in
    // app.slint and matching on it would break silently the day it is reworded.
    // The has-a-title test only excludes winit's untitled helper windows.
    struct Found { HICON small; HICON big; int confirmed; } found{small, big, 0};
    EnumThreadWindows(
        GetCurrentThreadId(),
        [](HWND wnd, LPARAM param) -> BOOL {
            if (GetWindow(wnd, GW_OWNER) != nullptr || !IsWindowVisible(wnd)) return TRUE;
            if (GetWindowTextLengthW(wnd) == 0) return TRUE;
            auto* f = reinterpret_cast<Found*>(param);
            if (f->small) SendMessageW(wnd, WM_SETICON, ICON_SMALL, (LPARAM)f->small);
            if (f->big) SendMessageW(wnd, WM_SETICON, ICON_BIG, (LPARAM)f->big);
            // ⚠ READ IT BACK. "We sent WM_SETICON" is not success — the first
            // attempt painted a window that was not the one the frame draws, and
            // reported a clean hit while the title bar stayed generic. Only the
            // icon coming back out of the window it was set on proves anything.
            if (SendMessageW(wnd, WM_GETICON, ICON_SMALL, 0) == (LRESULT)f->small)
                ++f->confirmed;
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&found));
    return found.confirmed > 0;
}

// Re-arm until the icon is confirmed on a real window, then stop. Bounded so a
// backend that never makes a Win32 window (or a future headless mode) cannot
// leave a timer cycling for the life of the process.
//
// ⚠ THE STOP CONDITION IS THE READ-BACK, NOT "a window was found". The window the
// frame draws does not exist on the first tick, and an earlier version that
// stopped at the first window it saw painted a transient one and quit — leaving
// the real title bar showing the generic Windows application icon.
void ScheduleWindowIcon(int attemptsLeft) {
    slint::Timer::single_shot(std::chrono::milliseconds(attemptsLeft == kIconAttempts ? 0 : 50),
                              [attemptsLeft] {
        if (ApplyWindowIcon() || attemptsLeft <= 1) return;
        ScheduleWindowIcon(attemptsLeft - 1);
    });
}
#endif  // _WIN32

// How long a finished, clean run stays on the Run screen before it advances
// itself. ⚠ NOT ZERO, AND NOT TUNED FOR COMFORT — it is how long the progress bar
// needs to be seen at 100%. The bar reaching 1.0 and the step changing arrive in
// the same event-loop callback, so with no delay the last frame the Run screen
// ever paints is the one before the bar filled.
constexpr int kAutoAdvanceMs = 500;

// The step numbering app.slint documents. Kept as named constants because the
// bottom bar's behavior keys off them and `step == 5` in three files is how they
// drift apart.
enum Step {
    kSplash = 0,
    kDirectory = 1,
    kAircraft = 2,
    kFeatures = 3,
    kReview = 4,
    kRun = 5,
    kComplete = 6,
};

// `--wing`'s three values, indexed the way features.slint's radios are.
const char* WingName(int index) {
    switch (index) {
        case 1: return "realwings";
        case 2: return "durantula";
        default: return "stock";
    }
}

// `WingName` backwards, for reading a `wingmod::Detect` verdict into a radio.
//
// ⚠ THE TWO ARE ONE TABLE WRITTEN TWICE and must round-trip — `WingIndexFor` is
// what turns "this aircraft is running Durantula" into a checked box, and off by
// one it would silently preselect RealWings on a Durantula aircraft. That is the
// worst shape this can fail in: the wrong build installs cleanly, says so, and the
// wingtip lights hold still while the wing flexes, which needs the aircraft loaded
// and airborne to notice. `WingRoundTripTests` pins it.
int WingIndexFor(const std::string& token) {
    if (token == wingmod::kRealWings) return 1;
    if (token == wingmod::kDurantula) return 2;
    return 0;
}

// ⚠ THE SAME THREE, SPELLED FOR A HUMAN. `WingName` returns the `--wing` argument
// and must keep returning exactly that; putting "RealWings" in a sentence is a
// different job, and using the CLI token there is what made the Complete screen
// say "(realwings + ...)".
const char* WingLabel(int index) {
    switch (index) {
        case 1: return "RealWings";
        case 2: return "Durantula";
        default: return "";
    }
}

// ─── the wing-mod probe ──────────────────────────────────────────────────────
//
// `wingmod::Detect` off the UI thread, started when an aircraft is SELECTED and
// collected when the wizard reaches Features.
//
// ⚠ THE COST IT MOVES IS I/O, NOT ARITHMETIC, WHICH IS WHY STARTING IT EARLY
// MATTERS MORE THAN THREADING IT. The check reads all four `.acf` variants (9.2 MB
// on the A320/A321 here) and both wing OBJs (a further 3.8-4.4 MB) — measured warm,
// the whole of `status --json` around it is 17 ms, and the first read of the same
// aircraft is 40 ms; on a cold cache, a slow library drive or with a scanner in the
// path it is far more. It was being paid inside the Next click on the Aircraft
// screen, where a GUI-subsystem window does not repaint, so it read as the button
// sticking.
//
// ⚠ A THREAD ALONE WOULD ONLY MOVE THE WAIT — Features cannot open on the right
// radio until the answer exists, so a probe started at Next would still be waited
// on. Starting it at SELECTION overlaps it with the user reading the screen and
// travelling to the button, which is the one interval in this wizard where nobody
// is waiting on anything. By the time Next is pressed the answer is normally
// already sitting in `done_`.
//
// ⚠ ONE WORKER, ONE PENDING SLOT, RESULTS KEPT FOREVER. Arrowing through the
// aircraft list re-selects on every keypress (see aircraft.slint — the list has no
// separate focus cursor), so a thread per selection would spawn one per key. The
// pending slot is overwritten instead: at most one probe runs and one waits, and an
// aircraft already answered is never asked again.
//
// ⚠ IT NEVER CALLS `slint::invoke_from_event_loop` AND MUST NOT LEARN TO. The C++
// wrapper is `slint_post_event`, which `.unwrap()`s the Rust result — posting after
// the loop has terminated ABORTS THE PROCESS, and a worker cannot check "is the
// loop still running" without racing the answer. The hand-off is a poll on a
// `slint::Timer` instead (`kWingPollMs`): everything that touches `Gui` or the UI
// then runs on the UI thread by construction, and a timer cannot outlive the loop.
constexpr int kWingPollMs = 40;

class WingProbe {
public:
    ~WingProbe() { Stop(); }

    void Start() {
        state_ = std::make_shared<State>();
        thread_ = std::thread(&WingProbe::Loop, state_);
    }

    // ⚠ JOINED, NOT DETACHED, AND CALLED AFTER THE EVENT LOOP RETURNS. The worker
    // touches nothing but its own shared state, so this is about the thread rather
    // than about lifetime — but a detached one reading an aircraft folder while the
    // process tears down is a crash report nobody could explain.
    void Stop() {
        if (!thread_.joinable()) return;
        {
            std::lock_guard<std::mutex> lock(state_->m);
            state_->stop = true;
        }
        state_->cv.notify_all();
        thread_.join();
    }

    // Ask for `folder`'s verdict, superseding any request not yet started. Cheap
    // and idempotent: an answer already held, or already being worked on, is not
    // asked for twice.
    void Request(const detect::Aircraft& ac) {
        if (!thread_.joinable()) return;
        {
            std::lock_guard<std::mutex> lock(state_->m);
            if (state_->done.count(ac.folder) != 0) return;
            if (state_->running == ac.folder) return;
            state_->want = Job{ac.folder, ac.path, ac.airframe};
            state_->pending = true;
        }
        state_->cv.notify_one();
    }

    // The verdict for `folder`, if it has arrived. ⚠ NEVER BLOCKS AND NEVER WAITS:
    // "not yet" is an answer this caller has to be able to handle, and a version of
    // this that waited "just for a moment" would be the original freeze back with a
    // shorter fuse.
    //
    // ⚠ RETURNS A COPY. `Result` is a handful of strings, and handing back a
    // pointer into a container another thread inserts into is a rule that holds
    // only for as long as nobody replaces the map.
    bool Ready(const std::string& folder, wingmod::Result& out) const {
        if (!thread_.joinable()) return false;
        std::lock_guard<std::mutex> lock(state_->m);
        const auto it = state_->done.find(folder);
        if (it == state_->done.end()) return false;
        out = it->second;
        return true;
    }

private:
    struct Job {
        std::string folder, path, airframe;
    };

    struct State {
        mutable std::mutex m;
        std::condition_variable cv;
        bool stop = false;
        bool pending = false;
        Job want;
        // The folder being worked on right now, so a repeated request for it is not
        // queued behind itself.
        std::string running;
        std::map<std::string, wingmod::Result> done;
    };

    static void Loop(std::shared_ptr<State> state) {
        for (;;) {
            Job job;
            {
                std::unique_lock<std::mutex> lock(state->m);
                state->cv.wait(lock, [&] { return state->stop || state->pending; });
                if (state->stop) return;
                job = state->want;
                state->pending = false;
                state->running = job.folder;
            }
            // ⚠ OUTSIDE THE LOCK. This is the part that takes the milliseconds, and
            // `Ready` is called from the UI thread on every poll tick.
            wingmod::Result result = wingmod::Detect(job.path, job.airframe);
            {
                std::lock_guard<std::mutex> lock(state->m);
                state->done.emplace(job.folder, std::move(result));
                state->running.clear();
            }
        }
    }

    std::shared_ptr<State> state_;
    std::thread thread_;
};

// Everything the flow needs. Lives on RunGui's stack: `app->run()` blocks until
// the loop exits, so every callback that captures this outlives none of it.
//
// ⚠ An aggregate, initialized `Gui g{App::create(), ...}` — `slint::ComponentHandle`
// has no default constructor, so `ui` cannot be assigned after the fact.
struct Gui {
    slint::ComponentHandle<App> ui;
    // ⚠ TAKEN ONCE, ON THE UI THREAD. `ComponentHandle` is not thread-safe and
    // must not be touched from the worker; the weak handle is what crosses, and
    // `invoke_from_event_loop` is what makes locking it legal.
    slint::ComponentWeakHandle<App> weak;
    bool dryRun = false;

    std::string xplaneRoot;
    // ⚠ WHAT DETECTION FOUND, KEPT FOR THE WHOLE SESSION. "Was this found for
    // you?" is a property of the CURRENT path, not of the moment it was set, and
    // passing it as an argument to the status writer meant every later call — Next
    // off the directory screen, then Back to it — had to guess and passed `false`.
    // The path had not changed, so the screen contradicted itself.
    std::string autoDetectedRoot;
    std::vector<detect::Aircraft> aircraft;
    int selected = -1;

    // ⚠ THESE ARE INTENT, NOT THE EFFECTIVE VALUES, and nothing may clamp them to
    // what the SELECTED airframe supports. They used to be clamped in place, which
    // destroyed the choice permanently: look at the A330 once — no interior, no
    // wing mods — and `interior` was forced false, so picking an A320 afterwards
    // showed Cockpit Lighting silently unchecked with no way to know why. What the
    // airframe supports is applied at the point of use, by the two helpers below.
    bool exterior = true;
    bool displays = true;
    bool interior = true;
    int wing = 0;
    // The aircraft folder `wing` was last seeded for from `wingmod::Detect`.
    //
    // ⚠ IT IS WHAT KEEPS THE SEED A DEFAULT RATHER THAN AN OVERRIDE. Availability
    // is re-applied on every walk from Aircraft to Features, so without it a user
    // who corrected the radio, stepped back to check the aircraft and came forward
    // again would find their correction silently undone by the detector — the one
    // behavior a preselection must never have, since the whole reason to touch that
    // radio is that you believe the detector is wrong.
    std::string wingSeededFor;

    // `wingmod::Detect`, off the UI thread and started at selection — see WingProbe.
    WingProbe wingProbe;
    // The folder whose verdict Features is still waiting for, empty when nothing is
    // outstanding. ⚠ IT IS `wingSeededFor`'s RULE EXTENDED ACROSS THE WAIT, and it
    // has to be a folder rather than a bool: the answer can arrive for an aircraft
    // the user has since moved off, and applying it then would preselect one
    // aeroplane's wing mod on another. It is cleared by the seed landing, by the
    // user touching the radio (their choice ends the seed's claim, exactly as it
    // does on a second visit), and by choosing a different aircraft.
    std::string wingSeedPending;
    // ⚠ ARMED ONLY WHILE `wingSeedPending` IS SET. A `slint::Timer` fires on the UI
    // thread, which is what makes the hand-off free of any cross-thread post — see
    // the note on WingProbe — but a poll left running for the life of the window
    // would be a wakeup every 40 ms for an answer that arrived once.
    slint::Timer wingPoll;

    // ⚠ THE ONE COPY OF THE LOG, and the Slint model is built FROM it. It went back
    // to a line list on 2026-08-14 because the log is drawn as colored rows now
    // (green marker, white message, red for a line that reported something) and a
    // Slint TextInput carries one color — see screens/run.slint. Keeping the lines
    // here rather than only in the model is what lets `copy-log` join exactly what
    // the screen is drawing instead of a second rendering of it.
    //
    // ⚠ TOUCHED ON THE UI THREAD ONLY. The worker never appends directly — it posts
    // through invoke_from_event_loop, the same discipline the model needs.
    std::shared_ptr<std::vector<LogLine>> log =
        std::make_shared<std::vector<LogLine>>();
    std::unique_ptr<FileLog> fileLog;
    std::thread worker;
    bool uninstalling = false;

    const detect::Aircraft* Selected() const {
        if (selected < 0 || selected >= static_cast<int>(aircraft.size())) {
            return nullptr;
        }
        return &aircraft[static_cast<std::size_t>(selected)];
    }

    // Intent, narrowed by what the selected airframe actually offers. EVERY
    // consumer — the review list, the install options, the summary — must go
    // through these rather than reading the fields.
    bool EffectiveInterior() const {
        const detect::Aircraft* ac = Selected();
        return interior && ac != nullptr && AirframeHasInterior(ac->airframe);
    }
    int EffectiveWing() const {
        const detect::Aircraft* ac = Selected();
        if (ac == nullptr || WingsFor(ac->airframe).size() <= 1) return 0;
        return wing;
    }
};

// ─── the aircraft list ───────────────────────────────────────────────────────

// The badge states. ⚠ These numbers are wire — `AircraftEntry.kind` in
// widgets.slint spells out the same four and maps them to colors.
enum BadgeKind {
    kBadgeNone = 0,     // nothing installed
    kBadgeUpdate = 1,   // installed, older version   -> accent
    kBadgeCurrent = 2,  // installed and up to date   -> green
    kBadgeBroken = 3,   // marker gone, reinstall     -> red
};

// ⚠ THE WORDING IS THE FRONT-END'S JOB. photoncore reports `photon` (the installed
// version, or empty) and `stale`; turning that into "0.8.1 → 0.8.2" is
// presentation, which is why AircraftEntry carries rendered strings.
//
// ⚠ `stale` IS NOT "an update is available" — it is a BROKEN install. `detect.cpp`
// sets it when the manifest says Photon is installed but the OBJ's marker is gone,
// i.e. an aircraft update reverted our edit and left the bookkeeping behind. That
// aircraft needs reinstalling whatever version it claims, so it is its own state
// and not a flavor of "out of date". Folding the two together (which this function
// did at first) shows a blue UPDATE badge on an aircraft whose lighting is simply
// missing, and the version numbers either side of the arrow can be identical.
AircraftEntry EntryFor(const detect::Aircraft& ac) {
    AircraftEntry e;
    e.name = slint::SharedString(ac.name.empty() ? ac.folder : ac.name);
    // ⚠ SHOWN WITH ITS `Aircraft\` PARENT. `detect::Aircraft::folder` is relative
    // to the Aircraft/ directory, and on its own it reads as a bare name with no
    // indication of where the installer is looking — which matters most in the
    // case the relative path exists to express, a ToLiss folder nested in a
    // sub-hangar.
    e.folder = slint::SharedString("Aircraft\\" + ac.folder);
    e.version = slint::SharedString(ac.acVer);

    if (ac.photon.empty()) {
        e.status = slint::SharedString("Not installed");
        e.badge = slint::SharedString("");
        e.kind = kBadgeNone;
    } else if (ac.stale) {
        // ⚠ STATES THE OBSERVATION, NOT A DIAGNOSIS. `stale` means the manifest
        // claims an install and the OBJ's marker is gone; photoncore cannot tell
        // WHY. For a user that is almost always a ToLiss update reverting the OBJ
        // — but on a dev machine it is also the normal state after `deploy.ps1`,
        // which writes OBJs straight from the DSL and never stamps a marker. The
        // lighting is present and working in that case, so "the install was
        // reverted" would be flatly wrong there. Both causes have the same fix.
        e.status = slint::SharedString("Install incomplete - reinstall");
        e.badge = slint::SharedString("REPAIR");
        e.kind = kBadgeBroken;
    } else if (ac.photon != kPhotonVersion) {
        e.status = slint::SharedString(ac.photon + " → " + kPhotonVersion);
        e.badge = slint::SharedString("UPDATE");
        e.kind = kBadgeUpdate;
    } else {
        e.status = slint::SharedString("Up to date (" + ac.photon + ")");
        e.badge = slint::SharedString("INSTALLED");
        e.kind = kBadgeCurrent;
    }
    return e;
}

void PublishAircraft(Gui& g) {
    auto model = std::make_shared<slint::VectorModel<AircraftEntry>>();
    for (const detect::Aircraft& ac : g.aircraft) model->push_back(EntryFor(ac));
    g.ui->set_aircraft(model);
    g.ui->set_selected(g.selected);

    const detect::Aircraft* ac = g.Selected();
    g.ui->set_uninstall_available(ac != nullptr && !ac->photon.empty());
}

// ─── the X-Plane root ────────────────────────────────────────────────────────

// Validates the root and reports it on the directory screen. Returns whether it is
// usable, which is what Next is allowed to act on.
//
// ⚠ TAKES NO "was this detected" ARGUMENT — it compares against what detection
// actually found. Every caller was otherwise obliged to remember, and the one that
// forgot turned "found it for you" into a different sentence about an
// unchanged path.
bool ApplyRoot(Gui& g) {
    if (g.xplaneRoot.empty()) {
        g.ui->set_root_status("No X-Plane 12 install found - browse to it.");
        g.ui->set_root_status_ok(false);
        return false;
    }
    if (!detect::IsValidRoot(g.xplaneRoot)) {
        // ⚠ Names what is missing rather than saying "invalid". The usual mistake
        // is pointing one level too high or at the Steam library root, and both
        // look correct until you know Aircraft/ and Resources/ are the test.
        g.ui->set_root_status(
            "Not an X-Plane 12 folder - it needs Aircraft/ and Resources/.");
        g.ui->set_root_status_ok(false);
        return false;
    }
    // ⚠ NO ✓ ON EITHER OF THESE ANY MORE (2026-08-13). The directory screen now
    // paints a good status in the palette's green and a bad one in its red, so the
    // glyph was the state said twice — and it only ever appeared on these two, so
    // it also made the two failures above look like a different KIND of message
    // rather than the same line reporting something else.
    const bool untouched =
        !g.autoDetectedRoot.empty() && g.xplaneRoot == g.autoDetectedRoot;
    g.ui->set_root_status(untouched ? "Path was automatically found."
                                    : "Valid X-Plane 12 installation.");
    g.ui->set_root_status_ok(true);
    return true;
}

void RefreshAircraft(Gui& g) {
    g.aircraft = detect::DetectAircraft(g.xplaneRoot);
    g.selected = -1;
    PublishAircraft(g);
}

// The same re-detection, but holding on to the aircraft the user chose — matched by
// FOLDER, which is the only stable identity here (the index is a position in a list
// that was just rebuilt, and the name comes out of the .acf).
//
// ⚠ IT EXISTS BECAUSE THE PLAIN `RefreshAircraft` CANNOT BE USED ON THE WAY TO
// Complete. That one clears the selection, and since 2026-08-14 the rail can walk
// from Complete back to Review — where Install would then find `Selected()` null and
// do nothing at all, with no error and no screen change. A button that silently does
// nothing is the worst of the three ways this could go wrong.
void RefreshAircraftKeepingSelection(Gui& g) {
    const detect::Aircraft* was = g.Selected();
    const std::string folder = was == nullptr ? std::string() : was->folder;
    g.aircraft = detect::DetectAircraft(g.xplaneRoot);
    g.selected = -1;
    if (!folder.empty()) {
        for (std::size_t i = 0; i < g.aircraft.size(); ++i) {
            if (g.aircraft[i].folder == folder) {
                g.selected = static_cast<int>(i);
                break;
            }
        }
    }
    PublishAircraft(g);
}

// Whether every supported ToLiss aircraft found under this root now carries the
// current version. It drives which of Complete's two buttons is Primary and takes
// the focus ring (`all-up-to-date` in app.slint).
//
// ⚠ AN EMPTY LIST IS NOT "ALL UP TO DATE". It is unreachable from Complete — you
// cannot install without an aircraft — but the sentence "everything is current" is
// false about nothing, and the safe default of the two is the one that offers to go
// back to the list.
//
// ⚠ IT IS `kBadgeCurrent`, THE BADGE THE LIST ALREADY DRAWS GREEN, and reading it
// through `EntryFor` rather than re-deriving it from `photon`/`stale` is what keeps
// the two in step. A second spelling here would let the Complete screen call an
// aircraft current that the previous screen badged REPAIR.
bool AllUpToDate(const Gui& g) {
    if (g.aircraft.empty()) return false;
    for (const detect::Aircraft& ac : g.aircraft) {
        if (EntryFor(ac).kind != kBadgeCurrent) return false;
    }
    return true;
}

// Everything that has to happen on the way to Complete, from EITHER route into it.
//
// ⚠ RE-DETECTS FIRST. The run just changed what is installed, and every number on
// this screen would otherwise describe the state before it — including the one that
// decides which button is blue, which is precisely wrong in the case the feature
// exists for: install the last out-of-date aircraft and the screen would still offer
// "Modify another" as the primary action.
void EnterComplete(Gui& g) {
    RefreshAircraftKeepingSelection(g);
    g.ui->set_all_up_to_date(AllUpToDate(g));
    g.ui->set_step(kComplete);
}

// ─── features ────────────────────────────────────────────────────────────────

// ⚠ AVAILABILITY IS PER AIRFRAME AND MUST BE APPLIED, NOT JUST DISPLAYED. The
// A339 has no interior variant, and `resolve_mount()` falls back to whatever mount
// exists — so offering RealWings on an airframe with no such variant is how a
// stock OBJ ships mislabeled as a mod build.
//
// ⚠ THE FLAGS NOW HIDE THE ROWS OUTRIGHT, WHICH MAKES THESE CLAMPS LOAD-BEARING
// RATHER THAN BELT-AND-BRACES. A greyed control at least showed its state; a
// hidden one cannot, so a stale `interior = true` carried over from a previously
// selected A320 would reach `actions::Install` with nothing on screen to reveal
// it. Selecting an airframe is the only place this can be corrected.
// Which wing radio to open the Features screen on, from what is ACTUALLY DRAWING on
// this aircraft (`core/wingmod.h`). 0 — stock — whenever the answer is anything
// other than a confirmed mod this airframe offers.
//
// ⚠ THIS IS NOT THE SAME ACT AS `actions::Install`'s WING CHECK, and the rule there
// — "it reports, and that is all" — is not being broken. That one runs after the
// user has chosen, and a detector that silently substituted their choice would be
// unexplainable from the screen they clicked through. This one runs BEFORE anyone
// has chosen anything: it fills in a default, on screen, where it can be seen and
// changed. The rule is about not overriding an answer, not about not offering one.
//
// ⚠ IT MUST STILL BE A GUESS THE USER CAN OVERRULE, which is why the mismatch
// warning at install time stays exactly as it was. A wrong seed then produces the
// same WARN a wrong manual choice would.
//
// ⚠ CLAMPED TO WHAT THE AIRFRAME OFFERS. `wingmod::Detect` answers about the
// aeroplane, `WingsFor` about what Photon can build — and `resolve_mount()` falls
// back to whatever mount exists, so a variant the airframe has no build for is how
// a stock OBJ ships mislabeled as a mod build.
//
// ⚠ IT NO LONGER READS ANYTHING — the verdict is handed in (2026-08-14). The read
// is the `.acf` attachment tables and, for Durantula, both wing OBJs; it used to
// happen right here, inside the Next click, and 13 MB of file I/O in a click
// handler is a window that does not repaint. `WingProbe` does it on its own thread,
// started when the aircraft is selected. Everything below is the part that has to
// stay on the UI thread anyway: a clamp, a radio index and a log line.
int SeedWing(const detect::Aircraft& ac, const wingmod::Result& wm, FileLog* log) {
    if (WingsFor(ac.airframe).size() <= 1) return 0;

    const int index = wm.determined ? WingIndexFor(wm.wing) : 0;
    const std::vector<std::string>& wings = WingsFor(ac.airframe);
    const bool offered =
        std::find(wings.begin(), wings.end(), WingName(index)) != wings.end();
    const int seed = offered ? index : 0;

    // ⚠ RECORDED, BECAUSE A PRESELECTED RADIO EXPLAINS ITSELF TO NOBODY. The user
    // sees a box already checked; the only place that can say why is the log, and
    // it is also the only place a WRONG detection leaves a trace worth reading.
    if (log != nullptr) {
        log->Write("features: " + wm.summary + " - preselecting --wing " +
                   WingName(seed));
    }
    return seed;
}

// A verdict, put on screen. ⚠ UI THREAD ONLY — it is reached from `ApplyAvailability`
// and from the poll timer, and both of those run there.
void ApplyWingSeed(Gui& g, const detect::Aircraft& ac, const wingmod::Result& wm) {
    g.wingSeedPending.clear();
    g.wingPoll.stop();
    g.wing = SeedWing(ac, wm, g.fileLog.get());
    g.ui->set_wing(g.wing);
}

// Has the probe answered yet? Armed only while a seed is outstanding.
//
// ⚠ THE VERDICT CAN ARRIVE AFTER THE SCREEN IT WOULD FILL IN, and everything except
// "still on Features with this aircraft chosen" means drop it. Nothing is logged on
// the way out: `actions::Install` runs the same check per run and writes both the
// summary and the mismatch WARN, so a run that happened is described either way, and
// a wizard the user walked away from has nothing to describe.
//
// ⚠ IT DOES NOT RE-CHECK "did the user touch the radio" — `on_set_wing` clears
// `wingSeedPending` for that, which is the same act as `wingSeededFor` on a second
// visit: a hand-made choice ends the seed's claim on that radio for good. The reason
// to touch it at all is believing the detector is wrong.
void PollWingSeed(Gui& g) {
    if (g.wingSeedPending.empty()) {
        g.wingPoll.stop();
        return;
    }
    const detect::Aircraft* ac = g.Selected();
    const bool gone = ac == nullptr || ac->folder != g.wingSeedPending;
    // Past Features the answer is no longer a default being offered — Review has
    // already shown the user what will be installed, and moving it under them is the
    // silent substitution the whole feature is written not to be.
    if (gone || g.ui->get_step() > kFeatures) {
        g.wingSeedPending.clear();
        g.wingPoll.stop();
        return;
    }
    wingmod::Result wm;
    if (!g.wingProbe.Ready(g.wingSeedPending, wm)) return;   // still reading
    ApplyWingSeed(g, *ac, wm);
}

void ApplyAvailability(Gui& g) {
    const detect::Aircraft* ac = g.Selected();
    if (ac == nullptr) return;

    // ⚠ ONCE PER AIRCRAFT, NOT ONCE PER VISIT — see `wingSeededFor`. Keyed on the
    // folder rather than the index, which is a position in a list that detection
    // rebuilds.
    if (g.wingSeededFor != ac->folder) {
        g.wingSeededFor = ac->folder;
        wingmod::Result wm;
        if (g.wingProbe.Ready(ac->folder, wm)) {
            // The ordinary path: the probe started when the card was clicked and
            // finished while the user was still on the list.
            ApplyWingSeed(g, *ac, wm);
        } else {
            // ⚠ NEXT DOES NOT WAIT. Selecting a row and pressing Enter straight
            // away is one keystroke apart and beats any read; blocking here for the
            // rest of it — even "just for a moment" — is the freeze this was written
            // to remove, with a shorter fuse. Features opens on `stock`, which is
            // both the safe default and what the screen showed before there was a
            // detector at all, and the poll ticks the radio over when the answer
            // lands. ⚠ `Request` again, because a selection restored by
            // `RefreshAircraftKeepingSelection` never went through a click.
            g.wing = 0;
            g.wingSeedPending = ac->folder;
            g.wingProbe.Request(*ac);
            g.wingPoll.start(slint::TimerMode::Repeated,
                             std::chrono::milliseconds(kWingPollMs),
                             [&g] { PollWingSeed(g); });
        }
    }

    g.ui->set_interior_available(AirframeHasInterior(ac->airframe));
    g.ui->set_wings_available(WingsFor(ac->airframe).size() > 1);

    // ⚠ THE INTENT IS PUBLISHED UNCHANGED — no clamping here. The rows are hidden
    // when the airframe cannot use them, so there is nothing on screen for a
    // narrowed value to explain, and narrowing it would be the destructive write
    // documented on the fields. `EffectiveInterior`/`EffectiveWing` do the
    // narrowing where it matters.
    g.ui->set_interior(g.interior);
    g.ui->set_wing(g.wing);
    g.ui->set_exterior(g.exterior);
    g.ui->set_displays(g.displays);
}

// ─── review ──────────────────────────────────────────────────────────────────

void BuildReview(Gui& g) {
    const detect::Aircraft* ac = g.Selected();
    if (ac == nullptr) return;

    g.ui->set_xplane_root(slint::SharedString(g.xplaneRoot));
    // Same `Aircraft\` prefix the card shows, for the same reason — and so the two
    // screens do not disagree about what the folder is called.
    g.ui->set_aircraft_folder(slint::SharedString("Aircraft\\" + ac->folder));
    g.ui->set_aircraft_name(slint::SharedString(
        ac->name.empty() ? ac->airframe : ac->name +
                                              (ac->acVer.empty()
                                                   ? std::string()
                                                   : " " + ac->acVer)));
    g.ui->set_version_transition(slint::SharedString(
        (ac->photon.empty() ? std::string("Not installed") : ac->photon) +
        " → " + kPhotonVersion));

    auto attrs = std::make_shared<slint::VectorModel<Attribute>>();
    auto add = [&](const char* label, const std::string& value) {
        Attribute a;
        a.label = slint::SharedString(label);
        a.value = slint::SharedString(value);
        attrs->push_back(a);
    };
    add("Exterior lighting", g.exterior ? "Yes" : "No");
    // ⚠ Same rule as the cockpit row below: ABSENT, not "None", on an airframe
    // that offers no wing mods — the A330. `WingsFor(...).size() > 1` is the same
    // predicate that hides the radios on the Features screen, and it has to be, or
    // the two screens disagree about whether the choice exists.
    //
    // ⚠ `WingLabel`, not `WingName` — this is a sentence, not a CLI argument.
    if (WingsFor(ac->airframe).size() > 1) {
        add("Wing mod",
            g.EffectiveWing() == 0 ? "None" : WingLabel(g.EffectiveWing()));
    }
    // ⚠ "Enabled"/"Disabled", never "Yes"/"No". This row is not about whether
    // anything gets installed — the display-glow OBJ and its .acf attachment are
    // part of the base install either way — but about the runtime default this
    // seeds. See docs/installer_gui_plan.md §3.1.
    add("Display effects", g.displays ? "Enabled" : "Disabled");
    // ⚠ THE ROW IS ABSENT, NOT "No", ON AN AIRFRAME WITH NO COCKPIT MOD — the
    // A330, today. Same predicate the Features screen drops its row with
    // (`interior_available`), and it has to be the same one or the two screens
    // disagree about whether the feature exists. "Cockpit lighting: No" reports a
    // choice the user was never offered and could not have made; an absent row
    // says "not applicable", which is the true statement.
    if (AirframeHasInterior(ac->airframe)) {
        add("Cockpit lighting", g.EffectiveInterior() ? "Yes" : "No");
    }
    g.ui->set_attributes(attrs);
}

// ─── the run ─────────────────────────────────────────────────────────────────

// Everything the worker needs, COPIED before it starts.
//
// ⚠ The worker holds no reference to `Gui`. It used to, and that was two races in
// one: `ComponentHandle` touched off the UI thread, and `selected`/`wing` read
// while the UI thread could still be writing them. A snapshot has neither problem
// and costs a few strings.
struct RunContext {
    slint::ComponentWeakHandle<App> weak;
    std::shared_ptr<std::vector<LogLine>> log;
    FileLog* file = nullptr;
    std::string aircraftName;
    bool uninstalling = false;
    bool dryRun = false;
    bool interior = false;
    bool displays = false;
    int wing = 0;
};

// ⚠ EVERY UI TOUCH FROM THE WORKER GOES THROUGH invoke_from_event_loop. Slint's
// properties are not thread-safe, and writing straight from the install thread is
// the kind of race that works on the dev machine for months. The buffer is
// appended INSIDE the posted lambda for the same reason — it is UI-thread state
// that the worker only ever asks to be extended.
// ⚠ THE MODEL IS REBUILT, NOT APPENDED TO. `slint::VectorModel` does support
// push_back with a row-added notification, but the handle would then have to be
// kept alive and reachable from here — a second owner of UI state living on the
// worker's snapshot. A log is tens of lines and this runs once per line, so the
// rebuild is not worth optimizing into a lifetime problem.
// ⚠ BOTH MODELS COME OFF THE ONE VECTOR, and that is the point of doing the filter
// here rather than in Slint. The Run screen repeats the ERROR/WARN lines under a
// rule once the run has stopped; built from the same `bad` flag the rows are
// colored by, a line cannot be red in the log and absent from the summary, or
// worded differently in the two places. Slint has no filtered model, and the
// alternative — a repeater over every line with the good ones hidden — leaves
// invisible elements occupying their layout cells.
void PublishLog(const slint::ComponentWeakHandle<App>& weak,
                const std::vector<LogLine>& lines) {
    auto model = std::make_shared<slint::VectorModel<LogLine>>();
    auto bad = std::make_shared<slint::VectorModel<LogLine>>();
    for (const LogLine& line : lines) {
        model->push_back(line);
        if (line.bad) bad->push_back(line);
    }
    if (auto ui = weak.lock()) {
        (*ui)->set_log_lines(model);
        (*ui)->set_problems(bad);
    }
}

// ⚠ `bad` IS ERROR *OR* WARN, and it is the same predicate `RunLog::Clean` uses —
// so a line drawn red and a line that stops the Run screen advancing itself are by
// construction the same set. Two spellings of that rule would drift the first time
// a level was added.
void PostLine(const RunContext& ctx, const std::string& text, bool bad) {
    auto weak = ctx.weak;
    auto lines = ctx.log;
    LogLine line;
    line.text = slint::SharedString(text);
    line.bad = bad;
    slint::invoke_from_event_loop([weak, lines, line] {
        lines->push_back(line);
        PublishLog(weak, *lines);
    });
}

void PostProgress(const RunContext& ctx, float value) {
    auto weak = ctx.weak;
    slint::invoke_from_event_loop([weak, value] {
        if (auto ui = weak.lock()) (*ui)->set_progress(value);
    });
}

// The `actions::Log` the run writes through: the file log for the record, the UI
// list for the user.
class RunLog : public actions::Log {
public:
    explicit RunLog(const RunContext& ctx) : ctx_(ctx) {}

    void Write(const std::string& msg, const std::string& level) override {
        if (ctx_.file != nullptr) ctx_.file->Write(msg, level);
        // ⚠ NO MARKER IS PREFIXED ANY MORE. It used to be " ✓  " / " !  " glued
        // onto the string; the Run screen draws it from `bad` instead, as an SVG
        // and a color — because NEITHER embedded font contains U+2713 and that
        // checkmark was being drawn by a system fallback. Putting a glyph back
        // into this string gets it in the message's color and doubled.
        const bool bad = (level == "ERROR" || level == "WARN");
        if (bad) clean_ = false;
        PostLine(ctx_, msg, bad);
    }

    // Whether the run reported nothing at all to look at — no ERROR and, ⚠
    // deliberately, no WARN either. It is what the Run screen's auto-advance is
    // gated on: a warning is a problem the user is meant to READ (the wing-mod
    // mismatch is the live example — the installer proceeds with what was asked
    // and says so), and skipping past the only screen that shows it would make
    // the warning mechanism pointless. "Succeeded" and "had nothing to say" are
    // not the same question, and the bar answers the second.
    //
    // ⚠ Read on the WORKER thread once the run returns, which is the only place
    // it is safe: `Write` runs there too, and nothing else touches this.
    bool Clean() const { return clean_; }

private:
    const RunContext& ctx_;
    bool clean_ = true;
};

// ⚠ THE FRACTION IS THE WHOLE RUN'S, NOT ONE LOOP'S. It used to be "textures copied
// out of eleven", reported from the only loop that ever called it — so an install
// without the cockpit mod never moved the bar at all. `core/progress.h` has the cost
// model that replaced it and the measurements behind it.
//
// ⚠ `step` IS DELIBERATELY UNUSED HERE. The plan names every phase ("Skin glow",
// "Cockpit textures"), and showing it would be a real improvement — but the Run
// screen's one text line is the dry-run announcement, which must survive on every
// screen it reaches, so a phase label needs its own row in the design rather than
// borrowing that one.
void OnProgress(double fraction, const std::string& step, void* userData) {
    const auto* ctx = static_cast<const RunContext*>(userData);
    if (ctx == nullptr) return;
    (void)step;
    PostProgress(*ctx, static_cast<float>(fraction));
}

// The Complete screen's one-line summary, in the design's own shape:
//   "ToLiss Photon v0.9 (Durantula + Cockpit lighting) installed in ToLiss A320."
//
// ⚠ THE PARENTHESES LIST WHAT WAS *ADDED TO THE AIRCRAFT*, and nothing else. Two
// things wrongly appeared there:
//   * the wing mod under its `--wing` token ("realwings"), because `WingName` was
//     reused for prose — hence `WingLabel`;
//   * "Display effects", which is not an installed component at all. That
//     checkbox presets a RUNTIME default (docs/installer_gui_plan.md §3.1); the
//     display-glow OBJ ships either way, so listing it beside the cockpit mod
//     claimed an install that never happened and made the line read as noise.
// With both fixed the common case — stock wings, no interior — has no parentheses
// at all, which is the design's default frame.
std::string SummaryLine(const RunContext& ctx, bool ok, const std::string& error) {
    if (!ok) return "Failed: " + error;
    const std::string where =
        ctx.aircraftName.empty() ? "the aircraft" : ctx.aircraftName;
    if (ctx.uninstalling) {
        return std::string(ctx.dryRun ? "ToLiss Photon would be removed from "
                                      : "ToLiss Photon removed from ") +
               where + ".";
    }

    std::vector<std::string> parts;
    if (ctx.wing != 0) parts.push_back(WingLabel(ctx.wing));
    if (ctx.interior) parts.push_back("Cockpit lighting");

    std::string extras;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        extras += (i == 0 ? "" : " + ") + parts[i];
    }
    return std::string("ToLiss Photon v") + kPhotonVersion +
           (extras.empty() ? "" : " (" + extras + ")") +
           (ctx.dryRun ? " would be installed in " : " installed in ") + where + ".";
}

// The Run screen's heading, which has TWO states — the run in progress and the run
// over — because the screen keeps showing after the work stops: `run_finished` only
// unlocks Next, and Complete's "View log" comes straight back here to re-read it.
//
// ⚠ A DRY RUN MUST SAY SO, ON EVERY SCREEN IT REACHES, AND THAT INCLUDES THIS ONE
// AFTER IT HAS FINISHED — which is why the done state has its own dry-run wording
// instead of falling through to the plain "Finished.". The summary on Complete is
// conditional ("would be installed") because nothing was written — correct, but
// unreadable if the run never announced itself, at which point the tense looks like
// a copy bug rather than the mode. Anything that changes the sentences has to change
// the headline above them too.
//
// The verbs match the TUI's own step title (`ScreenPerform`), which has said
// "Installing…" / "Uninstalling…" all along.
std::string RunHeadline(bool uninstalling, bool dryRun) {
    if (dryRun) return "Dry run - nothing is being written.";
    return uninstalling ? "Uninstalling..." : "Installing...";
}

// ⚠ A FAILED RUN IS NOT "Finished." — it stopped. The log it is sitting above is
// the one the user is about to read, so the heading must not read as success while
// an ERROR line is on screen; Complete says the same thing over the summary.
std::string RunDoneHeadline(bool ok, bool dryRun) {
    if (!ok) return "Something went wrong.";
    return dryRun ? "Finished - nothing was written." : "Finished.";
}

// The Complete screen's heading. It names the OPERATION, since by this point the
// summary underneath is the only other thing saying which one ran — and a dry run
// names itself instead, for the reason above.
std::string CompleteHeadline(bool ok, bool uninstalling, bool dryRun) {
    if (!ok) return "Something went wrong";
    if (dryRun) return "Dry run complete";
    return uninstalling ? "Uninstall complete" : "Install complete";
}

void StartRun(Gui& g) {
    const detect::Aircraft* ac = g.Selected();
    if (ac == nullptr) return;

    g.ui->set_step(kRun);
    g.ui->set_run_finished(false);
    g.ui->set_progress(0.0f);
    // ⚠ CLEARED HERE, not only on success. A second run after a failed one would
    // otherwise open on a red bar and a failure glyph before it had done anything.
    g.ui->set_run_failed(false);
    g.ui->set_run_headline(
        slint::SharedString(RunHeadline(g.uninstalling, g.dryRun)));
    g.log->clear();
    PublishLog(g.weak, *g.log);

    actions::Options opts;
    opts.aircraftPath = ac->path;
    opts.airframeKey = ac->airframe;
    opts.xplaneRoot = g.xplaneRoot;
    // ⚠ EFFECTIVE, NOT INTENT. `--wing realwings` on an airframe with no such
    // variant is how a stock OBJ ships mislabeled as a mod build, and `interior`
    // on the A339 asks for a payload that does not exist.
    opts.wing = WingName(g.EffectiveWing());
    opts.interior = g.EffectiveInterior();
    opts.dryRun = g.dryRun;
    opts.progress = &OnProgress;

    // The snapshot the worker owns — see RunContext.
    RunContext ctx;
    ctx.weak = g.weak;
    ctx.log = g.log;
    ctx.file = g.fileLog.get();
    ctx.aircraftName = ac->name.empty() ? ac->folder : ac->name;
    ctx.uninstalling = g.uninstalling;
    ctx.dryRun = g.dryRun;
    ctx.interior = g.EffectiveInterior();
    ctx.displays = g.displays;
    ctx.wing = g.EffectiveWing();

    // ⚠ ON A WORKER THREAD, because the interior texture copy is ~57 MiB and a
    // blocked event loop is an unrepainting window that the OS offers to kill.
    // The TUI does the same thing for the same reason.
    if (g.worker.joinable()) g.worker.join();
    g.worker = std::thread([opts, ctx]() mutable {
        opts.progressUserData = &ctx;

        bool ok = true;
        std::string error;
        RunLog log(ctx);
        try {
            if (ctx.uninstalling) {
                actions::Uninstall(opts, log);
            } else {
                actions::Install(opts, log);
            }
        } catch (const std::exception& e) {
            ok = false;
            error = e.what();
            log.Write(error, "ERROR");
        }

        const std::string summary = SummaryLine(ctx, ok, error);
        // ⚠ A CLEAN RUN WALKS ITSELF TO Complete; ANYTHING ELSE STOPS ON THE LOG.
        // The Run screen is 288 px of log against a 26 px bar precisely so a run
        // that went wrong can be read where it happened — so the auto-advance has
        // to be off for exactly the runs worth reading, which is `ok` AND
        // `RunLog::Clean()` (see it: a WARN counts). A clean install has nothing
        // in that log anybody needs, and making the user press Next to leave it is
        // a step that exists only because the screen does.
        //
        // ⚠ IT IS NOT A ONE-WAY DOOR. Complete's "View log" returns to this screen
        // with the log still on it — a plain `step` assignment, and Run holds its
        // state — which is what makes advancing on the user's behalf acceptable at
        // all rather than merely convenient.
        const bool advance = ok && log.Clean();
        // ⚠ THE RUN'S DONE HEADLINE IS POSTED HERE, NOT DERIVED FROM `progress` IN
        // THE UI. The bar reaching 1.000 and the run being over are the same event
        // only because they travel in this one hop: the cost model can be short a
        // phase (`SetUnknownStepHook`), so a Slint-side `progress >= 1.0` would be a
        // heading that says "Finished." with work still running.
        const std::string runDone = RunDoneHeadline(ok, ctx.dryRun);
        const std::string headline = CompleteHeadline(ok, ctx.uninstalling, ctx.dryRun);
        auto weak = ctx.weak;
        slint::invoke_from_event_loop([weak, summary, runDone, headline,
                                       advance, ok] {
            auto ui = weak.lock();
            if (!ui) return;
            // ⚠ ONLY A SUCCESSFUL RUN IS FORCED TO 100%. A failed one stops where
            // it stopped, so the red bar also reports HOW FAR it got — which is
            // the first thing anyone reading the log wants to know, and which a
            // full red bar reading "100%" would flatly contradict.
            if (ok) (*ui)->set_progress(1.0f);
            (*ui)->set_run_failed(!ok);
            (*ui)->set_run_finished(true);
            (*ui)->set_run_headline(slint::SharedString(runDone));
            (*ui)->set_complete_headline(slint::SharedString(headline));
            (*ui)->set_complete_summary(slint::SharedString(summary));
            if (!advance) return;

            // ⚠ ON A TIMER, AND THE DELAY IS THE POINT. Stepping from inside this
            // lambda would change the screen in the same frame that sets the bar
            // to 1.0, so the bar would never be PAINTED at 100% — an install would
            // end on a bar frozen at whatever it last drew and a screen that
            // vanished, which reads as the run being cut short.
            slint::Timer::single_shot(std::chrono::milliseconds(kAutoAdvanceMs),
                                      [weak] {
                auto later = weak.lock();
                if (!later) return;
                // ⚠ IT PRESSES Next RATHER THAN SETTING THE STEP (2026-08-14), so
                // that BOTH routes to Complete go through `GoNext` and cannot
                // diverge. Arriving there now re-detects and works out which button
                // should be blue (`EnterComplete`), and a timer that assigned the
                // step would skip all of it — leaving exactly the case the flag
                // exists for, an auto-advanced clean install, reporting the state
                // from before the run.
                //
                // ⚠ ONLY IF THE USER IS STILL ON Run, which `GoNext`'s own switch
                // would also enforce — it is stated here as well because this is
                // where the reason lives: they can press Next (or Escape, which is
                // Back) inside the delay, and yanking someone off a screen they
                // just chose is worse than the extra click this exists to save.
                if ((*later)->get_step() == kRun) (*later)->invoke_next();
            });
        });
    });
}

// ─── the state machine ───────────────────────────────────────────────────────
// ⚠ HERE, NOT IN THE UI. app.slint decides which screen a step shows; what the
// steps ARE is decided beside the code that can answer "is this installable".

void GoNext(Gui& g) {
    switch (g.ui->get_step()) {
        case kSplash:
            g.ui->set_step(kDirectory);
            break;
        case kDirectory:
            if (!ApplyRoot(g)) return;
            RefreshAircraft(g);
            g.ui->set_step(kAircraft);
            break;
        case kAircraft:
            if (g.Selected() == nullptr) return;
            ApplyAvailability(g);
            g.ui->set_step(kFeatures);
            break;
        case kFeatures:
            BuildReview(g);
            g.ui->set_step(kReview);
            break;
        case kReview:
            g.uninstalling = false;
            StartRun(g);
            break;
        case kRun:
            EnterComplete(g);
            break;
        case kComplete:
            slint::quit_event_loop();
            break;
        default:
            break;
    }
}

// A click on a completed rail row. ⚠ THE RULE LIVES HERE, not in app.slint, which
// only decides what LOOKS clickable — same split as `next()` and `back()`, and the
// reason it is not merely belt-and-braces is that the two are written separately:
// a UI that offered a row it should not must still be unable to move the wizard.
//
// ⚠ IT IS A PLAIN ASSIGNMENT AND MUST STAY ONE, exactly like `GoBack`. Every step's
// data is built on the way FORWARD — `GoNext` re-detects aircraft, publishes
// per-airframe availability and assembles the review list — so a backward jump
// lands on screens that already hold their answers, and needs no side effects to
// be correct. Restoring the screen as it was IS the feature; a jump that quietly
// re-derived things would make going back to check what you chose show you
// something else.
//
// ⚠ THE ONE PLACE THAT IS VISIBLY STALE is Review reached after a run: its version
// transition was assembled before the install and still says "Not installed → x".
// That is a caption on a screen, not a decision — nothing acts on it, and the
// Install button under it re-runs an install that is idempotent by construction (the
// backup-once rule consults `added[]` precisely so a second pass cannot back up our
// own copy as the stock original). Re-detecting here to freshen the line would cost
// the invariant above, which is worth more.
//
// Three refusals, all of which the UI also expresses but none of which it owns:
//
// ⚠ STRICTLY BACKWARD. Forward is where the side effects are; a jump past a screen
// would arrive with the previous run's answers — a Review built for the aircraft
// you had selected before you changed the X-Plane root. This is also what makes
// "Next locks you back in" fall out for free rather than needing a high-water mark:
// there is no state to reset, because the rail can never reach ahead of you, and
// `GoNext` from wherever you landed rebuilds everything after it.
//
// ⚠ NEVER *TO* Run, from anywhere. Every other screen is a set of answers and
// re-showing it costs nothing — which is why this is reachable from Complete
// (2026-08-14, reversing the "Complete ⇄ Run is a closed loop" rule that stood for
// two days). Run is not a set of answers; it is an action that has already happened,
// and a rail row landing on a finished log with an enabled Next under it gives no
// sign of which run it belongs to. The "View log" button goes there and says so.
//
// ⚠ NEVER *FROM* A RUN IN PROGRESS. `run_finished`, not `step != kRun`, so that
// Run-via-View-log is not a dead end — the same distinction `rail-navigable` makes.
void GoTo(Gui& g, int target) {
    const int step = g.ui->get_step();
    if (step <= kSplash) return;
    if (step == kRun && !g.ui->get_run_finished()) return;
    if (target < kDirectory || target >= step) return;
    if (target == kRun) return;
    g.ui->set_step(target);
}

void GoBack(Gui& g) {
    const int step = g.ui->get_step();
    // ⚠ Aircraft steps back to the ROOT screen, not to itself: a different root
    // has different aircraft under it, so re-choosing the aircraft alone would be
    // a dead end. The TUI makes the same exception.
    if (step <= kDirectory) {
        g.ui->set_step(kSplash);
        return;
    }
    g.ui->set_step(step - 1);
}

}  // namespace

int RunGui(const std::string& xplaneRootOverride, bool dryRun) {
    auto app = App::create();
    // Aggregate init: `ComponentHandle` has no default constructor, so `ui` and
    // the weak handle beside it have to be supplied here rather than assigned.
    //
    // ⚠ `ComponentWeakHandle` IS CONSTRUCTED FROM THE HANDLE, not obtained from
    // the component. `app->as_weak()` is the RUST API and does not exist in C++;
    // it compiles to "as_weak is not a member of App".
    Gui g{app, slint::ComponentWeakHandle<App>(app)};
    g.dryRun = dryRun;
    g.fileLog = std::make_unique<FileLog>(kPhotonVersion, dryRun);

    g.ui->set_version(slint::SharedString(std::string("Version ") + kPhotonVersion));
    g.ui->set_dry_run(dryRun);

    // Detection up front, so the directory screen opens already answered.
    // ⚠ `--xplane-root` is NOT "detected": the user supplied it, so the screen
    // should not claim credit for finding it.
    g.xplaneRoot = xplaneRootOverride;
    if (g.xplaneRoot.empty()) {
        const auto candidates = detect::XplaneCandidates();
        if (!candidates.empty()) {
            g.xplaneRoot = candidates.front().path;
            g.autoDetectedRoot = g.xplaneRoot;
        }
    }
    g.ui->set_xplane_root(slint::SharedString(g.xplaneRoot));
    ApplyRoot(g);

    // ⚠ ONE THREAD FOR THE WHOLE SESSION, started before any callback can ask it
    // anything. It idles on a condition variable until an aircraft is selected.
    g.wingProbe.Start();

    g.ui->on_next([&g] { GoNext(g); });
    g.ui->on_back([&g] { GoBack(g); });
    g.ui->on_go_to_step([&g](int target) { GoTo(g, target); });

    g.ui->on_path_edited([&g](const slint::SharedString& p) {
        g.xplaneRoot = std::string(p);
        ApplyRoot(g);
    });

    g.ui->on_browse([&g] {
        const std::string picked =
            platform::PickFolder("Select your X-Plane 12 folder", g.xplaneRoot);
        if (picked.empty()) return;   // cancelled, or no picker on this platform
        g.xplaneRoot = picked;
        g.ui->set_xplane_root(slint::SharedString(picked));
        ApplyRoot(g);
    });

    g.ui->on_select_aircraft([&g](int index) {
        g.selected = index;
        g.ui->set_selected(index);
        const detect::Aircraft* ac = g.Selected();
        g.ui->set_uninstall_available(ac != nullptr && !ac->photon.empty());

        // ⚠ THE WING READ STARTS HERE, NOT AT Next, AND THAT IS THE OPTIMIZATION —
        // see WingProbe. It costs nothing to be wrong about which row the user will
        // settle on (arrowing down the list supersedes the request rather than
        // queueing one per key), and being right buys the whole read for free out of
        // the time between choosing an aircraft and pressing the button.
        //
        // ⚠ A SEED OUTSTANDING FOR ANOTHER AIRCRAFT IS ABANDONED HERE. Its answer
        // may still be on the way, and applying it after the user has moved would
        // preselect one aeroplane's wing mod on another.
        //
        // ⚠ FOR *ANOTHER* AIRCRAFT — re-selecting the one already being waited on
        // must leave the wait alone. Stepping back from Features to check the list
        // and clicking the same card again goes through here, and `wingSeededFor`
        // then refuses to seed a second time (correctly: that is what protects a
        // hand-made correction), so abandoning the wait as well would leave that
        // aircraft on `stock` for the rest of the session with the answer sitting
        // unread in the probe.
        if (ac == nullptr || ac->folder != g.wingSeedPending) {
            g.wingSeedPending.clear();
            g.wingPoll.stop();
        }
        if (ac != nullptr) g.wingProbe.Request(*ac);
    });

    // ⚠ TAKES THE INDEX FROM THE MENU, NOT `g.selected`. A right-click on a card
    // deliberately does not select it (see AircraftCard in widgets.slint), so the
    // row the user asked about and the row that is about to be installed to are
    // allowed to differ — reading `Selected()` here would open the wrong folder
    // whenever they do, and would be right often enough not to be noticed.
    //
    // ⚠ RANGE-CHECKED AGAINST THE LIVE LIST. The model and this vector are
    // rebuilt together by `PublishAircraft`, but the index arrives from the UI and
    // is the one thing here that photoncore did not produce.
    g.ui->on_open_aircraft_folder([&g](int index) {
        if (index < 0 || index >= static_cast<int>(g.aircraft.size())) return;
        platform::OpenFolder(g.aircraft[static_cast<std::size_t>(index)].path);
    });

    g.ui->on_toggle_exterior([&g] {
        g.exterior = !g.exterior;
        g.ui->set_exterior(g.exterior);
    });
    g.ui->on_toggle_displays([&g] {
        g.displays = !g.displays;
        g.ui->set_displays(g.displays);
    });
    // The guards stay even though the controls are now hidden rather than
    // disabled: they are what stops a callback arriving for a row that is not on
    // screen from writing a value the user cannot see or undo.
    g.ui->on_toggle_interior([&g] {
        if (!g.ui->get_interior_available()) return;
        g.interior = !g.interior;
        g.ui->set_interior(g.interior);
    });
    g.ui->on_set_wing([&g](int w) {
        if (!g.ui->get_wings_available()) return;
        // ⚠ A HAND-MADE CHOICE ENDS THE SEED'S CLAIM, and it is the same rule
        // `wingSeededFor` states for a second visit to this screen — the reason to
        // touch this radio at all is believing the detector is wrong, so a verdict
        // still in flight must not arrive and undo the correction.
        g.wingSeedPending.clear();
        g.wingPoll.stop();
        g.wing = w;
        g.ui->set_wing(w);
    });

    g.ui->on_uninstall([&g] {
        g.uninstalling = true;
        StartRun(g);
    });

    g.ui->on_modify_another([&g] {
        // Re-detect rather than reuse the list: the run just changed what is
        // installed, and showing the pre-install state would be a lie.
        RefreshAircraft(g);
        g.ui->set_step(kAircraft);
    });
    // ⚠ NAVIGATION, NOT `OpenInEditor` (changed 2026-08-14, hours after it landed).
    // The Run screen is still holding the log it drew — colored, marked up, and the
    // thing the user was looking at ninety seconds ago — so sending them to the OS
    // text viewer swapped a designed screen for a wall of timestamps. The file is
    // still written and still flushed per line; it is the record for afterwards.
    //
    // ⚠ SCOPED TO Complete even though it is the only screen with the button. The
    // callback is reachable from anywhere the bar is, and a stray one from step 2
    // would jump the user into a run screen for a run that never happened.
    g.ui->on_view_log([&g] {
        if (g.ui->get_step() == kComplete) g.ui->set_step(kRun);
    });
    // ⚠ JOINS THE SAME LINES THE SCREEN IS DRAWING, which is the whole reason the
    // vector is kept here rather than only in the Slint model. It is also why the
    // markers are NOT re-added: what lands on the clipboard is what is on screen,
    // and the file log — which does carry levels — is one button away.
    g.ui->on_copy_log([&g] {
        std::string joined;
        for (const LogLine& line : *g.log) {
            if (!joined.empty()) joined += "\n";
            joined += std::string(line.text);
        }
        platform::SetClipboardText(joined);
    });
    // ⚠ READ BACK OUT OF THE UI RATHER THAN RE-RENDERED. `SummaryLine` runs on the
    // worker with a snapshot that is gone by now, and calling it again here would
    // need that snapshot rebuilt — at which point the copy could disagree with the
    // sentence the user right-clicked. The property IS the sentence.
    g.ui->on_copy_result([&g] {
        platform::SetClipboardText(std::string(g.ui->get_complete_summary()));
    });
    g.ui->on_open_org([] { platform::OpenUrl(kXplaneOrgUrl); });
    g.ui->on_open_github([] { platform::OpenUrl(kGithubUrl); });

    // ⚠ SPELLED OUT RATHER THAN `run()`, which is show + loop + hide in one call.
    // The window icon has to be corrected from INSIDE the loop -- the HWND does
    // not exist until winit's first iteration -- so the timer is armed here and
    // fires once the loop below is running. See ApplyWindowIcon().
    g.ui->show();
#ifdef _WIN32
    ScheduleWindowIcon(kIconAttempts);
#else
    // Everywhere else Slint's own property is the right mechanism; on Linux it is
    // the ONLY one. See the note on `window-icon` in app.slint.
    g.ui->set_window_icon(g.ui->get_icon_source());
#endif
    slint::run_event_loop();
    g.ui->hide();

    // ⚠ JOINED BEFORE Gui GOES OUT OF SCOPE. The worker holds `&g` and writes into
    // it; closing the window mid-install would otherwise leave it running against
    // a destroyed struct.
    if (g.worker.joinable()) g.worker.join();
    // The probe holds none of `g` — only its own shared state — but a thread still
    // reading an aircraft folder while the process tears down is nobody's idea of a
    // clean exit. `~WingProbe` would do this anyway; it is stated here so the two
    // threads this file starts are also both stopped here.
    g.wingProbe.Stop();
    return 0;
}

}  // namespace installer
}  // namespace photon
