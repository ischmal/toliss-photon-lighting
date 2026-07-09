-- if true then return end
--[[==========================================================================
  ToLiss Photon Lighting  -  exterior light reshaper  (FlyWithLua, X-Plane 12)
  ---------------------------------------------------------------------------
  Per-category light types now live in the Python plugin (PI_ToLissPhoton.py),
  which resolves the chosen profile into nine ToLissPhoton/exterior/* datarefs
  (0 = halogen/xenon, 1 = LED) that the OBJ reads for its colors.

  This script only reshapes the BEACON and STROBE flash waveforms, and it now
  follows the two category datarefs INDEPENDENTLY:
      ToLissPhoton/exterior/beacon  -> 1 = crisp square, 0 = xenon (strike + decay)
      ToLissPhoton/exterior/strobe  -> 1 = crisp square, 0 = xenon (strike + decay)
  (Both datarefs are owned by the Python plugin; this script binds to them.)

  SPLIT OF RESPONSIBILITIES:
    Python plugin : registers all category datarefs at sim startup, shows the
                    menu only on ToLiss aircraft, resolves the profile into the
                    category values, and saves/loads the selection per livery.
    This script   : the per-frame beacon/strobe flash waveform engine, picking a
                    waveform per light from that light's category dataref.

  ---- WAVEFORM MODEL -------------------------------------------------------
  Each light is a Channel with a cycle Period and a list of Pulses:
      { At, Duration, Shape, Hold, Tau, Peak }
    Shape = "Square" | "Decay" | "Triangle" | "Sine" | "RampUp" | "RampDown"
    Hold  = fraction of the pulse held at full peak before the shape runs (0..1)
    Tau   = decay sharpness for Shape="Decay" (smaller = sharper)
    Peak  = 0..1 peak brightness (default 1)
  Advancing uses a carry-preserving timer (no drift); rendering uses a RenderAge
  that resets to 0 at each onset, so the onset frame is always the exact peak and
  low framerates collapse to one peak frame then zero (no half-finished fade).
============================================================================]]

------------------------------------------------------------------ user toggles
local ForceEnable = false        -- true = ignore the aircraft check (testing)

-- On/off gating. "auto" infers from ToLiss's own value (self-contained, ~1 s
-- turn-off lag). A dataref path gates on a signal instead (zero lag).
-- Strobes are gated on anim/winglex (the dataref the ToLiss OBJ uses to swing
-- the strobe billboards in and out of view: ANIM_rotate ... anim/winglex).
local Gate = { Beacon = "auto", Strobe = "sim/cockpit/electrical/strobe_lights_on" }
local GateThreshold = 0.5         -- a dataref gate counts as "on" above this
local InvertStrobeGate = false    -- set true if strobes come out inverted

local ActivityThreshold     = 0.05    -- value above this = "ToLiss is lighting it"
local ActivityWindowSeconds = 1.2     -- hold "active" this long after last seen lit
local MaxDeltaTime          = 0.25    -- clamp frame dt (smooths over pauses)

------------------------------------------------------------------ waveforms
-- [0] = LED (square), [1] = Classic (halogen/xenon decay). The beacon half and
-- the strobe half are selected independently, from the beacon/strobe datarefs.
-- Strobe indices are guesses until confirmed: drive one at a time and watch the
-- aircraft. A320 wingtips double-flash; the tail flashes once.
local Config = { Profiles = {

  [0] = { Name = "LED",
    Beacon = { Period = 1.0, Offset = 0.5,
               Pulses = { { At = 0.00, Duration = 0.1, Shape = "Square" } } },
    Strobe = { Period = 1.0, Offset = 0.0, Index = {
        [0] = { Pulses = { { At = 0.00, Duration = 0.05, Shape = "Square" },
                           { At = 0.15, Duration = 0.05, Shape = "Square" } } },  -- double
        [1] = { Pulses = { { At = 0.00, Duration = 0.05, Shape = "Square" },
                           { At = 0.15, Duration = 0.05, Shape = "Square" } } },  -- double
        [2] = { Pulses = { { At = 0.00, Duration = 0.1, Shape = "Square" } } },  -- single
        [3] = { Pulses = { { At = 0.00, Duration = 0.08, Shape = "Square" } } },  -- single
    } },
  },

  [1] = { Name = "Classic",
    Beacon = { Period = 1.0, Offset = 0.5,
               Pulses = { { At = 0.00, Duration = 0.06, Hold = 0.40, Shape = "Decay", Tau = 0.30 } } },
    Strobe = { Period = 1.0, Offset = 0.0, Index = {
        [0] = { Pulses = { { At = 0.00, Duration = 0.04, Shape = "Square" },
                           { At = 0.12, Duration = 0.04, Shape = "Square" } } },
        [1] = { Pulses = { { At = 0.00, Duration = 0.04, Shape = "Square" },
                           { At = 0.12, Duration = 0.04, Shape = "Square" } } },
        [2] = { Pulses = { { At = 0.00, Duration = 0.06, Hold = 0.40, Shape = "Decay", Tau = 0.30 } } },
        [3] = { Pulses = { { At = 0.00, Duration = 0.04, Shape = "Square" } } },
    } },
  },

} }

-- Screenshot Option
-- local Config = { Profiles = {

--   [0] = { Name = "LED",
--     Beacon = { Period = 1.0, Offset = 0,
--                Pulses = { { At = 0.00, Duration = 1, Shape = "Square" } } },
--     Strobe = { Period = 1.0, Offset = 0.0, Index = {
--         [0] = { Pulses = { { At = 0.00, Duration = 1, Shape = "Square" },
--                            { At = 0.00, Duration = 1, Shape = "Square" } } },  -- double
--         [1] = { Pulses = { { At = 0.00, Duration = 1, Shape = "Square" },
--                            { At = 0.00, Duration = 1, Shape = "Square" } } },  -- double
--         [2] = { Pulses = { { At = 0.00, Duration = 1, Shape = "Square" } } },  -- single
--         [3] = { Pulses = { { At = 0.00, Duration = 1, Shape = "Square" } } },  -- single
--     } },
--   },

--   [1] = { Name = "Classic",
--     Beacon = { Period = 1.0, Offset = 0,
--                Pulses = { { At = 0.00, Duration = 1, Shape = "Square"} } },
--     Strobe = { Period = 1.0, Offset = 0.0, Index = {
--         [0] = { Pulses = { { At = 0, Duration = 1, Shape = "Square" },
--                            { At = 0, Duration = 1, Shape = "Square" } } },
--         [1] = { Pulses = { { At = 0, Duration = 1, Shape = "Square" },
--                            { At = 0, Duration = 1, Shape = "Square" } } },
--         [2] = { Pulses = { { At = 0, Duration = 1, Shape = "Square"} } },
--         [3] = { Pulses = { { At = 0, Duration = 1, Shape = "Square" } } },
--     } },
--   },

-- } }

-- LED flag (0/1) -> waveform profile index (Profiles[0] = LED, Profiles[1] = Classic)
local function LedToProfileIndex(led) return (led >= 0.5) and 0 or 1 end

----------------------------------------------------------------- aircraft check
local function DetectToLiss()
  if ForceEnable then return true end
  local signature = ((AIRCRAFT_PATH or "") .. "|" .. (AIRCRAFT_FILENAME or "")):lower()
  return signature:find("toliss") ~= nil
end
local isActiveAircraft = DetectToLiss()
logMsg("ToLiss Photon: exterior lights " ..
       (isActiveAircraft and "ACTIVE" or "inactive (not a ToLiss aircraft)"))

----------------------------------------------------------------- datarefs
-- Lua-owned debug on/off for the flash effect (the colors are unaffected by it).
define_shared_DataRef("ToLissPhoton/exterior/enabled", "Int")
dataref("masterEnable", "ToLissPhoton/exterior/enabled", "writable")
masterEnable = 1

-- Per-light types are owned by PI_ToLissPhoton.py (registered at sim startup).
-- We read the two that drive a flash waveform. 0 = xenon/halogen, 1 = LED.
dataref("beaconLed", "ToLissPhoton/exterior/beacon", "readonly")
dataref("strobeLed", "ToLissPhoton/exterior/strobe", "readonly")

dataref("simTime",      "sim/time/total_running_time_sec", "readonly")
dataref("overrideFlag", "sim/flightmodel2/lights/override_beacons_and_strobes", "readonly")

dataref("beaconRatio0", "sim/flightmodel2/lights/beacon_brightness_ratio", "writable", 0)
dataref("beaconRatio1", "sim/flightmodel2/lights/beacon_brightness_ratio", "writable", 1)
dataref("beaconRatio2", "sim/flightmodel2/lights/beacon_brightness_ratio", "writable", 2)
dataref("beaconRatio3", "sim/flightmodel2/lights/beacon_brightness_ratio", "writable", 3)
dataref("strobeRatio0", "sim/flightmodel2/lights/strobe_brightness_ratio", "writable", 0)
dataref("strobeRatio1", "sim/flightmodel2/lights/strobe_brightness_ratio", "writable", 1)
dataref("strobeRatio2", "sim/flightmodel2/lights/strobe_brightness_ratio", "writable", 2)
dataref("strobeRatio3", "sim/flightmodel2/lights/strobe_brightness_ratio", "writable", 3)

-- optional switch-dataref gate (bound only when Gate is a path, not "auto")
local gateReaders = { Beacon = nil, Strobe = nil }
local function BindGate(arrayName)
  if Gate[arrayName] ~= "auto" then
    local variableName = "gateSwitch" .. arrayName
    dataref(variableName, Gate[arrayName], "readonly")
    gateReaders[arrayName] = function() return _G[variableName] end
  end
end
BindGate("Beacon")
BindGate("Strobe")

local function ClampBrightness(value)
  if value < 0.0 then return 0.0 elseif value > 1.0 then return 1.0 else return value end
end

local RatioGetters = {
  Beacon = { [0] = function() return beaconRatio0 end, [1] = function() return beaconRatio1 end,
             [2] = function() return beaconRatio2 end, [3] = function() return beaconRatio3 end },
  Strobe = { [0] = function() return strobeRatio0 end, [1] = function() return strobeRatio1 end,
             [2] = function() return strobeRatio2 end, [3] = function() return strobeRatio3 end },
}
local RatioSetters = {
  Beacon = { [0] = function(v) beaconRatio0 = ClampBrightness(v) end,
             [1] = function(v) beaconRatio1 = ClampBrightness(v) end,
             [2] = function(v) beaconRatio2 = ClampBrightness(v) end,
             [3] = function(v) beaconRatio3 = ClampBrightness(v) end },
  Strobe = { [0] = function(v) strobeRatio0 = ClampBrightness(v) end,
             [1] = function(v) strobeRatio1 = ClampBrightness(v) end,
             [2] = function(v) strobeRatio2 = ClampBrightness(v) end,
             [3] = function(v) strobeRatio3 = ClampBrightness(v) end },
}

----------------------------------------------------------------- helpers
local function Clamp(value, low, high)
  if value < low then return low elseif value > high then return high else return value end
end

local function Envelope(shape, progress, tau)
  if shape == "Square" then
    return 1.0
  elseif shape == "Decay" then
    -- normalised exponential: exactly 1.0 at progress 0, exactly 0.0 at progress 1,
    -- for any Tau. Smaller Tau = sharper initial drop.
    local t = tau or 0.30
    local tail = math.exp(-1.0 / t)
    return (math.exp(-progress / t) - tail) / (1.0 - tail)
  elseif shape == "RampUp" then
    return progress
  elseif shape == "RampDown" then
    return 1.0 - progress
  elseif shape == "Triangle" then
    return 1.0 - math.abs(2.0 * progress - 1.0)
  elseif shape == "Sine" then
    return math.sin(progress * math.pi)
  else
    return 1.0
  end
end

local function BuildSegments(spec)
  local pulses = {}
  for index, pulse in ipairs(spec.Pulses) do pulses[index] = pulse end
  table.sort(pulses, function(a, b) return a.At < b.At end)

  local segments = {}
  local cursor = 0.0
  for _, pulse in ipairs(pulses) do
    if pulse.At > cursor + 1e-6 then
      segments[#segments + 1] = { Duration = pulse.At - cursor, IsOn = false }
    end
    segments[#segments + 1] = {
      Duration = pulse.Duration, IsOn = true,
      Shape = pulse.Shape or "Square", Tau = pulse.Tau or 0.30,
      Hold = pulse.Hold or 0.0, Peak = pulse.Peak or 1.0,
    }
    cursor = math.max(cursor, pulse.At) + pulse.Duration
  end
  if cursor < spec.Period - 1e-6 then
    segments[#segments + 1] = { Duration = spec.Period - cursor, IsOn = false }
  elseif cursor > spec.Period + 1e-3 then
    logMsg(string.format("ToLiss Photon: WARNING pulses total %.3fs exceed period %.3fs", cursor, spec.Period))
  end
  return segments
end

local function BuildChannel(spec)
  local segments = BuildSegments(spec)
  local total = 0.0
  for _, segment in ipairs(segments) do total = total + segment.Duration end
  return {
    Segments = segments, Period = spec.Period, Offset = spec.Offset or 0.0,
    TotalDuration = total, SegmentIndex = 1, SegmentElapsed = 0.0, RenderAge = 0.0,
  }
end

local function SeekChannel(channel, targetTime)
  if channel.TotalDuration <= 0 then
    channel.SegmentIndex = 1; channel.SegmentElapsed = 0.0; channel.RenderAge = 0.0
    return
  end
  targetTime = targetTime % channel.TotalDuration
  if targetTime < 0 then targetTime = targetTime + channel.TotalDuration end
  local accumulated = 0.0
  for index, segment in ipairs(channel.Segments) do
    if targetTime < accumulated + segment.Duration then
      channel.SegmentIndex = index
      channel.SegmentElapsed = targetTime - accumulated
      channel.RenderAge = channel.SegmentElapsed
      return
    end
    accumulated = accumulated + segment.Duration
  end
  channel.SegmentIndex = #channel.Segments
  channel.SegmentElapsed = 0.0
  channel.RenderAge = 0.0
end

-- Render the current segment from RenderAge (onset = peak, low-FPS = peak-then-zero),
-- then advance using the carry-preserving SegmentElapsed (no drift).
local function TickChannel(channel, deltaTime)
  local segment = channel.Segments[channel.SegmentIndex]
  local brightness = 0.0
  if segment.IsOn and channel.RenderAge < segment.Duration then
    local progress = Clamp(channel.RenderAge / segment.Duration, 0.0, 1.0)
    local hold = segment.Hold or 0.0
    if hold >= 1.0 then
      progress = 0.0
    elseif progress > hold then
      progress = (progress - hold) / (1.0 - hold)
    else
      progress = 0.0
    end
    brightness = Envelope(segment.Shape, progress, segment.Tau) * (segment.Peak or 1.0)
  end

  channel.SegmentElapsed = channel.SegmentElapsed + deltaTime
  channel.RenderAge = channel.RenderAge + deltaTime
  if channel.SegmentElapsed >= segment.Duration then
    channel.SegmentElapsed = channel.SegmentElapsed - segment.Duration
    channel.SegmentIndex = channel.SegmentIndex + 1
    if channel.SegmentIndex > #channel.Segments then channel.SegmentIndex = 1 end
    channel.RenderAge = 0.0
  end
  return brightness
end

----------------------------------------------------------------- gating
local lastActiveTime = { Beacon = {}, Strobe = {} }

local function IsActive(arrayName, index, sampledValue)
  if sampledValue > ActivityThreshold then lastActiveTime[arrayName][index] = simTime end
  if Gate[arrayName] == "auto" then
    return (simTime - (lastActiveTime[arrayName][index] or -1e9)) < ActivityWindowSeconds
  end
  local reader = gateReaders[arrayName]
  if not reader then return false end
  local on = reader() > GateThreshold
  if arrayName == "Strobe" and InvertStrobeGate then on = not on end
  return on
end

local function ApplyIndex(arrayName, index, brightness)
  local sampledValue = RatioGetters[arrayName][index]()
  if IsActive(arrayName, index, sampledValue) then
    RatioSetters[arrayName][index](brightness)
  end
end

----------------------------------------------------------------- build / state
-- Beacon and strobe waveforms are rebuilt independently, each only when its own
-- category dataref flips between LED (1) and classic (0).
local beaconChannel = nil
local strobeChannels = {}
local currentBeaconLed = -1
local currentStrobeLed = -1

local function RebuildBeacon(led)
  currentBeaconLed = led
  local profile = Config.Profiles[LedToProfileIndex(led)]
  beaconChannel = BuildChannel(profile.Beacon)
  SeekChannel(beaconChannel, simTime - beaconChannel.Offset)
  logMsg("ToLiss Photon: beacon waveform -> " .. (led >= 0.5 and "LED" or "Classic"))
end

local function RebuildStrobe(led)
  currentStrobeLed = led
  local profile = Config.Profiles[LedToProfileIndex(led)]
  strobeChannels = {}
  for index, spec in pairs(profile.Strobe.Index) do
    local channel = BuildChannel({
      Period = profile.Strobe.Period, Offset = profile.Strobe.Offset, Pulses = spec.Pulses,
    })
    SeekChannel(channel, simTime - channel.Offset)
    strobeChannels[index] = channel
  end
  logMsg("ToLiss Photon: strobe waveform -> " .. (led >= 0.5 and "LED" or "Classic"))
end

----------------------------------------------------------------- main loop
local lastSimTime = -1.0
local overrideWarningShown = false

function ToLissPhotonFrame()
  if not isActiveAircraft then return end

  if lastSimTime < 0 then lastSimTime = simTime end
  local deltaTime = simTime - lastSimTime
  lastSimTime = simTime
  if deltaTime < 0 then deltaTime = 0 end
  if deltaTime > MaxDeltaTime then deltaTime = MaxDeltaTime end

  if masterEnable < 0.5 then return end

  if (not overrideWarningShown) and overrideFlag < 0.5 then
    logMsg("ToLiss Photon: NOTE override_beacons_and_strobes is 0 - effect may not show.")
    overrideWarningShown = true
  end

  -- follow each light's own category dataref, rebuilding only on a change
  local beaconFlag = (beaconLed >= 0.5) and 1 or 0
  local strobeFlag = (strobeLed >= 0.5) and 1 or 0
  if beaconFlag ~= currentBeaconLed then RebuildBeacon(beaconFlag) end
  if strobeFlag ~= currentStrobeLed then RebuildStrobe(strobeFlag) end

  local beaconBrightness = TickChannel(beaconChannel, deltaTime)
  ApplyIndex("Beacon", 0, beaconBrightness)
  ApplyIndex("Beacon", 1, beaconBrightness)
  ApplyIndex("Beacon", 2, beaconBrightness)
  ApplyIndex("Beacon", 3, beaconBrightness)

  for index, channel in pairs(strobeChannels) do
    ApplyIndex("Strobe", index, TickChannel(channel, deltaTime))
  end
end

do_every_frame("ToLissPhotonFrame()")

add_macro("ToLiss Photon: effect ON",  "masterEnable = 1")
add_macro("ToLiss Photon: effect OFF", "masterEnable = 0")
