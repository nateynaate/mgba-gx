/****************************************************************************
 * Visual Boy Advance GX
 *
 * Tantric 2008-2023
 * softdev 2007
 *
 * video.cpp
 *
 * Video routines
 ***************************************************************************/

#include <gccore.h>
#include <ogcsys.h>
#include <ogc/machine/processor.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "vbagx.h"
#include "menu.h"
#include "input.h"
#include "vbasupport.h"
#include "videofilters.h"

s32 CursorX, CursorY;
bool CursorVisible;
bool CursorValid;
bool TiltScreen = false;
float TiltAngle = 0;
u32 FrameTimer = 0;

/*** External 2D Video ***/
/*** 2D Video Globals ***/
GXRModeObj *vmode = NULL; // Graphics Mode Object
u32 *xfb[2] = { NULL, NULL }; // Framebuffers
int whichfb = 0; // Frame buffer toggle

static Mtx GXmodelView2D;

GameScreenPng gameScreenPng = { NULL, 0, 0, 0, 0, 0, 0, 0 };

int screenheight = 480;
int screenwidth = 640;

u16 *InitialBorder = NULL;
int InitialBorderWidth = 0;
int InitialBorderHeight = 0;
bool SGBBorderLoadedFromGame = false;

/*** 3D GX ***/
#define DEFAULT_FIFO_SIZE ( 256 * 1024 )
static u8 gp_fifo[DEFAULT_FIFO_SIZE] ATTRIBUTE_ALIGN(32);
static volatile unsigned int copynow = GX_FALSE;

/*** Texture memory ***/
#define TEX_WIDTH 640
#define TEX_HEIGHT 480
#define TEXTUREMEM_SIZE 	TEX_WIDTH*TEX_HEIGHT*2
static u8 texturemem[TEXTUREMEM_SIZE] ATTRIBUTE_ALIGN (32);

// Scanline filter (FILTER_SCANLINES) - GX-native tiled-texture-multiply
// technique, actual data/texture setup now lives in videofilters.cpp/h
// (EnsureScanlineTexture(), scanlineTexObj) - see there for details. Used
// below in configure_tev_pipeline().

static GXTexObj texobj;
static Mtx view;
static int vwidth, vheight;

// Round-half-away-from-zero to int, used below wherever a computed EFB
// position/size gets stored into an int (realVX/Y/W/H, liveVX/Y/W/H) -
// plain (int) truncation always rounds toward zero, so a value like 78.5
// silently became 78 instead of the nearer 79. GX_SetViewport() itself
// still gets the precise float either way (this only affects the stored
// copies), but TakeScreenshot() and the pause-screen blur compositor
// (menu.cpp's BuildBlurredPauseScreen()) both position themselves off
// these stored ints, so the lost half-pixel was a real, if small, source
// of misalignment in both.
static inline int RoundToInt(float x)
{
	return (x >= 0.0f) ? (int)(x + 0.5f) : (int)(x - 0.5f);
}
static int updateScaling;

// Sharp Bilinear filter (FILTER_SHARP_BILINEAR - see vbagx.h): a nearest-
// neighbor integer prescale of the raw game texture, rendered to a scratch
// texture, which is then sampled with real bilinear filtering for the
// final on-screen quad in GX_Render() below. The scratch texture
// (sharpBilinearTexMem/sharpBilinearTexObj) and the prescale-size math now
// live in videofilters.cpp/h - see RenderSharpBilinearPrescale() below for
// the actual two-pass draw implementation and why this needs two passes
// instead of one shader (GX has no programmable shader stage to do it in
// one, unlike RetroArch's sharp-bilinear.slangp).
//
// The prescale factor is computed dynamically, per-frame, from the real
// final on-screen size (liveVW/liveVH) instead of a fixed 2x - a fixed
// factor left almost nothing for the second (visible, bilinear) pass to
// do whenever the actual output scale was already close to it, which is
// exactly why this was reported as looking no different from plain
// filtering, especially for GB/GBC's smaller 160x144 source where a CRT's
// full output scale is well past 2x. Not used for SGB-bordered content,
// which this filter is intentionally scoped off for (same restriction
// Scale2x already has, for the same reason: correctly compositing a
// prescaled game image into a non-prescaled border isn't handled here).

// Actual on-screen rectangle the game quad is drawn into (EFB pixel space,
// including any TV/scanout-only stretch such as the 240p width doubling or
// widescreen pillarbox correction). Used to restore normal rendering after a
// screenshot capture.
static int liveVX = 0, liveVY = 0, liveVW = 0, liveVH = 0;
// The GX viewport rectangle actually passed to GX_SetViewport() for normal
// (non-capture) gameplay rendering this frame. NOT the same thing as
// liveVX/Y/W/H above: in non-fixed (stretch) scaling mode the real viewport
// is always the full screen (the quad's on-screen shape instead comes from
// the square[] vertex positions), while liveV* there is a derived, already
// cropped-to-the-quad rectangle meant only for the blur/screenshot code.
// Anything that needs to temporarily borrow the viewport (e.g.
// RenderSharpBilinearPrescale's off-screen prescale pass) and restore it
// afterward must restore THIS, not liveV*, or it ends up compositing
// through an already-cropped viewport and effectively double-applies the
// aspect correction.
static int realVX = 0, realVY = 0, realVW = 0, realVH = 0;
// The game's true, undistorted content size (native resolution x zoom, with
// no TV-only stretches applied). Used only for cropping screenshots so the
// stored PNG has correct square-pixel aspect ratio.
static int gameVW = 0, gameVH = 0;
// UV sub-rectangle of the texture holding just the actual game viewport,
// excluding any SGB border area. (0,0)-(1,1) when there's no border.
static float gameU0 = 0, gameV0 = 0, gameU1 = 1, gameV1 = 1;
// Tracks what dimensions texobj was last actually initialized with, so
// GX_Render() can detect a mismatch (e.g. stale dims from ResetVideo_Emu())
// and reinitialize it - see the fix in GX_Render() below.
static int texObjWidth = -1, texObjHeight = -1;
bool progressive = false;

/* New texture based scaler */
typedef struct tagcamera
  {
    guVector pos;
    guVector up;
    guVector view;
  }
camera;

/*** Square Matrix
     This structure controls the size of the image on the screen.
***/
static s16 square[] ATTRIBUTE_ALIGN(32) = {
	/*
	* X,   Y,  Z
	* Values set are for roughly 4:3 aspect
	*/
	-200,  200, 0,	// 0
	 200,  200, 0,	// 1
	 200, -200, 0,	// 2
	-200, -200, 0	// 3
    };

static camera cam = { {0.0F, 0.0F, 0.0F},
                      {0.0F, 0.5F, 0.0F},
                      {0.0F, 0.0F, -0.5F}
                    };

/****************************************************************************
 * VideoThreading
 ***************************************************************************/
static lwp_t vbthread = LWP_THREAD_NULL;
static lwpq_t render_queue;          // Queue for the main thread to sleep on
static lwpq_t vb_queue;              // Queue for the VSync thread to sleep on
static volatile bool vb_done = true; // Tracks if the VSync thread has completed its wait

/****************************************************************************
 * vbgetback
 *
 * This callback enables the emulator to keep running while waiting for a
 * vertical blank.
 ***************************************************************************/
static void *
vbgetback (void *arg)
{
	while (1)
	{
		LWP_ThreadSleep(vb_queue);     // Sleep until kicked off at the end of GX_Render
		VIDEO_WaitVSync ();	         /**< Wait for video vertical blank */
		vb_done = true;
		LWP_ThreadSignal(render_queue); // Instantly alert the main thread if it is waiting
	}
	return NULL;
}

/****************************************************************************
 * copy_to_xfb
 *
 * Stock code to copy the GX buffer to the current display mode.
 * Also increments the frameticker, as it's called for each vb.
 ***************************************************************************/
static inline void
copy_to_xfb (u32 arg)
{
	if (copynow == GX_TRUE)
	{
		GX_CopyDisp (xfb[whichfb], GX_TRUE);
		GX_Flush ();
		copynow = GX_FALSE;
		LWP_ThreadSignal(render_queue); // Wake up the main thread if it is waiting for the copy
	}
	++FrameTimer;
}

/****************************************************************************
 * Scaler Support Functions
 ****************************************************************************/
// True for any filter that uses the shared 2-stage "multiply the game
// texture by a small repeating mask texture" TEV setup below (scanlines,
// LCD grid, LCD RGB) - as opposed to filters handled entirely outside
// configure_tev_pipeline() (Scale2x, Sharp Bilinear) or no filter at all.
static inline bool IsMaskFilter(int filterMethod)
{
	return filterMethod == FILTER_SCANLINES ||
	       filterMethod == FILTER_LCD_GRID ||
	       filterMethod == FILTER_LCD_RGB;
}

// Each mask filter's tile size in texels, needed both to load the right
// texobj here and to compute the right GX_REPEAT count in draw_square()
// below - scanlines/LCD grid are 8x4 I8 tiles, LCD RGB is a 4x4 RGB565
// tile (see videofilters.cpp for why each is sized the way it is).
static inline void GetMaskFilterTileSize(int filterMethod, int *tileW, int *tileH)
{
	if (filterMethod == FILTER_LCD_RGB) {
		*tileW = 4; *tileH = 4;
	} else {
		*tileW = 8; *tileH = 4;
	}
}

// Switches between our normal single-stage "just show the game texture"
// TEV setup and a 2-stage setup that multiplies it against a mask
// texture (scanlines, LCD grid, or LCD RGB - see IsMaskFilter() above).
// Called both from draw_init() (mode/scale changes) and at the top of
// every GX_Render() call, since draw_cursor() reconfigures TEV/texgen
// state for its own draw and this must be reasserted before the next
// frame's game quad regardless of which path was active last.
static inline void configure_tev_pipeline()
{
	if (IsMaskFilter(GCSettings.FilterMethod)) {
		if (GCSettings.FilterMethod == FILTER_SCANLINES) {
			EnsureScanlineTexture();
			GX_LoadTexObj(&scanlineTexObj, GX_TEXMAP1);
		} else if (GCSettings.FilterMethod == FILTER_LCD_GRID) {
			EnsureLCDGridTexture();
			GX_LoadTexObj(&lcdGridTexObj, GX_TEXMAP1);
		} else {
			EnsureLCDRGBTexture();
			GX_LoadTexObj(&lcdRgbTexObj, GX_TEXMAP1);
		}

		// Second texcoord stream, direct (not indexed like GX_VA_POS) so
		// draw_square() can emit per-vertex mask-texture UVs independent of
		// the indexed square[] position array.
		GX_SetVtxDesc(GX_VA_TEX1, GX_DIRECT);
		GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX1, GX_TEX_ST, GX_F32, 0);

		GX_SetNumTexGens(2);
		GX_SetNumTevStages(2);
		GX_SetNumChans(0);

		GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
		GX_SetTexCoordGen(GX_TEXCOORD1, GX_TG_MTX2x4, GX_TG_TEX1, GX_IDENTITY);

		// Stage 0: sample the game screen as-is.
		GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLORNULL);
		GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_TEXC);
		GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
		GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_TEXA);
		GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

		// Stage 1: multiply stage 0's result by the mask texture. Works
		// identically whether the mask is a monochrome intensity texture
		// (scanlines/LCD grid, GX_TF_I8 - broadcasts to R=G=B) or a real
		// color one (LCD RGB, GX_TF_RGB565) - GX_CC_TEXC is just a
		// per-channel multiply either way.
		GX_SetTevOrder(GX_TEVSTAGE1, GX_TEXCOORD1, GX_TEXMAP1, GX_COLORNULL);
		GX_SetTevColorIn(GX_TEVSTAGE1, GX_CC_ZERO, GX_CC_CPREV, GX_CC_TEXC, GX_CC_ZERO);
		GX_SetTevColorOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
		GX_SetTevAlphaIn(GX_TEVSTAGE1, GX_CA_ZERO, GX_CA_APREV, GX_CA_TEXA, GX_CA_ZERO);
		GX_SetTevAlphaOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	} else {
		// Restore our normal single-stage setup and make sure TEX1 is fully
		// disabled - if it's left GX_DIRECT with only 1 texgen configured,
		// GX's vertex format and texgen counts disagree and rendering breaks.
		GX_SetVtxDesc(GX_VA_TEX1, GX_NONE);

		GX_SetNumTexGens(1);
		GX_SetNumTevStages(1);
		GX_SetNumChans(0);

		GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
		GX_SetTevOp(GX_TEVSTAGE0, GX_REPLACE);
		GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLORNULL);
	}
}

