# SPDX-License-Identifier: MIT
# Sourced by build.sh (PROFILE=desktop-nocoherent). The A16 desktop's AdrenoX2 before IO-coherent leaves: the
# configuration of AdrenoX2 981e0b88 (AuxKC 43bed810, booted from b12v19c through b12v20). Section for section equal to
# that kext (compare with tools/macho-sections.py in the umbrella; the file bytes also carry object paths and a UUID).
#
# Every define is named per unit, so build.sh's FIXES is cleared. The sets, unit by unit:
#   AdrenoX2           frame and GPU perf windows, the 512 MiB residency default, asynchronous IBs, the present latch
#                      and present sync
#   AdrenoMemoryMap    the resource pool and its arena, the residency default, GPU perf, VA reclaim
#   AdrenoDisplay      cached reads, frame perf, the pool, 60 Hz vsync, GPU perf, the beam, the cursor stage, the
#                      present-maintenance skip, VA reclaim, the latch and present sync
#   adreno-resource    the pool, its arena and VA reclaim
# and every one of those eight units (with AdrenoSegmentList, adreno-resource-list, adreno-fill-command and
# adreno-ib-command) also gets PROFILE_ALL: sparse cache maintenance, GPU perf, the wide GPU VA window, the IB
# pipeline, GPU recovery, the present relatch, resident re-pin and aligned live-scanout copies.
# SnapintoshVirtualEthernet, AdrenoEarlyStop, adreno-submit and kmod_info take no feature define.
FIXES=
PROFILE_ALL="-DADRENO_CACHE_SPARSE=1 -DADRENO_PERF_GPU=1 -DADRENO_VA_WIDE=1 -DADRENO_PIPELINE=1 -DADRENO_GPU_RECOVER=1 -DADRENO_PRESENT_RELATCH=1 -DADRENO_RESIDENT_REPIN=1 -DADRENO_PRESENT_LIVE_ALIGNED=1"
U_AdrenoX2="-DADRENO_PERF_FRAMES=1 -DADRENO_RESIDENT_BUDGET_MIB=512 -DADRENO_PERF_GPU=1 -DADRENO_IB_ASYNC=1 -DADRENO_LATCH=1 -DADRENO_PRESENT_SYNC=1 $PROFILE_ALL"
U_AdrenoMemoryMap="-DADRENO_RESOURCE_POOL=1 -DADRENO_POOL_ARENA=1 -DADRENO_RESIDENT_BUDGET_MIB=512 -DADRENO_PERF_GPU=1 -DADRENO_VA_RECLAIM=1 $PROFILE_ALL"
U_AdrenoDisplay="-DADRENO_PRESENT_CACHED_READ=1 -DADRENO_PERF_FRAMES=1 -DADRENO_RESOURCE_POOL=1 -DADRENO_VSYNC60=1 -DADRENO_PERF_GPU=1 -DADRENO_BEAM=1 -DADRENO_PRESENT_CURSOR_STAGE=1 -DADRENO_PRESENT_SKIP_MAINT=1 -DADRENO_VA_RECLAIM=1 -DADRENO_LATCH=1 -DADRENO_PRESENT_SYNC=1 $PROFILE_ALL"
U_AdrenoSegmentList="$PROFILE_ALL"
U_adreno_resource="-DADRENO_RESOURCE_POOL=1 -DADRENO_POOL_ARENA=1 -DADRENO_VA_RECLAIM=1 $PROFILE_ALL"
U_adreno_resource_list="$PROFILE_ALL"
U_adreno_fill_command="$PROFILE_ALL"
U_adreno_ib_command="$PROFILE_ALL"
