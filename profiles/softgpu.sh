# SPDX-License-Identifier: MIT
# Sourced by build.sh (PROFILE=softgpu). The desktop-next AdrenoX2 plus the SoftGPU transport (-DADRENO_SOFTGPU=1 in
# AdrenoX2.o, AdrenoMemoryMap.o and AdrenoSegmentList.o; compat/softgpu-guest.c linked) and the SoftMetal plug-in in the
# personality (MetalPluginName SoftMetal, MetalPluginClassName SoftMTLDevice). plans/p6-softgpu-integration.md W2, W3.
#
# At run time the transport exists only when the ADT names the device (snapintosh,softgpu-reg, written by the monitor under
# SNAP_SOFTGPU=1); without it this kext is desktop-next's, apart from the plug-in name. The F1 self-test is compiled in only
# with SOFTGPU_SELFTEST=1 in the build environment (-DADRENO_SOFTGPU_SELFTEST=1); then the boot-arg softgpu_selftest=1 fills
# the 64 rows at the start of the boot console's framebuffer (softgpu_selftest_pa=<hex> moves the start inside it,
# softgpu_selftest_row sets the row bytes, default the console's) with softgpu_selftest_byte (default 0xc0) through the
# device at start, then destroys its object. Rollback is the old KC.
. "$D/profiles/desktop-next.sh"
PROFILE_SOFTGPU="-DADRENO_SOFTGPU=1"
[ -z "${SOFTGPU_SELFTEST:-}" ] || PROFILE_SOFTGPU="$PROFILE_SOFTGPU -DADRENO_SOFTGPU_SELFTEST=1"
U_AdrenoX2="$U_AdrenoX2 $PROFILE_SOFTGPU"
U_AdrenoMemoryMap="$U_AdrenoMemoryMap $PROFILE_SOFTGPU"
U_AdrenoSegmentList="$U_AdrenoSegmentList $PROFILE_SOFTGPU"
SOFTGPU=1
PLUGIN=SoftMetal
PLUGIN_CLASS=SoftMTLDevice
