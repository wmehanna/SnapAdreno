// SPDX-License-Identifier: MIT
// AdrenoBacklight (AdrenoBacklight.h) and AdrenoDisplay's brightness hooks; -DADRENO_BACKLIGHT=1 only.
// Part of AdrenoDisplay.cpp's translation unit: included from it in order, never compiled on its own.

// ---------------------------------------------------------------- AdrenoDisplay hooks

void AdrenoDisplay::backlightAttach()
{
    uint32_t on = 0;
    if (!PE_parse_boot_argn("adreno_backlight", &on, sizeof(on)) || !on) {
        IOLog("AdrenoBacklight: off (boot-arg adreno_backlight=1 not set): no backlight service\n");
        return;
    }
    fBacklight = AdrenoBacklight::create(this);
}

void AdrenoDisplay::backlightDetach()
{
    if (!fBacklight)
        return;
    fBacklight->terminate();
    OSSafeReleaseNULL(fBacklight);
}

// IOMobileFramebufferLegacy::set_parameter (KDK 26A428 0x2cd70): parameter name 1 with one value runs this, through
// the work-loop gate, from IOMobileFramebufferSetParameter (VERIFIED). The value's unit is INFERRED to be 16.16 nits,
// the unit the family's IOMFBBrightnessLevel property carries on a DCP Mac (OBSERVED on the 26A428 host); the first
// calls are logged raw so a run can tell.
IOReturn AdrenoDisplay::set_brightness_gated(int value)
{
    static uint32_t logged;
    if (logged < 8) {
        logged++;
        IOLog("AdrenoBacklight: IOMFB parameter 1 (brightness) = %d (%#x)%s\n", value, (uint32_t)value,
              fBacklight ? "" : ": no backlight service, unsupported");
    }
    if (!fBacklight)
        return kIOReturnUnsupported;   // the family's own answer
    return fBacklight->requestNits(value > 0 ? (uint64_t)value : 0, AdrenoBacklight::kSourceIOMFB);
}

// ---------------------------------------------------------------- AdrenoBacklight

#undef super
#define super IOService
OSDefineMetaClassAndStructors(AdrenoBacklight, IOService)

static const char *const kBacklightSources[AdrenoBacklight::kSources] = {"brightness-nits", "brightness",
                                                                        "IOMFB parameter 1"};

static const char *backlightState(uint32_t state)
{
    static const char *const names[] = {"UNPROBED", "READY", "MODE", "FAILED", "UNSUPPORTED"};
    return state < sizeof(names) / sizeof(names[0]) ? names[state] : "?";
}

// ADRENO_GUEST_BACKLIGHT (compat/edp-backlight.h). The monitor admits it only from this kext's AuxKC bytes at EL1.
static int64_t backlightCall(uint64_t op, uint64_t arg, uint64_t flags, uint64_t out[4])
{
    register uint64_t x0 __asm__("x0") = ADRENO_GUEST_BACKLIGHT;
    register uint64_t x1 __asm__("x1") = op;
    register uint64_t x2 __asm__("x2") = arg;
    register uint64_t x3 __asm__("x3") = flags;
    __asm__ volatile("hvc %4" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) : "i"(ADRENO_GUEST_HVC) : "memory", "cc");
    out[0] = x0;
    out[1] = x1;
    out[2] = x2;
    out[3] = x3;
    return (int64_t)x0;
}

static uint32_t backlightBootArg(const char *name, uint32_t fallback, uint32_t lo, uint32_t hi)
{
    uint32_t v = 0;
    if (!PE_parse_boot_argn(name, &v, sizeof(v)))
        return fallback;
    return v < lo ? lo : v > hi ? hi : v;
}

static uint64_t milli(uint64_t q16) { return (q16 * 1000 + 0x8000) >> 16; }