static inline void draw_init(void)
{
	GX_ClearVtxDesc ();
	GX_SetVtxDesc (GX_VA_POS, GX_INDEX8);
	GX_SetVtxDesc (GX_VA_TEX0, GX_DIRECT);

	GX_SetVtxAttrFmt (GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_S16, 0);
	GX_SetVtxAttrFmt (GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);

	GX_SetArray (GX_VA_POS, square, 3 * sizeof (s16));

	configure_tev_pipeline();

	memset (&view, 0, sizeof (Mtx));
	guLookAt(view, &cam.pos, &cam.up, &cam.view);
	GX_LoadPosMtxImm (view, GX_PNMTX0);

	GX_InvVtxCache ();	// update vertex cache

	GX_InitTexObj(&texobj, texturemem, vwidth, vheight, GX_TF_RGB565, GX_CLAMP, GX_CLAMP, GX_FALSE);

	if (GCSettings.render == RENDER_UNFILTERED)
		GX_InitTexObjFilterMode(&texobj,GX_NEAR,GX_NEAR); // original/unfiltered video mode: force texture filtering OFF
}

static inline void draw_vert(u8 pos, f32 s, f32 t)
{
	GX_Position1x8(pos);
	GX_TexCoord2f32(s, t);
}

// Same as draw_vert, but also emits the second (TEX1) texcoord the
// scanline texture needs. su/sv are in scanline-texture-tile units (i.e.
// "how many 8x4 tiles across/down"), not 0..1 - GX_REPEAT wrapping on the
// scanline texobj (see EnsureScanlineTexture, videofilters.cpp) tiles it
// across that range.
static inline void draw_vert_scanline(u8 pos, f32 s, f32 t, f32 su, f32 sv)
{
	GX_Position1x8(pos);
	GX_TexCoord2f32(s, t);
	GX_TexCoord2f32(su, sv);
}

static inline void draw_square(Mtx v)
{
	Mtx m;			// model matrix.
	Mtx mv;			// modelview matrix.

	if (TiltScreen)
	{
		guMtxRotDeg(m, 'z', -TiltAngle);
		guMtxScaleApply(m, m, 0.8, 0.8, 1);
	}
	else
	{
		guMtxIdentity(m);
	}

	guMtxTransApply(m, m, 0, 0, -100);
	guMtxConcat(v, m, mv);

	GX_LoadPosMtxImm(mv, GX_PNMTX0);
	GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
	if (IsMaskFilter(GCSettings.FilterMethod)) {
		// Repeat counts sized so each mask-texture tile roughly tracks one
		// real on-screen pixel cell - liveVW/liveVH (set in UpdateScaling())
		// is the actual EFB pixel size of the quad, already accounting for
		// fixed/stretch/240p mode, so this doesn't need to re-derive the
		// object-space-to-screen transform itself. Tile size varies by
		// filter (GetMaskFilterTileSize() above), so the divisor isn't a
		// fixed 8x4 anymore.
		int tileW, tileH;
		GetMaskFilterTileSize(GCSettings.FilterMethod, &tileW, &tileH);
		float repeatX = (liveVW > 0) ? (liveVW / (float)tileW) : 1.0f;
		float repeatY = (liveVH > 0) ? (liveVH / (float)tileH) : 1.0f;
		draw_vert_scanline(0, 0.0, 0.0, 0.0f,    0.0f);
		draw_vert_scanline(1, 1.0, 0.0, repeatX, 0.0f);
		draw_vert_scanline(2, 1.0, 1.0, repeatX, repeatY);
		draw_vert_scanline(3, 0.0, 1.0, 0.0f,    repeatY);
	} else {
		draw_vert(0, 0.0, 0.0);
		draw_vert(1, 1.0, 0.0);
		draw_vert(2, 1.0, 1.0);
		draw_vert(3, 0.0, 1.0);
	}
	GX_End();
}

// Same quad geometry as draw_square() would use in "fixed" mode, but with a
// restricted UV rectangle, and ALWAYS spanning the full -320..320/-240..240
// clip range via direct position coordinates - not the indexed square[]
// array, whose extent varies by mode (in "Disabled"/non-fixed mode it's
// only a fraction of the full range, proportional to the on-screen aspect
// fit, which previously caused the capture quad to only fill part of the
// screenshot viewport instead of all of it). Used only for screenshot
// capture: lets us crop out an SGB border by sampling only the inner
// sub-rectangle of the texture that holds the actual game viewport, while
// reliably filling whatever (smaller) viewport is set for the capture.
static inline void draw_square_cropped(Mtx v, float u0, float v0, float u1, float v1)
{
	Mtx m;
	Mtx mv;

	guMtxIdentity(m);
	guMtxTransApply(m, m, 0, 0, -100);
	guMtxConcat(v, m, mv);

	GX_LoadPosMtxImm(mv, GX_PNMTX0);

	// Temporarily switch to direct F32 positions - independent of whatever
	// extent the indexed square[] array currently has - then restore the
	// indexed S16 setup the rest of the rendering pipeline expects.
	GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);

	GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
	GX_Position3f32(-320,  240, 0); GX_TexCoord2f32(u0, v0);
	GX_Position3f32( 320,  240, 0); GX_TexCoord2f32(u1, v0);
	GX_Position3f32( 320, -240, 0); GX_TexCoord2f32(u1, v1);
	GX_Position3f32(-320, -240, 0); GX_TexCoord2f32(u0, v1);
	GX_End();

	GX_SetVtxDesc(GX_VA_POS, GX_INDEX8);
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_S16, 0);
}

/****************************************************************************
 * RenderSharpBilinearPrescale
 *
 * Pass 1 of the Sharp Bilinear filter (see sharpBilinearTexMem's own
 * comment above for the overall design). Point-samples texobj (the raw,
 * small game texture already loaded into texturemem by the caller) into an
 * offscreen corner of the EFB at borderWidth*SHARP_BILINEAR_PRESCALE x
 * borderHeight*SHARP_BILINEAR_PRESCALE, then copies that region into
 * sharpBilinearTexObj as a real texture via GX_CopyTex - the standard GX
 * render-to-texture technique. Nothing here is a programmable shader; it's
 * two ordinary textured draws using GX's fixed-function GX_NEAR/GX_LINEAR
 * texture filter modes, which is all "sharp bilinear" actually is under
 * the hood - RetroArch's sharp-bilinear.slangp (Wii U only, since original
 * Wii's GX has no shader stage) just does the same two steps as one
 * programmable pass instead of two fixed-function ones.
 *
 * Caller (GX_Render(), below) is responsible for loading sharpBilinearTexObj
 * into GX_TEXMAP0 afterward and issuing its own draw_square() call for the
 * final on-screen quad - this function only handles the prescale pass
 * itself and leaves the viewport restored to the live on-screen one
 * (liveVX/Y/W/H) so that follow-up call needs no viewport setup of its own.
 ***************************************************************************/
static void RenderSharpBilinearPrescale(int borderWidth, int borderHeight)
{
	// Dynamic prescale factor: the smallest integer scale that gets the
	// prescaled texture to at least the real final on-screen size in both
	// dimensions. That guarantees the second, visible bilinear stretch in
	// the normal draw_square() call afterward always has real room to
	// smooth pixel edges, instead of being left with almost nothing to do
	// whenever the real output scale happened to already sit close to a
	// fixed factor - which on a CRT (full analog TV resolution, no
	// integer-scale preference) is the common case, especially for GB/GBC's
	// smaller 160x144 source.
	//
	// Deliberately liveVW/liveVH here, NOT realVW/realVH: in non-fixed
	// (stretch) scaling modes, UpdateScaling() leaves realVW/realVH as the
	// full GX viewport (640x480) - the viewport itself is always
	// full-screen in that mode, with the actual quad size/position handled
	// separately by the vertex transform (see UpdateScaling()'s own
	// comment on this). Using realVW/realVH here was computing a factor
	// against the full screen size instead of the real (usually smaller)
	// drawn quad, over-prescaling the texture - which then made GX_LINEAR
	// minify it back down without a mip chain to reach the real quad size,
	// aliasing just as badly as plain nearest would have and making the
	// filter look like it wasn't doing anything at all. liveVW/liveVH
	// tracks the real drawn quad's pixel size in every scaling mode.
	int prescaleW, prescaleH;
	if (!ComputeSharpBilinearPrescaleSize(borderWidth, borderHeight, liveVW, liveVH,
	                                       vmode->fbWidth, vmode->efbHeight,
	                                       &prescaleW, &prescaleH))
		return; // degenerate - bail out, caller falls back to sampling texobj directly

	EnsureSharpBilinearTexture(prescaleW, prescaleH);

	// Pass 1: point-sample the raw game texture into the top-left corner of
	// the EFB at the prescaled integer size, regardless of what
	// GCSettings.render's Unfiltered/Filtered Sharp/Filtered Soft setting
	// would otherwise leave texobj's filter mode at - Sharp Bilinear always
	// wants a true point-sampled prescale here, that's the whole technique.
	GX_InitTexObjFilterMode(&texobj, GX_NEAR, GX_NEAR);
	GX_LoadTexObj(&texobj, GX_TEXMAP0);
	GX_SetViewport(0, 0, prescaleW, prescaleH, 0, 1);
	draw_square_cropped(view, 0.0f, 0.0f, 1.0f, 1.0f);

	GX_SetTexCopySrc(0, 0, prescaleW, prescaleH);
	GX_SetTexCopyDst(prescaleW, prescaleH, GX_TF_RGB565, GX_FALSE);
	// GX_TRUE also clears the scratch EFB region just read, as part of the
	// same copy - so the caller's own draw_square() call right after this
	// doesn't need to fully overdraw this corner itself to avoid leaving a
	// stray patch of pass-1 pixels visible in this frame's output.
	GX_CopyTex(sharpBilinearTexMem, GX_TRUE);

	// GX_CopyTex() kicks off the EFB->main-memory copy asynchronously on
	// real hardware - it does not block until the copy engine has actually
	// finished writing sharpBilinearTexMem. Dolphin performs the copy
	// synchronously in its own emulation of this call, so it never exposes
	// the hazard, but on a real Wii, the caller's very next steps
	// (GX_InvalidateTexAll() + loading sharpBilinearTexObj into TEXMAP0 for
	// this same frame's draw_square() call in GX_Render()) can then race
	// the copy engine and sample a partially-written or stale previous
	// frame's texture - a real, silent race, not a crash, so it can look
	// like "the filter does nothing" rather than an obvious glitch.
	// GX_PixModeSync() blocks until the copy completes, which is exactly
	// what's needed here since the copy's own destination is read again
	// within the same frame. This is a no-op on Dolphin (the copy is
	// already done by the time this runs) so it's safe there too.
	GX_PixModeSync();

	GX_InvalidateTexAll(); // sharpBilinearTexObj's data just changed on the GPU side - drop any stale cached copy before it's sampled below

	// Restore the real on-screen viewport (computed by UpdateScaling(),
	// tracked in realVX/Y/W/H) for the caller's normal draw_square() call.
	//
	// This must be realV*, NOT liveV*: liveV* is a derived pixel-space
	// rectangle for the blur/screenshot code and only coincides with the
	// actual applied viewport in fixed-ratio mode. In non-fixed (stretch)
	// scaling mode the real viewport is always full-screen - the quad's
	// on-screen shape comes from the square[] vertex positions instead -
	// so restoring liveV* there left draw_square() mapping those same
	// vertex positions through an already-cropped viewport, effectively
	// applying the aspect correction twice. That was harmless-looking for
	// GBA (aspect ratio close enough to 4:3 that the double-scaling was
	// subtle) but produced an obvious vertical squish for GB/GBC, whose
	// ~1.11:1 aspect is much further off.
	GX_SetViewport(realVX, realVY, realVW, realVH, 0, 1);
}

