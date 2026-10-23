/* SPDX-License-Identifier: MIT */
// AdrenoDisplay.cpp - see AdrenoDisplay.h. Each behaviour cites the AppleParavirtDisplay
// (KDK 26A428 AppleParavirtGPUIOGPUFamily, arm64e) routine it mirrors.
#include "AdrenoDisplay.h"
#include "AdrenoDisplayCopy.h"
#include "AdrenoEarlyStop.h"
#include <IOKit/IOLib.h>
#include <kern/clock.h>
#include <IOKit/IOWorkLoop.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSSet.h>
#include <pexpert/pexpert.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <uuid/uuid.h>

extern "C" task_t kernel_task;
#if ADRENO_PERF_FRAMES
extern "C" uint64_t gAdrenoIBCompleted, gAdrenoIBCompletedAt;   // AdrenoX2.cpp submitIB
#if ADRENO_RESOURCE_POOL
extern "C" void AdrenoGPUMapCounters(uint64_t *);              // AdrenoMemoryMap.cpp
#endif
#if ADRENO_VA_RECLAIM
extern "C" void AdrenoGPUVACounters(uint64_t *);               // AdrenoMemoryMap.cpp (agents/gpu-va)
#endif
#if ADRENO_LATCH
extern "C" uint64_t gAdrenoGPUBusy;                            // AdrenoX2.cpp: submissions in the kext (G1)
#endif
#if ADRENO_PERF_GPU
extern "C" void AdrenoGPUWorkCounters(uint64_t *);             // AdrenoMemoryMap.cpp
extern "C" uint64_t gAdrenoIBGpuTicks, gAdrenoIBGpuTicksMax, gAdrenoIBSubmitTicks, gAdrenoIBLockTicks;   // AdrenoX2.cpp
#endif
static uint64_t perfNs(uint64_t ticks)
{
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(ticks, &ns);
    return ns;
}
#endif
#if ADRENO_PRESENT_SYNC
extern "C" uint32_t AdrenoPresentSyncQuery(uint32_t surfaceID, uint64_t *oldest);   // AdrenoX2.cpp: writers, oldest mark
extern "C" void AdrenoPresentSyncCounters(uint64_t *marks, uint64_t *overflows, uint64_t *unmatched);
#endif
#if ADRENO_PIPELINE
extern "C" uint32_t AdrenoPipelineCounters(uint64_t *v);   // AdrenoX2.cpp: the engine's totals (AdrenoDisplayPipe.h)
#endif
#if ADRENO_PRESENT_RELATCH
extern "C" uint32_t AdrenoPipelineEarlyCounters(uint64_t *v);   // AdrenoX2.cpp: EP1-EP3 totals and switches (OP1)
#endif