AdrenoBacklight *AdrenoBacklight::create(IOService *display)
{
    uint64_t r[4];
    int64_t e = backlightCall(EDP_BL_QUERY, 0, 0, r);
    if (e) {
        IOLog("AdrenoBacklight: QUERY refused (%lld): no backlight service (a monitor without SNAP_EDP_BACKLIGHT "
              "answers ADRENO_E_PARAMS, -60)\n", e);
        return nullptr;
    }
    uint32_t state = edp_bl_reply_state(r[1]);
    IOLog("AdrenoBacklight: QUERY abi %u state %s detail %u, DPCD 0x700-0x703 %02llx %02llx %02llx %02llx, mode %#llx, "
          "levels %u..%u, panel level %u, %llu bits%s\n", edp_bl_reply_abi(r[1]), backlightState(state),
          edp_bl_reply_detail(r[1]), (r[1] >> 24) & 0xff, (r[1] >> 32) & 0xff, (r[1] >> 40) & 0xff,
          (r[1] >> 48) & 0xff, r[1] >> 56, (uint32_t)r[2], (uint32_t)(r[2] >> 32), (uint32_t)r[3], (r[3] >> 32) & 0xff,
          (r[3] >> 40) & 1 ? ", MSB+LSB" : ", MSB only");
    if (edp_bl_reply_abi(r[1]) != EDP_BL_ABI) {
        IOLog("AdrenoBacklight: not published: monitor ABI %u, this kext speaks %u\n", edp_bl_reply_abi(r[1]),
              EDP_BL_ABI);
        return nullptr;
    }
    bool allowMode = backlightBootArg("adreno_backlight_mode", 0, 0, 1) != 0;
    if (state != EDP_BL_STATE_READY && !(state == EDP_BL_STATE_MODE && allowMode)) {
        IOLog("AdrenoBacklight: not published: state %s detail %u%s; the panel keeps its brightness\n",
              backlightState(state), edp_bl_reply_detail(r[1]),
              state == EDP_BL_STATE_MODE ? " (not in DPCD mode; boot-arg adreno_backlight_mode=1 allows the switch)" : "");
        return nullptr;
    }
    AdrenoBacklight *b = OSTypeAlloc(AdrenoBacklight);
    if (!b)
        return nullptr;
    b->fAllowMode = allowMode;
    bool ok = b->init(nullptr) && b->configure(r[1], r[2], r[3]) && b->attach(display);
    if (ok && !b->start(display)) {
        b->detach(display);
        ok = false;
    }
    if (!ok) {
        OSSafeReleaseNULL(b);
        IOLog("AdrenoBacklight: not published: service setup failed; the panel keeps its brightness\n");
        return nullptr;
    }
    b->registerService();
    uint64_t now = edp_bl_level_to_nits(b->fLevel, b->fMaxNits, b->fMax);
    IOLog("AdrenoBacklight: published (backlight-control): %llu.%03llu..%u nits, level = "
          "nits * %u / %u in levels %u..%u, panel level %u (%llu.%03llu nits)%s\n", b->fMinQ16 >> 16,
          milli(b->fMinQ16 & 0xffff), b->fMaxNits, b->fMax, b->fMaxNits, b->fFloor, b->fMax, b->fLevel, now >> 16,
          milli(now & 0xffff), state == EDP_BL_STATE_MODE ? "; the first change switches the panel to DPCD mode" : "");
    return b;
}

bool AdrenoBacklight::configure(uint64_t x1, uint64_t x2, uint64_t x3)
{
    fState = edp_bl_reply_state(x1);
    fDetail = edp_bl_reply_detail(x1);
    for (unsigned i = 0; i < 4; i++)
        fCaps[i] = (uint8_t)(x1 >> (24 + 8 * i));
    fModeReg = (uint8_t)(x1 >> 56);
    fFloor = (uint32_t)x2;
    fMax = (uint32_t)(x2 >> 32);
    fLevel = (uint32_t)x3;
    fBits = (uint32_t)(x3 >> 32) & 0xff;
    fLsb = (x3 >> 40) & 1;
    if (!fFloor || fFloor >= fMax)
        return false;
    fMinNits = backlightBootArg("adreno_backlight_min_nits", EDP_BL_NITS_MIN_DEFAULT, EDP_BL_NITS_LIMIT_MIN,
                                EDP_BL_NITS_LIMIT_MAX);
    fMaxNits = backlightBootArg("adreno_backlight_max_nits", EDP_BL_NITS_MAX_DEFAULT, EDP_BL_NITS_LIMIT_MIN,
                                EDP_BL_NITS_LIMIT_MAX);
    if (fMinNits >= fMaxNits) {
        fMinNits = EDP_BL_NITS_MIN_DEFAULT;
        fMaxNits = EDP_BL_NITS_MAX_DEFAULT;
    }
    // Stays below the top level's luminance: fFloor < fMax (checked above) and fMinNits < fMaxNits.
    fMinQ16 = edp_bl_min_nits_q16(fMinNits, fMaxNits, fFloor, fMax);
    fLock = IOLockAlloc();
    fWorkLoop = IOWorkLoop::workLoop();
    if (!fLock || !fWorkLoop)
        return false;
    fTimer = IOTimerEventSource::timerEventSource(this, OSMemberFunctionCast(IOTimerEventSource::Action, this,
                                                                           &AdrenoBacklight::apply));
    return fTimer && fWorkLoop->addEventSource(fTimer) == kIOReturnSuccess;
}