#ifdef HW_RVL
static inline void draw_cursor(Mtx v)
{
	if (!CursorVisible || !CursorValid)
		return;

	GX_InitTexObj(&texobj, pointer[0]->GetImage(), 96, 96, GX_TF_RGBA8,GX_CLAMP, GX_CLAMP,GX_FALSE);
	GX_LoadTexObj(&texobj, GX_TEXMAP0);
	GX_SetBlendMode(GX_BM_BLEND,GX_BL_DSTALPHA,GX_BL_INVSRCALPHA,GX_LO_CLEAR);
	GX_SetTevOp (GX_TEVSTAGE0, GX_REPLACE);
	GX_SetVtxDesc (GX_VA_TEX0, GX_DIRECT);

	Mtx m;			// model matrix.

	guMtxIdentity(m);
	guMtxScaleApply(m, m, 0.070f, 0.10f, 0.06f);
	// I needed to hack this position
	guMtxTransApply(m, m, CursorX-315, 220-CursorY, -100);

	GX_LoadPosMtxImm(m, GX_PNMTX0);
	GX_Begin(GX_QUADS, GX_VTXFMT0, 4);

	// I needed to hack the texture coords to cut out the opaque bit around the outside
	draw_vert(0, 0.4, 0.45);
	draw_vert(1, 0.76, 0.45);
	draw_vert(2, 0.76, 0.97);
	draw_vert(3, 0.4, 0.97);

	GX_End();

	GX_ClearVtxDesc ();
	GX_SetVtxDesc (GX_VA_POS, GX_INDEX8);
	GX_SetVtxDesc (GX_VA_TEX0, GX_DIRECT);

	GX_SetVtxAttrFmt (GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_S16, 0);
	GX_SetVtxAttrFmt (GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);

	GX_SetArray (GX_VA_POS, square, 3 * sizeof (s16));

	GX_SetNumTexGens (1);
	GX_SetTexCoordGen (GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);

	GX_InvVtxCache ();	// update vertex cache

	GX_InitTexObj(&texobj, texturemem, vwidth, vheight, GX_TF_RGB565, GX_CLAMP, GX_CLAMP, GX_FALSE);

	if (GCSettings.render == RENDER_UNFILTERED)
		GX_InitTexObjFilterMode(&texobj,GX_NEAR,GX_NEAR); // original/unfiltered video mode: force texture filtering OFF
}
#endif

/****************************************************************************
 * StopGX
 *
 * Stops GX (when exiting)
 ***************************************************************************/
void StopGX()
{
	GX_AbortFrame();
	GX_Flush();

	VIDEO_SetBlack(true);
	VIDEO_Flush();
}

/****************************************************************************
 * FindVideoMode
 *
 * Finds the optimal video mode, or uses the user-specified one
 * Also configures original video modes
 ***************************************************************************/
static GXRModeObj * FindVideoMode(bool forceAuto = false)
{
	GXRModeObj * mode;
	
	// choose the desired video mode
	switch(forceAuto ? VIDEOMODE_AUTO : GCSettings.videomode)
	{
		case VIDEOMODE_NTSC: // NTSC (480i)
			mode = &TVNtsc480IntDf;
			break;
		case VIDEOMODE_PROGRESSIVE: // Progressive (480p)
			mode = &TVNtsc480Prog;
			break;
		case VIDEOMODE_PAL: // PAL (50Hz)
			mode = &TVPal576IntDfScale;
			break;
		case VIDEOMODE_EURGB: // PAL (60Hz)
			mode = &TVEurgb60Hz480IntDf;
			break;
		case VIDEOMODE_240P: // NTSC (240p)
			mode = &TVNtsc240Ds;
			break;
		case VIDEOMODE_EURGB_240P: // PAL (60Hz 240p)
			mode = &TVEurgb60Hz240Ds;
			break;
		default:
			mode = VIDEO_GetPreferredMode(NULL);
			break;
	}

	// check for progressive scan
	if ((mode->viTVMode & 3) == VI_PROGRESSIVE)
		progressive = true;
	else
		progressive = false;

	#ifdef HW_RVL
	if (CONF_GetAspectRatio() == CONF_ASPECT_16_9)
		mode->viWidth = 678;
	else
		mode->viWidth = 672;

    if ((mode->viTVMode >> 2) == VI_PAL)
	{
		mode->viXOrigin = (VI_MAX_WIDTH_PAL - mode->viWidth) / 2;
		mode->viYOrigin = (VI_MAX_HEIGHT_PAL - mode->viHeight) / 2;
	}
	else
	{
		mode->viXOrigin = (VI_MAX_WIDTH_NTSC - mode->viWidth) / 2;
		mode->viYOrigin = (VI_MAX_HEIGHT_NTSC - mode->viHeight) / 2;
	}
	#endif
	return mode;
}

/****************************************************************************
 * SetupVideoMode
 *
 * Sets up the given video mode
 ***************************************************************************/
static void SetupVideoMode(GXRModeObj * mode)
{
	if(vmode == mode)
		return;
	
	VIDEO_SetPostRetraceCallback (NULL);
	copynow = GX_FALSE;
	VIDEO_Configure (mode);
	VIDEO_Flush();

	// Clear framebuffers etc.
	VIDEO_ClearFrameBuffer (mode, xfb[0], COLOR_BLACK);
	VIDEO_ClearFrameBuffer (mode, xfb[1], COLOR_BLACK);
	VIDEO_SetNextFramebuffer (xfb[0]);

	VIDEO_SetBlack (false);
	VIDEO_Flush ();
	VIDEO_Flush ();
	
	VIDEO_SetPostRetraceCallback ((VIRetraceCallback)copy_to_xfb);
	vmode = mode;
}

/****************************************************************************
 * GetCurrentTVFrameRate
 *
 * Returns the display refresh rate (in Hz) that mgba_emuMain()'s VSync loop
 * is actually pacing to right now, for the currently active `vmode` (set by
 * SetupVideoMode() just above). vbasupport.cpp's audio code
 * (InitMGBAAudio()) uses this to seed its resampler rate-correction ratio
 * with the same clockRate/(desiredFrameRate*frameCycles) formula mGBA's own
 * frontends use (mCoreCalculateFramerateRatio), instead of starting from a
 * flat 1.0 guess and drifting into place over the first few seconds of
 * audio via wall-clock measurement alone.
 *
 * True PAL (50Hz field rate) is the only mode on real Wii hardware whose
 * refresh meaningfully differs from ~59.94Hz - NTSC, EURGB60/PAL60, and
 * the 240p variants of both all retrace at the same ~60/1.001 rate as
 * NTSC. This mirrors the exact (mode->viTVMode >> 2) == VI_PAL check
 * FindVideoMode() above already uses for VI positioning, so it can never
 * disagree with what SetupVideoMode() actually configured.
 ***************************************************************************/
double GetCurrentTVFrameRate()
{
	if (vmode && ((vmode->viTVMode >> 2) == VI_PAL))
		return 50.0;
	return 60.0 / 1.001;
}

/****************************************************************************
 * InitializeVideo
 *
 * This function MUST be called at startup.
 * - also sets up menu video mode
 ***************************************************************************/

void
InitializeVideo ()
{
	VIDEO_Init();

	// Allocate the video buffers. Sized for the largest supported mode
	// (640x576, 2 bytes per pixel) so the same buffers serve every mode.
	const u32 xfbSize = 640 * 576 * 2;
	xfb[0] = (u32 *) memalign(32, xfbSize);
	xfb[1] = (u32 *) memalign(32, xfbSize);
	DCInvalidateRange(xfb[0], xfbSize);
	DCInvalidateRange(xfb[1], xfbSize);
	xfb[0] = (u32 *) MEM_K0_TO_K1 (xfb[0]);
	xfb[1] = (u32 *) MEM_K0_TO_K1 (xfb[1]);

	GXRModeObj *rmode = FindVideoMode();
	SetupVideoMode(rmode);

	// Setup synchronization queues
	LWP_InitQueue(&render_queue);
	LWP_InitQueue(&vb_queue);
	vb_done = true;

	LWP_CreateThread (&vbthread, vbgetback, NULL, NULL, 0, 68);

	// Initialise GX
	GXColor background = { 0, 0, 0, 0xff };
	memset (gp_fifo, 0, DEFAULT_FIFO_SIZE);
	GX_Init (&gp_fifo, DEFAULT_FIFO_SIZE);
	GX_SetCopyClear (background, GX_MAX_Z24);
	GX_SetDispCopyGamma (GX_GM_1_0);
	GX_SetCullMode (GX_CULL_NONE);
}

