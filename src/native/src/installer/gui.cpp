#include "installer/gui.h"

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "core/actions.h"
#include "core/constants.h"
#include "core/detect.h"
#include "core/fsutil.h"
#include "core/manifest.h"
#include "core/version.h"
#include "installer/log.h"
#include "installer/platform.h"

// Generated from ui/app.slint by slint_target_sources(). The header is named for
// the .slint file, not for the component inside it.
#include "app.h"

namespace photon {
namespace installer {
namespace {

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

    // ⚠ ONE STRING, NOT A MODEL OF LINES. The Run screen renders the log in a
    // read-only TextInput so it can be selected and copied, which needs the whole
    // thing as one value; keeping a line model beside it would be a second
    // representation of the same log to hold in step.
    //
    // ⚠ TOUCHED ON THE UI THREAD ONLY. The worker never appends to it directly —
    // it posts through invoke_from_event_loop, the same discipline the model
    // needed, and for the same reason.
    std::shared_ptr<std::string> log = std::make_shared<std::string>();
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
void ApplyAvailability(Gui& g) {
    const detect::Aircraft* ac = g.Selected();
    if (ac == nullptr) return;

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
    std::shared_ptr<std::string> log;
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
void PostLine(const RunContext& ctx, const std::string& line) {
    auto weak = ctx.weak;
    auto text = ctx.log;
    const std::string copy = line;
    slint::invoke_from_event_loop([weak, text, copy] {
        if (!text->empty()) *text += "\n";
        *text += copy;
        if (auto ui = weak.lock()) (*ui)->set_log_text(slint::SharedString(*text));
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
        // The design's log lines are prefixed with a check; a warning or an error
        // is not a check, and showing one as such is how a failed step reads as a
        // completed one.
        const char* mark = (level == "ERROR" || level == "WARN") ? " !  " : " ✓  ";
        PostLine(ctx_, mark + msg);
    }

private:
    const RunContext& ctx_;
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

void StartRun(Gui& g) {
    const detect::Aircraft* ac = g.Selected();
    if (ac == nullptr) return;

    g.ui->set_step(kRun);
    g.ui->set_run_finished(false);
    g.ui->set_progress(0.0f);
    // ⚠ A DRY RUN MUST SAY SO, ON EVERY SCREEN IT REACHES. The summary on Complete
    // is conditional ("would be installed") because nothing was written — correct,
    // but unreadable if the run never announced itself, at which point the tense
    // looks like a copy bug rather than the mode. Anything that changes the
    // sentences has to change the headline above them too.
    g.ui->set_run_headline(g.dryRun
                               ? "Dry run - nothing is being written."
                               : (g.uninstalling ? "Removing..." : "Working..."));
    g.log->clear();
    g.ui->set_log_text(slint::SharedString());

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
        const bool hasLog = ctx.file != nullptr;
        const std::string headline =
            !ok ? "Something went wrong" : (ctx.dryRun ? "Dry run complete" : "Done");
        auto weak = ctx.weak;
        slint::invoke_from_event_loop([weak, ok, summary, hasLog, headline] {
            auto ui = weak.lock();
            if (!ui) return;
            (*ui)->set_progress(1.0f);
            (*ui)->set_run_finished(true);
            (*ui)->set_complete_headline(slint::SharedString(headline));
            (*ui)->set_complete_summary(slint::SharedString(summary));
            (*ui)->set_log_available(hasLog);
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
            g.ui->set_step(kComplete);
            break;
        case kComplete:
            slint::quit_event_loop();
            break;
        default:
            break;
    }
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

    g.ui->on_next([&g] { GoNext(g); });
    g.ui->on_back([&g] { GoBack(g); });

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
    g.ui->on_view_log([&g] {
        if (g.fileLog) platform::OpenInEditor(g.fileLog->Path());
    });
    g.ui->on_open_org([] { platform::OpenUrl(kXplaneOrgUrl); });
    g.ui->on_open_github([] { platform::OpenUrl(kGithubUrl); });

    g.ui->run();

    // ⚠ JOINED BEFORE Gui GOES OUT OF SCOPE. The worker holds `&g` and writes into
    // it; closing the window mid-install would otherwise leave it running against
    // a destroyed struct.
    if (g.worker.joinable()) g.worker.join();
    return 0;
}

}  // namespace installer
}  // namespace photon
