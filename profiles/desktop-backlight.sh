# SPDX-License-Identifier: MIT
# Sourced by build.sh (PROFILE=desktop-backlight). The desktop profile plus the panel backlight service
# (-DADRENO_BACKLIGHT=1, src/AdrenoBacklight.h) in AdrenoDisplay.o only; every other unit is desktop's, so the other
# objects are section for section the desktop profile's. Not deployed yet: built for the umbrella's
# installed-fresh --edp-backlight (KEXT_PROFILE=desktop-backlight tools/build-auxkc-desktop.sh NAME, then pinned).
#
# At run time the service exists only with the boot-arg adreno_backlight=1 (the umbrella's --edp-backlight adds it)
# and a monitor built with SNAP_EDP_BACKLIGHT that answers ADRENO_GUEST_BACKLIGHT QUERY; otherwise AdrenoDisplay is
# the desktop profile's in behaviour and the panel keeps the firmware's brightness.
. "$D/profiles/desktop.sh"
U_AdrenoDisplay="$U_AdrenoDisplay -DADRENO_BACKLIGHT=1"