static inline void UpdateScaling()
{
	int xscale;
	int yscale;

	float TvAspectRatio;
	float GameboyAspectRatio;
	float MaxStretchRatio = 1.6f;

	if (GCSettings.scaling == SCALING_PARTIAL_STRETCH)
		MaxStretchRatio = 1.3f;
	else if (GCSettings.scaling == SCALING_STRETCH_TO_FIT)
		MaxStretchRatio = 1.6f;
	else
		MaxStretchRatio = 1.0f;

	#ifdef HW_RVL
	if (CONF_GetAspectRatio() == CONF_ASPECT_16_9)
		TvAspectRatio = 16.0f/9.0f;
	else
		TvAspectRatio = 4.0f/3.0f;
	#else
	if (GCSettings.scaling == SCALING_WIDESCREEN_CORRECTION)
		TvAspectRatio = 16.0f/9.0f;
	else
		TvAspectRatio = 4.0f/3.0f;
	#endif

	GameboyAspectRatio = ((vwidth * 1.0) / vheight);

	if (TvAspectRatio>GameboyAspectRatio)
	{
		yscale = 240; // half of TV resolution 640x480
		float StretchRatio = TvAspectRatio/GameboyAspectRatio;
		if (StretchRatio > MaxStretchRatio)
			StretchRatio = MaxStretchRatio;
		xscale = 240.0f*GameboyAspectRatio*StretchRatio * ((4.0f/3.0f)/TvAspectRatio);
	}
	else
	{
		xscale = 320; // half of TV resolution 640x480
		float StretchRatio = GameboyAspectRatio/TvAspectRatio;
		if (StretchRatio > MaxStretchRatio)
			StretchRatio = MaxStretchRatio;
		yscale = 320.0f/GameboyAspectRatio*StretchRatio / ((4.0f/3.0f)/TvAspectRatio);
	}

	// change zoom
	float zoomHor, zoomVert;
	int fixed;
	if (cartridgeType == CARTRIDGE_GBA)
	{
		zoomHor = GCSettings.gbaZoomHor;
		zoomVert = GCSettings.gbaZoomVert;
		fixed = GCSettings.gbaFixed;
	}
	else
	{
		zoomHor = GCSettings.gbZoomHor;
		zoomVert = GCSettings.gbZoomVert;
		fixed = GCSettings.gbFixed;
	}

	if (fixed) {
		xscale = 320;
		yscale = 240;
	} else {
		xscale *= zoomHor;
		yscale *= zoomVert;
	}
	
	#ifdef HW_RVL
	if (fixed && CONF_GetAspectRatio() == CONF_ASPECT_16_9 && (*(u32*)(0xCD8005A0) >> 16) == 0xCAFE) // Wii U
	{
		/* vWii widescreen patch by tueidj */
		write32(0xd8006a0, fixed ? 0x30000002 : 0x30000004), mask32(0xd8006a8, 0, 2);
	}
	#endif

	// Set new aspect
	square[0] = square[9]  = -xscale + GCSettings.xshift;
	square[3] = square[6]  =  xscale + GCSettings.xshift;
	square[1] = square[4]  =  yscale - GCSettings.yshift;
	square[7] = square[10] = -yscale - GCSettings.yshift;
	DCFlushRange (square, 32); // update memory BEFORE the GPU accesses it!

	draw_init ();

	memset(&view, 0, sizeof(Mtx));
	guLookAt(view, &cam.pos, &cam.up, &cam.view);

	// vwidth/vheight reflect whatever the core is actually outputting each
	// frame, which includes any SGB border area (e.g. 256x224). gGbNativeW/H
	// is the true console resolution (160x144 GB/GBC, 240x160 GBA) regardless
	// of any border, exported by vbasupport.cpp. Used below to size and crop
	// screenshots to just the actual game viewport.
	extern int gGbNativeW, gGbNativeH;
	int nativeW = gGbNativeW > 0 ? gGbNativeW : vwidth;
	int nativeH = gGbNativeH > 0 ? gGbNativeH : vheight;

	float marginU = (vwidth > nativeW) ? ((float)(vwidth - nativeW) / 2.0f / vwidth) : 0.0f;
	float marginV = (vheight > nativeH) ? ((float)(vheight - nativeH) / 2.0f / vheight) : 0.0f;
	gameU0 = marginU; gameU1 = 1.0f - marginU;
	gameV0 = marginV; gameV1 = 1.0f - marginV;

	if (fixed) {
		int ratio = fixed % 10;
		bool widescreen = fixed / 10;
	
		float vw = vwidth * ratio;
		if (widescreen) vw /= 4.0 / 3.0;
		float vh = vheight * ratio;

		// Captured as separate variables from vw/vh so the two can still
		// diverge for other reasons (widescreen ratio, clamping) below,
		// but the 240p adjustment right after this now applies to both
		// together - see that block's comment for why.
		float rawVw = vw, rawVh = vh;
		
		// 240p adjustment - applies to BOTH the live viewport (vw, for
		// GX_SetViewport) AND the raw/fraction capture (rawVw, for
		// liveVW/viewW - see the fraction computation further down). These
		// two must move together: the HEIGHT half (efbHeight=240 vs 480)
		// already gets an automatic 2x correction from the fraction math
		// on its own (dividing by the smaller modeH=240 here, then later
		// multiplying by the menu's larger screenheight=480 to composite),
		// but there's no equivalent automatic correction for WIDTH, since
		// fbWidth is IDENTICAL (640) between 240p and standard modes - so
		// excluding this doubling from rawVw (as a previous fix reasoned)
		// left width uncorrected (1:1) while height got corrected (2:1),
		// producing exactly the tall/squished distortion this fixes.
		if (GCSettings.videomode == VIDEOMODE_240P || GCSettings.videomode == VIDEOMODE_EURGB_240P) {
			vw *= 2;
			rawVw *= 2;
		}
		
		float vx = (vmode->fbWidth - vw) / 2;
		float vy = (vmode->efbHeight - vh) / 2;

		// Clamp to screen bounds — a small texture (e.g. GB 160x144 at 3x = 432 tall)
		// can produce a negative vy that clips the top of the image off-screen.
		if (vx < 0) vx = 0;
		if (vy < 0) vy = 0;
		// Also clamp the rendered size so it doesn't exceed the EFB
		if (vw > vmode->fbWidth)  vw = vmode->fbWidth;
		if (vh > vmode->efbHeight) vh = vmode->efbHeight;

		GX_SetViewport(vx, vy, vw, vh, 0, 1);
		realVX = RoundToInt(vx); realVY = RoundToInt(vy); realVW = RoundToInt(vw); realVH = RoundToInt(vh);

		// Same centering math, but against the RAW (pre-240p-hack) size,
		// for the reasons above.
		float rawVx = (vmode->fbWidth - rawVw) / 2;
		float rawVy = (vmode->efbHeight - rawVh) / 2;
		if (rawVx < 0) rawVx = 0;
		if (rawVy < 0) rawVy = 0;
		if (rawVw > vmode->fbWidth)  rawVw = vmode->fbWidth;
		if (rawVh > vmode->efbHeight) rawVh = vmode->efbHeight;

		liveVX = RoundToInt(rawVx); liveVY = RoundToInt(rawVy); liveVW = RoundToInt(rawVw); liveVH = RoundToInt(rawVh);

		// Screenshot output size: always the native console resolution (1x),
		// completely independent of the live "Fixed Pixel Ratio" zoom
		// setting above. A screenshot has no reason to scale with a display
		// zoom preference, and tying it to `ratio` previously meant high
		// zoom settings could produce an oversized capture (e.g. GB at 4x
		// zoom = 576px tall, exceeding the 480-line EFB) or otherwise behave
		// inconsistently depending on the live viewport math. Deliberately
		// using nativeW/nativeH rather than vwidth/vheight, since
		// vwidth/vheight include any SGB border area, and a screenshot
		// should only capture the actual game content.
		gameVW = nativeW;
		gameVH = nativeH;
	} else {
		GX_SetViewport(0, 0, vmode->fbWidth, vmode->efbHeight, 0, 1);
		realVX = 0; realVY = 0; realVW = vmode->fbWidth; realVH = vmode->efbHeight;

		// The viewport itself is always full-screen in this (non-fixed
		// zoom) mode - the game quad's real on-screen position/size is
		// controlled separately, by the 3D vertex transform (square[]
		// above + the view/camera matrix), not by GX_SetViewport. That
		// transform operates in the CENTER-origin, Y-UP camera space set
		// up by ResetVideo_Emu()'s own ortho projection
		// (guOrtho(p, 480/2, -(480/2), -(640/2), 640/2, ...) - i.e.
		// X: -320..+320 left-to-right, Y: -240(bottom)..+240(top)), a
		// fixed 640x480 assumption matching screenwidth/screenheight
		// exactly, not tied to vmode's real dimensions (which can differ,
		// e.g. PAL's 574-line efbHeight).
		//
		// liveVX/Y/W/H need to be in the SAME top-left-origin, Y-DOWN
		// pixel space Menu_DrawImg565/the pause-blur code use (0..640,
		// 0..480) - previously this branch just reported the full-screen
		// viewport unconditionally, which is technically true but tells
		// you nothing about where the actual (usually smaller, usually
		// centered) game quad sits within it. Converting properly:
		//   screen_x = camera_x + 320        (shift only, no scale - both
		//   screen_y = 240 - camera_y         spaces are 1 unit = 1 pixel)
		// The quad spans [-xscale+xshift, xscale+xshift] horizontally and
		// [-yscale-yshift, yscale-yshift] vertically (see the square[]
		// assignment above) - converting both edges and taking width/
		// height from the difference:
		// guOrtho(p, 240, -240, -320, 320, ...) above is a FIXED 640x480
		// camera box regardless of vmode, then mapped by GX_SetViewport
		// onto the REAL efb (vmode->fbWidth x vmode->efbHeight). For most
		// modes efbHeight is 480 so this was invisible, but 240p's
		// efbHeight=240 means the 640x480 camera-space numbers below are
		// 2x too large in EFB-pixel terms - exactly what TakeScreenshot()
		// (which divides by vmode->fbWidth/efbHeight, expecting true EFB
		// pixels here, same as the fixed-ratio branch above) needs. Scale
		// camera-space into real EFB pixel space so both branches agree
		// on what liveVX/Y/W/H actually mean.
		float efbScaleX = vmode->fbWidth   / 640.0f;
		float efbScaleY = vmode->efbHeight / 480.0f;
		liveVX = RoundToInt((320.0f - xscale + GCSettings.xshift) * efbScaleX);
		liveVY = RoundToInt((240.0f - yscale + GCSettings.yshift) * efbScaleY);
		liveVW = RoundToInt(2.0f * xscale * efbScaleX);
		liveVH = RoundToInt(2.0f * yscale * efbScaleY);

		// Screenshot output size: always the native console resolution (1x),
		// for the same reason as the fixed-mode branch above.
		gameVW = nativeW;
		gameVH = nativeH;
	}

	updateScaling = 0;
}

/****************************************************************************
 * ResetVideo_Emu
 *
 * Reset the video/rendering mode for the emulator rendering
****************************************************************************/
void
ResetVideo_Emu ()
{
	Mtx44 p;
	GXRModeObj * rmode = FindVideoMode();

	SetupVideoMode(rmode); // reconfigure VI

	// reconfigure GX
	GX_SetViewport (0, 0, rmode->fbWidth, rmode->efbHeight, 0, 1);
	GX_SetDispCopyYScale ((f32) rmode->xfbHeight / (f32) rmode->efbHeight);
	GX_SetScissor (0, 0, rmode->fbWidth, rmode->efbHeight);

	GX_SetDispCopySrc (0, 0, rmode->fbWidth, rmode->efbHeight);
	GX_SetDispCopyDst (rmode->fbWidth, rmode->xfbHeight);
	u8 sharp[7] = {0,0,21,22,21,0,0};
	u8 soft[7] = {8,8,10,12,10,8,8};
	u8* vfilter =
		GCSettings.render == RENDER_FILTERED_SHARP ? sharp
		: GCSettings.render == RENDER_FILTERED_SOFT ? soft
		: rmode->vfilter;

	// Deflicker blends adjacent EFB lines together on the way to the XFB -
	// worthwhile for general anti-flicker on full-screen antialiased
	// content, but actively counterproductive for GBA Fixed Pixel Ratio:
	// that mode draws each source pixel as an exact, fully-covered
	// integer-multiple block with no fine per-line detail to flicker in
	// the first place, so this was blurring a perfectly sharp nearest-
	// scaled image for no benefit. Previously this was tied 1:1 to
	// GCSettings.render (only off for RENDER_UNFILTERED), which meant
	// Filtered/Sharp/Soft + Fixed Pixel Ratio still got blurred here even
	// though FPR's own scaling has nothing for deflicker to help with -
	// now FPR forces it off regardless of the render filter choice.
	bool fixedPixelRatioActive = (cartridgeType == CARTRIDGE_GBA) ? (GCSettings.gbaFixed != 0) : (GCSettings.gbFixed != 0);
	bool useDeflicker = (GCSettings.render != RENDER_UNFILTERED) && !fixedPixelRatioActive;
	GX_SetCopyFilter (rmode->aa, rmode->sample_pattern, useDeflicker ? GX_TRUE : GX_FALSE, vfilter);

	GX_SetFieldMode (rmode->field_rendering, ((rmode->viHeight == 2 * rmode->xfbHeight) ? GX_ENABLE : GX_DISABLE));
	
	if (rmode->aa)
		GX_SetPixelFmt(GX_PF_RGB565_Z16, GX_ZC_LINEAR);
	else
		GX_SetPixelFmt(GX_PF_RGB8_Z24, GX_ZC_LINEAR);
	
	GX_SetCullMode (GX_CULL_NONE);
	GX_SetDispCopyGamma (GX_GM_1_0);
	GX_SetBlendMode(GX_BM_BLEND,GX_BL_DSTALPHA,GX_BL_INVSRCALPHA,GX_LO_CLEAR);

	guOrtho(p, 480/2, -(480/2), -(640/2), 640/2, 100, 1000);	// matrix, t, b, l, r, n, f
	GX_LoadProjectionMtx (p, GX_ORTHOGRAPHIC);

	// reinitialize texture
	GX_InvalidateTexAll ();
	GX_InitTexObj (&texobj, texturemem, vwidth, vheight, GX_TF_RGB565, GX_CLAMP, GX_CLAMP, GX_FALSE);	// initialize the texture obj we are going to use

	if (GCSettings.render == RENDER_UNFILTERED)
		GX_InitTexObjFilterMode(&texobj,GX_NEAR,GX_NEAR); // original/unfiltered video mode: force texture filtering OFF

	GX_Flush();
	draw_init();

	// set aspect ratio
	updateScaling = 1;
}

