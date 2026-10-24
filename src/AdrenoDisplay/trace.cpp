// SPDX-License-Identifier: MIT
// The IOMFB call trace (-DADRENO_DISPLAY_TRACE_MODES=1, AdrenoDisplay.h): what a client asks of the framebuffer when
// display settings are opened or changed, and what the family answers.
// Part of AdrenoDisplay.cpp's translation unit: included from it in order, never compiled on its own.
//
// Why: System Settings > Displays turned the desktop into alternating stale frames in b12v23 and org2 while the kext
// logged nothing about modes, colour or properties. Every call below is forwarded unchanged; the trace only counts and
// logs it. What the family answers on 26A428 (KDK IOMobileGraphicsFamily, arm64e; VERIFIED from its code):
//   set/get_gamma_table, set/get_matrix, set_contrast, set_color_remap_mode, setBrightnessCorrection,
//   splc_set/get_brightness, swap_set_color_matrix: kIOReturnUnsupported, unconditionally;
//   get_color_remap_mode: mode 6; set_block/get_block/get_block_size: kIOReturnUnsupported (inline in the header);
//   set_digital_out_mode: this class (one mode, the published pair only); set_parameter: a switch over 25 names.
// Pointer arguments are read only where the family's own implementation reads them (set_parameter's values, the
// outputs of a successful get), so the trace adds no access a client could aim.
#if ADRENO_DISPLAY_TRACE_MODES
#include <libkern/c++/OSCollectionIterator.h>
#include <libkern/c++/OSSymbol.h>

enum {
    kTraceDigitalOutMode, kTraceDigitalOutState, kTraceDisplayDevice, kTraceCanvas, kTraceGammaSet, kTraceGammaGet,
    kTraceMatrixSet, kTraceMatrixGet, kTraceContrast, kTraceWhiteOnBlack, kTraceRemapSet, kTraceRemapGet,
    kTraceBrightnessCorrection, kTraceSplcSet, kTraceSplcGet, kTraceBlockSet, kTraceBlockGet, kTraceBlockSize,
    kTraceSwapColorMatrix, kTraceParameter, kTraceDithering, kTraceProperties, kTraceUserClient,
    kTraceSwapScaled, kTraceSwapPlaced, kTraceSwapCropped, kTraceSwapSized, kTraceKinds
};
// One display: file-scope counters in bss (no initialized data ahead of kmod_info). Calls arrive on user-client
// threads outside the gate, so they are counted atomically; the window marks are touched only under the gate.
static uint32_t gTraceCalls[kTraceKinds], gTraceMark[kTraceKinds];

