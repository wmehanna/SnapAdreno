/* SPDX-License-Identifier: MIT */
// AdrenoBacklight.h - the A16 panel's brightness for macOS (-DADRENO_BACKLIGHT=1, boot-arg adreno_backlight=1).
//
// How a brightness change reaches a backlight service on 26A428 (host analysis of the dyld cache and the KDK; the
// labels are AGENTS.md's):
//  1. corebrightnessd's BrightnessSystem creates BLControl, which gives each display brightness control SkyLight
//     reports a CBDisplayContainerSKL; its CBDisplayModuleSKL turns the slider, the keys and auto-brightness into
//     nits and hands them to WindowServer (VERIFIED). Nothing in CoreBrightness or corebrightnessd instantiates
//     CBLEManager, so +[CBLEContainer isValidBacklightService:]'s AppleARMBacklight class check is not on this path
//     (VERIFIED: CBLEManager's only class reference is its own).
//  2. WindowServer's IOMobileFramebuffer.framework asks BrightnessControl for every control and binds an embedded IOMFB
//     to a valid one whose capabilities say BuiltIn and name no other display (VERIFIED, iomfb_bc_up; it syslogs
//     "matched IOMFB (%p) to BC (%p) for embedded panel with display ID (%d)"). BrightnessControl finds backlight
//     services by {IOPropertyMatch: {backlight-control: true}}, and BCAppleBacklightBrtControl builds a control's
//     capabilities from IODisplayParameters ("brightness" and "BrightnessMilliNits" min/max) and the L*Product keys
//     (VERIFIED).
//  3. IOMobileFramebufferSwapSetBrightness puts the nits in the next swap and calls BCBrtControlSetNits (VERIFIED).
//     -[BCAppleBacklightBrtControl setNits:error:] writes IORegistryEntrySetCFProperties(service, {"brightness-nits":
//     (int)(nits * 65536)}) only for a Touch Bar control or for a service that carries, itself or in a parent,
//     "force-brightness-control" (OSData, first byte nonzero); otherwise it returns an error and writes nothing
//     (VERIFIED). On a DCP Mac the swap's value drives the panel instead (OBSERVED on the 26A428 host: the internal
//     IOMFB's IOMFBBrightnessLevel follows the slider while AppleARMBacklight's IODisplayParameters do not move).
// Run next1b fits step 3's refusal: CoreBrightness's CBDisplayModuleSKL ramped to 140 nits while this service saw no
// request at all (OBSERVED; INFERRED cause, since that run kept no WindowServer log of the match). So this service
// publishes force-brightness-control and is otherwise what step 2 reads; Apple's code stays on its own path, with no
// AppleARMBacklight, no device-tree backlight node and no Apple class layout involved.
//
// Also accepted: {"brightness": 0..65536} (the IODisplayParameters scale) for tools that write that key, and
// AdrenoDisplay::set_brightness_gated (IOMobileFramebufferSetParameter name 1), forwarded as 16.16 nits (INFERRED unit).
//
// Nits become a panel level with compat/edp-backlight.h's luminance model (luminance proportional to level, the top
// level = boot-arg adreno_backlight_max_nits, default 500), never below the darkest offer (boot-arg
// adreno_backlight_min_nits, default 5, raised to what the monitor's floor shows). The panel is never touched from
// here: the level goes to the EL2 monitor's ADRENO_GUEST_BACKLIGHT, which clamps it to the panel's [floor, max] and
// writes DPCD 0x722/0x723. Nothing is written at start: the firmware's brightness stays until macOS asks. Requests are
// coalesced on the service's own work loop, at most one monitor call per kMinIntervalMs; the latest request wins, and
// one whose level the panel already has makes no call. A monitor that refuses (an older one, a panel without AUX
// brightness, or one that stopped after failures) leaves the brightness as it was: before start no service is
// published, after a FAILED reply every later request is refused.
#pragma once
#include <IOKit/IOService.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>

class AdrenoBacklight : public IOService
{
    OSDeclareDefaultStructors(AdrenoBacklight)
public:
    enum Source : uint32_t { kSourceNits, kSourceSlider, kSourceIOMFB, kSources };
    static constexpr uint32_t kMinIntervalMs = 16;
    static constexpr uint32_t kLoggedApplies = 16;   // every apply up to here, then every 64th and every failure

    // Queries the monitor; returns a started, registered child of display, or nullptr with the reason logged.
    static AdrenoBacklight *create(IOService *display);
    virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
    virtual void stop(IOService *provider) APPLE_KEXT_OVERRIDE;
    virtual void free() APPLE_KEXT_OVERRIDE;
    virtual IOReturn setProperties(OSObject *properties) APPLE_KEXT_OVERRIDE;
    // nits in 16.16 fixed point. kIOReturnSuccess when queued (or already shown); kIOReturnNotReady once the monitor
    // has stopped.
    IOReturn requestNits(uint64_t nits_q16, Source source);

private:
    bool configure(uint64_t x1, uint64_t x2, uint64_t x3);
    void apply(IOTimerEventSource *sender);
    void publish();

    IOLock *fLock;
    IOWorkLoop *fWorkLoop;
    IOTimerEventSource *fTimer;
    // The monitor's last reply (compat/edp-backlight.h).
    uint32_t fState, fDetail, fFloor, fMax, fLevel, fBits;
    uint8_t fCaps[4], fModeReg;
    bool fLsb;
    bool fAllowMode;              // boot-arg adreno_backlight_mode=1: allow the DPCD mode switch
    uint32_t fMinNits, fMaxNits;  // boot-args (or defaults): the darkest request and the top level's luminance
    uint64_t fMinQ16;             // BrightnessMilliNits "min" in 16.16: fMinNits, raised to the floor's luminance
    // Under fLock.
    bool fPending, fArmed, fStopped;
    bool fShownReady;             // the monitor is in DPCD mode and fShownLevel is on the panel
    uint32_t fPendingLevel, fShownLevel;
    uint64_t fPendingNits;
    Source fPendingSource;
    uint64_t fLastApply;          // mach_absolute_time of the last monitor call
    uint64_t fRequests[kSources], fApplied, fClamped, fErrors, fCoalesced, fUnchanged;
    int64_t fLastError;
};
