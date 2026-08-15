// Resource IDs shared by `photon.rc` and the C++ that loads them back.
//
// ⚠ BOTH SIDES MUST READ THIS. `gui.cpp` calls LoadImage() with this ID to pull
// the correctly-sized icon out of the group at window-creation time, so a number
// typed twice is a number that can disagree — and the symptom of disagreement is
// a window with no icon, which looks exactly like the icon never having been
// added. tests/test_icon.py pins that the .rc still spells it this way.
//
// ⚠ 1 IS NOT ARBITRARY, and it must stay the LOWEST icon ID in the binary:
// Explorer, the taskbar and Alt-Tab all take "the application icon" to mean the
// icon resource with the numerically lowest ID.
#pragma once

#define PHOTON_ICON_ID 1
