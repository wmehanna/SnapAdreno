# SPDX-License-Identifier: MIT
# Sourced by build.sh (PROFILE=desktop). The A16 desktop's AdrenoX2: desktop-nocoherent plus IO-coherent maintenance
# skipping (-DADRENO_COHERENT_SKIP_MAINT=1) in the same eight units. The configuration of AdrenoX2 c66edb6b (AuxKC
# bd057773, booted in b12v23 with the monitor's --gpu-coherent). Section for section equal to that kext.
#
# The kext skips CPU cache maintenance only after the monitor's ADRENO_GUEST_COHERENT answers ON and the guest
# boot-arg adreno_coherent=1 is set; with an older monitor, or without the boot-arg, it maintains as desktop-nocoherent.
. "$D/profiles/desktop-nocoherent.sh"
PROFILE_COHERENT="-DADRENO_COHERENT_SKIP_MAINT=1"
U_AdrenoX2="$U_AdrenoX2 $PROFILE_COHERENT"
U_AdrenoMemoryMap="$U_AdrenoMemoryMap $PROFILE_COHERENT"
U_AdrenoDisplay="$U_AdrenoDisplay $PROFILE_COHERENT"
U_AdrenoSegmentList="$U_AdrenoSegmentList $PROFILE_COHERENT"
U_adreno_resource="$U_adreno_resource $PROFILE_COHERENT"
U_adreno_resource_list="$U_adreno_resource_list $PROFILE_COHERENT"
U_adreno_fill_command="$U_adreno_fill_command $PROFILE_COHERENT"
U_adreno_ib_command="$U_adreno_ib_command $PROFILE_COHERENT"