// ---------------------------------------------------------------- IOAVFamily
// Only the exported factories AppleParavirtDisplay imports; the class layouts stay opaque.
// Manglings (checked by tools/check-iomfb-vtable.py against the kernelcache exports):
//   __ZN22IOAVVideoTimingElement8withDataEPK19IOAVVideoTimingData16IOAVStandardType23IOAVVideoTimingStandard
//   __ZN22IOAVVideoTimingElement16setColorElementsEP7OSArray
//   __ZN21IOAVVideoColorElement16withCapabilitiesE16IOAVStandardTypej22IOAVVideoPixelEncoding
//     26IOAVVideoColorDynamicRange18IOAVVideoColorEOTF20IOAVVideoColorimetrybPK12OSDictionaryS7_S7_PK5OSSet
enum IOAVStandardType : uint32_t {};
enum IOAVVideoTimingStandard : uint32_t {};
enum IOAVVideoPixelEncoding : uint32_t {};
enum IOAVVideoColorDynamicRange : uint32_t {};
enum IOAVVideoColorEOTF : uint32_t {};
enum IOAVVideoColorimetry : uint32_t {};
// createTimingElementWithDimensionsAndRate (PV 0xe49c) zeroes 0x50 bytes and fills four words.
struct IOAVVideoTimingData {
    uint32_t h0, h1;
    uint32_t hActive, h3;         // +0x08
    uint32_t h4, h5;
    uint32_t hSyncRate, h7;       // +0x18 = refresh * vActive (line rate, same fixed point)
    uint32_t v0, v1;
    uint32_t vActive, v3;         // +0x28
    uint32_t v4, v5;
    uint32_t vSyncRate, v7;       // +0x38 = refresh
    uint32_t tail[4];
};
static_assert(sizeof(IOAVVideoTimingData) == 0x50, "IOAVVideoTimingData size (PV zeroes 0x50)");
class IOAVVideoTimingElement : public OSObject {
public:
    static IOAVVideoTimingElement *withData(const IOAVVideoTimingData *, IOAVStandardType, IOAVVideoTimingStandard);
    void setColorElements(OSArray *);
};
class IOAVVideoColorElement : public OSObject {
public:
    static IOAVVideoColorElement *withCapabilities(IOAVStandardType, unsigned int, IOAVVideoPixelEncoding,
                                                   IOAVVideoColorDynamicRange, IOAVVideoColorEOTF,
                                                   IOAVVideoColorimetry, bool, const OSDictionary *,
                                                   const OSDictionary *, const OSDictionary *, const OSSet *);
};
// IOAVElement keeps an ID at +0x10 (u64) and a score at +0x18 (u32); PV writes both directly
// (createTimingElementWithDimensionsAndRate 0xe4f4..0xe504, createColorElementWithDepthAndEDR 0x9554).
static void setAVElementIdentity(OSObject *element, uint32_t id, uint32_t score)
{
    *(uint64_t *)((uint8_t *)element + 0x10) = id;
    *(uint32_t *)((uint8_t *)element + 0x18) = score;
}

// IOUserClient::sendAsyncResult64WithOptions is protected; PV reaches it through its user-client
// subclass (AppleParavirtDisplayUserClient::sendAsyncResult64WithOptions, 0x8000). Never instantiated.
struct AdrenoDisplayAsync : public IOUserClient {
    static IOReturn send(OSAsyncReference64 ref, io_user_reference_t *args, uint32_t n)
    {
        return sendAsyncResult64WithOptions(ref, kIOReturnSuccess, args, n, kIOUserNotifyOptionCanDrop);
    }
};

#define super IOMobileFramebufferLegacy
OSDefineMetaClassAndStructors(AdrenoDisplay, IOMobileFramebufferLegacy)

static constexpr IOMFBSwapCompletionState kSwapStateStarted = (IOMFBSwapCompletionState)2;
static bool putNum(OSDictionary *d, const char *key, uint64_t v, unsigned bits = 32);

// ---------------------------------------------------------------- accelerator hook

bool AdrenoDisplayAttach(IOService *accelerator)
{
    // AppleParavirtGPU::start 0x15a28..0x15ad4: OSTypeAlloc, init(nullptr), attach(gpu),
    // start(gpu), detach on failure. The registry's attach reference keeps the display
    // alive; terminating the accelerator terminates this only child with it.
    //
    // Opt-in by board: the display is created only on a device tree whose external
    // display graph is withdrawn (the -adreno-metal-display profile). On any board that
    // still has /arm-io/dispext0 (the deployed -adreno-metal profile) the framework
    // expects two IOMFB services, so this one would become the main display next to a
    // 30 s wait per client, and the GOP remap would run on a board never meant to
    // present. boot-arg adreno_display=0 forces it off, adreno_display=1 forces it on.
    uint32_t enabled = 0;
    if (!accelerator)
        return false;
    if (PE_parse_boot_argn("adreno_display", &enabled, sizeof(enabled))) {
        if (!enabled) {
            IOLog("AdrenoDisplay: disabled by boot-arg adreno_display=0\n");
            return false;
        }
    } else {
        IORegistryEntry *external = IORegistryEntry::fromPath("/arm-io/dispext0", gIODTPlane);
        if (external) {
            external->release();
            IOLog("AdrenoDisplay: not created: /arm-io/dispext0 present (not the display board; "
                  "boot-arg adreno_display=1 forces it)\n");
            return false;
        }
    }
    AdrenoDisplay *display = OSTypeAlloc(AdrenoDisplay);
    if (!display)
        return false;
    bool ok = display->init(nullptr);
    if (ok) {
        ok = display->attach(accelerator);
        if (ok && !display->start(accelerator)) {
            display->detach(accelerator);
            ok = false;
        }
    }
    display->release();
    IOLog("AdrenoDisplay: %s\n", ok ? "started under the accelerator" : "not started; accelerator unaffected");
    return ok;
}

