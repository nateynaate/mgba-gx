/****************************************************************************
 * Visual Boy Advance GX
 *
 * videofilters.cpp
 *
 * Video filter implementations - extracted out of video.cpp so filter
 * code (data + algorithms) is separated from the core GX render loop that
 * calls into it. See videofilters.h for what's actually exposed to
 * video.cpp; everything else here is private to this file.
 ***************************************************************************/

#include <gccore.h>
#include <ogc/machine/processor.h>
#include <string.h>

#include "videofilters.h"

/****************************************************************************
 * Scanline filter (FILTER_SCANLINES)
 *
 * An 8x4 I8 (one byte per pixel) repeating tile, rows alternating full
 * brightness / darkened, sampled via GX_REPEAT and multiplied against the
 * game texture in a second TEV stage (see configure_tev_pipeline() in
 * video.cpp). Ported from VBA-GX 3.0.0's InitScanlineTexture()/
 * SetupScanlineFilterTEV() - this is real GX hardware work (one extra
 * texture sample + multiply per pixel), not a CPU pixel loop, so it's
 * effectively free compared to any of the CPU-side scaling filters in the
 * same upstream release.
 ***************************************************************************/
static unsigned char scanline_tex_data[32] ATTRIBUTE_ALIGN(32);
GXTexObj scanlineTexObj;
static bool scanlineTexInited = false;

void EnsureScanlineTexture()
{
	if (scanlineTexInited)
		return;

	// 8x4 tile: rows 0/2 full brightness, rows 1/3 darkened - 0xA0
	// controls how visible the scanlines are.
	for (int y = 0; y < 4; y++) {
		u8 intensity = (y % 2 == 0) ? 0xFF : 0xA0;
		for (int x = 0; x < 8; x++)
			scanline_tex_data[y * 8 + x] = intensity;
	}

	// GX reads texture data directly from main memory, not cache - flush
	// before the GPU ever touches this.
	DCFlushRange(scanline_tex_data, 32);

	// GX_REPEAT on both axes so this tiles across the whole screen from a
	// single 8x4 source. GX_NEAR (not GX_LINEAR) is required - linear
	// filtering blurs the alternating rows into flat gray, defeating the
	// effect entirely.
	GX_InitTexObj(&scanlineTexObj, scanline_tex_data, 8, 4, GX_TF_I8, GX_REPEAT, GX_REPEAT, GX_FALSE);
	GX_InitTexObjFilterMode(&scanlineTexObj, GX_NEAR, GX_NEAR);

	scanlineTexInited = true;
}

/****************************************************************************
 * LCD grid / aperture-grille filter (FILTER_LCD_GRID)
 *
 * Same GX-native multiply-texture technique as the scanline filter above
 * (see its own comment for the general approach and why GX_NEAR matters),
 * just rotated 90 degrees: an 8x4 I8 tile with alternating COLUMNS instead
 * of rows, so it reads as a vertical grid/grille pattern rather than
 * horizontal scan lines when multiplied against the game texture in
 * video.cpp's configure_tev_pipeline() (same TEV stage 1 setup the
 * scanline filter uses - only which texture gets loaded into GX_TEXMAP1
 * changes). Monochrome intensity mask, not a colored RGB subpixel mask -
 * keeps this an exact drop-in alternative to the scanline tile with no
 * changes needed to the TEV color math that already multiplies stage 0's
 * output by stage 1's texture color.
 ***************************************************************************/
static unsigned char lcdgrid_tex_data[32] ATTRIBUTE_ALIGN(32);
GXTexObj lcdGridTexObj;
static bool lcdGridTexInited = false;

void EnsureLCDGridTexture()
{
	if (lcdGridTexInited)
		return;

	// 8x4 tile: columns 0/2/4/6 full brightness, columns 1/3/5/7 darkened -
	// same 0xA0 grid intensity the scanline tile uses for its rows, so the
	// two filters read as equally strong.
	for (int y = 0; y < 4; y++) {
		for (int x = 0; x < 8; x++) {
			u8 intensity = (x % 2 == 0) ? 0xFF : 0xA0;
			lcdgrid_tex_data[y * 8 + x] = intensity;
		}
	}

	DCFlushRange(lcdgrid_tex_data, 32);

	// GX_NEAR, not GX_LINEAR, for the same reason as the scanline tile -
	// linear filtering blurs the alternating columns into flat gray.
	GX_InitTexObj(&lcdGridTexObj, lcdgrid_tex_data, 8, 4, GX_TF_I8, GX_REPEAT, GX_REPEAT, GX_FALSE);
	GX_InitTexObjFilterMode(&lcdGridTexObj, GX_NEAR, GX_NEAR);

	lcdGridTexInited = true;
}