static const u16* lastCopiedBorder = NULL;
static int sgbBorderCheckCounter = 0;
static bool borderJustChanged = false;

/****************************************************************************
 * GX_Render_Init
 ***************************************************************************/
void GX_Render_Init(int width, int height) {
	memset(texturemem, 0, TEXTUREMEM_SIZE);

	/*** Setup for first call to scaler ***/
	vwidth = width;
	vheight = height;

	// Force GX_Render()'s texture-object resync check to run on the next frame
	texObjWidth = -1;
	texObjHeight = -1;

	// Reset state trackers upon texture recreation
	lastCopiedBorder = NULL;
	sgbBorderCheckCounter = 0;
	borderJustChanged = false;
}

static bool borderAreaEmpty(const u16* buffer) {
	u16 reference = buffer[0];
	for (int y = 0; y < 40; y++) {
		for (int x = 0; x < 256; x++) {
			if (buffer[256 * y + x] != reference)
				return false;
		}
	}
	for (int y = 40; y < 184; y++) {
		for (int x = 0; x < 48; x++) {
			if (buffer[256 * y + x] != reference)
				return false;
		}
		for (int x = 208; x < 224; x++) {
			if (buffer[256 * y + x] != reference)
				return false;
		}
	}
	for (int y = 184; y < 224; y++) {
		for (int x = 0; x < 256; x++) {
			if (buffer[256 * y + x] != reference)
				return false;
		}
	}
	return true;
}

/****************************************************************************
 * ProcessSGBBorder
 ***************************************************************************/
static void ProcessSGBBorder(u8* buffer, int gbWidth, int gbHeight) {
	if (GCSettings.SGBBorder != 2) return; // only relevant to the custom "From .png file" mode

	if (gbWidth == 256 && gbHeight == 224 && !SGBBorderLoadedFromGame) {
		// Throttle heavy pixel scanning path to once per second
		sgbBorderCheckCounter++;
		if (sgbBorderCheckCounter >= 60) {
			sgbBorderCheckCounter = 0;
			if (!borderAreaEmpty((u16*)buffer)) {
				// don't try to load the default border anymore
				SGBBorderLoadedFromGame = true;
				SaveSGBBorderIfNoneExists(buffer);
			}
		}
	}
}

/****************************************************************************
 * DrawBorderAndGetDest
 ***************************************************************************/
static long long int* DrawBorderAndGetDest(void* textureBase, int gbWidth, int gbHeight, int borderWidth, int borderHeight) {
	long long int* dst = (long long int*) textureBase;
	borderJustChanged = false;

	if (InitialBorder) {
		// Only copy the 600 KB border once when it changes!
		if (InitialBorder != lastCopiedBorder) {
			memcpy(dst, InitialBorder, borderWidth * borderHeight * 2);
			lastCopiedBorder = InitialBorder;
			borderJustChanged = true; // Signal that the GPU needs a full RAM sync
		}

		int rows_to_skip = (borderHeight - gbHeight) / 2;
		if (rows_to_skip > 0)
			dst += rows_to_skip * borderWidth / 4;
		dst += (borderWidth - gbWidth) / 2;
	} else {
		lastCopiedBorder = NULL; // Reset tracking if running borderless
	}

	return dst;
}

/****************************************************************************
 * MakeTextureVBA
 *
 * High-performance texture swizzling (Linear to 4x4 Tiled)
 * specifically optimized for VBA GX's dynamic widths and border gaps.
 * Uses a pipelined 32-bit integer strategy to strictly enforce 4-byte
 * alignment.
 ***************************************************************************/
/****************************************************************************
 * MakeTextureVBA_Impl (Static Template)
 *
 * Employs compile-time constants to embed memory offsets directly into
 * the PowerPC load instructions. Completely eliminates pointer arithmetic
 * from the inner loop.
 ***************************************************************************/
template <int PITCH>
static inline void MakeTextureVBA_Impl(const void *src, void *dst, s32 width, s32 height, s32 dst_gap_bytes)
{
    u32 src_row_stride = PITCH * 4;
    u32 r_src_row;
    u32 tmpA, tmpB, tmpC, tmpD;

    __asm__ __volatile__ (
        "srwi   %[width], %[width], 2\n"
        "srwi   %[height], %[height], 2\n"

    "2: mtctr   %[width]\n"
        "mr     %[r_src_row], %[src]\n"

    "1: dcbz    0, %[dst]\n"

        // Load Row 0
        "lwz    %[tmpA], 0(%[src])\n"
        "lwz    %[tmpB], 4(%[src])\n"

        // Load Row 1, Store Row 0
        "lwz    %[tmpC], %c[p1](%[src])\n"
        "stw    %[tmpA], 0(%[dst])\n"
        "lwz    %[tmpD], %c[p1_4](%[src])\n"
        "stw    %[tmpB], 4(%[dst])\n"

        // Load Row 2, Store Row 1
        "lwz    %[tmpA], %c[p2](%[src])\n"
        "stw    %[tmpC], 8(%[dst])\n"
        "lwz    %[tmpB], %c[p2_4](%[src])\n"
        "stw    %[tmpD], 12(%[dst])\n"

        // Load Row 3, Store Row 2
        "lwz    %[tmpC], %c[p3](%[src])\n"
        "stw    %[tmpA], 16(%[dst])\n"
        "lwz    %[tmpD], %c[p3_4](%[src])\n"
        "stw    %[tmpB], 20(%[dst])\n"

        // Store Row 3
        "stw    %[tmpC], 24(%[dst])\n"
        "stw    %[tmpD], 28(%[dst])\n"

        // Advance inner loop (X)
        "addi   %[src], %[src], 8\n"
        "addi   %[dst], %[dst], 32\n"
        "bdnz   1b\n"

        // Advance outer loop (Y)
        "add    %[src], %[r_src_row], %[src_row_stride]\n"
        "add    %[dst], %[dst], %[dst_gap_bytes]\n"

        "subic. %[height], %[height], 1\n"
        "bne    2b\n"

        : [r_src_row] "=&b" (r_src_row),
          [tmpA] "=&r" (tmpA),
          [tmpB] "=&r" (tmpB),
          [tmpC] "=&r" (tmpC),
          [tmpD] "=&r" (tmpD),
          [src] "+b" (src),
          [dst] "+b" (dst),
          [width] "+r" (width),
          [height] "+r" (height)
        : [p1] "n" (PITCH),
          [p1_4] "n" (PITCH + 4),
          [p2] "n" (PITCH * 2),
          [p2_4] "n" (PITCH * 2 + 4),
          [p3] "n" (PITCH * 3),
          [p3_4] "n" (PITCH * 3 + 4),
          [src_row_stride] "r" (src_row_stride),
          [dst_gap_bytes] "r" (dst_gap_bytes)
        : "memory", "cc"
    );
}

// Fallback for custom/bizarre resolutions (Uses dynamic row_ptr)
static void MakeTextureVBA_Dynamic(const void *src, void *dst, s32 width, s32 height, s32 pitch, s32 dst_gap_bytes)
{
    u32 src_row_stride = pitch * 4;
    u32 r_src_row, row_ptr;
    u32 tmpA, tmpB, tmpC, tmpD;

    __asm__ __volatile__ (
        "srwi   %[width], %[width], 2\n"       // num_tiles_x = width / 4
        "srwi   %[height], %[height], 2\n"     // num_tiles_y = height / 4

    "2: mtctr   %[width]\n"                    // Set inner loop counter (X)
        "mr     %[r_src_row], %[src]\n"        // Save the start of the current source 4-row block

    "1: dcbz    0, %[dst]\n"                   // ZERO L1 CACHE: Dest is perfectly 32-byte aligned
        "mr     %[row_ptr], %[src]\n"

        // Load Row 0
        "lwz    %[tmpA], 0(%[row_ptr])\n"
        "lwz    %[tmpB], 4(%[row_ptr])\n"
        "add    %[row_ptr], %[row_ptr], %[pitch]\n"

        // Load Row 1, Store Row 0
        // Interleaving hides the 3-cycle load latency
        "lwz    %[tmpC], 0(%[row_ptr])\n"
        "stw    %[tmpA], 0(%[dst])\n"
        "lwz    %[tmpD], 4(%[row_ptr])\n"
        "stw    %[tmpB], 4(%[dst])\n"
        "add    %[row_ptr], %[row_ptr], %[pitch]\n"

        // Load Row 2, Store Row 1
        "lwz    %[tmpA], 0(%[row_ptr])\n"      // Recycle tmpA and tmpB
        "stw    %[tmpC], 8(%[dst])\n"
        "lwz    %[tmpB], 4(%[row_ptr])\n"
        "stw    %[tmpD], 12(%[dst])\n"
        "add    %[row_ptr], %[row_ptr], %[pitch]\n"

        // Load Row 3, Store Row 2
        "lwz    %[tmpC], 0(%[row_ptr])\n"
        "stw    %[tmpA], 16(%[dst])\n"
        "lwz    %[tmpD], 4(%[row_ptr])\n"
        "stw    %[tmpB], 20(%[dst])\n"

        // Store Row 3
        "stw    %[tmpC], 24(%[dst])\n"
        "stw    %[tmpD], 28(%[dst])\n"

        // Advance pointers for the next tile in the row
        "addi   %[src], %[src], 8\n"           // Advance src X by 4 pixels (8 bytes)
        "addi   %[dst], %[dst], 32\n"          // Advance dst by 1 full tile (32 bytes)
        "bdnz   1b\n"                          // Decrement CTR, loop inner if > 0

        // Advance pointers to the next row of tiles
        "add    %[src], %[r_src_row], %[src_row_stride]\n" // Jump down 4 source rows
        "add    %[dst], %[dst], %[dst_gap_bytes]\n"        // Skip right/left borders in dest

        "subic. %[height], %[height], 1\n"     // Decrement height counter (Y)
        "bne    2b\n"                          // Loop outer if > 0

        : [r_src_row] "=&b" (r_src_row),
          [row_ptr] "=&b" (row_ptr),
          [tmpA] "=&r" (tmpA),
          [tmpB] "=&r" (tmpB),
          [tmpC] "=&r" (tmpC),
          [tmpD] "=&r" (tmpD),
          [src] "+b" (src),
          [dst] "+b" (dst),
          [width] "+r" (width),
          [height] "+r" (height)
        : [pitch] "r" (pitch),
          [src_row_stride] "r" (src_row_stride),
          [dst_gap_bytes] "r" (dst_gap_bytes)
        : "memory", "cc"
    );
}

/****************************************************************************
 * SwizzleLinearToGXTiled
 *
 * Pure swizzle: converts a linear RGB565 buffer (with optional stride padding)
 * into GX 4x4-tiled format. No border compositing, no centering, no recursion.
 *
 * srcStride: bytes per source row (e.g. (width+2)*2 for mGBA's +2 padding)
 * dst:       32-byte aligned destination buffer, size = width*height*2 bytes
 ****************************************************************************/
void SwizzleLinearToGXTiled(const u8 *src, u8 *dst, int width, int height, int srcStride)
{
    // Tile the source into 4x4 blocks (GX RGB565 tiled format)
    for (int ty = 0; ty < height; ty += 4) {
        for (int tx = 0; tx < width; tx += 4) {
            for (int row = 0; row < 4; row++) {
                const u16 *srcRow = (const u16 *)(src + (ty + row) * srcStride) + tx;
                u16 *dstPixel = (u16 *)dst;
                dstPixel[0] = srcRow[0];
                dstPixel[1] = srcRow[1];
                dstPixel[2] = srcRow[2];
                dstPixel[3] = srcRow[3];
                dst += 8; // 4 pixels * 2 bytes
            }
        }
    }
}

// Scale2x filter (FILTER_SCALE2X) - CPU pixel-doubling algorithm, moved to
// videofilters.cpp/h (Scale2xRGB565(), scale2xBuffer) - see there for the
// algorithm itself. Used below in WriteFrameToTextureMemory().