// ---------------------------------------------------------------- IOService

bool AdrenoDisplay::start(IOService *provider)
{
    // PV start (0x9664): one property, then the family start. The family start creates
    // the gate, event sources and user-client plumbing and calls start_hardware()
    // (IOMobileFramebufferLegacy::start 0x1f6c4) and registerService (0x1f790).
    // PV sets "CursorPlane" = 1 (0x969c..0x96b0: setProperty("CursorPlane", 1, 32)) and
    // programs a hardware cursor from layer 1. QuartzCore 26A428 reads it in
    // CA::WindowServer::AppleDisplay::AppleDisplay (0x18b0a23d4, a CFNumber SInt8 into the
    // display's cursor plane, this+0x69b3) only when CADeviceIsVirtualized (MobileGestalt
    // IsVirtualDevice); otherwise the plane is IOMFBSupportsGPLite ? 3 : 1 (0x18b0a189c).
    // The hardware cursor is used unless the CAUseSWCursor default is set
    // (IOMFBDisplay::hw_supports_cursor 0x18b39fffc), so it never was a software cursor in
    // layer 0: the old 0 here named layer 0 itself on a virtualized device and was ignored
    // otherwise, and the cursor layer was dropped. Layer 1 is now composited
    // (updateCursorGated); 2 and 3 are accepted too.
    setProperty("CursorPlane", 1ULL, 32);
    fSwapInfoHead = fSwapInfoTail = 0;
    return super::start(provider);
}

void AdrenoDisplay::stop(IOService *provider)
{
    super::stop(provider);
}

void AdrenoDisplay::free()
{
    OSSafeReleaseNULL(fColorElements);
    OSSafeReleaseNULL(fTimingElements);
    releaseSurfaceMaps();
    releaseCursor();
    releasePresentBuffers();
    unmapScanout();
#if ADRENO_BEAM
    beamUnmap();
#endif
    super::free();
}

// ---------------------------------------------------------------- pure virtuals

IOReturn AdrenoDisplay::set_notify_tag_mask_gated(IOMFB_NotificationType type, IOMFBNotifyRequest *req,
                                                  unsigned int mask)
{
    // PV 0xefb8: only the swap-info notification (type 3) has a tag mask, kept at req+0x78.
    if ((uint32_t)type != 3)
        return kIOReturnBadArgument;
    *(uint32_t *)((uint8_t *)req + 0x78) = mask;
    return kIOReturnSuccess;
}

IOReturn AdrenoDisplay::supported_frame_info(unsigned int, unsigned int *, char (*)[64], unsigned int *)
{
    return kIOReturnUnsupported;                                // PV 0xefdc
}

IOReturn AdrenoDisplay::factory_portal(void *, task *)
{
    return kIOReturnUnsupported;                                // PV 0xefec
}

IOReturn AdrenoDisplay::executeFactoryConfigurations_gated()
{
    return kIOReturnUnsupported;                                // PV 0xeffc
}

void AdrenoDisplay::set_frame_info_notification_state_gated(IOMFBNotifyRequest *, bool)
{
    // PV 0xf00c: nothing.
}