/****************************************************************************
 * LCD subpixel (RGB) filter (FILTER_LCD_RGB)
 *
 * Unlike the two mask filters above (monochrome intensity, GX_TF_I8), this
 * tile carries real per-channel color: a repeating 4-column pattern of
 * red / green / blue / dark-gap, approximating a real LCD panel's RGB
 * subpixel triads and the black matrix between them. Multiplied against
 * the game texture in the same TEV stage 1 setup the other two mask
 * filters use (see video.cpp's configure_tev_pipeline()) - GX_CC_TEXC
 * there is a straight per-channel multiply, so this tile's red column
 * (full red, dimmed green/blue) tints the pixels under it toward red
 * without needing any different TEV math, just a color texture instead of
 * an intensity one.
 *
 * 4x4 is both the tile's natural repeat unit (4 columns: R, G, B, gap) and
 * RGB565's native GX tile-block width, so - same trick as the scanline/grid
 * I8 tiles being exactly one native I8 block - no manual swizzling is
 * needed here either.
 ***************************************************************************/
#define LCDRGB_FULL_R  0x1F  // 5-bit red channel, full intensity
#define LCDRGB_FULL_G  0x3F  // 6-bit green channel, full intensity
#define LCDRGB_FULL_B  0x1F  // 5-bit blue channel, full intensity
// The other two channels' response to a colored column, and the gap
// column's response on all three channels. Originally ~28%/~19% - a real
// subpixel filter reads fine at native LCD density because your eye
// blends the triad, but at Wii output resolution each column is many real
// pixels wide, so that much darkening/saturation just reads as color
// noise sitting on top of a dimmed image ("muddy"). Raised close to a
// mild tint instead of a strong one - still visibly colored, but leaves
// most of the original brightness and hue intact.
#define LCDRGB_DIM_R   0x14  // ~63%
#define LCDRGB_DIM_G   0x28
#define LCDRGB_DIM_B   0x14
#define LCDRGB_GAP_R   0x11  // ~55% - lighter than a true black gap, for the same reason
#define LCDRGB_GAP_G   0x23
#define LCDRGB_GAP_B   0x11
#define RGB565(r, g, b) (u16)(((r) << 11) | ((g) << 5) | (b))

static u16 lcdrgb_tex_data[16] ATTRIBUTE_ALIGN(32); // 4x4 RGB565
GXTexObj lcdRgbTexObj;
static bool lcdRgbTexInited = false;

void EnsureLCDRGBTexture()
{
	if (lcdRgbTexInited)
		return;

	u16 red  = RGB565(LCDRGB_FULL_R, LCDRGB_DIM_G,  LCDRGB_DIM_B);
	u16 grn  = RGB565(LCDRGB_DIM_R,  LCDRGB_FULL_G,  LCDRGB_DIM_B);
	u16 blu  = RGB565(LCDRGB_DIM_R,  LCDRGB_DIM_G,   LCDRGB_FULL_B);
	u16 gap  = RGB565(LCDRGB_GAP_R,  LCDRGB_GAP_G,   LCDRGB_GAP_B);
	u16 row[4] = { red, grn, blu, gap };

	// Same column pattern in all 4 rows - the effect is purely horizontal
	// subpixel structure, no vertical variation, so this tiles cleanly in
	// both directions via GX_REPEAT.
	for (int y = 0; y < 4; y++)
		memcpy(&lcdrgb_tex_data[y * 4], row, sizeof(row));

	DCFlushRange(lcdrgb_tex_data, sizeof(lcdrgb_tex_data));

	// GX_NEAR - same reasoning as the other two mask filters: GX_LINEAR
	// would blend the red/green/blue/gap columns into flat gray-ish mush,
	// which for this filter means losing the color entirely, not just the
	// contrast.
	GX_InitTexObj(&lcdRgbTexObj, lcdrgb_tex_data, 4, 4, GX_TF_RGB565, GX_REPEAT, GX_REPEAT, GX_FALSE);
	GX_InitTexObjFilterMode(&lcdRgbTexObj, GX_NEAR, GX_NEAR);

	lcdRgbTexInited = true;
}

/****************************************************************************
 * Scale2x filter (FILTER_SCALE2X)
 *
 * Standard Scale2x/AdvMAME2x algorithm (Andrea Mazzoleni,
 * https://www.scale2x.it/, BSD-style license) - doubles a linear RGB565
 * image using simple neighbor-comparison edge detection. No lookup table,
 * cheap per-pixel, safe starting point for a Wii CPU budget.
 *
 * srcStride/dstStride are given in PIXELS (not bytes) and, like mGBA's own
 * framebuffer, are expected to carry the same 2-pixel row padding
 * WriteFrameToTextureMemory's gbPitch (= width*2+4 bytes, video.cpp)
 * already assumes - both this function's input and output keep that
 * padding so the existing swizzle path downstream doesn't need to know
 * anything changed.
 ***************************************************************************/
