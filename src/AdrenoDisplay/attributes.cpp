// SPDX-License-Identifier: MIT
// DisplayAttributes: the property helpers and the attributes start_hardware publishes and stop withdraws.
// Part of AdrenoDisplay.cpp's translation unit: included from it in order, never compiled on its own.

// ---------------------------------------------------------------- DisplayAttributes

static bool putNum(OSDictionary *d, const char *key, uint64_t v, unsigned bits)
{
    OSNumber *n = OSNumber::withNumber(v, bits);
    bool ok = n && d->setObject(key, n);
    OSSafeReleaseNULL(n);
    return ok;
}

static bool putStr(OSDictionary *d, const char *key, const char *v)
{
    OSString *s = OSString::withCString(v);
    bool ok = s && d->setObject(key, s);
    OSSafeReleaseNULL(s);
    return ok;
}

static bool putDict(OSDictionary *d, const char *key, OSDictionary *v)
{
    bool ok = v && d->setObject(key, v);
    OSSafeReleaseNULL(v);
    return ok;
}

// PV createColorPointDict (0xe28c): {X, Y} in 16.16 (float + 2^-17, truncated), Gamma 1.0.
static OSDictionary *colorPoint(double x, double y)
{
    OSDictionary *d = OSDictionary::withCapacity(3);
    if (!d)
        return nullptr;
    bool ok = putNum(d, "X", (uint32_t)((x + 0x1p-17) * 65536.0)) &&
              putNum(d, "Y", (uint32_t)((y + 0x1p-17) * 65536.0)) &&
              putNum(d, "Gamma", 0x10000);
    if (!ok)
        OSSafeReleaseNULL(d);
    return d;
}