void AdrenoDisplay::do_swap_info_notify_gated(IOMFBNotifyRequest *req,
                                              const IOMobileFramebufferLegacy::SwapInfoBlob *blob)
{
    // PV 0xf014: args[0] = tag mask (req+0x78), args[1] = swap id (blob+0); then for each
    // set mask bit i < 5 the blob quadword at +8+8i (bit 1: delta against req+0x80),
    // split in two 32-bit halves when mask bit 31 is set; at most 12 arguments.
    const uint8_t *b = blob->bytes;
    uint32_t mask = *(const uint32_t *)((const uint8_t *)req + 0x78);
    uint64_t *last = (uint64_t *)((uint8_t *)req + 0x80);
    io_user_reference_t args[12] = {};
    uint32_t n = 0;
    args[n++] = mask;
    args[n++] = *(const uint32_t *)b;
    const uint64_t *q = (const uint64_t *)(b + 8);
    for (uint32_t i = 0; i < 5; i++) {
        if (!((mask >> i) & 1))
            continue;
        uint64_t v = (i == 1) ? q[0] - *last : q[i];
        if (n == 12) {
            IOLog("AdrenoDisplay: swap-info notification argument overflow\n");
            continue;
        }
        if (mask & 0x80000000u) {
            args[n++] = (uint32_t)v;
            if (n < 12)
                args[n++] = v >> 32;
        } else {
            args[n++] = v;
        }
    }
    *last = q[0];
    AdrenoDisplayAsync::send(*(OSAsyncReference64 *)((uint8_t *)req + 0x18), args, n);
}

IOReturn AdrenoDisplay::notify_swap_done_gated(unsigned int swapID, ::SwapInfoBlob *, unsigned int *, unsigned int)
{
    // PV 0xf148: refuse when the current swap was aborted (state != 0); otherwise fill the
    // ring entry {id, this+0x108, 0, swap+0x390, swap+0x3d0, 0} and, unless dispatch is
    // held (u16 at this+0x1dd2), drain the ring through swap_info_notify_dispatch_gated.
    IOMFBSwapIORequest *swap = fCurrentSwap;
    if (!swap || IOMFBSwapFields::state(swap) != 0)
        return kIOReturnUnsupported;
    uint8_t *e = fSwapInfo[fSwapInfoTail].bytes;
    bzero(e, sizeof(fSwapInfo[0]));
    *(uint32_t *)e = swapID;
    *(uint64_t *)(e + 0x08) = *(const uint64_t *)((const uint8_t *)this + 0x108);
    *(uint64_t *)(e + 0x18) = *(const uint64_t *)((const uint8_t *)swap + 0x390);
    *(uint64_t *)(e + 0x20) = *(const uint32_t *)((const uint8_t *)swap + 0x3d0);
    uint16_t held = *(const uint16_t *)((const uint8_t *)this + 0x1dd2);
    uint32_t tail = fSwapInfoTail;
    fSwapInfoTail = (fSwapInfoTail + 1) % kSwapInfoRing;
    if (held)
        return kIOReturnSuccess;
    while (fSwapInfoHead != tail) {
        swap_info_notify_dispatch_gated(&fSwapInfo[fSwapInfoHead]);
        fSwapInfoHead = (fSwapInfoHead + 1) % kSwapInfoRing;
    }
    swap_info_notify_dispatch_gated(&fSwapInfo[tail]);
    fSwapInfoHead = fSwapInfoTail;
    return kIOReturnSuccess;
}

void AdrenoDisplay::notify_crc_ready_gated(unsigned int)
{
    // PV 0xf278: nothing.
}

bool AdrenoDisplay::are_timings_enabled() const
{
    return false;                                               // PV 0x126d8
}

// The rest of this translation unit, in its original order: the parts keep each file under 1,000 lines and
// are never compiled on their own.
#include "AdrenoDisplay/presentation.cpp"
#include "AdrenoDisplay/surfaces.cpp"
#include "AdrenoDisplay/stats.cpp"
#include "AdrenoDisplay/cursor.cpp"
#include "AdrenoDisplay/live_copy.cpp"
#include "AdrenoDisplay/modeset.cpp"
#include "AdrenoDisplay/frame_latch.cpp"
#include "AdrenoDisplay/attributes.cpp"