void Scale2xRGB565(const u16 *src, int srcStride, int width, int height, u16 *dst, int dstStride)
{
	for (int y = 0; y < height; y++)
	{
		const u16 *rowE = src + y * srcStride;
		const u16 *rowB = src + (y > 0 ? y - 1 : y) * srcStride;
		const u16 *rowH = src + (y < height - 1 ? y + 1 : y) * srcStride;

		u16 *dst0 = dst + (y * 2) * dstStride;
		u16 *dst1 = dst + (y * 2 + 1) * dstStride;

		for (int x = 0; x < width; x++)
		{
			u16 B = rowB[x];
			u16 D = rowE[x > 0 ? x - 1 : x];
			u16 E = rowE[x];
			u16 F = rowE[x < width - 1 ? x + 1 : x];
			u16 H = rowH[x];

			u16 E0 = E, E1 = E, E2 = E, E3 = E;
			if (B != H && D != F)
			{
				if (D == B) E0 = D;
				if (B == F) E1 = F;
				if (D == H) E2 = D;
				if (H == F) E3 = F;
			}

			dst0[x * 2]     = E0;
			dst0[x * 2 + 1] = E1;
			dst1[x * 2]     = E2;
			dst1[x * 2 + 1] = E3;
		}
	}
}

// Scratch buffer for Scale2x output before it gets swizzled into
// texturemem by WriteFrameToTextureMemory() (video.cpp). Sized for the
// largest supported case (GBA 240x160 doubled to 480x320), with the same
// 2-pixel row padding WriteFrameToTextureMemory expects: (240*2+2 pixels)
// * 2 bytes * (160*2) rows.
u8 scale2xBuffer[SCALE2X_BUF_SIZE] ATTRIBUTE_ALIGN(32);

/****************************************************************************
 * Sharp Bilinear filter (FILTER_SHARP_BILINEAR)
 *
 * See videofilters.h for the overall two-pass design and why the actual
 * GX draw calls stay in video.cpp. This half owns the scratch texture the
 * two passes hand off through, plus the pure size computation and the
 * texture-object (re)init - both ported straight out of video.cpp's old
 * RenderSharpBilinearPrescale(), just with the GX draw/copy calls left
 * behind for the caller.
 ***************************************************************************/
u8 sharpBilinearTexMem[SHARP_BILINEAR_MAX_W * SHARP_BILINEAR_MAX_H * 2] ATTRIBUTE_ALIGN(32);
GXTexObj sharpBilinearTexObj;
static int sharpBilinearTexW = -1, sharpBilinearTexH = -1;
static bool sharpBilinearTexInited = false;

bool ComputeSharpBilinearPrescaleSize(int borderWidth, int borderHeight,
                                       int onScreenW, int onScreenH,
                                       int efbMaxW, int efbMaxH,
                                       int *outW, int *outH)
{
	int factor = 1;
	if (borderWidth > 0 && borderHeight > 0 && onScreenW > 0 && onScreenH > 0)
	{
		int neededW = (onScreenW + borderWidth - 1) / borderWidth;
		int neededH = (onScreenH + borderHeight - 1) / borderHeight;
		factor = (neededW > neededH) ? neededW : neededH;
		if (factor < 1) factor = 1;
	}

	int prescaleW = borderWidth * factor;
	int prescaleH = borderHeight * factor;

	// Clamp to the EFB's actual bounds (240p mode has a much shorter
	// efbHeight than standard modes) and to the scratch buffer's fixed
	// capacity. Rounded down to a multiple of 4 to stay aligned with GX's
	// tiled texture format, same as every real game resolution this filter
	// runs on already naturally is.
	if (efbMaxW > 0 && prescaleW > efbMaxW) prescaleW = efbMaxW;
	if (efbMaxH > 0 && prescaleH > efbMaxH) prescaleH = efbMaxH;
	if (prescaleW > SHARP_BILINEAR_MAX_W) prescaleW = SHARP_BILINEAR_MAX_W;
	if (prescaleH > SHARP_BILINEAR_MAX_H) prescaleH = SHARP_BILINEAR_MAX_H;
	prescaleW &= ~3;
	prescaleH &= ~3;
	if (prescaleW <= 0 || prescaleH <= 0)
		return false; // degenerate - caller falls back to sampling the raw texture directly

	*outW = prescaleW;
	*outH = prescaleH;
	return true;
}

void EnsureSharpBilinearTexture(int prescaleW, int prescaleH)
{
	if (sharpBilinearTexInited && prescaleW == sharpBilinearTexW && prescaleH == sharpBilinearTexH)
		return;

	GX_InitTexObj(&sharpBilinearTexObj, sharpBilinearTexMem, prescaleW, prescaleH, GX_TF_RGB565, GX_CLAMP, GX_CLAMP, GX_FALSE);
	GX_InitTexObjFilterMode(&sharpBilinearTexObj, GX_LINEAR, GX_LINEAR); // this IS the bilinear step - always on for this texture
	sharpBilinearTexW = prescaleW;
	sharpBilinearTexH = prescaleH;
	sharpBilinearTexInited = true;
}
