// SPDX-License-Identifier: MIT
// The cursor: updateCursorGated captures a swap's cursor layer for the present to composite.
// Part of AdrenoDisplay.cpp's translation unit: included from it in order, never compiled on its own.

bool AdrenoDisplay::updateCursorGated(IOMFBSwapIORequest *swap, uint8_t layers)
{
    // PV program_hardware_gated 0x118d8..0x1190c: with mask bit 1, the layer-1 surface
    // (swap+0xb88) and destRect[1].x/.y (swap+0x61/+0x65, int) go to
    // program_hardware_cursor_gated, which refuses a surface that is not 'BGRA' with 4 bytes
    // per element (0x11004/0x11014) and programs the host cursor glyph 1:1 (no scaling).
    // Here: the layer's srcRect is captured 1:1 and drawn with its pixel (0,0) at the
    // destRect origin. A layer whose bit is clear keeps its state (the family's own rule,
    // swapWillChangeDisplayedData); a set bit with no surface (ID 0) switches it off.
    bool report = ++fCursorSwaps == 1;
    if (!(layers & 1))
        fCursorOnlySwaps++;
    for (uint32_t l = 1; l < IOMFBSwapFields::kLayerCount; l++) {
        if (!(layers & (1u << l)))
            continue;
        IOSurface *surface = IOMFBSwapFields::layerSurface(swap, l);
        IOMFBSwapFields::Rect src = IOMFBSwapFields::srcRect(swap, l), dst = IOMFBSwapFields::dstRect(swap, l);
        struct adreno_display_rect rect = {src.x, src.y, src.w, src.h};
        if (!surface) {
            fCursorHides++;
            adreno_display_cursor_layer(&fCursor, l, nullptr, rect, dst.x, dst.y, nullptr, nullptr);
            continue;
        }
        fCursorLayer = l;
        fCursorSrc = src;
        fCursorDst = dst;
        fCursorFormat = surface->getPixelFormat();
        fCursorSurfaceWidth = surface->getWidth();
        fCursorSurfaceHeight = surface->getHeight();
        fCursorSurfaceRowBytes = surface->getBytesPerRow();
        // Mapped only when it can be shown: 'BGRA' and the two cursor buffers present.
        struct adreno_display_surface s = {
            nullptr, 0, surface->getBaseOffset(), fCursorSurfaceRowBytes, fCursorSurfaceWidth,
            fCursorSurfaceHeight, fCursorFormat};
        int error = 0;
        if (fCursorFormat == ADRENO_DISPLAY_FORMAT_BGRA) {
            if (!cursorBuffersGated()) {
                error = (int)kIOReturnNoMemory;
            } else {
                IOMemoryDescriptor *md = surface->getMemoryDescriptor();
                IOMemoryMap *map = md ? mapSurfaceGated(md, fCursorMaps, kCursorMapSlots) : nullptr;
                if (map) {
                    s.base = (const uint8_t *)map->getVirtualAddress();
                    s.length = map->getLength();
                }
            }
        }
        if (error)
            adreno_display_cursor_hide(&fCursor);
        else
            error = adreno_display_cursor_layer(&fCursor, l, &s, rect, dst.x, dst.y, maintainSourceRow, nullptr);
        if (error) {
            if (error == ADRENO_DISPLAY_E_FORMAT)
                fCursorFormatSkips++;               // hidden, nothing mapped or read
            else
                fCursorErrors++;
            if (error != fLastCursorError || fCursorErrors + fCursorFormatSkips <= 4) {
                IOLog("AdrenoDisplay: cursor layer %u %08x %llux%llu src (%d,%d %dx%d) not shown (%d)\n", l,
                      fCursorFormat, fCursorSurfaceWidth, fCursorSurfaceHeight, fCursorSrc.x, fCursorSrc.y,
                      fCursorSrc.w, fCursorSrc.h, error);
                report = true;
            }
            fLastCursorError = error;
            continue;
        }
        if ((uint32_t)fCursorDst.w != fCursor.width || (uint32_t)fCursorDst.h != fCursor.height)
            fCursorScaled++;
        if (++fCursorUpdates <= 2) {
            IOLog("AdrenoDisplay: cursor layer %u %08x %llux%llu src (%d,%d %dx%d) dst (%d,%d %dx%d)\n", l,
                  fCursorFormat, fCursorSurfaceWidth, fCursorSurfaceHeight, fCursorSrc.x, fCursorSrc.y,
                  fCursorSrc.w, fCursorSrc.h, fCursorDst.x, fCursorDst.y, fCursorDst.w, fCursorDst.h);
            report = true;
        }
    }
    return report;
}