bool AdrenoBacklight::start(IOService *provider)
{
    if (!super::start(provider))
        return false;
    // The keys CoreBrightness and BrightnessControl read from a backlight service (AdrenoBacklight.h). The 16.16
    // luminance products are the J700 backlight node's property names; their values are this panel's nominal range,
    // its mid point CoreBrightness's usual 140 nits (INFERRED).
    uint64_t mid = 140ull << 16, top = (uint64_t)fMaxNits << 16;
    mid = mid < fMinQ16 ? fMinQ16 : mid > top ? top : mid;
    setProperty("backlight-control", kOSBooleanTrue);
    setProperty("LminProduct", (unsigned long long)fMinQ16, 32);
    setProperty("LmidProduct", (unsigned long long)mid, 32);
    setProperty("LmaxProduct", (unsigned long long)top, 32);
    publish();
    return true;
}

void AdrenoBacklight::stop(IOService *provider)
{
    IOLockLock(fLock);
    fStopped = true;
    fPending = false;
    IOLockUnlock(fLock);
    if (fTimer) {
        fTimer->cancelTimeout();
        fWorkLoop->removeEventSource(fTimer);
    }
    super::stop(provider);
}

void AdrenoBacklight::free()
{
    OSSafeReleaseNULL(fTimer);
    OSSafeReleaseNULL(fWorkLoop);
    if (fLock) {
        IOLockFree(fLock);
        fLock = nullptr;
    }
    super::free();
}

IOReturn AdrenoBacklight::setProperties(OSObject *properties)
{
    OSDictionary *d = OSDynamicCast(OSDictionary, properties);
    if (!d)
        return kIOReturnBadArgument;
    OSNumber *n = OSDynamicCast(OSNumber, d->getObject("brightness-nits"));
    if (n) {
        // BrightnessControl sends (int)(nits * 65536): a negative value is a request below every level.
        int64_t v = (int64_t)(int32_t)n->unsigned32BitValue();
        if (n->numberOfBits() > 32)
            v = (int64_t)n->unsigned64BitValue();
        return requestNits(v > 0 ? (uint64_t)v : 0, kSourceNits);
    }
    n = OSDynamicCast(OSNumber, d->getObject("brightness"));
    if (n)
        return requestNits(edp_bl_slider_to_nits(n->unsigned64BitValue(), fMinQ16, (uint64_t)fMaxNits << 16),
                           kSourceSlider);
    return kIOReturnUnsupported;
}

IOReturn AdrenoBacklight::requestNits(uint64_t nits_q16, Source source)
{
    // The darkest offer bounds every source, so adreno_backlight_min_nits also holds for set_parameter and "brightness".
    uint32_t level = edp_bl_nits_to_level(nits_q16 < fMinQ16 ? fMinQ16 : nits_q16, fMaxNits, fFloor, fMax);
    uint64_t interval, last;
    IOLockLock(fLock);
    if (fStopped) {
        IOLockUnlock(fLock);
        return kIOReturnNotReady;
    }
    fRequests[source]++;
    if (fPending)
        fCoalesced++;
    fPending = true;
    fPendingLevel = level;
    fPendingNits = nits_q16;
    fPendingSource = source;
    bool arm = !fArmed;
    fArmed = true;
    last = fLastApply;
    IOLockUnlock(fLock);
    if (arm) {
        uint64_t now = mach_absolute_time();
        clock_interval_to_absolutetime_interval(kMinIntervalMs, kMillisecondScale, &interval);
        fTimer->wakeAtTime(last && last + interval > now ? last + interval : now);
    }
    return kIOReturnSuccess;
}

