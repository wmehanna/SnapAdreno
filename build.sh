#!/bin/sh
# SPDX-License-Identifier: MIT
# Build AdrenoX2.kext (arm64e, MH_KEXT_BUNDLE) against the SDK Kernel.framework. No KDK needed for this
# step; kmutil (tools/mkauxkc.sh) resolves the undefined IOGPUFamily/kernel symbols against the J700 KC.
#
# PROFILE=<name> sh build.sh builds a named configuration from profiles/<name>.sh instead: each translation unit gets
# the profile's own define set (the per-unit sets the deployed kexts were built with) and the link is unchanged.
# Without PROFILE the build is exactly the default one. Output: OUT, else ../out (default) or ../out/<name>.
set -eu
D=$(cd "$(dirname "$0")" && pwd)
PROFILE=${PROFILE:-}
O=${OUT:-$D/../out${PROFILE:+/$PROFILE}}; OBJ=$O/obj
SDK=$(xcrun --show-sdk-path)
KH=$SDK/System/Library/Frameworks/Kernel.framework/Headers
# The *26A428.h headers reconstruct private Apple class layouts from the KDK and
# are not distributed. Generate them from your own KDK_27.0_26A428 into include/
# (tools/gen-iomfb-header.py writes IOMobileGraphicsFamily26A428.h) before building.
# The scan reads every source the compiler sees, the ordered parts in src/<unit>/ included.
for h in $(cat "$D"/src/*.* "$D"/src/*/*.cpp "$D"/include/*.h "$D"/../include/*.h 2>/dev/null | sed -n 's/^#include "\([A-Za-z0-9_]*26A428\.h\)".*/\1/p' | sort -u); do
    [ -f "$D/include/$h" ] || { echo "error: missing $D/include/$h (reconstructed from Apple's KDK 26A428, not distributed; generate it from your KDK)" >&2; exit 2; }
done
mkdir -p "$OBJ"
# Copies into the live scanout (Device memory) must stay naturally aligned: an unaligned store there is an
# alignment fault, i.e. a kernel panic (see AdrenoDisplayPresent.h).
FIXES="-DADRENO_PRESENT_LIVE_ALIGNED=1"
# Per-unit defines, empty in the default build. A profile sets FIXES (usually empty: it names every define per unit)
# and the U_* sets.
U_AdrenoX2= U_AdrenoMemoryMap= U_AdrenoSegmentList= U_AdrenoDisplay= U_SnapintoshVirtualEthernet= U_AdrenoEarlyStop=
U_adreno_resource= U_adreno_submit= U_adreno_resource_list= U_adreno_fill_command= U_adreno_ib_command= U_kmod_info=
if [ -n "$PROFILE" ]; then
    case "$PROFILE" in */*|.*) echo "error: PROFILE is a name in $D/profiles" >&2; exit 2;; esac
    [ -f "$D/profiles/$PROFILE.sh" ] || { echo "error: no profile $D/profiles/$PROFILE.sh" >&2; exit 2; }
    . "$D/profiles/$PROFILE.sh"
fi
COMMON="$FIXES -target arm64e-apple-macos27.0 -isysroot $SDK -mkernel -nostdinc -I$KH -I$D/include -I$D/../include -DKERNEL -DAPPLE -DNeXT -O2 -g -Wall -Werror -Wno-unused-private-field -fno-common"
CXXF="$COMMON -std=gnu++17 -fapple-kext -fno-exceptions -fno-rtti -Wno-inconsistent-missing-override"
xcrun clang++ $CXXF $U_AdrenoX2 -c "$D/src/AdrenoX2.cpp" -o "$OBJ/AdrenoX2.o"
xcrun clang++ $CXXF $U_AdrenoMemoryMap -c "$D/src/AdrenoMemoryMap.cpp" -o "$OBJ/AdrenoMemoryMap.o"
xcrun clang++ $CXXF $U_AdrenoSegmentList -c "$D/src/AdrenoSegmentList.cpp" -o "$OBJ/AdrenoSegmentList.o"
xcrun clang++ $CXXF $U_AdrenoDisplay -c "$D/src/AdrenoDisplay.cpp" -o "$OBJ/AdrenoDisplay.o"
xcrun clang++ $CXXF $U_SnapintoshVirtualEthernet -c "$D/src/SnapintoshVirtualEthernet.cpp" -o "$OBJ/SnapintoshVirtualEthernet.o"
xcrun clang++ $CXXF $U_AdrenoEarlyStop -c "$D/src/AdrenoEarlyStop.cpp" -o "$OBJ/AdrenoEarlyStop.o"
xcrun clang $COMMON $U_adreno_resource -c "$D/../../../compat/adreno-resource.c" -o "$OBJ/adreno-resource.o"
xcrun clang $COMMON $U_adreno_submit -c "$D/../../../compat/adreno-submit.c" -o "$OBJ/adreno-submit.o"
xcrun clang $COMMON $U_adreno_resource_list -c "$D/../../../compat/adreno-resource-list.c" -o "$OBJ/adreno-resource-list.o"
xcrun clang $COMMON $U_adreno_fill_command -c "$D/../../../compat/adreno-fill-command.c" -o "$OBJ/adreno-fill-command.o"
xcrun clang $COMMON $U_adreno_ib_command -c "$D/../../../compat/adreno-ib-command.c" -o "$OBJ/adreno-ib-command.o"
xcrun clang $COMMON $U_kmod_info -c "$D/src/kmod_info.c" -o "$OBJ/kmod_info.o"
K=$O/AdrenoX2.kext
rm -rf "$K"; mkdir -p "$K/Contents/MacOS"
cp "$D/Info.plist" "$K/Contents/Info.plist"
xcrun clang++ -target arm64e-apple-macos27.0 -isysroot "$SDK" -mkernel -nostdlib -Xlinker -kext \
  -Xlinker -export_dynamic -o "$K/Contents/MacOS/AdrenoX2" "$OBJ/AdrenoX2.o" "$OBJ/AdrenoMemoryMap.o" \
  "$OBJ/AdrenoSegmentList.o" "$OBJ/AdrenoDisplay.o" "$OBJ/SnapintoshVirtualEthernet.o" "$OBJ/AdrenoEarlyStop.o" "$OBJ/adreno-resource.o" "$OBJ/adreno-submit.o" "$OBJ/adreno-resource-list.o" "$OBJ/adreno-fill-command.o" "$OBJ/adreno-ib-command.o" "$OBJ/kmod_info.o" \
  -L"$SDK/usr/lib" -lkmodc++ -lkmod -lcc_kext
codesign -f -s - "$K"
if [ -n "$PROFILE" ]; then
    # The define sets this build used, unit by unit (the record a section comparison is read against).
    {
        echo "profile $PROFILE"
        for u in AdrenoX2 AdrenoMemoryMap AdrenoSegmentList AdrenoDisplay SnapintoshVirtualEthernet AdrenoEarlyStop \
                 adreno_resource adreno_submit adreno_resource_list adreno_fill_command adreno_ib_command kmod_info; do
            eval "set -- $FIXES \$U_$u"; echo "$u: $*"
        done
    } > "$O/DEFINES.txt"
fi
echo "kext: $K"