/****************************************************************************
 * WriteFrameToTextureMemory
 ****************************************************************************/
void WriteFrameToTextureMemory(u8* srcBuffer, void* textureBase, int width, int height)
{
    int borderWidth = InitialBorder ? InitialBorderWidth : width;
    int borderHeight = InitialBorder ? InitialBorderHeight : height;

    ProcessSGBBorder(srcBuffer, width, height);
    long long int* dst_ptr = DrawBorderAndGetDest(textureBase, width, height, borderWidth, borderHeight);

    int gbPitch = width * 2 + 4;
    int dst_gap_bytes = ((borderWidth - width) / 4) * 32;

    // Route to the statically optimized ASM based on console resolution width
    switch (width) {
        case 160: // GB / GBC (Pitch = 324)
            MakeTextureVBA_Impl<324>(srcBuffer, dst_ptr, width, height, dst_gap_bytes);
            break;
        case 240: // GBA (Pitch = 484)
            MakeTextureVBA_Impl<484>(srcBuffer, dst_ptr, width, height, dst_gap_bytes);
            break;
        case 256: { // SGB - use the actual configured stride, not an assumed one
            extern int gGbVideoStride;
            int pitch = (gGbVideoStride > 0 ? gGbVideoStride : (width + 2)) * 2;
            MakeTextureVBA_Dynamic(srcBuffer, dst_ptr, width, height, pitch, dst_gap_bytes);
            break;
        }
        default:  // Fallback for custom borders/resolutions
            MakeTextureVBA_Dynamic(srcBuffer, dst_ptr, width, height, gbPitch, dst_gap_bytes);
            break;
    }

    // High-efficiency targeted data cache flushing
    if (InitialBorder && !borderJustChanged) {
        // Normal Frame: Flush ONLY the game screen cache lines (Saves ~87% bus bandwidth)
        u8* flush_ptr = (u8*)dst_ptr;
        u32 row_bytes = width * 8;         // bytes per tile row for game screen
        u32 stride_bytes = borderWidth * 8; // full texture pitch stride bytes
        int tile_rows = height / 4;
        for (int i = 0; i < tile_rows; i++) {
            DCStoreRange(flush_ptr, row_bytes);
            flush_ptr += stride_bytes;
        }
    } else {
        // Flush everything if borderless, OR if the border was just copied this frame
        DCStoreRange(textureBase, borderWidth * borderHeight * 2);
    }
}

/****************************************************************************
 * GX_Render
 *
 * Pass in a buffer, width and height to update as a tiled RGB565 texture
 * (2 bytes per pixel)
 ****************************************************************************/
void GX_Render(int gbWidth, int gbHeight, u8 * buffer)
{
	// Scale2x doubles the raw game image before it reaches the
	// border-compositing/swizzle path below. Scoped off for now when:
	//  - a border is present: the border itself isn't filtered/doubled,
	//    so compositing a 2x game image into a 1x border needs handling
	//    not yet implemented.
	//  - Fixed Pixel Ratio is active: that mode's "N game pixels = N TV
	//    pixels" math is defined in terms of the native resolution
	//    captured in vwidth/vheight below; doubling those via a filter
	//    changes what "1x/2x/3x" means and needs its own dedicated
	//    handling rather than silently interacting with this.
	//  - the source exceeds scale2xBuffer's GBA-sized capacity.
	int fixedForCurrentCart = (cartridgeType == CARTRIDGE_GBA) ? GCSettings.gbaFixed : GCSettings.gbFixed;
	bool useScale2x = (GCSettings.FilterMethod == FILTER_SCALE2X) &&
	                   !InitialBorder && !fixedForCurrentCart &&
	                   gbWidth <= 240 && gbHeight <= 160;

	// Sharp Bilinear (see sharpBilinearTexMem's comment near the top of this
	// file for the full design) - scoped off under the same conditions as
	// Scale2x above, for the same reasons: it's a per-pixel prescale of the
	// raw game image, and correctly composing that with a border or with
	// Fixed Pixel Ratio's "N game pixels = N TV pixels" math isn't handled
	// here yet.
	bool useSharpBilinear = (GCSettings.FilterMethod == FILTER_SHARP_BILINEAR) &&
	                         !InitialBorder && !fixedForCurrentCart &&
	                         gbWidth <= 240 && gbHeight <= 160;

	if (useScale2x)
	{
		Scale2xRGB565((const u16 *)buffer, gbWidth + 2, gbWidth, gbHeight,
		              (u16 *)scale2xBuffer, gbWidth * 2 + 2);
		buffer = scale2xBuffer;
		gbWidth *= 2;
		gbHeight *= 2;
	}

	int borderWidth = InitialBorder ? InitialBorderWidth : gbWidth;
	int borderHeight = InitialBorder ? InitialBorderHeight : gbHeight;

	vwidth = borderWidth;
	vheight = borderHeight;

	// Ensure previous frame copy and background VSync block have finished cleanly
	while (!vb_done || (copynow == GX_TRUE))
	{
		LWP_ThreadSleep(render_queue); // Halts main thread with 0 CPU load until signals occur
	}

	whichfb ^= 1;

	if(updateScaling)
		UpdateScaling();

	// texobj's declared dimensions only get set in ResetVideo_Emu()/
	// GX_Render_Init(), using whatever vwidth/vheight happened to be at that
	// moment - which can be stale (e.g. a previous game's resolution, or set
	// before the core reports its real SGB-border-adjusted size). If the
	// actual data we're about to write doesn't match what the texture object
	// was last told, the GPU samples the correctly-tiled pixel data with the
	// wrong stride, producing diagonal/streaked corruption. Resync here.
	if (borderWidth != texObjWidth || borderHeight != texObjHeight)
	{
		GX_InitTexObj(&texobj, texturemem, borderWidth, borderHeight, GX_TF_RGB565, GX_CLAMP, GX_CLAMP, GX_FALSE);
		if (GCSettings.render == RENDER_UNFILTERED)
			GX_InitTexObjFilterMode(&texobj, GX_NEAR, GX_NEAR);
		texObjWidth = borderWidth;
		texObjHeight = borderHeight;
	}

	// Reassert every frame, not just once in draw_init(): draw_cursor()
	// (further down, after draw_square()) reconfigures TEV/texgen state for
	// its own quad and only partially restores it afterward, so whichever
	// pipeline (plain or scanline) should be active for THIS frame's game
	// quad needs to be re-established here regardless of what ran last frame.
	configure_tev_pipeline();

	// load texture into GX
	WriteFrameToTextureMemory(buffer, texturemem, gbWidth, gbHeight);

	// Only NOW - after the CPU has fully written and flushed the new frame
	// into texturemem - tell the GPU's own texture cache to drop whatever
	// it had cached and re-fetch from main memory. Doing this any earlier
	// leaves a window where other GPU texture activity (e.g. the cursor,
	// which title/menu screens draw far more distinctly than continuous
	// gameplay does) can repopulate the cache with stale pre-write data
	// before the real draw call ever runs.
	GX_InvalidateTexAll();

	GX_SetNumChans(1);
	GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
	GX_SetColorUpdate(GX_TRUE);

	if (useSharpBilinear)
	{
		// Runs its own pass-1 draw + GX_CopyTex, then leaves the viewport
		// restored to the live on-screen one - see its own header comment.
		RenderSharpBilinearPrescale(borderWidth, borderHeight);
		GX_LoadTexObj(&sharpBilinearTexObj, GX_TEXMAP0); // bilinear-filtered prescaled texture, not the raw small one
	}
	else
	{
		// Reassert every frame, not just on the resize-triggered resync
		// above - RenderSharpBilinearPrescale() unconditionally forces
		// texobj to GX_NEAR whenever it runs, so if Sharp Bilinear was
		// active on a previous frame and just got switched off, texobj's
		// filter mode needs to be put back here regardless of whether its
		// dimensions changed.
		GX_InitTexObjFilterMode(&texobj, (GCSettings.render == RENDER_UNFILTERED) ? GX_NEAR : GX_LINEAR, (GCSettings.render == RENDER_UNFILTERED) ? GX_NEAR : GX_LINEAR);
		GX_LoadTexObj(&texobj, GX_TEXMAP0);
	}

	draw_square(view); // render textured quad
	#ifdef HW_RVL
	draw_cursor(view); // render cursor
	#endif

	// GX_DrawDone() blocks the CPU until the GPU has fully finished every
	// queued draw command - a real, synchronous stall, not just a queue
	// flush. The only thing below that actually needs a completed frame is
	// TakeScreenshot() (it reads back rendered pixel data, so a torn/
	// incomplete frame would produce a torn/incomplete screenshot).
	// VIDEO_SetNextFramebuffer()/VIDEO_Flush()/the copynow handoff further
	// down don't need this - the real EFB->XFB copy happens later, async,
	// in copy_to_xfb() at the next physical VBlank (see vbgetback above),
	// by which point the GPU has had a full frame's worth of real time to
	// finish on its own.
	//
	// This used to run unconditionally every single frame - a genuine CPU
	// stall waiting on the GPU, on the exact thread that also drives
	// core->runFrame()/PushAudio() in mgba_emuMain(). Screenshots are rare;
	// paying this stall on every frame regardless was pure waste, and
	// intermittent GPU backlog (sharp bilinear's extra pass, a texture
	// resync, cursor TEV reconfiguration) is a plausible source of the
	// periodic, not-every-callback audio underrun pattern seen in testing -
	// a stall here delays the next runFrame()/PushAudio() call in real
	// wall-clock time even though GB/GBC audio generation itself is
	// perfectly cycle-accurate inside that call.
	if (ScreenshotRequested)
	{
		GX_DrawDone();
	}

	if(ScreenshotRequested)
	{
		ScreenshotRequested = 0;
		TakeScreenshot();
		ConfigRequested = 1;
	}

	// EFB is ready to be copied into XFB
	VIDEO_SetNextFramebuffer(xfb[whichfb]);
	VIDEO_Flush();
	copynow = GX_TRUE;

	// Reset state and signal background VSync thread to begin waiting for next blanking interval
	vb_done = false;
	LWP_ThreadSignal(vb_queue);
}

/****************************************************************************
 * GX_ThrottleVSync
 *
 * Paces the caller to real vertical-blank timing WITHOUT doing any of
 * GX_Render()'s actual work (texture upload, draw, EFB->XFB copy). For
 * frameskip's skipped-video frames in mgba_emuMain() (vbasupport.cpp) -
 * before this existed, that path just returned early, skipping the ONLY
 * thing that ever throttled the emulation loop to real time (the vsync
 * wait normally buried inside GX_Render() itself, see the top of that
 * function above). That let core->runFrame()/PushAudio() for however many
 * frames Frameskip is set to run back-to-back as fast as the CPU could go,
 * which is what was speeding up both audio and actual gameplay - not just
 * video - on skipped frames. A direct VIDEO_WaitVSync() call here is
 * independent of the vb_done/copynow double-buffering state GX_Render()
 * itself manages, so it's safe to call without touching any of that.
 ***************************************************************************/
void GX_ThrottleVSync()
{
	VIDEO_WaitVSync();
}

/****************************************************************************
 * DecodePNGToRGB565
 *
 * Same PNGU convention already used elsewhere in this codebase (see the
 * SGB border PNG loader in vbasupport.cpp): SelectImageFromBuffer ->
 * GetImageProperties -> DecodeTo4x4RGB565 -> ReleaseImageContext.
 ***************************************************************************/
