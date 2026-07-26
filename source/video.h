/****************************************************************************
 * Visual Boy Advance GX
 *
 * Tantric 2008-2023
 * softdev 2007
 *
 * video.h
 *
 * Video routines
 ***************************************************************************/

#ifndef _GCVIDEOH_
#define _GCVIDEOH_

#include <ogcsys.h>

void InitializeVideo ();
void GX_Render_Init(int width, int height);
void GX_Render(int gbWidth, int gbHeight, u8 * buffer);
void StopGX();
void ResetVideo_Emu();
void ResetVideo_Menu();
void TakeScreenshot();
void ClearScreenshot();
void Menu_Render();
void Menu_DrawImg(f32 xpos, f32 ypos, u16 width, u16 height, u8 data[], f32 degrees, f32 scaleX, f32 scaleY, u8 alphaF );
void Menu_DrawImg565(f32 xpos, f32 ypos, u16 width, u16 height, u8 data[], f32 degrees, f32 scaleX, f32 scaleY, u8 alphaF );
void Menu_DrawRectangle(f32 x, f32 y, f32 width, f32 height, GXColor color, u8 filled);

extern GXRModeObj *vmode;
extern int screenheight;
extern int screenwidth;
extern s32 CursorX, CursorY;
extern bool CursorVisible;
extern bool CursorValid;
extern bool TiltScreen;
extern float TiltAngle;
extern u8 * gameScreenTex;

// Replaces the old plain (u8 *gameScreenPng, int gameScreenPngSize) globals.
// TakeScreenshot() now captures directly from texturemem at the game's
// native resolution (e.g. 240x160) rather than a full-display-resolution
// EFB grab, so anything that wants to display this screenshot back at the
// right size/position on screen needs to know what "the right size/
// position" actually was - i.e. where the game was really being drawn at
// capture time (which varies with zoom/aspect/scaling settings), not just
// "stretch to fill the TV".
//
// viewX/viewY/viewW/viewH are FRACTIONS (0.0-1.0) of the game's real video
// mode dimensions at capture time (vmode->fbWidth/efbHeight), NOT absolute
// pixels. The game can be running in a different real video mode than the
// menu (which always forces auto/preferred via ResetVideo_Menu()
// regardless of GCSettings.videomode) - storing absolute pixels here and
// reusing them directly against the menu's own (differently-sized) canvas
// caused the pause-screen background to appear squished into a corner.
// Multiply by the menu's own screenwidth/screenheight to get real pixels
// for compositing (see BuildBlurredPauseScreen(), menu.cpp).
typedef struct {
	u8  *pngData;
	int  pngSize;
	int  nativeW, nativeH;                  // resolution the PNG was ACTUALLY encoded at - includes any SGB border, since TakeScreenshot() now captures the full bordered image (not just the bare game content) so the pause-menu blur shows the border too
	int  gameNativeW, gameNativeH;           // the GAME's own native resolution (e.g. 160x144 for GB), border excluded - used ONLY as the scale-factor reference (see BuildBlurredPauseScreen(), menu.cpp), kept separate from nativeW/H above since those two are no longer the same number once a border is included
	float viewX, viewY, viewW, viewH;       // on-screen position/size the game was displayed at when captured, as fractions of ITS OWN video mode's dimensions
} GameScreenPng;
extern GameScreenPng gameScreenPng;

// Decodes a PNG (already in memory, e.g. GameScreenPng.pngData) into a
// GX-native 4x4-tiled RGB565 texture buffer, ready to hand to GX_InitTexObj
// with GX_TF_RGB565 (see Menu_DrawImg565). This vendored PNGU
// (utils/pngu.h) only supports decoding to RGB565 (PNGU_DecodeTo4x4RGB565)
// - there's no RGBA8 decoder - and GX's actual RGBA8 tiled layout is a
// more complex split A/R + G/B plane format, not simple 4x4 packing, so
// this uses the proven RGB565 path instead of hand-writing that swizzle
// unverified. The screenshot itself has no transparency of its own
// anyway; the pause-screen blur's semi-transparency comes from
// Menu_DrawImg565's per-vertex alpha blending, which doesn't need the
// texture format itself to carry an alpha channel.
// Returns NULL on failure. *outWidth/*outHeight are set to the dimensions
// actually used for decoding (rounded up to a multiple of 4, since
// PNGU_DecodeTo4x4RGB565 requires that) - these may be larger than the
// source PNG's real dimensions; the extra edge pixels are undefined, so
// texture-coordinate math should use the source PNG's real width/height
// (e.g. GameScreenPng.nativeW/nativeH) rather than these outputs when
// deciding how much of the texture to actually draw.
// Caller owns the returned buffer (free() it when done).
u8 *DecodePNGToRGB565(const u8 *pngData, int pngSize, int *outWidth, int *outHeight);

// Crops the SGB border back out of gameScreenPng for SavePreviewImg()
// (vbasupport.cpp) - see this function's own header comment in video.cpp
// for why the border needs to be there for the pause-blur but not here.
// Returns NULL (and sets *outSize to 0) if there's no border to crop;
// caller should fall back to gameScreenPng.pngData/pngSize directly in
// that case. Caller owns the returned buffer (free() it when done).
u8 *CropGameScreenBorderForSave(int *outSize);

extern u32 FrameTimer;

void SaveSGBBorderIfNoneExists(const void* buffer);
void LoadGBABorderIfEnabled(const char* title);
void LoadGBBorderFileIfEnabled(void);
void SwizzleLinearToGXTiled(const u8 *src, u8 *dst, int width, int height, int srcStride);
extern u16 *InitialBorder;
extern int InitialBorderWidth;
extern int InitialBorderHeight;
extern bool SGBBorderLoadedFromGame;

#endif