bool AdrenoDisplay::publishDisplayAttributes()
{
    // PV createDisplayAttributes (0xa370..0xb170): ProductAttributes, Chromaticity, the
    // panel geometry, Transport, IOMFBMaxSrcPixels, IOMFBScalingLimits, and the IOAV
    // ColorElements / TimingElements arrays. Values are for the fixed A16 panel mode.
    if (fAttributesPublished)
        return true;
    uuid_t uuid;
    uuid_string_t text;
    uuid_generate(uuid);
    uuid_unparse(uuid, text);
    setProperty("IOMFBUUID", text);
    setProperty("external", false);

    OSDictionary *product = OSDictionary::withCapacity(6);
    OSDictionary *chroma = OSDictionary::withCapacity(3);
    OSDictionary *attrs = OSDictionary::withCapacity(12);
    OSDictionary *transport = OSDictionary::withCapacity(2);
    OSDictionary *maxSrc = OSDictionary::withCapacity(5);
    OSDictionary *scaling = OSDictionary::withCapacity(2);
    bool ok = product && chroma && attrs && transport && maxSrc && scaling;
    ok = ok && putStr(product, "ManufacturerID", "APP") && putNum(product, "ProductID", 0) &&
         putNum(product, "SerialNumber", 0) && putStr(product, "ProductName", "Adreno X2 Panel") &&
         putNum(product, "WeekOfManufacture", 24) && putNum(product, "YearOfManufacture", 2021);
    // sRGB / Rec.709 primaries, D65 white (INFERRED for the A16 panel; PV takes them from the host).
    ok = ok && putDict(chroma, "Red", colorPoint(0.640, 0.330)) &&
         putDict(chroma, "Green", colorPoint(0.300, 0.600)) &&
         putDict(chroma, "Blue", colorPoint(0.150, 0.060));
    ok = ok && attrs->setObject("SupportsPQEOTF", kOSBooleanFalse) &&
         attrs->setObject("ProductAttributes", product) && attrs->setObject("Chromaticity", chroma) &&
         attrs->setObject("DefaultColorSpaceIsSRGB", kOSBooleanFalse) &&
         putDict(attrs, "DefaultWhitePoint", colorPoint(0.3127, 0.3290)) &&
         attrs->setObject("SupportsStandby", kOSBooleanTrue) &&
         putNum(attrs, "ContinuousFrequencySupport", 3) &&
         putNum(attrs, "MaxHorizontalImageSize", (kPanelWidthMM + 9) / 10) &&
         putNum(attrs, "MaxVerticalImageSize", (kPanelHeightMM + 9) / 10) &&
         putNum(attrs, "NativeFormatHorizontalPixels", kWidth) &&
         putNum(attrs, "NativeFormatVerticalPixels", kHeight) &&
         putNum(attrs, "PreciseAspectRatio", (uint32_t)((float)kWidth * 65536.0f / (float)kHeight));
    ok = ok && putStr(transport, "Upstream", "Other") && putStr(transport, "Downstream", "Other");
    ok = ok && putNum(maxSrc, "MaxSrcBufferWidth", 16384) && putNum(maxSrc, "MaxSrcBufferHeight", 16384) &&
         putNum(maxSrc, "MaxSrcRectWidth", 16384) && putNum(maxSrc, "MaxSrcRectTotal", 0) &&
         putNum(maxSrc, "IOMFBMaxCompressedSizeInBytes", 0);
    ok = ok && putNum(scaling, "RGBLayer_MinScaleFraction", 4) && putNum(scaling, "RGBLayer_MaxScale", 4);

    // IOAV elements, as PV's init (0x9018..0x9100) and createDisplayAttributes (0xa854..0xaa58),
    // except that only PV's first color element is published: depth 8, SDR (EOTF 0), RGB, id 100
    // -- the only one the 32 bpp firmware scanout (fScanoutDepth == 32) can show. PV also offers
    // depth-10 SDR (102) and EDR (103); from those, CoreAnimation 26A428 builds 10-bit modes
    // (IOMFBDisplay::update_digital_modes assigns color codes 16..28 only when Depth >= 10) and
    // then renders into 10-bit framebuffers (framebuffer_pixel_format), which neither this copy
    // path nor the plug-in's RGBA8/BGRA8 clear accepts (generation71). One timing element per
    // mode (id 1000 + index, score 1000 - index) owns the color list.
    // Colorimetry 10 is sRGB (IOAVVideoColorimetryString). PV's 16, "Default RGB", is the panel's
    // native gamut: QuartzCore's update_digital_modes gives an RGB Default-RGB element its native
    // colour code, update_framebuffer_locked then sets supportsExtendedColors, and the framebuffer
    // becomes the two-plane 10-bit 'b3a8' (Metal 550) instead of 'BGRA'
    // (docs/ADRENO-WINDOWSERVER-TARGETS-20261017.md). This panel is sRGB: the primaries above, and
    // /product artwork-display-gamut "sRGB".
    if (ok && !fColorElements) {
        fColorElements = OSArray::withCapacity(1);
        IOAVVideoColorElement *c = fColorElements ? IOAVVideoColorElement::withCapabilities(
            (IOAVStandardType)2, 8, (IOAVVideoPixelEncoding)0, (IOAVVideoColorDynamicRange)0,
            (IOAVVideoColorEOTF)0, (IOAVVideoColorimetry)10, false, nullptr, nullptr, nullptr, nullptr) : nullptr;
        if (c) {
            setAVElementIdentity(c, kColorIDSDR8, kColorIDSDR8);
            ok = fColorElements->setObject(c);
            c->release();
        } else {
            ok = false;
        }
    }
    if (ok && !fTimingElements) {
        fTimingElements = OSArray::withCapacity(1);
        IOAVVideoTimingData t = {};
        uint32_t rate = kRefreshHz << 16;                        // INFERRED 16.16 Hz, as PV's mode table
        t.hActive = kWidth;
        t.hSyncRate = rate * kHeight;
        t.vActive = kHeight;
        t.vSyncRate = rate;
        IOAVVideoTimingElement *e = fTimingElements
            ? IOAVVideoTimingElement::withData(&t, (IOAVStandardType)2, (IOAVVideoTimingStandard)4) : nullptr;
        if (e) {
            setAVElementIdentity(e, kTimingID, 1000);
            OSArray *colors = OSArray::withCapacity(1);
            if (colors) {
                colors->merge(fColorElements);
                e->setColorElements(colors);
                colors->release();
            }
            ok = fTimingElements->setObject(e);
            e->release();
        } else {
            ok = false;
        }
    }
    if (ok) {
        setProperty("DisplayAttributes", attrs);
        setProperty("Transport", transport);
        setProperty("ColorElements", fColorElements);
        setProperty("TimingElements", fTimingElements);
        setProperty("IOMFBMaxSrcPixels", maxSrc);
        setProperty("IOMFBScalingLimits", scaling);
        fAttributesPublished = true;
    } else {
        IOLog("AdrenoDisplay: DisplayAttributes construction failed\n");
    }
    OSSafeReleaseNULL(product);
    OSSafeReleaseNULL(chroma);
    OSSafeReleaseNULL(attrs);
    OSSafeReleaseNULL(transport);
    OSSafeReleaseNULL(maxSrc);
    OSSafeReleaseNULL(scaling);
    if (ok) {
        // PV handleHostInterrupt 0x93bc: after (re)creating the attributes, tell clients the
        // display configuration changed (message 0xe0000130).
        messageClients(0xe0000130);
    }
    return ok;
}

void AdrenoDisplay::withdrawDisplayAttributes()
{
    // PV destroyDisplayAttributes 0x977c: the properties go and the size reads zero.
    if (!fAttributesPublished)
        return;
    removeProperty("DisplayAttributes");
    removeProperty("Transport");
    removeProperty("ColorElements");
    removeProperty("TimingElements");
    removeProperty("IOMFBMaxSrcPixels");
    removeProperty("IOMFBScalingLimits");
    fDisplayWidth = fDisplayHeight = 0;
    fAttributesPublished = false;
}
