/* SPDX-License-Identifier: MIT */
// AdrenoBacklight.h - the A16 panel's brightness for macOS (-DADRENO_BACKLIGHT=1, boot-arg adreno_backlight=1).
//
// macOS finds a built-in panel's backlight by property, not by class (VERIFIED, host 26A428 disassembly):
// CoreBrightness's +[CBLEContainer copyBacklightMatchingDictionary] is {IOPropertyMatch: {backlight-control: true}},
// and BrightnessControl's BCAppleBacklightBrtControl reads IODisplayParameters ("brightness" min/max,
// "BrightnessMilliNits" min/max/value) from that service and applies a change with
// IORegistryEntrySetCFProperties(service, {"brightness-nits": (int)(nits * 65536)}) (-setNits:error:). On a Mac that
// service is AppleARMBacklight; here this IOService, a child of AdrenoDisplay, plays its part with the keys above.
// It also takes {"brightness": 0..65536} (the IODisplayParameters scale) for tools that write that key, and
// AdrenoDisplay::set_brightness_gated (IOMobileFramebufferSetParameter name 1) forwards its value here as 16.16 nits
// (INFERRED units).
//
// Nits become a panel level with compat/edp-backlight.h's luminance model (luminance proportional to level, the top
// level = boot-arg adreno_backlight_max_nits, default 500), never below the darkest offer (boot-arg
// adreno_backlight_min_nits, default 5, raised to what the monitor's floor shows). The panel is never touched from
// here: the level goes to the EL2 monitor's ADRENO_GUEST_BACKLIGHT, which clamps it to the panel's [floor, max] and
// writes DPCD 0x722/0x723. Nothing is
// written at start: the firmware's brightness stays until macOS asks. Requests are coalesced on the service's own
// work loop, at most one monitor call per kMinIntervalMs; the latest request wins. A monitor that refuses
// (an older one, a panel without AUX brightness, or one that stopped after failures) leaves the brightness as it was:
// before start no service is published, after a FAILED reply every later request is refused.
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
    // nits in 16.16 fixed point. kIOReturnSuccess when queued; kIOReturnNotReady once the monitor has stopped.
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
    uint32_t fPendingLevel;
    uint64_t fPendingNits;
    Source fPendingSource;
    uint64_t fLastApply;          // mach_absolute_time of the last monitor call
    uint64_t fRequests[kSources], fApplied, fClamped, fErrors, fCoalesced;
    int64_t fLastError;
};