u8 *DecodePNGToRGB565(const u8 *pngData, int pngSize, int *outWidth, int *outHeight)
{
	(void)pngSize; // PNGU_SelectImageFromBuffer reads the PNG's own embedded
	               // length from its chunk structure; it doesn't take a
	               // separate size parameter, same as every other call site
	               // of this function in this codebase.

	*outWidth = 0;
	*outHeight = 0;

	IMGCTX ctx = PNGU_SelectImageFromBuffer((u8 *)pngData);
	if (!ctx)
		return NULL;

	PNGUPROP props;
	PNGU_GetImageProperties(ctx, &props);

	// PNGU_DecodeTo4x4RGB565 requires both dimensions divisible by 4.
	int decodeW = (props.imgWidth  + 3) & ~3;
	int decodeH = (props.imgHeight + 3) & ~3;

	u8 *texture = (u8 *)memalign(32, decodeW * decodeH * 2); // 2 bytes/pixel for RGB565
	if (!texture) {
		PNGU_ReleaseImageContext(ctx);
		return NULL;
	}

	if (PNGU_DecodeTo4x4RGB565(ctx, decodeW, decodeH, texture) != PNGU_OK) {
		free(texture);
		PNGU_ReleaseImageContext(ctx);
		return NULL;
	}

	PNGU_ReleaseImageContext(ctx);

	*outWidth = decodeW;
	*outHeight = decodeH;
	return texture;
}

/****************************************************************************
 * CropGameScreenBorderForSave
 *
 * gameScreenPng (captured by TakeScreenshot(), below) includes the SGB
 * border when one is present - needed so the pause-menu blur shows it
 * (see menu.cpp's BuildBlurredPauseScreen()), matching what was on screen
 * a moment earlier. The separate "save a screenshot to the SD card"
 * feature (SavePreviewImg(), vbasupport.cpp - used for the ROM browser's
 * preview thumbnail) wants just the bare game content, no border, so this
 * crops it back out at SAVE time rather than needing a second, independent
 * capture. Returns NULL (and leaves *outSize at 0) if there's nothing to
 * crop (no border was actually present) - callers should fall back to
 * gameScreenPng.pngData directly in that case. Caller owns the returned
 * buffer (free() it when done).
 ***************************************************************************/
u8 *CropGameScreenBorderForSave(int *outSize)
{
	*outSize = 0;

	if (!gameScreenPng.pngData || gameScreenPng.pngSize <= 0)
		return NULL;
	// Nothing to crop - captured without a border in the first place.
	if (gameScreenPng.nativeW <= 0 || gameScreenPng.gameNativeW <= 0 ||
	    gameScreenPng.nativeW == gameScreenPng.gameNativeW)
		return NULL;

	int decodeW = 0, decodeH = 0;
	u8 *tiled = DecodePNGToRGB565(gameScreenPng.pngData, gameScreenPng.pngSize, &decodeW, &decodeH);
	if (!tiled)
		return NULL;

	// Same center-crop margin math as menu.cpp's blur-positioning fix
	// (gameScreenDrawX/Y offset), just used to find the crop rectangle
	// instead of a draw offset.
	int marginX = (gameScreenPng.nativeW - gameScreenPng.gameNativeW) / 2;
	int marginY = (gameScreenPng.nativeH - gameScreenPng.gameNativeH) / 2;
	int cropW = gameScreenPng.gameNativeW;
	int cropH = gameScreenPng.gameNativeH;

	int blocksPerRow = decodeW / 4;
	const u16 *texBase = (const u16 *)tiled;

	int rowBytes = cropW * 3;
	if (rowBytes % 4) rowBytes = ((rowBytes >> 2) + 1) << 2;

	u8 *rgb888 = (u8 *)malloc(rowBytes * cropH);
	if (!rgb888) {
		free(tiled);
		return NULL;
	}
	memset(rgb888, 0, rowBytes * cropH);

	// Same tile-address-with-offset formula TakeScreenshot() used before
	// the border-inclusive rewrite - unswizzles just the cropped center
	// region out of the full (bordered) tiled source, rather than the
	// whole thing.
	for (int y = 0; y < cropH; y++) {
		u8 *dstRow = rgb888 + y * rowBytes;
		int fullY = y + marginY;
		for (int x = 0; x < cropW; x++) {
			int fullX = x + marginX;
			int blockIndex = (fullY / 4) * blocksPerRow + (fullX / 4);
			int withinBlockRow = fullY % 4;
			int withinBlockCol = fullX % 4;
			const u16 *srcPixel = texBase + (blockIndex * 16) + (withinBlockRow * 4) + withinBlockCol;
			u16 px = *srcPixel;

			int r5 = (px >> 11) & 0x1F;
			int g6 = (px >> 5)  & 0x3F;
			int b5 =  px        & 0x1F;
			dstRow[x*3 + 0] = (r5 << 3) | (r5 >> 2);
			dstRow[x*3 + 1] = (g6 << 2) | (g6 >> 4);
			dstRow[x*3 + 2] = (b5 << 3) | (b5 >> 2);
		}
	}
	free(tiled);

	IMGCTX pngContext = PNGU_SelectImageFromBuffer(savebuffer);
	if (pngContext == NULL) {
		free(rgb888);
		return NULL;
	}

	// Same PNGU_EncodeFromRGB return-value convention as TakeScreenshot():
	// on success this returns the actual encoded byte count directly (see
	// pngu.c: PNGU_EncodeFromRGB returns ctx->cursor, not a status code) -
	// the small PNGU_* named constants (0-9) are only ever returned on
	// failure, and are always far smaller than any real encoded PNG.
	int res = PNGU_EncodeFromRGB(pngContext, cropW, cropH, rgb888, rowBytes);
	int pngSize = (res > PNGU_LIB_ERROR) ? res : 0;

	PNGU_ReleaseImageContext(pngContext);
	free(rgb888);

	if (pngSize == 0)
		return NULL;

	u8 *pngData = (u8 *)malloc(pngSize);
	if (pngData == NULL)
		return NULL;
	memcpy(pngData, savebuffer, pngSize);

	*outSize = pngSize;
	return pngData;
}

/****************************************************************************
 * TakeScreenshot
 *
 * Ported from VBA-GX 3.0.0: captures directly from texturemem (the game's
 * own source texture, at its native small resolution) instead of the EFB.
 * This is mode-independent - no viewport manipulation, no dependency on
 * the current TV video mode's resolution/addressing, unlike the previous
 * PNGU_EncodeFromEFB-based approach this replaces.
 *
 * Stride argument note: upstream's own call passes width*3 against an
 * RGB565 (2 bytes/pixel) source, which doesn't match a literal
 * bytes-per-row interpretation - "stride" here appears to mean something
 * PNGU computes/uses internally (possibly the output RGB8 stride) rather
 * than the input buffer's real row pitch. Since I can't confirm the exact
 * semantics from PNGU's source/docs, this mirrors upstream's exact known-
 * working call rather than deriving a different value - if screenshots
 * come out corrupted/skewed, this is the first place to check.
 *
 * Border offset note: unlike upstream (which has no SGB border concept
 * and assumes texturemem is exactly width x height with no padding), our
 * texturemem can hold a larger bordered image with the actual game
 * content starting at a nonzero offset - same offset math
 * DrawBorderAndGetDest() already uses. borderStride is the real on-screen
 * texture width (with border, if any); gameVW/gameVH is the actual game
 * content size being captured.
 ***************************************************************************/
void TakeScreenshot()
{
	// InitialBorder only tracks OUR custom PNG-border feature
	// (GCSettings.SGBBorder == 2) - it stays NULL when mGBA's own native
	// SGB border rendering is what's actually active (SGBBorder == 1), even
	// though texturemem genuinely contains a bordered (e.g. 256x224) image
	// in that case. The old fallback here (gameVW/gameVH, the fixed native
	// resolution) was wrong whenever ANY border - native or custom - was
	// really present, since it doesn't match texturemem's real dimensions;
	// vwidth/vheight (see GX_Render()) is the actual per-frame source of
	// truth for what's really in texturemem right now, regardless of which
	// border mechanism produced it. This was the real cause of SGB-bordered
	// screenshots coming out as noise - the read-back stride didn't match
	// the write stride.
	int borderStride = InitialBorder ? InitialBorderWidth : vwidth;
	int borderTotalH  = InitialBorder ? InitialBorderHeight : vheight;

	int blocksPerRow = borderStride / 4;
	const u16 *texBase = (const u16 *)texturemem;

	// Unswizzle the RGB565 GX-tiled source (see SwizzleLinearToGXTiled's
	// header comment for the tiling scheme this inverts) into a linear
	// RGB888 buffer, expanding each 5/6/5-bit channel to 8 bits the same
	// way the color-correction code does. PNGU_EncodeFromGXTexture is NOT
	// used here even though it exists - its internal unswizzle assumes GX's
	// RGBA8 tiled layout (a different, more complex split A/R + G/B plane
	// format), not RGB565, and texturemem in this codebase is always
	// RGB565 (every GX_InitTexObj call for the live game texture uses
	// GX_TF_RGB565) - so that function would have silently produced a
	// garbled screenshot.
	//
	// Captures the FULL region (borderStride x borderTotalH - border
	// included, when present), not just the bare game content
	// (gameVW x gameVH) like before, so the pause-menu blur shows the
	// border too instead of just the bare game content looking jarring
	// against the border that was visible a moment ago during gameplay.
	int rowBytes = borderStride * 3;
	if (rowBytes % 4) rowBytes = ((rowBytes >> 2) + 1) << 2; // pad to 4-byte boundary, matching PNGU_EncodeFromRGB's own internal row assumption

	u8 *rgb888 = (u8 *)malloc(rowBytes * borderTotalH);
	if (!rgb888)
		return;
	memset(rgb888, 0, rowBytes * borderTotalH);

	for (int y = 0; y < borderTotalH; y++) {
		u8 *dstRow = rgb888 + y * rowBytes;
		for (int x = 0; x < borderStride; x++) {
			int blockIndex = (y / 4) * blocksPerRow + (x / 4);
			int withinBlockRow = y % 4;
			int withinBlockCol = x % 4;
			const u16 *srcPixel = texBase + (blockIndex * 16) + (withinBlockRow * 4) + withinBlockCol;
			u16 px = *srcPixel;

			int r5 = (px >> 11) & 0x1F;
			int g6 = (px >> 5)  & 0x3F;
			int b5 =  px        & 0x1F;

			dstRow[x*3 + 0] = (r5 << 3) | (r5 >> 2);
			dstRow[x*3 + 1] = (g6 << 2) | (g6 >> 4);
			dstRow[x*3 + 2] = (b5 << 3) | (b5 >> 2);
		}
	}

	IMGCTX pngContext = PNGU_SelectImageFromBuffer(savebuffer);
	if (pngContext == NULL) {
		free(rgb888);
		return;
	}

	// On success this returns the actual encoded byte count directly (see
	// pngu.c: PNGU_EncodeFromRGB returns ctx->cursor, not a status code) -
	// the small PNGU_* named constants (0-9) are only ever returned on
	// failure, and are always far smaller than any real encoded PNG.
	int res = PNGU_EncodeFromRGB(pngContext, borderStride, borderTotalH, rgb888, rowBytes);
	int pngSize = (res > PNGU_LIB_ERROR) ? res : 0;

	PNGU_ReleaseImageContext(pngContext);
	free(rgb888);

	if (pngSize == 0)
		return;

	u8 *pngData = (u8 *) malloc(pngSize);
	if (pngData == NULL)
		return;
	memcpy(pngData, savebuffer, pngSize);

	if (gameScreenPng.pngData)
		free(gameScreenPng.pngData);

	gameScreenPng.pngData  = pngData;
	gameScreenPng.pngSize  = pngSize;
	gameScreenPng.nativeW  = borderStride;
	gameScreenPng.nativeH  = borderTotalH;
	gameScreenPng.gameNativeW = gameVW;
	gameScreenPng.gameNativeH = gameVH;

	// Stored as FRACTIONS of the game's real current video mode
	// (vmode->fbWidth/efbHeight), not absolute pixels. liveVX/Y/W/H are
	// computed relative to whatever video mode the GAME is actually
	// running in (which can differ from 640x480 if GCSettings.videomode
	// isn't Auto - PAL, 240p, etc.), but the pause-menu background this
	// gets composited onto (BuildBlurredPauseScreen(), menu.cpp) always
	// assumes a fixed screenwidth/screenheight (640x480) canvas, since the
	// menu always forces auto/preferred mode via ResetVideo_Menu()
	// regardless of the game's setting. Storing absolute pixels here and
	// reusing them directly on a differently-sized canvas is exactly what
	// caused the pause-background to appear squished into a corner
	// whenever the game's real video mode differed from 640x480 -
	// fractions stay correct regardless of the size mismatch between the
	// two contexts.
	float modeW = (vmode && vmode->fbWidth > 0) ? (float)vmode->fbWidth : (float)screenwidth;
	float modeH = (vmode && vmode->efbHeight > 0) ? (float)vmode->efbHeight : (float)screenheight;
	gameScreenPng.viewX = liveVX / modeW;
	gameScreenPng.viewY = liveVY / modeH;
	gameScreenPng.viewW = liveVW / modeW;
	gameScreenPng.viewH = liveVH / modeH;
}