// On fWorkLoop: one monitor call for the latest request.
void AdrenoBacklight::apply(IOTimerEventSource *)
{
    IOLockLock(fLock);
    if (!fPending || fStopped) {
        fArmed = false;
        IOLockUnlock(fLock);
        return;
    }
    uint32_t level = fPendingLevel;
    uint64_t nits = fPendingNits;
    Source source = fPendingSource;
    fPending = false;
    IOLockUnlock(fLock);

    uint64_t r[4];
    uint64_t flags = fState == EDP_BL_STATE_MODE && fAllowMode ? EDP_BL_SET_MODE : 0;
    int64_t e = backlightCall(EDP_BL_SET, level, flags, r);
    if (e != EDP_BL_E_ORIGIN && e != EDP_BL_E_OP) {   // the reply carries the monitor's state
        fState = edp_bl_reply_state(r[1]);
        fDetail = edp_bl_reply_detail(r[1]);
        fModeReg = (uint8_t)(r[1] >> 56);
        fLevel = (uint32_t)r[3];
    }
    bool clamped = !e && ((r[3] >> 41) & 1);
    bool stopNow = e && (fState == EDP_BL_STATE_FAILED || fState == EDP_BL_STATE_UNSUPPORTED ||
                         e == EDP_BL_E_ORIGIN || e == EDP_BL_E_OP);
    IOLockLock(fLock);
    fLastApply = mach_absolute_time();
    if (e) {
        fErrors++;
        fLastError = e;
    } else {
        fApplied++;
        fClamped += clamped;
    }
    if (stopNow)
        fStopped = true;
    bool again = fPending && !fStopped;
    if (!again)
        fArmed = false;
    uint64_t calls = fApplied + fErrors;
    IOLockUnlock(fLock);
    if (e || calls <= kLoggedApplies || !(calls & 63))
        IOLog("AdrenoBacklight: SET %u from %s (%llu.%03llu nits) -> %lld: panel level %u%s, state %s detail %u "
              "(calls %llu)\n", level, kBacklightSources[source], nits >> 16, milli(nits & 0xffff), e, fLevel,
              clamped ? " (clamped)" : "", backlightState(fState), fDetail, calls);
    if (stopNow)
        IOLog("AdrenoBacklight: the monitor stopped (state %s detail %u, %lld): the panel stays at level %u for "
              "this boot\n", backlightState(fState), fDetail, e, fLevel);
    publish();
    if (again) {
        uint64_t interval;
        clock_interval_to_absolutetime_interval(kMinIntervalMs, kMillisecondScale, &interval);
        fTimer->wakeAtTime(fLastApply + interval);
    }
}

// IODisplayParameters as BCAppleBacklightBrtControl reads it, from the panel's level (not the request: a clamped
// or failed request shows), and the diagnostics a run's ioreg captures.
void AdrenoBacklight::publish()
{
    uint64_t top = (uint64_t)fMaxNits << 16, q = edp_bl_level_to_nits(fLevel, fMaxNits, fMax);
    q = q < fMinQ16 ? fMinQ16 : q > top ? top : q;
    OSDictionary *params = OSDictionary::withCapacity(2), *slider = OSDictionary::withCapacity(3),
                 *nits = OSDictionary::withCapacity(3), *diag = OSDictionary::withCapacity(16);
    bool ok = params && slider && nits && diag && putNum(slider, "min", 0) &&
              putNum(slider, "max", EDP_BL_SLIDER_MAX) &&
              putNum(slider, "value", edp_bl_nits_to_slider(q, fMinQ16, top)) &&
              putNum(nits, "min", milli(fMinQ16)) && putNum(nits, "max", (uint64_t)fMaxNits * 1000) &&
              putNum(nits, "value", milli(q));
    if (ok) {   // putDict consumes its dictionary either way
        ok = putDict(params, "brightness", slider);
        ok = putDict(params, "BrightnessMilliNits", nits) && ok;
        slider = nits = nullptr;
    }
    OSSafeReleaseNULL(slider);
    OSSafeReleaseNULL(nits);
    if (ok)
        setProperty("IODisplayParameters", params);
    IOLockLock(fLock);
    uint64_t requests[kSources] = {fRequests[0], fRequests[1], fRequests[2]};
    uint64_t applied = fApplied, clamped = fClamped, errors = fErrors, coalesced = fCoalesced;
    int64_t lastError = fLastError;
    bool stopped = fStopped;
    IOLockUnlock(fLock);
    if (diag && putNum(diag, "State", fState) && putNum(diag, "Detail", fDetail) && putNum(diag, "Floor", fFloor) &&
        putNum(diag, "Max", fMax) && putNum(diag, "Level", fLevel) && putNum(diag, "Bits", fBits) &&
        putNum(diag, "LSB", fLsb) && putNum(diag, "Mode", fModeReg) &&
        putNum(diag, "DPCD700", (uint32_t)fCaps[0] | (uint32_t)fCaps[1] << 8 | (uint32_t)fCaps[2] << 16 |
                                (uint32_t)fCaps[3] << 24) &&
        putNum(diag, "RequestsNits", requests[kSourceNits], 64) &&
        putNum(diag, "RequestsBrightness", requests[kSourceSlider], 64) &&
        putNum(diag, "RequestsIOMFB", requests[kSourceIOMFB], 64) && putNum(diag, "Applied", applied, 64) &&
        putNum(diag, "Clamped", clamped, 64) && putNum(diag, "Errors", errors, 64) &&
        putNum(diag, "Coalesced", coalesced, 64) && putNum(diag, "LastError", (uint64_t)lastError, 64) &&
        putNum(diag, "Stopped", stopped))
        setProperty("AdrenoBacklight", diag);
    OSSafeReleaseNULL(params);
    OSSafeReleaseNULL(diag);
}
