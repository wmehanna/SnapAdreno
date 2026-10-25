/* SPDX-License-Identifier: MIT */
// AdrenoBacklight.h - the A16 panel's brightness for macOS (-DADRENO_BACKLIGHT=1, boot-arg adreno_backlight=1).
//
// How a brightness change reaches the panel on 26A428 (host analysis of the dyld cache and the KDK, and runs next1b and
// next2c; the labels are AGENTS.md's):
//  1. corebrightnessd's BrightnessSystem creates BLControl, which gives each display brightness control SkyLight
//     reports a CBDisplayContainerSKL; its CBDisplayModuleSKL turns the slider, the keys and auto-brightness into
//     nits and hands them to WindowServer (VERIFIED). Nothing in CoreBrightness or corebrightnessd instantiates
//     CBLEManager, so +[CBLEContainer isValidBacklightService:]'s AppleARMBacklight class check is not on this path
//     (VERIFIED: CBLEManager's only class reference is its own).
//  2. WindowServer's IOMobileFramebuffer.framework asks BrightnessControl for every control and binds an embedded IOMFB
//     to a valid one whose capabilities say BuiltIn and name no other display (VERIFIED, iomfb_bc_up). BrightnessControl
//     finds backlight services by {IOPropertyMatch: {backlight-control: true}}, and BCAppleBacklightBrtControl builds
//     a control's capabilities from IODisplayParameters ("brightness" and "BrightnessMilliNits" min/max) and the
//     L*Product keys (VERIFIED). With this service published that bind happens (OBSERVED, next2c: "matched IOMFB ...
//     to BC ... for embedded panel with display ID (0)"), and with it CoreAnimation has a brightness control for the
//     display.
//  3. CoreAnimation's IOMFBDisplay::swap_brightness sends sdr_nits() (times the EDR headroom in most modes) with
//     IOMobileFramebufferSwapSetBrightness in a swap whenever the value changed (VERIFIED). That call writes a
//     "present" byte and the nits as a double into the swap record handed to swap_submit (offsets in backlight.cpp)
//     and also calls BCBrtControlSetNits (VERIFIED). IOMobileFramebufferLegacy::swap_submit never reads those fields;
//     a DCP framebuffer passes the whole record to its firmware (VERIFIED), which is how a Mac's panel follows the
//     slider (OBSERVED on the 26A428 host: IOMFBBrightnessLevel follows it, AppleARMBacklight's IODisplayParameters
//     do not). CoreAnimation's "Display N swap brightness: %g" log line prints nan for each value a swap did not
//     change (VERIFIED), so a nan there means "unchanged", not a broken value.
//  4. BCBrtControlSetNits reaches this service only for a Touch Bar control or one carrying force-brightness-control
//     (VERIFIED), and even then WindowServer's sandbox lets it set properties only on AppleARMBacklight and
//     AppleBacklightDisplay (OBSERVED, next2c: "deny(1) iokit-set-properties iokit-class:AdrenoBacklight
//     property:brightness-nits", and the host's com.apple.WindowServer.sb).
// So AdrenoDisplay::swap_submit reads the brightness from every swap record and hands it here as source "swap"
// (backlight.cpp): Apple's DCP-style path, no sandbox rule involved. This service publishes what step 2 reads and not
// force-brightness-control, which would only add one refused registry write per change on WindowServer's swap path.
//
// Also accepted: {"brightness-nits": 16.16} and {"brightness": 0..65536} (the IODisplayParameters scale) from clients
// the sandbox lets through (tools run as root), and AdrenoDisplay::set_brightness_gated (IOMobileFramebufferSetParameter
// name 1), forwarded as 16.16 nits (INFERRED unit).
//
// Nits become a panel level with compat/edp-backlight.h's luminance model (luminance proportional to level, the top
// level = boot-arg adreno_backlight_max_nits, default 500), never below the darkest offer (boot-arg
// adreno_backlight_min_nits, default 5, raised to what the monitor's floor shows). The panel is never touched from
// here: the level goes to the EL2 monitor's ADRENO_GUEST_BACKLIGHT, which clamps it to the panel's [floor, max] and
// writes DPCD 0x722/0x723. Nothing is written at start: the firmware's brightness stays until macOS asks. Requests are
// coalesced on the service's own work loop, at most one monitor call per kMinIntervalMs (a ramp sends one value per
// swap; each call is two AUX transactions under the monitor's service lock); the latest request wins, and one whose
// level the panel already has makes no call. A monitor that refuses (an older one, a panel without AUX
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
    enum Source : uint32_t { kSourceNits, kSourceSlider, kSourceIOMFB, kSourceSwap, kSources };
    static constexpr uint32_t kMinIntervalMs = 33;   // at most ~30 monitor calls per second
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
    // Swap records whose brightness named no value (NaN, an infinity, negative): counted for the diagnostics only.
    void noteSwapRejected(uint32_t count);

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
    uint64_t fRequests[kSources], fApplied, fClamped, fErrors, fCoalesced, fUnchanged, fRejectedSwap;
    int64_t fLastError;
};
