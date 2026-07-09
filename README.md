# ToLiss Photon
Overhauled exterior lighting for ToLiss aircraft in X-Plane 12.

# Installation
**1. Install Requirements**
* [FlyWithLua NG+ for X-Plane 12](https://forums.x-plane.org/files/file/82888-flywithlua-ng-next-generation-plus-edition-for-x-plane-12-win-lin-mac/)
* [XPPython3 v4.7.0](https://xppython3.readthedocs.io/en/latest/usage/installation_plugin.html) or above

**2. Copy the `Resources` folder into your root `X-Plane 12` directory.**
When asked if you want to overwrite existing files, select **Yes**.

**3. Copy the `objects` folder for each aircraft into the corresponding `X-Plane 12\Aircraft\ToLissA3__V_p_p_` folder.**
When asked if you want to overwrite existing files, select **Yes**.

**4. Open X-Plane and proceed with any first-time install windows.**

**5. Reload or relaunch in a supported ToLiss aircraft. A ToLiss Photon configuration sub-menu should now appear in the Plugins menu.**

# Features
* Complete rework of exterior lighting for supported ToLiss aircraft
* Reduced excessive light intensity of default lights
* Realistic red beacon flash behavior
* Improved white strobe flash behavior
* Ability to select between new LED lighting or older halogen/incandescent and Xenon flash-tube lighting
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

## Supported Aircraft
* **ToLiss Airbus A320** (both NEO and CEO)
_Additional aircraft support being added._

# About A320 Lighting
The A320 originally used incandescent halogen light bulbs for exterior illumination and navigation lights, and Xenon flash-tubes for both the red beacons and white strobes. The transition to LEDs did not begin until 2015. Initially, only the navigation, beacon and strobe lights were switched to LED. While aftermarket LED bulbs were widely available, Airbus itself did not ship all-LED aircraft until about 2022, corresponding with the launch of the Multifunction Runway Light (MFRL), which merged taxi, takeoff and landing lights into a unified housing on each wing.

For all aircraft manufactured prior to 2022, the actual lighting kits and configurations currently in use vary by operator. Some have retrofitted their fleets to all-LED, some still only use halogen and Xenon flash-tubes, but most are mixed. For that reason, this plugin supports per-light customization.

While LED illumination lights can be readily identified by their cool-white color temperature, Xenon flash-tube strobes can be harder to spot. In general, the red beacon and white tail strobe will remain lit for 0.1s if they are LED. When powered by a conventional Xenon flash-tube, then the flashing is essentially instantaneous. Seen on video during the day, Xenon lights are often flashing too fast to be consistently captured by the camera.

# About
I created an emergency lighting system for Garry's Mod called Photon and have extensive experience working with lighting effects in 3D engines.

This project initially began as an attempt to correct the unrealistic fade-in/fade-out appearance of the beacon light. The scope significantly expanded the more I learned about how X-Plane lighting effects work. The light colors, appearance, and behavior were painsakingly adjusted by hand. I then leveraged Claude Code to implement much of the coding and add quality-of-life functionality.

## Photon
This plugin is a Photon project. Join the (Photon Community Discord)[http://photon.lighting/discord] and see the `#x-plane` channel.

## How it Works
Light positioning, color and intensity is hard-coded in plain-text .OBJ aircraft files. X-Plane uses two major types of light: "billboards" and "spill lights". Billboards are 2D sprites that give a light source its on-screen glow, while spill lights are what actually illuminate surrounding objects in the 3D environment.

To make billboard lights convincing, they need to have a defined direction. When the camera directly faces them, the light is at its most intense appearance. As the camera pans around away from the light, the intensity drops and eventually the light disappears entirely. Many of the default ToLiss billboard lights skip that second step, which is why they are often visible from all 360 degrees, and it is the primary reason for their low-quality appearance.

ToLiss Photon modifies every single light in the .OBJ and specifies a light direction for everything except the upper and lower beacons (as they are truly visible from 360 degrees). It also uses custom DataRefs to offer a different appearance for halogen/Xenon lights and LED lights.

The strobe and beacon flashing is controlled using a Lua script. The script has a function that executes every frame and overrides specific DataRefs set by ToLiss that control light intensity for both the beacon and the strobe. By default, ToLiss uses a sine wave (or similar equation) to control the brightness of the beacon, which means it smoothly fades in and out. While this kind of fading _can_ occur with halogen bulbs as they quickly warm and cool, the beacon is not halogen. It's either a Xenon flash-tube or LED. Neither option produces anything like the perceptible fade-in/fade-out you see.

### Components
* Aircraft .obj files: Modifies light colors, positioning, anda appearance.
* Lua (via FlyWithLua): Controls beacon and flash behavior.
* Python (via XPPython3): Used for the GUI, Plugins sub-menu, and profile saving/loading.
  * _I would have preferred to just use Lua, but FlyWithLua cannot create usable DataRefs for the .obj files nor can it create a sub-menu in Plugins._

## Other Recommended Mods
* [KOSP Project](https://store.x-plane.org/KOSP-PROJECT--A319-A320-A321-Full-Soundscape_p_1773.html) for audio
* [GusRodrigues'](https://forums.x-plane.org/profile/6767-gusrodrigues/) A320 Family Light Mod ([A319](https://forums.x-plane.org/files/file/93336-a319-light-mod/), [A320](https://forums.x-plane.org/files/file/93337-a320-light-mod/), [A321](https://forums.x-plane.org/files/file/93338-a321-light-mod/)) for additional lighting improvements
  * _These mods also optionally provide improved exterior lighting. They have a different artistic direction compared to ToLiss Photon and are a good alternative if you decide you don't like this mod._

## Credits
The original lighting file was authored by ToLiss Simulations. Scripting was done with the assistance of Claude Code (I don't know Python). 

## Disclaimer
This project is an independent third-party modification and is not affiliated with ToLiss Simulations Solutions Inc.

## Usage
ToLiss Photon is licensed under an MIT License. If you've discovered this addon and see that it's been years since I've updated it or fixed any bugs, feel free to fork it, fix it, and reupload it yourself. 