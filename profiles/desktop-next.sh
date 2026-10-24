# SPDX-License-Identifier: MIT
# Sourced by build.sh (PROFILE=desktop-next). The desktop-nocoherent profile plus the panel backlight service
# (-DADRENO_BACKLIGHT=1, src/AdrenoBacklight.h) in AdrenoDisplay.o only: the kext for a monitor without --gpu-coherent
# that serves the umbrella's installed-fresh --edp-backlight. desktop-backlight is the same addition on desktop, which
# needs a --gpu-coherent monitor to skip anything; this one keeps desktop-nocoherent's maintenance.
#
# From desktop-nocoherent it carries the head rule (ADRENO_RESIDENT_HEAD_OFFSET, boot-arg adreno_head_offset=0 turns it
# off), the IOMFB call trace (ADRENO_DISPLAY_TRACE_MODES, logging only) and the CS sampler opt-in (ADRENO_IB_CS_SAMPLERS,
# inert without the boot-arg adreno_cs_samplers=1). Every unit other than AdrenoDisplay is desktop-nocoherent's, so
# those objects are section for section that profile's.
#
# At run time the backlight service exists only with the boot-arg adreno_backlight=1 and a monitor built with
# SNAP_EDP_BACKLIGHT that answers ADRENO_GUEST_BACKLIGHT QUERY; otherwise AdrenoDisplay behaves as desktop-nocoherent's
# and the panel keeps the firmware's brightness.
. "$D/profiles/desktop-nocoherent.sh"
U_AdrenoDisplay="$U_AdrenoDisplay -DADRENO_BACKLIGHT=1"