static uint32_t traceCount(unsigned kind)
{
    return __atomic_add_fetch(&gTraceCalls[kind], 1, __ATOMIC_RELAXED);
}
// The first 8 of a kind, then every 256th: a client polling every frame costs one line per ~4 s.
static bool traceDue(uint32_t n)
{
    return n <= 8 || !(n & 255);
}
#define TRACE_CALL(kind, fmt, ...)                                                                                     \
    do {                                                                                                               \
        const uint32_t traceN = traceCount(kind);                                                                      \
        if (traceDue(traceN)) IOLog("AdrenoDisplay: iomfb " fmt " (%u)\n", ##__VA_ARGS__, traceN);                    \
    } while (0)

IOReturn AdrenoDisplay::set_digital_out_mode(unsigned int colorID, unsigned int timingID)
{
    // Color first, timing second (modeset.cpp); only (100, 1000), the published pair, succeeds.
    const IOReturn r = digitalOutMode(colorID, timingID);
    TRACE_CALL(kTraceDigitalOutMode, "set_digital_out_mode(color %u, timing %u) = 0x%x", colorID, timingID, r);
    return r;
}

IOReturn AdrenoDisplay::setProperties(OSObject *properties)
{
    const IOReturn r = IOMobileFramebufferLegacy::setProperties(properties);
    const uint32_t n = traceCount(kTraceProperties);
    if (!traceDue(n)) return r;
    char keys[192] = "";
    OSDictionary *d = OSDynamicCast(OSDictionary, properties);
    OSCollectionIterator *it = d ? OSCollectionIterator::withCollection(d) : nullptr;
    size_t used = 0;
    while (it) {
        const OSSymbol *key = OSDynamicCast(OSSymbol, it->getNextObject());
        if (!key) break;
        const int w = snprintf(keys + used, sizeof keys - used, "%s%s", used ? "," : "", key->getCStringNoCopy());
        if (w < 0 || (size_t)w >= sizeof keys - used) { keys[sizeof keys - 2] = '+'; break; }
        used += (size_t)w;
    }
    OSSafeReleaseNULL(it);
    IOLog("AdrenoDisplay: iomfb setProperties(%s) = 0x%x (%u)\n", d ? keys : "not a dictionary", r, n);
    return r;
}

IOReturn AdrenoDisplay::newUserClient(task_t owningTask, void *securityID, UInt32 type, IOUserClient **handler)
{
    const IOReturn r = IOMobileFramebufferLegacy::newUserClient(owningTask, securityID, type, handler);
    TRACE_CALL(kTraceUserClient, "newUserClient(type %u) = 0x%x", (unsigned)type, r);
    return r;
}

IOReturn AdrenoDisplay::set_display_device(unsigned int a0)
{
    const IOReturn r = IOMobileFramebufferLegacy::set_display_device(a0);
    TRACE_CALL(kTraceDisplayDevice, "set_display_device(%u) = 0x%x", a0, r);
    return r;
}

IOReturn AdrenoDisplay::get_digital_out_state(unsigned int *a0)
{
    const IOReturn r = IOMobileFramebufferLegacy::get_digital_out_state(a0);
    TRACE_CALL(kTraceDigitalOutState, "get_digital_out_state() = 0x%x state %u", r, r == kIOReturnSuccess && a0 ? *a0 : 0);
    return r;
}

IOReturn AdrenoDisplay::set_canvas_size(unsigned int a0, unsigned int a1)
{
    const IOReturn r = IOMobileFramebufferLegacy::set_canvas_size(a0, a1);
    TRACE_CALL(kTraceCanvas, "set_canvas_size(%u, %u) = 0x%x", a0, a1, r);
    return r;
}

IOReturn AdrenoDisplay::get_gamma_table(IOMFBGammaTable *a0)
{
    const IOReturn r = IOMobileFramebufferLegacy::get_gamma_table(a0);
    TRACE_CALL(kTraceGammaGet, "get_gamma_table() = 0x%x", r);
    return r;
}

IOReturn AdrenoDisplay::set_gamma_table(IOMFBGammaTable *a0)
{
    const IOReturn r = IOMobileFramebufferLegacy::set_gamma_table(a0);
    TRACE_CALL(kTraceGammaSet, "set_gamma_table(%s) = 0x%x", a0 ? "table" : "null", r);
    return r;
}

IOReturn AdrenoDisplay::get_matrix(unsigned int a0, unsigned long long (*a1)[3][3]) const
{
    const IOReturn r = IOMobileFramebufferLegacy::get_matrix(a0, a1);
    TRACE_CALL(kTraceMatrixGet, "get_matrix(%u) = 0x%x", a0, r);
    return r;
}

IOReturn AdrenoDisplay::set_matrix(unsigned int a0, unsigned long long const (*a1)[3][3])
{
    const IOReturn r = IOMobileFramebufferLegacy::set_matrix(a0, a1);
    TRACE_CALL(kTraceMatrixSet, "set_matrix(%u, %s) = 0x%x", a0, a1 ? "matrix" : "null", r);
    return r;
}

IOReturn AdrenoDisplay::set_contrast(float *a0)
{
    const IOReturn r = IOMobileFramebufferLegacy::set_contrast(a0);
    TRACE_CALL(kTraceContrast, "set_contrast(%s) = 0x%x", a0 ? "value" : "null", r);
    return r;
}

IOReturn AdrenoDisplay::set_white_on_black_mode(unsigned int a0)
{
    const IOReturn r = IOMobileFramebufferLegacy::set_white_on_black_mode(a0);
    TRACE_CALL(kTraceWhiteOnBlack, "set_white_on_black_mode(%u) = 0x%x", a0, r);
    return r;
}

IOReturn AdrenoDisplay::set_color_remap_mode(DisplayColorRemapMode a0)
{
    const IOReturn r = IOMobileFramebufferLegacy::set_color_remap_mode(a0);
    TRACE_CALL(kTraceRemapSet, "set_color_remap_mode(%u) = 0x%x", (unsigned)a0, r);
    return r;
}

IOReturn AdrenoDisplay::get_color_remap_mode(DisplayColorRemapMode *a0) const
{
    const IOReturn r = IOMobileFramebufferLegacy::get_color_remap_mode(a0);
    TRACE_CALL(kTraceRemapGet, "get_color_remap_mode() = 0x%x mode %u", r, r == kIOReturnSuccess && a0 ? (unsigned)*a0 : 0);
    return r;
}

IOReturn AdrenoDisplay::setBrightnessCorrection(unsigned int a0)
{
    const IOReturn r = IOMobileFramebufferLegacy::setBrightnessCorrection(a0);
    TRACE_CALL(kTraceBrightnessCorrection, "setBrightnessCorrection(%u) = 0x%x", a0, r);
    return r;
}

IOReturn AdrenoDisplay::splc_set_brightness(unsigned int a0)
{
    const IOReturn r = IOMobileFramebufferLegacy::splc_set_brightness(a0);
    TRACE_CALL(kTraceSplcSet, "splc_set_brightness(%u) = 0x%x", a0, r);
    return r;
}

IOReturn AdrenoDisplay::splc_get_brightness(unsigned int *a0)
{
    const IOReturn r = IOMobileFramebufferLegacy::splc_get_brightness(a0);
    TRACE_CALL(kTraceSplcGet, "splc_get_brightness() = 0x%x value %u", r, r == kIOReturnSuccess && a0 ? *a0 : 0);
    return r;
}

IOReturn AdrenoDisplay::set_block(task *a0, unsigned int a1, unsigned int a2, unsigned long long const *a3,
                                  unsigned int a4, unsigned char const *a5, unsigned long a6)
{
    const IOReturn r = IOMobileFramebufferLegacy::set_block(a0, a1, a2, a3, a4, a5, a6);
    TRACE_CALL(kTraceBlockSet, "set_block(type 0x%x, %u, scalars %u, bytes %lu) = 0x%x", a1, a2, a4, a6, r);
    return r;
}

IOReturn AdrenoDisplay::get_block(task *a0, unsigned int a1, unsigned int a2, unsigned long long const *a3,
                                  unsigned int a4, unsigned char *a5, unsigned long a6) const
{
    const IOReturn r = IOMobileFramebufferLegacy::get_block(a0, a1, a2, a3, a4, a5, a6);
    TRACE_CALL(kTraceBlockGet, "get_block(type 0x%x, %u, scalars %u, bytes %lu) = 0x%x", a1, a2, a4, a6, r);
    return r;
}

IOReturn AdrenoDisplay::get_block_size(task *a0, unsigned int a1, unsigned int *a2)
{
    const IOReturn r = IOMobileFramebufferLegacy::get_block_size(a0, a1, a2);
    TRACE_CALL(kTraceBlockSize, "get_block_size(type 0x%x) = 0x%x", a1, r);
    return r;
}

IOReturn AdrenoDisplay::swap_set_color_matrix(IOMFBColorFixedMatrix *a0, IOMFBColorMatrixFunction a1, unsigned int a2)
{
    const IOReturn r = IOMobileFramebufferLegacy::swap_set_color_matrix(a0, a1, a2);
    TRACE_CALL(kTraceSwapColorMatrix, "swap_set_color_matrix(%s, function %u, %u) = 0x%x", a0 ? "matrix" : "null",
               (unsigned)a1, a2, r);
    return r;
}

IOReturn AdrenoDisplay::set_parameter(IOMFBParameterName a0, unsigned long long const *a1, unsigned int a2)
{
    // The family's switch reads a1[0 .. a2) itself (iomgf 0x2ce20), so the first value is read here only when a2 > 0.
    const unsigned long long first = a1 && a2 ? a1[0] : 0;
    const IOReturn r = IOMobileFramebufferLegacy::set_parameter(a0, a1, a2);
    TRACE_CALL(kTraceParameter, "set_parameter(name %u, count %u, first 0x%llx) = 0x%x", (unsigned)a0, a2, first, r);
    return r;
}

IOReturn AdrenoDisplay::enable_disable_dithering(unsigned int a0)
{
    const IOReturn r = IOMobileFramebufferLegacy::enable_disable_dithering(a0);
    TRACE_CALL(kTraceDithering, "enable_disable_dithering(%u) = 0x%x", a0, r);
    return r;
}

void AdrenoDisplay::traceSwapGeometryGated(IOSurface *surface)
{
    // presentGated just recorded this swap's layer-0 rectangles. The copy applies neither (1:1 from the top-left), so a
    // swap that needs scaling, placement or cropping is shown wrong: these lines say whether a client asked for it.
    const IOMFBSwapFields::Rect s = fLastSrc, d = fLastDst;
    const uint64_t w = surface ? surface->getWidth() : 0, h = surface ? surface->getHeight() : 0;
    const unsigned g = adreno_display_swap_geometry(s.x, s.y, s.w, s.h, d.x, d.y, d.w, d.h, w, h, kWidth, kHeight);
    if (!g) return;
    const unsigned kind = (g & ADRENO_DISPLAY_GEOMETRY_SCALED) ? kTraceSwapScaled :
                          (g & ADRENO_DISPLAY_GEOMETRY_PLACED) ? kTraceSwapPlaced :
                          (g & ADRENO_DISPLAY_GEOMETRY_CROPPED) ? kTraceSwapCropped : kTraceSwapSized;
    TRACE_CALL(kind, "layer-0 swap%s%s%s%s: surface %llux%llu %08x src (%d,%d %dx%d) dst (%d,%d %dx%d)",
               (g & ADRENO_DISPLAY_GEOMETRY_SCALED) ? " scaled" : "", (g & ADRENO_DISPLAY_GEOMETRY_PLACED) ? " placed" : "",
               (g & ADRENO_DISPLAY_GEOMETRY_CROPPED) ? " cropped" : "", (g & ADRENO_DISPLAY_GEOMETRY_SIZED) ? " sized" : "",
               w, h, surface ? surface->getPixelFormat() : 0, s.x, s.y, s.w, s.h, d.x, d.y, d.w, d.h);
}

void AdrenoDisplay::traceWindowGated()
{
    uint32_t v[kTraceKinds];
    for (unsigned i = 0; i < kTraceKinds; i++) {
        const uint32_t now = __atomic_load_n(&gTraceCalls[i], __ATOMIC_RELAXED);
        v[i] = now - gTraceMark[i];
        gTraceMark[i] = now;
    }
#if ADRENO_PERF_FRAMES
    const unsigned long long seq = fPerfSeq;
#else
    const unsigned long long seq = 0;
#endif
    IOLog("AdrenoDisplay: iomfb s=%llu mode=%u/%u dev=%u canvas=%u gamma=%u/%u matrix=%u/%u contrast=%u wob=%u "
          "remap=%u/%u bc=%u splc=%u/%u block=%u/%u/%u cm=%u param=%u dither=%u props=%u uc=%u swap=%u/%u/%u/%u\n",
          seq, v[kTraceDigitalOutMode], v[kTraceDigitalOutState], v[kTraceDisplayDevice], v[kTraceCanvas],
          v[kTraceGammaSet], v[kTraceGammaGet], v[kTraceMatrixSet], v[kTraceMatrixGet], v[kTraceContrast],
          v[kTraceWhiteOnBlack], v[kTraceRemapSet], v[kTraceRemapGet], v[kTraceBrightnessCorrection], v[kTraceSplcSet],
          v[kTraceSplcGet], v[kTraceBlockSet], v[kTraceBlockGet], v[kTraceBlockSize], v[kTraceSwapColorMatrix],
          v[kTraceParameter], v[kTraceDithering], v[kTraceProperties], v[kTraceUserClient], v[kTraceSwapScaled],
          v[kTraceSwapPlaced], v[kTraceSwapCropped], v[kTraceSwapSized]);
}

#undef TRACE_CALL
#endif
