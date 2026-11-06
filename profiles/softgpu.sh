# SPDX-License-Identifier: MIT
# Sourced by build.sh (PROFILE=softgpu). The desktop-next AdrenoX2 plus the SoftGPU transport (-DADRENO_SOFTGPU=1 in
# AdrenoX2.o, AdrenoMemoryMap.o and AdrenoSegmentList.o; compat/softgpu-guest.c linked) and the SoftMetal plug-in in the
# personality (MetalPluginName SoftMetal, MetalPluginClassName SoftMTLDevice). plans/p6-softgpu-integration.md W2, W3.
#
# At run time the transport exists only when the ADT names the device (snapintosh,softgpu-reg, written by the monitor under
# SNAP_SOFTGPU=1); without it this kext is desktop-next's, apart from the plug-in name. Boot-args: softgpu_selftest=1 fills
# the panel's first 64 rows with softgpu_selftest_byte (default 0xc0) through the device at start. Rollback is the old KC.
. "$D/profiles/desktop-next.sh"
PROFILE_SOFTGPU="-DADRENO_SOFTGPU=1"
U_AdrenoX2="$U_AdrenoX2 $PROFILE_SOFTGPU"
U_AdrenoMemoryMap="$U_AdrenoMemoryMap $PROFILE_SOFTGPU"
U_AdrenoSegmentList="$U_AdrenoSegmentList $PROFILE_SOFTGPU"
SOFTGPU=1
PLUGIN=SoftMetal
PLUGIN_CLASS=SoftMTLDevice