void ClearScreenshot()
{
	if (gameScreenPng.pngData)
	{
		free(gameScreenPng.pngData);
		gameScreenPng.pngData = NULL;
	}
	gameScreenPng.pngSize = 0;
}

/****************************************************************************
 * ResetVideo_Menu
 *
 * Reset the video/rendering mode for the menu
****************************************************************************/
void
ResetVideo_Menu ()
{
	#ifdef HW_RVL
	if (CONF_GetAspectRatio() == CONF_ASPECT_16_9 && (*(u32*)(0xCD8005A0) >> 16) == 0xCAFE) // Wii U
	{
		/* vWii widescreen patch by tueidj */
		write32(0xd8006a0, 0x30000004), mask32(0xd8006a8, 0, 2);
	}
	#endif
	
	Mtx44 p;
	f32 yscale;
	u32 xfbHeight;
	GXRModeObj * rmode = FindVideoMode(true); // always use auto/preferred mode for menus

	SetupVideoMode(rmode); // reconfigure VI

	// clears the bg to color and clears the z buffer
	GXColor background = {0, 0, 0, 255};
	GX_SetCopyClear (background, GX_MAX_Z24);

	yscale = GX_GetYScaleFactor(vmode->efbHeight,vmode->xfbHeight);
	xfbHeight = GX_SetDispCopyYScale(yscale);
	GX_SetScissor(0,0,vmode->fbWidth,vmode->efbHeight);
	GX_SetDispCopySrc(0,0,vmode->fbWidth,vmode->efbHeight);
	GX_SetDispCopyDst(vmode->fbWidth,xfbHeight);
	GX_SetCopyFilter(vmode->aa,vmode->sample_pattern,GX_TRUE,vmode->vfilter);
	GX_SetFieldMode(vmode->field_rendering,((vmode->viHeight==2*vmode->xfbHeight)?GX_ENABLE:GX_DISABLE));

	if (vmode->aa)
		GX_SetPixelFmt(GX_PF_RGB565_Z16, GX_ZC_LINEAR);
	else
		GX_SetPixelFmt(GX_PF_RGB8_Z24, GX_ZC_LINEAR);

	// setup the vertex descriptor
	// tells the flipper to expect direct data
	GX_ClearVtxDesc();
	GX_InvVtxCache ();
	GX_InvalidateTexAll();

	GX_SetVtxDesc(GX_VA_TEX0, GX_NONE);
	GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
	GX_SetVtxDesc (GX_VA_CLR0, GX_DIRECT);

	GX_SetVtxAttrFmt (GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
	GX_SetVtxAttrFmt (GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
	GX_SetZMode (GX_FALSE, GX_LEQUAL, GX_TRUE);

	GX_SetNumChans(1);
	GX_SetNumTexGens(1);
	// The scanline filter (gameplay-only - see configure_tev_pipeline() in
	// this file) switches the GPU to a 2-stage TEV setup while active.
	// Nothing here previously reset that back down to 1 stage on entering
	// the menu, so if the last gameplay frame left it configured for
	// scanlines, that 2-stage multiply-against-a-stale-texture config
	// silently carried over into menu rendering too - the scanline effect
	// visibly (and incorrectly) applying to menu screens. The menu should
	// never run the scanline TEV stage regardless of what
	// GCSettings.FilterMethod currently says (that's a gameplay-only
	// display filter, not a global one).
	GX_SetNumTevStages(1);
	GX_SetVtxDesc(GX_VA_TEX1, GX_NONE);
	GX_SetTevOp (GX_TEVSTAGE0, GX_PASSCLR);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
	GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);

	guMtxIdentity(GXmodelView2D);
	guMtxTransApply (GXmodelView2D, GXmodelView2D, 0.0F, 0.0F, -50.0F);
	GX_LoadPosMtxImm(GXmodelView2D,GX_PNMTX0);

	guOrtho(p,0,479,0,639,0,300);
	GX_LoadProjectionMtx(p, GX_ORTHOGRAPHIC);

	GX_SetViewport(0,0,vmode->fbWidth,vmode->efbHeight,0,1);
	GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
	GX_SetAlphaUpdate(GX_TRUE);
}

/****************************************************************************
 * Menu_Render
 *
 * Renders everything current sent to GX, and flushes video
 ***************************************************************************/
void Menu_Render()
{
	whichfb ^= 1; // flip framebuffer
	GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
	GX_SetColorUpdate(GX_TRUE);
	GX_CopyDisp(xfb[whichfb],GX_TRUE);
	GX_DrawDone();
	VIDEO_SetNextFramebuffer(xfb[whichfb]);
	VIDEO_Flush();
	VIDEO_WaitVSync(); // cap menu loop to 60fps — fixes input sensitivity
}

/****************************************************************************
 * Menu_DrawImg
 *
 * Draws the specified image on screen using GX
 ***************************************************************************/
void Menu_DrawImg(f32 xpos, f32 ypos, u16 width, u16 height, u8 data[],
	f32 degrees, f32 scaleX, f32 scaleY, u8 alpha)
{
	if(data == NULL)
		return;

	GXTexObj texObj;

	GX_InitTexObj(&texObj, data, width,height, GX_TF_RGBA8,GX_CLAMP, GX_CLAMP,GX_FALSE);
	GX_LoadTexObj(&texObj, GX_TEXMAP0);
	GX_InvalidateTexAll();

	GX_SetTevOp (GX_TEVSTAGE0, GX_MODULATE);
	GX_SetVtxDesc (GX_VA_TEX0, GX_DIRECT);

	Mtx m,m1,m2, mv;
	width  >>= 1;
	height >>= 1;

	guMtxIdentity (m1);
	guMtxScaleApply(m1,m1,scaleX,scaleY,1.0);
	guVector axis = (guVector) {0 , 0, 1 };
	guMtxRotAxisDeg (m2, &axis, degrees);
	guMtxConcat(m2,m1,m);

	guMtxTransApply(m,m, xpos+width,ypos+height,0);
	guMtxConcat (GXmodelView2D, m, mv);
	GX_LoadPosMtxImm (mv, GX_PNMTX0);

	GX_Begin(GX_QUADS, GX_VTXFMT0,4);
	GX_Position3f32(-width, -height,  0);
	GX_Color4u8(0xFF,0xFF,0xFF,alpha);
	GX_TexCoord2f32(0, 0);

	GX_Position3f32(width, -height,  0);
	GX_Color4u8(0xFF,0xFF,0xFF,alpha);
	GX_TexCoord2f32(1, 0);

	GX_Position3f32(width, height,  0);
	GX_Color4u8(0xFF,0xFF,0xFF,alpha);
	GX_TexCoord2f32(1, 1);

	GX_Position3f32(-width, height,  0);
	GX_Color4u8(0xFF,0xFF,0xFF,alpha);
	GX_TexCoord2f32(0, 1);
	GX_End();
	GX_LoadPosMtxImm (GXmodelView2D, GX_PNMTX0);

	GX_SetTevOp (GX_TEVSTAGE0, GX_PASSCLR);
	GX_SetVtxDesc (GX_VA_TEX0, GX_NONE);
}

/****************************************************************************
 * Menu_DrawImg565
 *
 * Identical to Menu_DrawImg above, except the texture is GX_TF_RGB565
 * instead of GX_TF_RGBA8. Used for the pause-screen blur (see
 * DecodePNGToRGB565() and the pause screenshot code in menu.cpp) - this
 * vendored PNGU only supports decoding to RGB565 (PNGU_DecodeTo4x4RGB565),
 * not RGBA8, and GX's actual RGBA8 tiled layout is a more complex split
 * A/R + G/B plane format (not simple 4x4 packing like RGB565), so rather
 * than hand-writing that swizzle unverified, this reuses the already-
 * proven RGB565 GX texture path used elsewhere in this file for the game's
 * own live rendering.
 ***************************************************************************/
void Menu_DrawImg565(f32 xpos, f32 ypos, u16 width, u16 height, u8 data[],
	f32 degrees, f32 scaleX, f32 scaleY, u8 alpha)
{
	if(data == NULL)
		return;

	GXTexObj texObj;

	GX_InitTexObj(&texObj, data, width,height, GX_TF_RGB565,GX_CLAMP, GX_CLAMP,GX_FALSE);
	GX_LoadTexObj(&texObj, GX_TEXMAP0);
	GX_InvalidateTexAll();

	GX_SetTevOp (GX_TEVSTAGE0, GX_MODULATE);
	GX_SetVtxDesc (GX_VA_TEX0, GX_DIRECT);

	Mtx m,m1,m2, mv;
	width  >>= 1;
	height >>= 1;

	guMtxIdentity (m1);
	guMtxScaleApply(m1,m1,scaleX,scaleY,1.0);
	guVector axis = (guVector) {0 , 0, 1 };
	guMtxRotAxisDeg (m2, &axis, degrees);
	guMtxConcat(m2,m1,m);

	guMtxTransApply(m,m, xpos+width,ypos+height,0);
	guMtxConcat (GXmodelView2D, m, mv);
	GX_LoadPosMtxImm (mv, GX_PNMTX0);

	GX_Begin(GX_QUADS, GX_VTXFMT0,4);
	GX_Position3f32(-width, -height,  0);
	GX_Color4u8(0xFF,0xFF,0xFF,alpha);
	GX_TexCoord2f32(0, 0);

	GX_Position3f32(width, -height,  0);
	GX_Color4u8(0xFF,0xFF,0xFF,alpha);
	GX_TexCoord2f32(1, 0);

	GX_Position3f32(width, height,  0);
	GX_Color4u8(0xFF,0xFF,0xFF,alpha);
	GX_TexCoord2f32(1, 1);

	GX_Position3f32(-width, height,  0);
	GX_Color4u8(0xFF,0xFF,0xFF,alpha);
	GX_TexCoord2f32(0, 1);
	GX_End();
	GX_LoadPosMtxImm (GXmodelView2D, GX_PNMTX0);

	GX_SetTevOp (GX_TEVSTAGE0, GX_PASSCLR);
	GX_SetVtxDesc (GX_VA_TEX0, GX_NONE);
}

/****************************************************************************
 * Menu_DrawRectangle
 *
 * Draws a rectangle at the specified coordinates using GX
 ***************************************************************************/
void Menu_DrawRectangle(f32 x, f32 y, f32 width, f32 height, GXColor color, u8 filled)
{
	long n = 4;
	f32 x2 = x+width;
	f32 y2 = y+height;
	guVector v[] = {{x,y,0.0f}, {x2,y,0.0f}, {x2,y2,0.0f}, {x,y2,0.0f}, {x,y,0.0f}};
	u8 fmt = GX_TRIANGLEFAN;

	if(!filled)
	{
		fmt = GX_LINESTRIP;
		n = 5;
	}

	GX_Begin(fmt, GX_VTXFMT0, n);
	for(long i=0; i<n; ++i)
	{
		GX_Position3f32(v[i].x, v[i].y,  v[i].z);
		GX_Color4u8(color.r, color.g, color.b, color.a);
	}
	GX_End();
}