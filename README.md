# ToLiss Photon
Overhauled exterior lighting for ToLiss aircraft in X-Plane 12.

## Supported Aircraft
* **ToLiss Airbus A319**
* **ToLiss Airbus A320** (NEO & CEO)
* **ToLiss Airbus A321** (CEO & NEO)
* **ToLiss Airbus A330-900**

# Installation
**1. Download the latest release installer for your platform from the [Releases](../../releases) page and unzip it.**

**2. Extract the installer archive in your downloads folder (or wherever you want).**

**3. Run the installer file.**

**_4. For Durantula and RealWings mod users:_ Run the installer after the wing mod already been installed.**

# Features
* Complete rework of exterior lighting for supported ToLiss aircraft
* Reduced excessive light intensity of default lights
* Realistic red beacon flash behavior
* Improved white strobe flash behavior
* Ability to select between new LED lighting or older halogen/incandescent and Xenon flash-tube lighting
* **Optional interior (cockpit) lighting**, switchable between old halogen, new halogen, and LED — by [GusRodrigues](https://forums.x-plane.org/profile/6767-gusrodrigues/), integrated with permission (A319/A320/A321 only)
* Optionally change individual lights for a custom lighting configuration
* Instantly adjust lighting options while loaded in the aircraft
* Preferences are saved per aircraft livery

## Color Effects
* Halogen lamps emit a visibly warmer color temperature with desaturated navigation lights
* White LEDs present a cool-white appearance while red and green are highly saturated
* The flash-tube beacon trends slightly pink as a result of the bluish Xenon being filtered through the red glass

## Improved Beacon and Strobe Flashing
* LED-style beacon blinks realistically instead of fading in and out
* Xenon flash-tubes mimic real-world behavior with instant-on and imperceptible fade-out
* Strobe flash appearance appropriately varies depending on type

## Available Light Profiles
* **Classic**: Full halogen/incandescent lighting with xenon flash-tubes on the strobes and beacons.
* **Hybrid LED**: LEDs for navigation and anti-collision lights. Halogen/incandescent for illumination. (c. 2015+)
* **Full LED**: All exterior lights are LED. (c. 2022+)
* **Auto**: Automatically selects profile based on aircraft equipment.
* **Custom**: Change each light individually for a custom, mixed configuration:
  * Taxi Light: Halogen or LED
  * Takeoff Light: Halogen or LED
  * Runway Turnoff Lights: Halogen or LED
  * Landing Lights: Halogen or LED
  * Wing Inspection Lights: Halogen or LED
  * Beacon: Xenon or LED
  * Strobes: Xenon or LED
  * Logo Lights: Halogen or LED

## Interior (Cockpit) Lighting
An **optional** add-on, offered during install and easy to decline if you only want the
exterior mod. The cockpit lighting itself — its design, placement, intensities, and the
three looks — is the work of [GusRodrigues](https://forums.x-plane.org/profile/6767-gusrodrigues/),
used with permission. Photon's only contribution is folding his three separate variant
files into one that can be switched in-sim, the same way the exterior lights work.

* **Old Halogen**: the warm orange cockpit of an early A320.
* **New Halogen**: a lighter amber, as fitted to later builds.
* **LED**: cool white domes and flood, warm white on the smaller lamps.
* **Auto**: follows whatever era the exterior profile resolved to.
* **Custom**: set each group independently — Dome Lights, Map Lights, Main Panel Flood,
  Pedestal & Tables, and Console Lights. Every group offers all three looks.

There is also a **Reduced light count** option (Cockpit tab, or the Cockpit sub-menu). The
main panel floods are drawn as a stack of two or three overlapping lights each, which is
what gives them their soft edge; this replaces each stack with one wider, brighter light —
same colors, same knobs, six fewer lights for your GPU. Worth a try if the cockpit costs
you frames.

Installing it also replaces a set of cockpit textures and adjusts the aircraft's `.acf`
files so the sim's three built-in cockpit spotlights follow the same switch. Uninstalling
puts all of it back. Not available for the A330-900.

> If you also install Gus's original zip **by hand**, do it *before* running this
> installer, not after — his package contains its own exterior light object, which would
> overwrite Photon's and silently revert the exterior mod.

## Changelog
### v0.5 (2026-07-24)
* All new cross-platform installers. Simply download for your platform, unzip, and run the installer file from your downloads folder.
* New 100% native plugin. XPPython3 is no longer required and can be removed if desired.
* Full support for ToLiss A330-900.
* Full support on A319/A320/A321 for Durantula's Wing Mod (wing-flex + flaps).
* Full support on A319/A320/A321 for RealWings (wings).
* Slightly tweaked strobe and beacon timing on both LED and Xenon versions (special thanks to tuongminh5833).
* Updated automatic light profile detection logic.

### v0.2-beta (2026-07-10)
* Moved all scripting to single Python script (removed Lua).
* Updated A321 lighting (ToLiss A321 v1.8 -> v1.9)

### v0.1-beta (2026-07-09)
Initial release.

# Known Issues
* Nosegear and landing billboard lights on the A321 may still appear as though they're halogen even though they should be LED. This is unfortunately due to ToLiss using an older, less-flexible lighting system for these specific lights. I'm investigating possible workarounds.

# About A320 Lighting
The A320 originally used incandescent halogen light bulbs for exterior illumination and navigation lights, and Xenon flash-tubes for both the red beacons and white strobes. The transition to LEDs did not begin until 2015. Initially, only the navigation, beacon and strobe lights were switched to LED. While aftermarket LED bulbs were widely available, Airbus itself did not ship all-LED aircraft until about 2022, corresponding with the launch of the Multifunction Runway Light (MFRL), which merged taxi, takeoff and landing lights into a unified housing on each wing.

For all aircraft manufactured prior to 2022, the actual lighting kits and configurations currently in use vary by operator. Some have retrofitted their fleets to all-LED, some still only use halogen and Xenon flash-tubes, but most are mixed. For that reason, this plugin supports per-light customization.

While LED illumination lights can be readily identified by their cool-white color temperature, Xenon flash-tube strobes can be harder to spot. In general, the red beacon and white tail strobe will remain lit for 0.1s if they are LED. When powered by a conventional Xenon flash-tube, then the flashing is essentially instantaneous. Seen on video during the day, Xenon lights are often flashing too fast to be consistently captured by the camera.

# About
I created an emergency lighting system for Garry's Mod called Photon and have extensive experience working with lighting effects in 3D engines.

This project initially began as an attempt to correct the unrealistic fade-in/fade-out appearance of the beacon light. The scope significantly expanded the more I learned about how X-Plane lighting effects work. The light colors, appearance, and behavior were painsakingly adjusted by hand. I then leveraged Claude Code to implement much of the coding and add quality-of-life functionality.

## Photon
This plugin is a Photon project. Join the [Photon Community Discord](http://photon.lighting/discord) and see the `#x-plane` channel.

## How it Works
Light positioning, color and intensity is hard-coded in plain-text .OBJ aircraft files. X-Plane uses two major types of light: "billboards" and "spill lights". Billboards are 2D sprites that give a light source its on-screen glow, while spill lights are what actually illuminate surrounding objects in the 3D environment.

To make billboard lights convincing, they need to have a defined direction. When the camera directly faces them, the light is at its most intense appearance. As the camera pans around away from the light, the intensity drops and eventually the light disappears entirely. Many of the default ToLiss billboard lights skip that second step, which is why they are often visible from all 360 degrees, and it is the primary reason for their low-quality appearance.

ToLiss Photon modifies every single light in the .OBJ and specifies a light direction for everything except the upper and lower beacons (as they are truly visible from 360 degrees). It also uses custom DataRefs to offer a different appearance for halogen/Xenon lights and LED lights.

The strobe and beacon flashing is controlled by the ToLiss Photon plugin. It runs a function every frame that overrides specific DataRefs set by ToLiss that control light intensity for both the beacon and the strobe. By default, ToLiss uses a sine wave (or similar equation) to control the brightness of the beacon, which means it smoothly fades in and out. While this kind of fading _can_ occur with halogen bulbs as they quickly warm and cool, the beacon is not halogen. It's either a Xenon flash-tube or LED. Neither option produces anything like the perceptible fade-in/fade-out you see.


### Components
* Aircraft .obj files: Modifies light colors, positioning, and appearance.
* ToLiss Photon plugin: Controls beacon/strobe flash behavior, provides the custom DataRefs the .obj files read for their halogen/Xenon vs. LED appearance, and adds the Plugins sub-menu and per-livery profile saving/loading. It's a compiled native plugin, so it needs **no XPPython3, FlyWithLua, or other add-on**.

## Other Recommended Mods
* [KOSP Project](https://store.x-plane.org/KOSP-PROJECT--A319-A320-A321-Full-Soundscape_p_1773.html) for audio
* [GusRodrigues'](https://forums.x-plane.org/profile/6767-gusrodrigues/) A320 Family Light Mod ([A319](https://forums.x-plane.org/files/file/93336-a319-light-mod/), [A320](https://forums.x-plane.org/files/file/93337-a320-light-mod/), [A321](https://forums.x-plane.org/files/file/93338-a321-light-mod/)) for additional lighting improvements
  * _The **interior** half of these mods is now included in ToLiss Photon as an optional install (see [Interior (Cockpit) Lighting](#interior-cockpit-lighting) above) — you don't need to install it separately._
  * _These mods also optionally provide improved **exterior** lighting. That part has a different artistic direction compared to ToLiss Photon and is a good alternative if you decide you don't like this mod. Note the two can't coexist: installing Gus's zip by hand after Photon will overwrite Photon's exterior lights._

## Credits
The **interior (cockpit) lighting is entirely the work of [GusRodrigues](https://forums.x-plane.org/profile/6767-gusrodrigues/)** and is included here with his permission. The light design, placement, intensities, color looks, and the cockpit textures that ship with it are all his; ToLiss Photon only makes them switchable in-sim. If you like how the cockpit looks, that's Gus. Go say thanks on [his profile](https://forums.x-plane.org/profile/6767-gusrodrigues/).

The original .obj lighting files were authored by ToLiss Simulations. Coding was done with the assistance of Claude Code (Sonnet 5, Opus 4.8 and Fable 5). 

The file you're currently reading was still 100% written by me (schmal), a real human.

## Disclaimer
This project is an independent third-party modification and is not affiliated with ToLiss Simulations Solutions Inc.

## Usage
ToLiss Photon is licensed under the **GNU General Public License v3.0** (see `LICENSE`). If you've discovered this addon and see that it's been years since I've updated it or fixed any bugs, feel free to fork it, fix it, and reupload it yourself — the GPL exists to make sure you can. What it asks in return is that your version stays open too: ship the source, and keep it under the GPL.

The graphical installer is built with [Slint](https://slint.dev/), used here under its GPLv3 option, which is why the project as a whole is GPLv3.

Third-party components keep their own licenses: the cockpit lighting data in `reference/gus/` is Gus Rodrigues's work, vendored with permission; `src/native/third_party/` holds Dear ImGui, nlohmann/json and stb; and the embedded Roboto fonts are Apache 2.0. All are compatible with the GPL.
