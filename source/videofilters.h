/****************************************************************************
 * Visual Boy Advance GX
 *
 * videofilters.h
 *
 * Video filter implementations (scanlines, Scale2x, and future filters),
 * split out of video.cpp to keep filter-specific data/algorithms separate
 * from the core GX render loop that calls into them. Mirrors the general
 * shape of VBA-GX 3.0.0's per-filter source files, adapted to this port's
 * mix of GX-native (GPU texture/TEV) and CPU pixel-buffer filters.
 *
 * GX_Render()/configure_tev_pipeline() in video.cpp remain the only
 * callers of this file's functions - the small amount of state exposed
 * here (scanlineTexObj, scale2xBuffer) is only what video.cpp genuinely
 * needs to load into GX or read out of every frame; everything else about
 * each filter's implementation stays private to videofilters.cpp.
 ***************************************************************************/

#ifndef _VIDEOFILTERS_H_
#define _VIDEOFILTERS_H_

#include <gccore.h>

/*** Scanline filter (FILTER_SCANLINES) ***
 * GX-native: an 8x4 I8 repeating tile texture, sampled via GX_REPEAT and
 * multiplied against the game texture in a second TEV stage. See
 * videofilters.cpp for the tile generation and video.cpp's
 * configure_tev_pipeline() for the TEV stage setup that uses it.
 *
 * EnsureScanlineTexture() lazily generates the tile texture on first call
 * and is a cheap no-op on every call after that - safe to call
 * unconditionally every frame the scanline filter is active. */
void EnsureScanlineTexture();
extern GXTexObj scanlineTexObj;

/*** LCD grid / aperture-grille filter (FILTER_LCD_GRID) ***
 * GX-native, same technique as the scanline filter above (a small
 * repeating texture multiplied against the game texture in a second TEV
 * stage - see video.cpp's configure_tev_pipeline(), which now picks
 * between scanlineTexObj and this based on GCSettings.FilterMethod) but a
 * different tile: vertical RGB subpixel stripes instead of horizontal
 * darkened rows, approximating an aperture-grille CRT/LCD's per-pixel RGB
 * mask rather than its horizontal scan structure.
 *
 * EnsureLCDGridTexture() lazily generates the tile texture on first call
 * and is a cheap no-op on every call after that - safe to call
 * unconditionally every frame the filter is active, same as
 * EnsureScanlineTexture(). */
void EnsureLCDGridTexture();
extern GXTexObj lcdGridTexObj;

/*** LCD subpixel (RGB) filter (FILTER_LCD_RGB) ***
 * GX-native, same multiply-texture technique as the two filters above, but
 * this tile carries actual per-channel COLOR instead of a monochrome
 * intensity mask - a repeating red/green/blue/black-gap column pattern
 * that approximates a real LCD panel's RGB subpixel structure (and the
 * dark gap between subpixel triads), rather than a CRT's scan or grille
 * structure. Needs a color texture format (GX_TF_RGB565) instead of
 * FILTER_LCD_GRID's GX_TF_I8, so it gets its own tile/texobj rather than
 * reusing lcdGridTexObj - see video.cpp's configure_tev_pipeline() for how
 * the three mask filters share the same TEV stage 1 wiring and only swap
 * which texture (and which per-filter tile width) gets loaded.
 *
 * EnsureLCDRGBTexture() lazily generates the tile texture on first call
 * and is a cheap no-op on every call after that, same as the other two
 * mask filters' Ensure*Texture() functions. */
void EnsureLCDRGBTexture();
extern GXTexObj lcdRgbTexObj;

/*** Scale2x filter (FILTER_SCALE2X) ***
 * CPU-side: standard Scale2x/AdvMAME2x neighbor-comparison pixel doubling
 * on an RGB565 buffer. See videofilters.cpp for the algorithm itself.
 *
 * srcStride/dstStride are in PIXELS, and both buffers are expected to
 * carry the same 2-pixel row padding WriteFrameToTextureMemory's gbPitch
 * (video.cpp) already assumes. */
void Scale2xRGB565(const u16 *src, int srcStride, int width, int height, u16 *dst, int dstStride);

// Scratch buffer for Scale2x output, sized for the largest supported case
// (GBA 240x160 doubled to 480x320) with the same 2-pixel row padding
// WriteFrameToTextureMemory expects.
#define SCALE2X_BUF_SIZE ((240 * 2 + 2) * (160 * 2) * 2)
extern u8 scale2xBuffer[SCALE2X_BUF_SIZE];

/*** Sharp Bilinear filter (FILTER_SHARP_BILINEAR) ***
 * Two-pass technique: a nearest-neighbor integer prescale of the raw game
 * texture, then a real bilinear-filtered sample of THAT for the final
 * on-screen quad. This header owns the scratch prescale texture and the
 * pure sizing/init logic; the actual GX draw calls (pass 1's point-sampled
 * draw + GX_CopyTex, pass 2's on-screen draw_square()) stay in video.cpp's
 * RenderSharpBilinearPrescale()/GX_Render(), since those depend on that
 * file's own view matrix, texobj, and draw_square_cropped() - mirroring
 * how the scanline filter's tile texture lives here but
 * configure_tev_pipeline() (the GX pipeline wiring that uses it) stays in
 * video.cpp. */
#define SHARP_BILINEAR_MAX_W 720
#define SHARP_BILINEAR_MAX_H 576
extern u8 sharpBilinearTexMem[SHARP_BILINEAR_MAX_W * SHARP_BILINEAR_MAX_H * 2];
extern GXTexObj sharpBilinearTexObj;

// Pure math, no GX calls: the smallest integer prescale factor that gets
// borderWidth/borderHeight to at least onScreenW/onScreenH in both
// dimensions (so pass 2's bilinear stretch always has real room to smooth
// edges), clamped to the EFB bounds (efbMaxW/efbMaxH - pass vmode->fbWidth/
// efbHeight) and to SHARP_BILINEAR_MAX_W/H, then rounded down to a multiple
// of 4 for GX's tiled texture format. Returns false (leaving *outW/*outH
// untouched) if the result is degenerate (<=0 in either dimension) -
// caller should fall back to sampling the raw texture directly in that
// case.
//
// Pass liveVW/liveVH (video.cpp) as onScreenW/onScreenH, NOT realVW/realVH
// - see RenderSharpBilinearPrescale()'s own comment in video.cpp for why:
// realVW/realVH is the full GX viewport in non-fixed scaling modes, which
// over-prescales and aliases right back away.
bool ComputeSharpBilinearPrescaleSize(int borderWidth, int borderHeight,
                                       int onScreenW, int onScreenH,
                                       int efbMaxW, int efbMaxH,
                                       int *outW, int *outH);

// Lazily (re)inits sharpBilinearTexObj (GX_TF_RGB565, GX_CLAMP, always
// GX_LINEAR - the bilinear step itself) for a new prescaleW x prescaleH
// size. Safe to call every frame; no-op if the size hasn't changed since
// the last call.
void EnsureSharpBilinearTexture(int prescaleW, int prescaleH);

#endif
