# SPDX-License-Identifier: MIT
# Sourced by build.sh (PROFILE=desktop-early-done). The desktop-backlight profile (desktop's IO-coherent maintenance
# skipping plus the panel backlight service) plus early completion (-DADRENO_PRESENT_EARLY_DONE=1,
# src/AdrenoDisplayEarlyDone.h) in AdrenoDisplay.o only: the kext for the umbrella's installed-fresh --edp-backlight with
# --gpu-coherent or --present-early-done. Every unit other than AdrenoDisplay is desktop's.
#
# Each addition is inert without its run-time switch:
#   - coherent skipping only after the monitor's ADRENO_GUEST_COHERENT answers ON (a --gpu-coherent monitor whose boot
#     self-test passed) and with the boot-arg adreno_coherent=1; otherwise every map keeps its maintenance, as
#     desktop-nocoherent;
#   - the backlight service only with adreno_backlight=1 and a SNAP_EDP_BACKLIGHT monitor;
#   - early completion only with adreno_present_early_done=1 or 2. Without it the swap completes at the next tick as
#     before; the one difference left is the perf done line (arrival measurements) in each perf window.
. "$D/profiles/desktop-backlight.sh"
U_AdrenoDisplay="$U_AdrenoDisplay -DADRENO_PRESENT_EARLY_DONE=1"
