# SPDX-License-Identifier: MIT
# Sourced by build.sh (PROFILE=desktop-nocoherent). The A16 desktop's AdrenoX2 before IO-coherent leaves: the
# configuration of AdrenoX2 981e0b88 (AuxKC 43bed810, booted from b12v19c through b12v20). Built from the sources of
# the commit that added this profile it is section for section that kext (the umbrella's tools/macho-sections.py; the
# file bytes also carry object paths and a UUID). Since the re-pin log stopped reading the map's IOGPUTask from a
# private offset, AdrenoMemoryMap.o differs from it, and with it the linked kext's code and data addresses.
# AdrenoX2 and adreno-ib-command also carry ADRENO_IB_CS_SAMPLERS (CS sampler loads of more than one unit for
# CoreImage's multi-input kernels), inert unless the guest boots with adreno_cs_samplers=1: without that boot-arg the
# validator and the registry are those of 981e0b88, though AdrenoX2.o and adreno-ib-command.o now differ from it.
#
# Every define is named per unit, so build.sh's FIXES is cleared. The sets, unit by unit:
#   AdrenoX2           frame and GPU perf windows, the 512 MiB residency default, asynchronous IBs, the present latch,
#                      present sync and the CS sampler opt-in
#   AdrenoMemoryMap    the resource pool and its arena, the residency default, GPU perf, VA reclaim
#   AdrenoDisplay      cached reads, frame perf, the pool, 60 Hz vsync, GPU perf, the beam, the cursor stage, the
#                      present-maintenance skip, VA reclaim, the latch and present sync
#   adreno-resource    the pool, its arena and VA reclaim
#   adreno-ib-command  the CS sampler opt-in (its validator half)
# and every one of those eight units (with AdrenoSegmentList, adreno-resource-list, adreno-fill-command and
# adreno-ib-command) also gets PROFILE_ALL: sparse cache maintenance, GPU perf, the wide GPU VA window, the IB
# pipeline, GPU recovery, the present relatch, resident re-pin and aligned live-scanout copies.
# SnapintoshVirtualEthernet, AdrenoEarlyStop, adreno-submit and kmod_info take no feature define.
FIXES=
PROFILE_ALL="-DADRENO_CACHE_SPARSE=1 -DADRENO_PERF_GPU=1 -DADRENO_VA_WIDE=1 -DADRENO_PIPELINE=1 -DADRENO_GPU_RECOVER=1 -DADRENO_PRESENT_RELATCH=1 -DADRENO_RESIDENT_REPIN=1 -DADRENO_PRESENT_LIVE_ALIGNED=1"
U_AdrenoX2="-DADRENO_PERF_FRAMES=1 -DADRENO_RESIDENT_BUDGET_MIB=512 -DADRENO_PERF_GPU=1 -DADRENO_IB_ASYNC=1 -DADRENO_LATCH=1 -DADRENO_PRESENT_SYNC=1 -DADRENO_IB_CS_SAMPLERS=1 $PROFILE_ALL"
U_AdrenoMemoryMap="-DADRENO_RESOURCE_POOL=1 -DADRENO_POOL_ARENA=1 -DADRENO_RESIDENT_BUDGET_MIB=512 -DADRENO_PERF_GPU=1 -DADRENO_VA_RECLAIM=1 $PROFILE_ALL"
U_AdrenoDisplay="-DADRENO_PRESENT_CACHED_READ=1 -DADRENO_PERF_FRAMES=1 -DADRENO_RESOURCE_POOL=1 -DADRENO_VSYNC60=1 -DADRENO_PERF_GPU=1 -DADRENO_BEAM=1 -DADRENO_PRESENT_CURSOR_STAGE=1 -DADRENO_PRESENT_SKIP_MAINT=1 -DADRENO_VA_RECLAIM=1 -DADRENO_LATCH=1 -DADRENO_PRESENT_SYNC=1 $PROFILE_ALL"
U_AdrenoSegmentList="$PROFILE_ALL"
U_adreno_resource="-DADRENO_RESOURCE_POOL=1 -DADRENO_POOL_ARENA=1 -DADRENO_VA_RECLAIM=1 $PROFILE_ALL"
U_adreno_resource_list="$PROFILE_ALL"
U_adreno_fill_command="$PROFILE_ALL"
U_adreno_ib_command="-DADRENO_IB_CS_SAMPLERS=1 $PROFILE_ALL"
