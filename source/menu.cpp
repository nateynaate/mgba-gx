/****************************************************************************
 * mGBA-GX
 *
 * Fork of Visual Boy Advance GX (Tantric, 2008-2023)
 * mGBA-GX modifications 2026
 *
 * menu.cpp
 *
 * Menu flow routines - handles all menu logic
 ***************************************************************************/

#include <gccore.h>
#include <ogcsys.h>
#include <ogc/cond.h>
#include <ogc/lwp_watchdog.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <algorithm>
#include <sys/stat.h>

#ifdef HW_RVL
#include <di/di.h>
#include <wiiuse/wpad.h>
#endif

#include "vbagx.h"
#include "vbasupport.h"
#include "video.h"
#include "filebrowser.h"
#include "gcunzip.h"
#include "fileop.h"
#include "preferences.h"
#include "button_mapping.h"
#include "input.h"
#include "filelist.h"
#include "menu.h"
#include "gamesettings.h"
#include "gui/gui.h"
#include "utils/gettext.h"
#include "utils/FreeTypeGX.h"

#define THREAD_SLEEP 100

#ifdef HW_RVL
GuiImageData * pointer[4];
#endif

#ifdef HW_RVL
	#include "mem2.h"

	#define MEM_ALLOC(A) (u8*)mem2_malloc(A)
	#define MEM_DEALLOC(A) mem2_free(A)
#else
	#define MEM_ALLOC(A) (u8*)memalign(32, A)
	#define MEM_DEALLOC(A) free(A)
#endif

static GuiTrigger * trigA = NULL;
static GuiTrigger * trig2 = NULL;
// Dedicated, focus-independent B-button trigger for "go back"/"cancel"
// navigation - see its init below for why this is separate from trig2.
static GuiTrigger * trigB = NULL;

static GuiButton * btnLogo = NULL;
#ifdef HW_RVL
static GuiButton * batteryBtn[4];
#endif
// gameScreen (GuiImageData) removed - the screenshot is now decoded once
// directly to a raw, GX-native 4x4-tiled RGB565 buffer via
// DecodePNGToRGB565() (this vendored PNGU has no RGBA8 decoder - see
// video.h) and drawn with Menu_DrawImg565() directly, NOT wrapped in a
// GuiImage. GuiImage/GuiImageData only support RGBA8 (see gui.h) and GX's
// real RGBA8 tiled layout is a different split-plane format than
// RGB565's tiling - converting between the two pixel-by-pixel without a
// verified reference would risk a silent, hard-to-spot rendering bug
// rather than a compile error, so this bypasses GuiImage entirely for the
// real screenshot instead. gameScreenImg (GuiImage*) is still used, but
// ONLY for the solid-color placeholder case (no ROM loaded / capture
// failed) via GuiImage's separate (w, h, GXColor) constructor, which
// never touches raw pixel data at all.
static u8 * gameScreenTexture = NULL;      // raw decoded 4x4-tiled RGB565 buffer; owned here, freed manually
static int  gameScreenTexW = 0, gameScreenTexH = 0;  // decoded (possibly padded) texture dims - see DecodePNGToRGB565's header comment
static bool gameScreenIsBlurred = false;   // true: draw the real screenshot manually via Menu_DrawImg565 (see UpdateGUI() and the credits-screen loop). false: gameScreenImg (solid color) is the background instead.
static f32  gameScreenDrawX = 0, gameScreenDrawY = 0, gameScreenScaleX = 1.0f, gameScreenScaleY = 1.0f;
static GuiImage * gameScreenImg = NULL;

// Real pause-screen blur, built once (not per-frame): unswizzle the small
// decoded screenshot to a plain linear RGB565 buffer, nearest-neighbor
// upscale + composite it onto a full-screen linear canvas at the correct
// position (matching how/where the game was actually being displayed when
// captured), run a genuine separable box blur over the whole canvas, blend
// in a dark overlay tint, then re-swizzle the result back to GX-tiled
// format for a single plain Menu_DrawImg565() call per frame. This
// replaces an earlier "poor man's blur" (same texture drawn 4x at small
// offsets with reduced alpha) which was both a weaker blur and entangled
// scale/position math with draw-time parameters whose semantics weren't
// fully verified - see chat history. Unswizzling/re-swizzling use the
// exact same tile-addressing formula already proven correct in
// TakeScreenshot() / SwizzleLinearToGXTiled() (video.cpp) - not
// re-derived here, just reapplied.
#define BLUR_RADIUS 4          // 9-tap box blur (radius*2+1), matching upstream's blurAmount=4
#define BLUR_OVERLAY_R 50
#define BLUR_OVERLAY_G 50
#define BLUR_OVERLAY_B 50
#define BLUR_OVERLAY_ALPHA 160 // 0-255, how strongly the dark tint is blended in

static u8 * gameScreenBlurred = NULL;  // full-screen (screenwidth x screenheight) GX-tiled RGB565 buffer, built once

// Unswizzles a width x height region from a GX-tiled RGB565 buffer (tiled
// at tiledStride pixels/row) into a plain linear RGB565 buffer. Same
// addressing as TakeScreenshot()'s unswizzle loop in video.cpp.
static void UnswizzleTiledRGB565ToLinear(const u8 *tiled, u16 *linearOut, int width, int height, int tiledStride)
{
	const u16 *tiledPx = (const u16 *)tiled;
	int blocksPerRow = tiledStride / 4;
	for (int y = 0; y < height; y++) {
		for (int x = 0; x < width; x++) {
			int blockIndex = (y / 4) * blocksPerRow + (x / 4);
			int wr = y % 4, wc = x % 4;
			linearOut[y * width + x] = tiledPx[blockIndex * 16 + wr * 4 + wc];
		}
	}
}

// Builds gameScreenBlurred from gameScreenTexture/gameScreenPng (must
// already be set up by the caller). Frees any previous gameScreenBlurred.
// No-op (leaves gameScreenBlurred NULL) on allocation failure.
static void BuildBlurredPauseScreen()
{
	if (gameScreenBlurred) {
		free(gameScreenBlurred);
		gameScreenBlurred = NULL;
	}

	int nativeW = gameScreenPng.nativeW, nativeH = gameScreenPng.nativeH;
	if (nativeW <= 0 || nativeH <= 0)
		return;

	// Step 1: unswizzle the small screenshot to a plain linear buffer.
	u16 *smallLinear = (u16 *)malloc(nativeW * nativeH * 2);
	if (!smallLinear) return;
	UnswizzleTiledRGB565ToLinear(gameScreenTexture, smallLinear, nativeW, nativeH, gameScreenTexW);

	// Step 2: nearest-neighbor upscale + composite onto a full-screen
	// linear RGB565 canvas, positioned/sized to match how the game was
	// actually being shown on screen at capture time. Working buffer is
	// unpacked to one byte per channel (R8G8B8) so the box blur below can
	// do plain integer averaging without corrupting RGB565's bit-packed
	// fields - repacked to RGB565 only at the very end.
	u8 *canvas = (u8 *)calloc((size_t)screenwidth * screenheight * 3, 1); // black outside the game area
	if (!canvas) { free(smallLinear); return; }

	int dstX0 = (int)gameScreenDrawX, dstY0 = (int)gameScreenDrawY;
	int dstW = (int)(nativeW * gameScreenScaleX);
	int dstH = (int)(nativeH * gameScreenScaleY);
	if (dstW < 1) dstW = 1;
	if (dstH < 1) dstH = 1;

	for (int dy = 0; dy < dstH; dy++) {
		int dstY = dstY0 + dy;
		if (dstY < 0 || dstY >= screenheight) continue;
		int srcY = (int)(dy / gameScreenScaleY);
		if (srcY >= nativeH) srcY = nativeH - 1;
		for (int dx = 0; dx < dstW; dx++) {
			int dstX = dstX0 + dx;
			if (dstX < 0 || dstX >= screenwidth) continue;
			int srcX = (int)(dx / gameScreenScaleX);
			if (srcX >= nativeW) srcX = nativeW - 1;

			u16 px = smallLinear[srcY * nativeW + srcX];
			int r5 = (px >> 11) & 0x1F, g6 = (px >> 5) & 0x3F, b5 = px & 0x1F;
			u8 *dstPx = canvas + (dstY * screenwidth + dstX) * 3;
			dstPx[0] = (r5 << 3) | (r5 >> 2);
			dstPx[1] = (g6 << 2) | (g6 >> 4);
			dstPx[2] = (b5 << 3) | (b5 >> 2);
		}
	}
	free(smallLinear);

	// Step 3: separable box blur (horizontal pass, then vertical pass),
	// clamp-to-edge at the canvas boundary. BLUR_RADIUS=4 -> 9-tap average,
	// matching upstream's blurAmount=4.
	u8 *tmp = (u8 *)malloc((size_t)screenwidth * screenheight * 3);
	if (!tmp) { free(canvas); return; }

	// Horizontal pass: canvas -> tmp
	for (int y = 0; y < screenheight; y++) {
		u8 *srcRow = canvas + (size_t)y * screenwidth * 3;
		u8 *dstRow = tmp + (size_t)y * screenwidth * 3;
		for (int x = 0; x < screenwidth; x++) {
			int sumR = 0, sumG = 0, sumB = 0, count = 0;
			for (int k = -BLUR_RADIUS; k <= BLUR_RADIUS; k++) {
				int sx = x + k;
				if (sx < 0) sx = 0;
				if (sx >= screenwidth) sx = screenwidth - 1;
				sumR += srcRow[sx * 3 + 0];
				sumG += srcRow[sx * 3 + 1];
				sumB += srcRow[sx * 3 + 2];
				count++;
			}
			dstRow[x * 3 + 0] = sumR / count;
			dstRow[x * 3 + 1] = sumG / count;
			dstRow[x * 3 + 2] = sumB / count;
		}
	}
	// Vertical pass: tmp -> canvas (reuse canvas as the final buffer)
	for (int x = 0; x < screenwidth; x++) {
		for (int y = 0; y < screenheight; y++) {
			int sumR = 0, sumG = 0, sumB = 0, count = 0;
			for (int k = -BLUR_RADIUS; k <= BLUR_RADIUS; k++) {
				int sy = y + k;
				if (sy < 0) sy = 0;
				if (sy >= screenheight) sy = screenheight - 1;
				u8 *srcPx = tmp + ((size_t)sy * screenwidth + x) * 3;
				sumR += srcPx[0];
				sumG += srcPx[1];
				sumB += srcPx[2];
				count++;
			}
			u8 *dstPx = canvas + ((size_t)y * screenwidth + x) * 3;
			dstPx[0] = sumR / count;
			dstPx[1] = sumG / count;
			dstPx[2] = sumB / count;
		}
	}
	free(tmp);

	// Step 4: blend in the dark overlay tint, then pack RGB888 -> RGB565
	// directly into a linear u16 buffer.
	u16 *finalLinear = (u16 *)malloc((size_t)screenwidth * screenheight * 2);
	if (!finalLinear) { free(canvas); return; }

	for (int i = 0; i < screenwidth * screenheight; i++) {
		int r = canvas[i * 3 + 0], g = canvas[i * 3 + 1], b = canvas[i * 3 + 2];
		r = (r * (255 - BLUR_OVERLAY_ALPHA) + BLUR_OVERLAY_R * BLUR_OVERLAY_ALPHA) / 255;
		g = (g * (255 - BLUR_OVERLAY_ALPHA) + BLUR_OVERLAY_G * BLUR_OVERLAY_ALPHA) / 255;
		b = (b * (255 - BLUR_OVERLAY_ALPHA) + BLUR_OVERLAY_B * BLUR_OVERLAY_ALPHA) / 255;
		finalLinear[i] = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
	}
	free(canvas);

	// Step 5: re-swizzle to GX-tiled format using the same proven function
	// gameplay itself depends on every frame - not a new/unverified path.
	u8 *tiled = (u8 *)memalign(32, (size_t)screenwidth * screenheight * 2);
	if (!tiled) { free(finalLinear); return; }
	SwizzleLinearToGXTiled((const u8 *)finalLinear, tiled, screenwidth, screenheight, screenwidth * 2);
	free(finalLinear);

	gameScreenBlurred = tiled;
}

// Draws the pre-built full-screen blurred buffer, plain and unscaled - all
// scaling/positioning/blur/overlay work already happened once in
// BuildBlurredPauseScreen(). No-op if that hasn't produced a buffer yet
// (gameScreenIsBlurred false, or build failed) - callers don't need to
// check that themselves.
static inline void DrawGameScreenBlur()
{
	if (!gameScreenIsBlurred || !gameScreenBlurred) return;
	Menu_DrawImg565(0, 0, screenwidth, screenheight, gameScreenBlurred, 0, 1.0f, 1.0f, 255);
}
static GuiImage * bgTopImg = NULL;
static GuiImage * bgBottomImg = NULL;
static GuiSound * bgMusic = NULL;
static GuiSound * enterSound = NULL;
static GuiSound * exitSound = NULL;
static GuiWindow * mainWindow = NULL;
static GuiText * settingText = NULL;
static GuiText * settingText2 = NULL;
static int lastMenu = MENU_NONE;
static int mapMenuCtrl = 0;

static lwp_t guithread = LWP_THREAD_NULL;
static lwp_t progressthread = LWP_THREAD_NULL;
static volatile bool guiHalt = true;
static volatile int showProgress = 0;

// GUI thread synchronization
static mutex_t guiMutex    = LWP_MUTEX_NULL;
static cond_t  guiHaltCond = LWP_COND_NULL; // GUI thread -> main: halted
static cond_t  guiWakeCond = LWP_COND_NULL; // main -> GUI thread: resume
static bool    guiHalted   = false;          // protected by guiMutex

// progress thread synchronization
static mutex_t progMutex      = LWP_MUTEX_NULL;
static cond_t  progActiveCond = LWP_COND_NULL; // main -> progress: work available
static cond_t  progIdleCond   = LWP_COND_NULL; // progress -> main: now idle
static bool    progIdle       = true;           // protected by progMutex

static char progressTitle[101];
static char progressMsg[201];
static int progressDone = 0;
static int progressTotal = 0;

u8 * bg_music;
u32 bg_music_size;

/****************************************************************************
 * ResumeGui
 *
 * Signals the GUI thread to start, and resumes the thread. This is called
 * after finishing the removal/insertion of new elements, and after initial
 * GUI setup.
 ***************************************************************************/
static void
ResumeGui()
{
	LWP_MutexLock(guiMutex);
	guiHalt = false;
	LWP_CondSignal(guiWakeCond);
	LWP_MutexUnlock(guiMutex);
}

/****************************************************************************
 * HaltGui
 *
 * Signals the GUI thread to stop, and waits for GUI thread to stop
 * This is necessary whenever removing/inserting new elements into the GUI.
 * This eliminates the possibility that the GUI is in the middle of accessing
 * an element that is being changed.
 ***************************************************************************/
static void
HaltGui()
{
	LWP_MutexLock(guiMutex);
	guiHalt = true;
	while(!guiHalted)
		LWP_CondWait(guiHaltCond, guiMutex);
	LWP_MutexUnlock(guiMutex);
}

static void ResetText()
{
	LoadLanguage();

	if(mainWindow)
	{
		HaltGui();
		mainWindow->ResetText();
		ResumeGui();
	}
}

static int currentLanguage = -1;

void ChangeLanguage() {
	if(currentLanguage == GCSettings.language) {
		return;
	}

	if(GCSettings.language == LANG_JAPANESE || GCSettings.language == LANG_KOREAN || GCSettings.language == LANG_SIMP_CHINESE) {
#ifdef HW_RVL
		char filepath[MAXPATHLEN];

		switch(GCSettings.language) {
			case LANG_KOREAN:
				sprintf(filepath, "%s/ko.ttf", appPath);
				break;
			case LANG_JAPANESE:
				sprintf(filepath, "%s/jp.ttf", appPath);
				break;
			case LANG_SIMP_CHINESE:
				sprintf(filepath, "%s/zh.ttf", appPath);
				break;
		}

		size_t fontSize = LoadFont(filepath);

		if(fontSize > 0) {
			HaltGui();
			DeinitFreeType();
			InitFreeType((u8*)ext_font_ttf, fontSize);
		}
		else {
			GCSettings.language = currentLanguage;
		}
#else
	GCSettings.language = currentLanguage;
	ErrorPrompt("Unsupported language!");
#endif
	}
#ifdef HW_RVL
	else {
		if(ext_font_ttf != NULL) {
			HaltGui();
			DeinitFreeType();
			mem2_free(ext_font_ttf);
			ext_font_ttf = NULL;
			InitFreeType((u8*)font_ttf, font_ttf_size);
		}
	}
#endif
	ResetText();
	currentLanguage = GCSettings.language;
}

/****************************************************************************
 * WindowPrompt
 *
 * Displays a prompt window to user, with information, an error message, or
 * presenting a user with a choice
 ***************************************************************************/
int
WindowPrompt(const char *title, const char *msg, const char *btn1Label, const char *btn2Label, bool btn1Default)
{
	if(!mainWindow || ExitRequested || ShutdownRequested)
		return 0;

	int choice = -1;

	GuiWindow promptWindow(448,288);
	promptWindow.SetAlignment(ALIGN_CENTRE, ALIGN_MIDDLE);
	promptWindow.SetPosition(0, -10);
	GuiSound btnSoundOver(button_over_pcm, button_over_pcm_size, SOUND_PCM);
	GuiSound btnSoundClick(button_click_pcm, button_click_pcm_size, SOUND_PCM);
	GuiImageData btnOutline(button_prompt_png);
	GuiImageData btnOutlineOver(button_prompt_over_png);

	GuiImageData dialogBox(dialogue_box_png);
	GuiImage dialogBoxImg(&dialogBox);

	GuiText titleTxt(title, 26, (GXColor){255, 255, 255, 255});
	titleTxt.SetAlignment(ALIGN_CENTRE, ALIGN_TOP);
	titleTxt.SetPosition(0,14);
	GuiText msgTxt(msg, 26, (GXColor){0, 0, 0, 255});
	msgTxt.SetAlignment(ALIGN_CENTRE, ALIGN_MIDDLE);
	msgTxt.SetPosition(0,-20);
	msgTxt.SetWrap(true, 430);

	GuiText btn1Txt(btn1Label, 22, (GXColor){0, 0, 0, 255});
	GuiImage btn1Img(&btnOutline);
	GuiImage btn1ImgOver(&btnOutlineOver);
	GuiButton btn1(btnOutline.GetWidth(), btnOutline.GetHeight());

	if(btn2Label)
	{
		btn1.SetAlignment(ALIGN_LEFT, ALIGN_BOTTOM);
		btn1.SetPosition(20, -25);
	}
	else
	{
		btn1.SetAlignment(ALIGN_CENTRE, ALIGN_BOTTOM);
		btn1.SetPosition(0, -25);
	}

	btn1.SetLabel(&btn1Txt);
	btn1.SetImage(&btn1Img);
	btn1.SetImageOver(&btn1ImgOver);
	btn1.SetSoundOver(&btnSoundOver);
	btn1.SetSoundClick(&btnSoundClick);
	btn1.SetTrigger(trigA);
	btn1.SetTrigger(trig2);
	btn1.SetState(STATE_SELECTED);
	btn1.SetEffectGrow();

	GuiText btn2Txt(btn2Label, 22, (GXColor){0, 0, 0, 255});
	GuiImage btn2Img(&btnOutline);
	GuiImage btn2ImgOver(&btnOutlineOver);
	GuiButton btn2(btnOutline.GetWidth(), btnOutline.GetHeight());
	btn2.SetAlignment(ALIGN_RIGHT, ALIGN_BOTTOM);
	btn2.SetPosition(-20, -25);
	btn2.SetLabel(&btn2Txt);
	btn2.SetImage(&btn2Img);
	btn2.SetImageOver(&btn2ImgOver);
	btn2.SetSoundOver(&btnSoundOver);
	btn2.SetSoundClick(&btnSoundClick);
	btn2.SetTrigger(trigA);
	btn2.SetTrigger(trig2);
	btn2.SetEffectGrow();

	promptWindow.Append(&dialogBoxImg);
	promptWindow.Append(&titleTxt);
	promptWindow.Append(&msgTxt);
	promptWindow.Append(&btn1);

	if(btn2Label)
		promptWindow.Append(&btn2);

	promptWindow.SetEffect(EFFECT_SLIDE_TOP | EFFECT_SLIDE_IN, 50);
	CancelAction();
	HaltGui();
	mainWindow->SetState(STATE_DISABLED);
	mainWindow->Append(&promptWindow);
	mainWindow->ChangeFocus(&promptWindow);
	if(btn2Label)
	{
		if (btn1Default)
		{
			btn2.ResetState();
			btn1.SetState(STATE_SELECTED);
		}
		else
		{
			btn1.ResetState();
			btn2.SetState(STATE_SELECTED);
		}
	}
	ResumeGui();

	while(choice == -1)
	{
		usleep(THREAD_SLEEP);

		if(btn1.GetState() == STATE_CLICKED)
			choice = 1;
		else if(btn2.GetState() == STATE_CLICKED)
			choice = 0;
	}

	promptWindow.SetEffect(EFFECT_SLIDE_TOP | EFFECT_SLIDE_OUT, 50);
	while(promptWindow.GetEffect() > 0) usleep(THREAD_SLEEP);
	HaltGui();
	mainWindow->Remove(&promptWindow);
	mainWindow->SetState(STATE_DEFAULT);
	ResumeGui();
	return choice;
}

int
WindowPrompt(const char *title, const char *msg, const char *btn1Label, const char *btn2Label)
{
	return WindowPrompt(title, msg, btn1Label, btn2Label, false);
}

/****************************************************************************
 * UpdateGUI
 *
 * Primary thread to allow GUI to respond to state changes, and draws GUI
 ***************************************************************************/
static void *
UpdateGUI (void *arg)
{
	int i;

	while(1)
	{
		// if halted, block here until ResumeGui wakes us; signal HaltGui we have stopped
		LWP_MutexLock(guiMutex);
		if(guiHalt)
		{
			guiHalted = true;
			LWP_CondBroadcast(guiHaltCond);
			while(guiHalt)
				LWP_CondWait(guiWakeCond, guiMutex);
			guiHalted = false;
		}
		LWP_MutexUnlock(guiMutex);

		UpdatePads();
		DrawGameScreenBlur();
		mainWindow->Draw();

		if (mainWindow->GetState() != STATE_DISABLED)
			mainWindow->DrawTooltip();

		#ifdef HW_RVL
		i = 3;
		do
		{
			if(userInput[i].wpad->ir.valid)
				Menu_DrawImg(userInput[i].wpad->ir.x-48, userInput[i].wpad->ir.y-48,
					96, 96, pointer[i]->GetImage(), userInput[i].wpad->ir.angle, 1, 1, 255);
			DoRumble(i);
			--i;
		} while(i>=0);
		#endif

		Menu_Render();

		mainWindow->Update(&userInput[3]);
		mainWindow->Update(&userInput[2]);
		mainWindow->Update(&userInput[1]);
		mainWindow->Update(&userInput[0]);

		if(ExitRequested || ShutdownRequested)
		{
			for(i = 0; i <= 255; i += 15)
			{
				DrawGameScreenBlur();
				mainWindow->Draw();
				Menu_DrawRectangle(0,0,screenwidth,screenheight,(GXColor){0, 0, 0, (u8)i},1);
				Menu_Render();
			}
			ExitApp();
		}
		usleep(THREAD_SLEEP);
	}
	return NULL;
}

/****************************************************************************
 * ProgressWindow
 *
 * Opens a window, which displays progress to the user. Can either display a
 * progress bar showing % completion, or a throbber that only shows that an
 * action is in progress.
 ***************************************************************************/
static void
ProgressWindow(char *title, char *msg)
{
	GuiWindow promptWindow(448,288);
	promptWindow.SetAlignment(ALIGN_CENTRE, ALIGN_MIDDLE);
	promptWindow.SetPosition(0, -10);
	GuiSound btnSoundOver(button_over_pcm, button_over_pcm_size, SOUND_PCM);
	GuiSound btnSoundClick(button_click_pcm, button_click_pcm_size, SOUND_PCM);
	GuiImageData btnOutline(button_png);
	GuiImageData btnOutlineOver(button_over_png);

	GuiImageData dialogBox(dialogue_box_png);
	GuiImage dialogBoxImg(&dialogBox);

	GuiImageData progressbarOutline(progressbar_outline_png);
	GuiImage progressbarOutlineImg(&progressbarOutline);
	progressbarOutlineImg.SetAlignment(ALIGN_LEFT, ALIGN_MIDDLE);
	progressbarOutlineImg.SetPosition(25, 40);

	GuiImageData progressbarEmpty(progressbar_empty_png);
	GuiImage progressbarEmptyImg(&progressbarEmpty);
	progressbarEmptyImg.SetAlignment(ALIGN_LEFT, ALIGN_MIDDLE);
	progressbarEmptyImg.SetPosition(25, 40);
	progressbarEmptyImg.SetTile(100);

	GuiImageData progressbar(progressbar_png);
	GuiImage progressbarImg(&progressbar);
	progressbarImg.SetAlignment(ALIGN_LEFT, ALIGN_MIDDLE);
	progressbarImg.SetPosition(25, 40);

	GuiImageData throbber(throbber_png);
	GuiImage throbberImg(&throbber);
	throbberImg.SetAlignment(ALIGN_CENTRE, ALIGN_MIDDLE);
	throbberImg.SetPosition(0, 40);

	GuiText titleTxt(title, 26, (GXColor){255, 255, 255, 255});
	titleTxt.SetAlignment(ALIGN_CENTRE, ALIGN_TOP);
	titleTxt.SetPosition(0,14);
	GuiText msgTxt(msg, 26, (GXColor){0, 0, 0, 255});
	msgTxt.SetAlignment(ALIGN_CENTRE, ALIGN_TOP);
	msgTxt.SetPosition(0,80);

	promptWindow.Append(&dialogBoxImg);
	promptWindow.Append(&titleTxt);
	promptWindow.Append(&msgTxt);

	if(showProgress == 1)
	{
		promptWindow.Append(&progressbarEmptyImg);
		promptWindow.Append(&progressbarImg);
		promptWindow.Append(&progressbarOutlineImg);
	}
	else
	{
		promptWindow.Append(&throbberImg);
	}

	// wait to see if progress flag changes soon
	int progsleep = 800000;

	while(progsleep > 0)
	{
		if(!showProgress)
			break;
		usleep(THREAD_SLEEP);
		progsleep -= THREAD_SLEEP;
	}

	if(!showProgress)
		return;

	HaltGui();
	int oldState = mainWindow->GetState();
	mainWindow->SetState(STATE_DISABLED);
	mainWindow->Append(&promptWindow);
	mainWindow->ChangeFocus(&promptWindow);
	ResumeGui();

	float angle = 0;
	u32 count = 0;

	while(showProgress)
	{
		int progsleep = 20000;

		while(progsleep > 0)
		{
			if(!showProgress)
				break;
			usleep(THREAD_SLEEP);
			progsleep -= THREAD_SLEEP;
		}

		if(showProgress == 1)
		{
			progressbarImg.SetTile(100*progressDone/progressTotal);
		}
		else if(showProgress == 2)
		{
			if(count % 5 == 0)
			{
				angle+=45.0f;
				if(angle >= 360.0f)
					angle = 0;
				throbberImg.SetAngle(angle);
			}
			++count;
		}
	}

	HaltGui();
	mainWindow->Remove(&promptWindow);
	mainWindow->SetState(oldState);
	ResumeGui();
}

static void * ProgressThread (void *arg)
{
	LWP_MutexLock(progMutex);
	while(1)
	{
		// sleep until ShowProgress/ShowAction signals there is work to do
		while(!showProgress)
			LWP_CondWait(progActiveCond, progMutex);
		progIdle = false;
		LWP_MutexUnlock(progMutex);

		ProgressWindow(progressTitle, progressMsg);

		LWP_MutexLock(progMutex);
		progIdle = true;
		LWP_CondBroadcast(progIdleCond); // wake CancelAction callers
	}
	return NULL;
}

/****************************************************************************
 * InitGUIThread
 *
 * Startup GUI threads
 ***************************************************************************/
void
InitGUIThreads()
{
	LWP_MutexInit(&guiMutex, false);
	LWP_CondInit(&guiHaltCond);
	LWP_CondInit(&guiWakeCond);

	LWP_MutexInit(&progMutex, false);
	LWP_CondInit(&progActiveCond);
	LWP_CondInit(&progIdleCond);

	LWP_CreateThread(&guithread, UpdateGUI, NULL, NULL, 24576, 70);
	LWP_CreateThread(&progressthread, ProgressThread, NULL, NULL, 0, 40);
}

/****************************************************************************
 * CancelAction
 *
 * Signals the GUI progress window thread to halt, and waits for it to
 * finish. Prevents multiple progress window events from interfering /
 * overriding each other.
 ***************************************************************************/
void
CancelAction()
{
	LWP_MutexLock(progMutex);
	showProgress = 0;
	while(!progIdle)
		LWP_CondWait(progIdleCond, progMutex);
	LWP_MutexUnlock(progMutex);
}

/****************************************************************************
 * ShowProgress
 *
 * Updates the variables used by the progress window for drawing a progress
 * bar. Also resumes the progress window thread if it is suspended.
 ***************************************************************************/
void
ShowProgress (const char *msg, int done, int total)
{
	if(!mainWindow || ExitRequested || ShutdownRequested)
		return;

	if(total < (256*1024))
		return;
	else if(done > total) // this shouldn't happen
		done = total;

	if(done/total > 0.99)
		done = total;

	if(showProgress != 1)
		CancelAction(); // wait for previous progress window to finish

	LWP_MutexLock(progMutex);
	snprintf(progressMsg, 200, "%s", msg);
	sprintf(progressTitle, "Please Wait");
	showProgress = 1;
	progressTotal = total;
	progressDone = done;
	LWP_CondSignal(progActiveCond);
	LWP_MutexUnlock(progMutex);
}

/****************************************************************************
 * ShowAction
 *
 * Shows that an action is underway. Also resumes the progress window thread
 * if it is suspended.
 ***************************************************************************/
void
ShowAction (const char *msg)
{
	if(!mainWindow || ExitRequested || ShutdownRequested)
		return;

	if(showProgress != 0)
		CancelAction(); // wait for previous progress window to finish

	LWP_MutexLock(progMutex);
	snprintf(progressMsg, 200, "%s", msg);
	sprintf(progressTitle, "Please Wait");
	showProgress = 2;
	progressDone = 0;
	progressTotal = 0;
	LWP_CondSignal(progActiveCond);
	LWP_MutexUnlock(progMutex);
}

void ErrorPrompt(const char *msg)
{
	WindowPrompt("Error", msg, "OK", NULL);
}

int ErrorPromptRetry(const char *msg)
{
	return WindowPrompt("Error", msg, "Retry", "Cancel");
}

void InfoPrompt(const char *msg)
{
	WindowPrompt("Information", msg, "OK", NULL);
}

int YesNoPrompt(const char *msg, bool yesDefault)
{
	return WindowPrompt("Goomba", msg, "Yes", "No", yesDefault);
}

/****************************************************************************
 * OnScreenKeyboard
 *
 * Opens an on-screen keyboard window, with the data entered being stored
 * into the specified variable.
 ***************************************************************************/
static void OnScreenKeyboard(char * var, u32 maxlen)
{
	int save = -1;

	GuiKeyboard keyboard(var, maxlen);

	GuiSound btnSoundOver(button_over_pcm, button_over_pcm_size, SOUND_PCM);
	GuiSound btnSoundClick(button_click_pcm, button_click_pcm_size, SOUND_PCM);
	GuiImageData btnOutline(button_png);
	GuiImageData btnOutlineOver(button_over_png);

	GuiText okBtnTxt("OK", 22, (GXColor){0, 0, 0, 255});
	GuiImage okBtnImg(&btnOutline);
	GuiImage okBtnImgOver(&btnOutlineOver);
	GuiButton okBtn(btnOutline.GetWidth(), btnOutline.GetHeight());

	okBtn.SetAlignment(ALIGN_LEFT, ALIGN_BOTTOM);
	okBtn.SetPosition(25, -25);

	okBtn.SetLabel(&okBtnTxt);
	okBtn.SetImage(&okBtnImg);
	okBtn.SetImageOver(&okBtnImgOver);
	okBtn.SetSoundOver(&btnSoundOver);
	okBtn.SetSoundClick(&btnSoundClick);
	okBtn.SetTrigger(trigA);
	okBtn.SetEffectGrow();

	GuiText cancelBtnTxt("Cancel", 22, (GXColor){0, 0, 0, 255});
	GuiImage cancelBtnImg(&btnOutline);
	GuiImage cancelBtnImgOver(&btnOutlineOver);
	GuiButton cancelBtn(btnOutline.GetWidth(), btnOutline.GetHeight());
	cancelBtn.SetAlignment(ALIGN_RIGHT, ALIGN_BOTTOM);
	cancelBtn.SetPosition(-25, -25);
	cancelBtn.SetLabel(&cancelBtnTxt);
	cancelBtn.SetImage(&cancelBtnImg);
	cancelBtn.SetImageOver(&cancelBtnImgOver);
	cancelBtn.SetSoundOver(&btnSoundOver);
	cancelBtn.SetSoundClick(&btnSoundClick);
	cancelBtn.SetTrigger(trigA);
	cancelBtn.SetTrigger(trigB);
	cancelBtn.SetEffectGrow();

	keyboard.Append(&okBtn);
	keyboard.Append(&cancelBtn);

	HaltGui();
	mainWindow->SetState(STATE_DISABLED);
	mainWindow->Append(&keyboard);
	mainWindow->ChangeFocus(&keyboard);
	ResumeGui();

	while(save == -1)
	{
		usleep(THREAD_SLEEP);

		if(okBtn.GetState() == STATE_CLICKED)
			save = 1;
		else if(cancelBtn.GetState() == STATE_CLICKED)
			save = 0;
	}

	if(save)
	{
		snprintf(var, maxlen, "%s", keyboard.kbtextstr);
	}

	HaltGui();
	mainWindow->Remove(&keyboard);
	mainWindow->SetState(STATE_DEFAULT);
	ResumeGui();
}

/****************************************************************************
 * SettingWindow
 *
 * Opens a new window, with the specified window element appended. Allows
 * for a customizable prompted setting.
 ***************************************************************************/
static int
SettingWindow(const char * title, GuiWindow * w)
{
	int save = -1;

	GuiWindow promptWindow(448,288);
	promptWindow.SetAlignment(ALIGN_CENTRE, ALIGN_MIDDLE);
	GuiSound btnSoundOver(button_over_pcm, button_over_pcm_size, SOUND_PCM);
	GuiSound btnSoundClick(button_click_pcm, button_click_pcm_size, SOUND_PCM);
	GuiImageData btnOutline(button_png);
	GuiImageData btnOutlineOver(button_over_png);

	GuiImageData dialogBox(dialogue_box_png);
	GuiImage dialogBoxImg(&dialogBox);

	GuiText titleTxt(title, 26, (GXColor){255, 255, 255, 255});
	titleTxt.SetAlignment(ALIGN_CENTRE, ALIGN_TOP);
	titleTxt.SetPosition(0,14);

	GuiText okBtnTxt("OK", 22, (GXColor){0, 0, 0, 255});
	GuiImage okBtnImg(&btnOutline);
	GuiImage okBtnImgOver(&btnOutlineOver);
	GuiButton okBtn(btnOutline.GetWidth(), btnOutline.GetHeight());

	okBtn.SetAlignment(ALIGN_LEFT, ALIGN_BOTTOM);
	okBtn.SetPosition(20, -25);

	okBtn.SetLabel(&okBtnTxt);
	okBtn.SetImage(&okBtnImg);
	okBtn.SetImageOver(&okBtnImgOver);
	okBtn.SetSoundOver(&btnSoundOver);
	okBtn.SetSoundClick(&btnSoundClick);
	okBtn.SetTrigger(trigA);
	// Focus-independent Confirm, mirroring cancelBtn's focus-independent
	// trigB below (both use SetButtonOnlyTrigger - fires regardless of
	// what currently has focus, unlike trigA which only fires when okBtn
	// itself is focused/selected). Needed because some SettingWindow
	// content (e.g. ScreenZoomWindow's four zoom arrows) legitimately
	// consumes all four D-pad directions for its own adjustment, leaving
	// no D-pad direction free to shift focus off that content and onto
	// okBtn on a GameCube or Classic Controller - Wiimote IR pointing
	// isn't affected since it clicks okBtn directly regardless of focus.
	// Without this, okBtn's trigA can never fire on those screens and
	// there's no way to confirm/save with a digital-only controller.
	GuiTrigger trigConfirm;
	trigConfirm.SetButtonOnlyTrigger(-1, WPAD_BUTTON_PLUS | WPAD_CLASSIC_BUTTON_PLUS, PAD_TRIGGER_Z, WIIDRC_BUTTON_PLUS);
	okBtn.SetTrigger(&trigConfirm);
	okBtn.SetEffectGrow();

	GuiText cancelBtnTxt("Cancel", 22, (GXColor){0, 0, 0, 255});
	GuiImage cancelBtnImg(&btnOutline);
	GuiImage cancelBtnImgOver(&btnOutlineOver);
	GuiButton cancelBtn(btnOutline.GetWidth(), btnOutline.GetHeight());
	cancelBtn.SetAlignment(ALIGN_RIGHT, ALIGN_BOTTOM);
	cancelBtn.SetPosition(-20, -25);
	cancelBtn.SetLabel(&cancelBtnTxt);
	cancelBtn.SetImage(&cancelBtnImg);
	cancelBtn.SetImageOver(&cancelBtnImgOver);
	cancelBtn.SetSoundOver(&btnSoundOver);
	cancelBtn.SetSoundClick(&btnSoundClick);
	cancelBtn.SetTrigger(trigA);
	// No focus-independent trigB here (unlike WindowPrompt's cancelBtn) -
	// deliberately left off so B falls through to mainWindow's existing
	// ToggleFocus() (gui_window.cpp), which already cycles focus between
	// mainWindow's direct children (promptWindow <-> w) on B/1. With
	// trigB bound directly to this button, B fired an immediate cancel
	// before that cross-window focus shift could ever be seen - which is
	// also what made it impossible to ever reach okBtn/cancelBtn by pad
	// on screens like ScreenZoomWindow, whose content consumes all four
	// D-pad directions. Without it: B moves focus from the content
	// window onto this promptWindow (landing on okBtn - see
	// MoveSelectionVert's left-distance tie-break in gui_window.cpp),
	// Left/Right then moves between okBtn and cancelBtn, and A confirms
	// whichever is selected. A second B press cycles focus back to the
	// content window to keep adjusting.
	cancelBtn.SetEffectGrow();

	promptWindow.Append(&dialogBoxImg);
	promptWindow.Append(&titleTxt);
	promptWindow.Append(&okBtn);
	promptWindow.Append(&cancelBtn);

	HaltGui();
	mainWindow->SetState(STATE_DISABLED);
	mainWindow->Append(&promptWindow);
	mainWindow->Append(w);
	mainWindow->ChangeFocus(w);
	ResumeGui();

	while(save == -1)
	{
		usleep(THREAD_SLEEP);

		if(okBtn.GetState() == STATE_CLICKED)
			save = 1;
		else if(cancelBtn.GetState() == STATE_CLICKED)
			save = 0;
	}
	HaltGui();
	mainWindow->Remove(&promptWindow);
	mainWindow->Remove(w);
	mainWindow->SetState(STATE_DEFAULT);
	ResumeGui();
	return save;
}

/****************************************************************************
 * WindowCredits
 * Display credits, legal copyright and licence
 *
 * THIS MUST NOT BE REMOVED OR DISABLED IN ANY DERIVATIVE WORK
 ***************************************************************************/
static void WindowCredits(void * ptr)
{
	if(btnLogo->GetState() != STATE_CLICKED)
		return;

	btnLogo->ResetState();

	bool exit = false;
	int i = 0;
	int y = 20;

	GuiWindow creditsWindow(screenwidth,screenheight);
	GuiWindow creditsWindowBox(580,448);
	creditsWindowBox.SetAlignment(ALIGN_CENTRE, ALIGN_MIDDLE);

	GuiImageData creditsBox(credits_box_png);
	GuiImage creditsBoxImg(&creditsBox);
	creditsBoxImg.SetAlignment(ALIGN_CENTRE, ALIGN_MIDDLE);
	creditsWindowBox.Append(&creditsBoxImg);

	int numEntries = 24;
	GuiText * txt[numEntries];

	txt[i] = new GuiText("mGBA-GX Credits", 20, (GXColor){0, 0, 0, 255});
	txt[i]->SetAlignment(ALIGN_CENTRE, ALIGN_TOP); txt[i]->SetPosition(0,y); i++; y+=32;

	txt[i] = new GuiText("Official Site: https://github.com/nateynaate/mgba-gx", 20, (GXColor){0, 0, 0, 255});
	txt[i]->SetAlignment(ALIGN_CENTRE, ALIGN_TOP); txt[i]->SetPosition(0,y); i++; y+=32;

	GuiText::SetPresets(20, (GXColor){0, 0, 0, 255}, 0, FTGX_JUSTIFY_LEFT | FTGX_ALIGN_TOP, ALIGN_LEFT, ALIGN_TOP);

	txt[i] = new GuiText("Emulation core (mGBA)");
	txt[i]->SetPosition(40,y); i++;
	txt[i] = new GuiText("endrift & mGBA contributors");
	txt[i]->SetPosition(250,y); i++; y+=24;

	txt[i] = new GuiText("Frontend (VBA-GX)");
	txt[i]->SetPosition(40,y); i++;
	txt[i] = new GuiText("Tantric");
	txt[i]->SetPosition(250,y); i++; y+=48;

	txt[i] = new GuiText("Additional coding");
	txt[i]->SetPosition(40,y); i++;
	txt[i] = new GuiText("Zopenko, Glitch, libertyernie");
	txt[i]->SetPosition(250,y); i++; y+=24;
	txt[i] = new GuiText("cebolleto, bgK, Carl Kenner, dancinninjac");
	txt[i]->SetPosition(250,y); i++; y+=48;

	txt[i] = new GuiText("Menu artwork");
	txt[i]->SetPosition(40,y); i++;
	txt[i] = new GuiText("the3seashells");
	txt[i]->SetPosition(250,y); i++; y+=24;
	txt[i] = new GuiText("Menu sound");
	txt[i]->SetPosition(40,y); i++;
	txt[i] = new GuiText("Peter de Man");
	txt[i]->SetPosition(250,y); i++; y+=32;

	txt[i] = new GuiText("VBA GameCube");
	txt[i]->SetPosition(40,y); i++;
	txt[i] = new GuiText("SoftDev, emukidid");
	txt[i]->SetPosition(250,y); i++; y+=24;
	txt[i] = new GuiText("Visual Boy Advance");
	txt[i]->SetPosition(40,y); i++;
	txt[i] = new GuiText("Forgotten");
	txt[i]->SetPosition(250,y); i++; y+=24;

	txt[i] = new GuiText("libogc / devkitPPC");
	txt[i]->SetPosition(40,y); i++;
	txt[i] = new GuiText("shagkur & WinterMute");
	txt[i]->SetPosition(250,y); i++; y+=24;
	txt[i] = new GuiText("FreeTypeGX");
	txt[i]->SetPosition(40,y); i++;
	txt[i] = new GuiText("Armin Tamzarian");
	txt[i]->SetPosition(250,y); i++;

	char wiiDetails[30];
	char wiiInfo[20];

#ifdef HW_RVL
	if(!IsWiiU()) {
		sprintf(wiiInfo, "Wii");
	}
	else if(IsWiiUFastCPU()) {
		sprintf(wiiInfo, "vWii (1.215 GHz)");
	}
	else {
		sprintf(wiiInfo, "vWii (729 MHz)");
	}
	sprintf(wiiDetails, "IOS: %d / %s", IOS_GetVersion(), wiiInfo);
#endif

	txt[i] = new GuiText(wiiDetails, 14, (GXColor){0, 0, 0, 255});
	txt[i]->SetAlignment(ALIGN_RIGHT, ALIGN_BOTTOM);
	txt[i]->SetPosition(-20, -46); i++;

	GuiText::SetPresets(12, (GXColor){0, 0, 0, 255}, 0, FTGX_JUSTIFY_CENTER | FTGX_ALIGN_TOP, ALIGN_CENTRE, ALIGN_BOTTOM);

	txt[i] = new GuiText("This software is open source and may be copied, distributed, or modified");
	txt[i]->SetPosition(0, -32); i++;
	txt[i] = new GuiText("under the terms of the GNU General Public License (GPL) Version 2.");
	txt[i]->SetPosition(0, -20);

	for(i=0; i < numEntries; i++)
		creditsWindowBox.Append(txt[i]);

	creditsWindow.Append(&creditsWindowBox);

	while(!exit)
	{
		UpdatePads();

		DrawGameScreenBlur();
		if (gameScreenImg) gameScreenImg->Draw(); // solid-color fallback case only - NULL when the real screenshot is active
		bgBottomImg->Draw();
		bgTopImg->Draw();
		creditsWindow.Draw();

		#ifdef HW_RVL
		i = 3;
		do {	
		if(userInput[i].wpad->ir.valid)
			Menu_DrawImg(userInput[i].wpad->ir.x-48, userInput[i].wpad->ir.y-48,
				96, 96, pointer[i]->GetImage(), userInput[i].wpad->ir.angle, 1, 1, 255);
			DoRumble(i);
			--i;
		} while(i >= 0);
		#endif

		Menu_Render();

		if((userInput[0].wpad->btns_d || userInput[0].pad.btns_d || userInput[0].wiidrcdata.btns_d) ||
		   (userInput[1].wpad->btns_d || userInput[1].pad.btns_d || userInput[1].wiidrcdata.btns_d) ||
		   (userInput[2].wpad->btns_d || userInput[2].pad.btns_d || userInput[2].wiidrcdata.btns_d) ||
		   (userInput[3].wpad->btns_d || userInput[3].pad.btns_d || userInput[3].wiidrcdata.btns_d))
		{
			exit = true;
		}
		usleep(THREAD_SLEEP);
	}

	// clear buttons pressed
	for(i=0; i < 4; i++)
	{
		userInput[i].wiidrcdata.btns_d = 0;
		userInput[i].wpad->btns_d = 0;
		userInput[i].pad.btns_d = 0;
	}

	for(i=0; i < numEntries; i++)
		delete txt[i];
}

/****************************************************************************
 * Recent Games list
 *
 * GCSettings.RecentROMs stores one entry per game, most-recent-first,
 * '|'-delimited, capped at MAX_RECENT_ROMS entries. Each entry is itself
 * "path<RECENT_FIELD_SEP>displayname<RECENT_FIELD_SEP>size" - the display
 * name and file size are cached at AddRecentROM() time (once, right after
 * a game successfully boots) rather than recomputed every time the Recent
 * tab is opened.
 *
 * RECENT_FIELD_SEP was originally '\x01' (on the theory that a raw control
 * byte could never collide with a real path or display name) - but this
 * is settings.xml field, and XML 1.0 forbids raw control characters
 * outright, not just ones that need escaping. There is no valid escape for
 * 0x01 in XML at all, so Mini-XML correctly refused to parse the file back
 * out on the very next boot after any game was added to the recent list -
 * and because that failure was a single mxmlLoadString() call for the
 * WHOLE settings.xml, it silently reset every setting to defaults on every
 * subsequent boot, not just RecentROMs. ':' gives the same "can't collide
 * with real content" guarantee (it's illegal in Windows/FAT/NTFS
 * filenames, so it's structurally impossible for a real path, or a display
 * name StripExt() derived from one, to contain it) while still being a
 * normal printable character XML has no trouble with.
 *
 * Previously PopulateRecentList() called ChangeInterface() + stat() for
 * every entry on every visit to the tab - up to MAX_RECENT_ROMS device-
 * interface switches and disk reads, sequentially, each time - which is
 * what made the tab noticeably slow to open. Caching the two pieces of
 * data that were actually being fetched (name, size) turns opening the
 * tab into pure string parsing, no device/disk I/O at all. The one-time
 * stat() this shifts onto ROM boot is comparatively free - it happens once,
 * off the critical path of anything the player is waiting on.
 *
 * Trade-off: an entry whose file has since been moved/deleted no longer
 * gets silently filtered out of the list (that required the stat() this
 * removes) - it'll still show, and selecting it will just fail to load,
 * same as any other missing-file case elsewhere in the browser.
 *
 * Old-format entries from before this change (bare paths, no separator
 * fields) are still handled - see the fallback in PopulateRecentList()
 * below - so an existing settings.xml with old-style recent entries
 * doesn't need to be reset, they just don't have cached name/size until
 * next time they're re-added to the front of the list.
 *
 * BUG FIX: RECENT_FIELD_SEP was ':' on the theory that colon can't appear
 * in a real path or display name (illegal on FAT/NTFS/Windows). That's
 * true of the filename itself, but every path in this app is device-
 * qualified with a colon right up front - "sd:/mgbagx/roms/gb/Foo.gb",
 * "usb:/...". strchr() finds THAT colon first, not the intended field
 * separator after the extension, so the split landed in the wrong place:
 * pathPart truncated to just "sd" (unloadable - selecting the entry did
 * nothing) while namePart absorbed the rest of the real path (displayed
 * in the Recent tab instead of a clean name - the "full file locations"
 * symptom). '*' keeps the same "structurally can't collide" guarantee
 * (also illegal in FAT/NTFS/Windows filenames) without colliding with the
 * device prefix scheme, and like ':' needs no XML escaping.
 ***************************************************************************/
#define RECENT_FIELD_SEP '*'

static int SplitRecentROMs(char *outPaths[MAX_RECENT_ROMS], char *buf, size_t bufSize)
{
	snprintf(buf, bufSize, "%s", GCSettings.RecentROMs);
	int count = 0;
	char *tok = strtok(buf, "|");
	while (tok && count < MAX_RECENT_ROMS) {
		outPaths[count++] = tok;
		tok = strtok(NULL, "|");
	}
	return count;
}

// Extracts just the path portion of a (possibly composite) recent-list
// token, for de-dupe comparisons - a token may be
// "path<RECENT_FIELD_SEP>name<RECENT_FIELD_SEP>size" (new format) or just
// "path" (old format/no cached fields yet).
static void RecentEntryPathOnly(const char *token, char *out, size_t outSize)
{
	const char *sep = strchr(token, RECENT_FIELD_SEP);
	size_t len = sep ? (size_t)(sep - token) : strlen(token);
	if (len >= outSize) len = outSize - 1;
	memcpy(out, token, len);
	out[len] = '\0';
}

// Extracts just the cached display name portion of a (possibly composite)
// recent-list token, for de-dupe comparisons - companion to
// RecentEntryPathOnly() above. Old-format entries (bare path, no cached
// name yet) fall back to deriving the name from the path the same way
// PopulateRecentList()/AddRecentROM() do everywhere else, so an old-format
// duplicate still gets recognized and merged rather than silently missed.
static void RecentEntryNameOnly(const char *token, char *out, size_t outSize)
{
	const char *sep1 = strchr(token, RECENT_FIELD_SEP);
	if (sep1) {
		const char *namePart = sep1 + 1;
		const char *sep2 = strchr(namePart, RECENT_FIELD_SEP);
		size_t len = sep2 ? (size_t)(sep2 - namePart) : strlen(namePart);
		if (len >= outSize) len = outSize - 1;
		memcpy(out, namePart, len);
		out[len] = '\0';
		return;
	}
	// Old format: derive from the path, same as PopulateRecentList()'s
	// own fallback.
	const char *base = strrchr(token, '/');
	base = base ? base + 1 : token;
	StripExt(out, base); // StripExt writes into out; caller-sized (MAXJOLIET+1) buffers assumed, same as everywhere else this pattern is used
}

// Adds fullPath to the front of the recent list, de-duplicating and capping
// at MAX_RECENT_ROMS entries. Computes and caches the display name + file
// size once here (see the block comment above for why).
static void AddRecentROM(const char *fullPath)
{
	char buf[MAXPATHLEN * MAX_RECENT_ROMS];
	char *paths[MAX_RECENT_ROMS];
	int count = SplitRecentROMs(paths, buf, sizeof(buf));

	char displayname[MAXJOLIET + 1];
	const char *base = strrchr(fullPath, '/');
	base = base ? base + 1 : fullPath;
	StripExt(displayname, base);

	long long fileLength = 0;
	struct stat st;
	if (stat(fullPath, &st) == 0)
		fileLength = (long long)st.st_size;

	char newList[MAXPATHLEN * MAX_RECENT_ROMS];
	int newLen = snprintf(newList, sizeof(newList), "%s%c%s%c%lld",
		fullPath, RECENT_FIELD_SEP, displayname, RECENT_FIELD_SEP, fileLength);
	int total = 1;

	for (int i = 0; i < count && total < MAX_RECENT_ROMS; i++) {
		char existingPath[MAXPATHLEN];
		char existingName[MAXJOLIET + 1];
		RecentEntryPathOnly(paths[i], existingPath, sizeof(existingPath));
		RecentEntryNameOnly(paths[i], existingName, sizeof(existingName));
		if (strcasecmp(existingPath, fullPath) == 0 ||
		    strcasecmp(existingName, displayname) == 0)
			continue; // de-dupe (by path OR by name) - fullPath is already at the front above

		// Re-emit the existing token as-is (whatever fields it already has,
		// old or new format) - no need to touch entries we're not adding.
		int added = snprintf(newList + newLen, sizeof(newList) - newLen, "|%s", paths[i]);
		if (added < 0 || added >= (int)(sizeof(newList) - newLen))
			break; // would overflow - stop here
		newLen += added;
		total++;
	}
	snprintf(GCSettings.RecentROMs, sizeof(GCSettings.RecentROMs), "%s", newList);

	// Save immediately rather than waiting for the user to visit Settings -
	// loading a game never routes through the Settings menu tree, so
	// without this the Recent list would only ever persist by accident.
	SavePrefs(SILENT);
}

// Populates browserList with the recent-games entries, purely from the
// cached path<RECENT_FIELD_SEP>name<RECENT_FIELD_SEP>size fields - see the
// block comment above for why this no longer touches the disk or switches
// device interfaces. Each entry's filename field holds its FULL path (not
// just a bare filename like normal directory listings) since entries can
// span multiple folders/devices - this is resolved back down to a normal
// browser.dir + filename pair right before loading (see the Recent-tab
// handling in MenuGameSelection).
static void PopulateRecentList()
{
	ResetBrowser();
	browser.dir[0] = 0;

	char buf[MAXPATHLEN * MAX_RECENT_ROMS];
	char *paths[MAX_RECENT_ROMS];
	int count = SplitRecentROMs(paths, buf, sizeof(buf));

	for (int i = 0; i < count; i++)
	{
		char *token = paths[i];

		// Split "path<RECENT_FIELD_SEP>name<RECENT_FIELD_SEP>size" in
		// place. Old-format entries (just a bare path, no separator) fall
		// through with namePart/lenPart left NULL.
		char *pathPart = token;
		char *namePart = NULL;
		char *lenPart = NULL;
		char *sep1 = strchr(token, RECENT_FIELD_SEP);
		if (sep1) {
			*sep1 = '\0';
			namePart = sep1 + 1;
			char *sep2 = strchr(namePart, RECENT_FIELD_SEP);
			if (sep2) {
				*sep2 = '\0';
				lenPart = sep2 + 1;
			}
		}

		if (pathPart[0] == '\0')
			continue;

		if (!AddBrowserEntry())
			break; // out of browser slots

		int idx = browser.numEntries;
		snprintf(browserList[idx].filename, MAXJOLIET, "%s", pathPart); // full path

		if (namePart && namePart[0] != '\0') {
			snprintf(browserList[idx].displayname, sizeof(browserList[idx].displayname), "%s", namePart);
		} else {
			// Old-format entry with no cached name yet - fall back to
			// deriving it from the path, same as this function always did.
			const char *base = strrchr(pathPart, '/');
			base = base ? base + 1 : pathPart;
			StripExt(browserList[idx].displayname, base);
		}

		browserList[idx].isdir = 0;
		browserList[idx].icon = ICON_NONE;
		browserList[idx].length = lenPart ? (u64)strtoull(lenPart, NULL, 10) : 0;
		browser.numEntries++;
	}
}

// Classic (non-tabbed) mode used to just show whatever GCSettings.LoadFolder
// currently pointed at (manual single-folder navigation, VBA-GX style).
// This instead merges the top-level contents of all three configured ROM
// folders (GBFolder/GBCFolder/GBAFolder) into one flat list - same request
// as the tab strip minus the tabs. Entries store their FULL device-
// qualified path directly in filename, exactly like PopulateRecentList()
// above (for the same reason: entries here span more than one folder), and
// are resolved back down to a normal browser.dir + filename pair right
// before loading (see the ClassicBrowser handling in MenuGameSelection,
// which now shares that resolution with the Recent tab).
//
// Non-recursive - only the top level of each folder is scanned, same as
// each tab already does individually. A missing/unmounted folder is just
// skipped rather than aborting the whole scan, so e.g. an empty GBCFolder
// doesn't prevent GB/GBA entries from showing.
// One flattened ROM entry for Classic mode's combined list - see
// PopulateClassicCombinedList()/ScanFolderRecursive() below.
struct CombinedEntry {
	char fullpath[MAXPATHLEN];
	char displayname[MAXJOLIET + 1];
	u64  length;
};

// Recursively scans dirPath (and everything under it) into combined.
// relPath is the path so far, relative to whichever of GBFolder/GBCFolder/
// GBAFolder this walk started at - "" at the top level, "Subfolder/" one
// level down, etc. - used only to prefix nested files' display names so
// two identically-named ROMs in different subfolders (impossible in the
// old flat, non-recursive version, but now very possible) don't show up as
// indistinguishable duplicate entries in the list.
//
// browser.dir/browserList are shared globals that get overwritten by every
// ParseDirectory() call, including the recursive ones this makes into
// subfolders - so each level's entries are copied out to a local buffer
// before recursing into any of that level's subfolders, the same reasoning
// PopulateRecentList() above uses for why full paths get captured before
// browserList can be reset out from under them.
static void ScanFolderRecursive(const char *dirPath, const char *relPath, std::vector<CombinedEntry> &combined, int depth)
{
	if (depth > 8)
		return; // sane recursion cap - not a realistic ROM folder structure past this, just a safety net

	snprintf(browser.dir, sizeof(browser.dir), "%s", dirPath);

	ResetBrowser();
	ParseDirectory(true, true); // true,true (not false like the old flat scan) - need directory entries back too, to recurse into them

	// Copy this level's entries out before any recursive call below can
	// overwrite browserList out from under us.
	struct SubEntry {
		char name[MAXJOLIET + 1];
		char displayname[MAXJOLIET + 1];
		bool isdir;
		u64  length;
	};
	int numEntries = browser.numEntries;
	std::vector<SubEntry> entries(numEntries);
	for (int i = 0; i < numEntries; i++)
	{
		snprintf(entries[i].name, sizeof(entries[i].name), "%s", browserList[i].filename);
		snprintf(entries[i].displayname, sizeof(entries[i].displayname), "%s", browserList[i].displayname);
		entries[i].isdir = browserList[i].isdir;
		entries[i].length = browserList[i].length;
	}

	for (int i = 0; i < numEntries; i++)
	{
		if (entries[i].isdir)
		{
			if (strcmp(entries[i].name, ".") == 0 || strcmp(entries[i].name, "..") == 0)
				continue; // don't recurse into self/parent - would loop forever on ".."

			char subDir[MAXPATHLEN];
			char subRel[MAXPATHLEN];
			snprintf(subDir, sizeof(subDir), "%s%s/", dirPath, entries[i].name);
			snprintf(subRel, sizeof(subRel), "%s%s/", relPath, entries[i].name);
			ScanFolderRecursive(subDir, subRel, combined, depth + 1);
			continue;
		}

		CombinedEntry e;
		snprintf(e.fullpath, sizeof(e.fullpath), "%s%s", dirPath, entries[i].name);
		if (relPath[0] != '\0')
			snprintf(e.displayname, sizeof(e.displayname), "%s%s", relPath, entries[i].displayname);
		else
			snprintf(e.displayname, sizeof(e.displayname), "%s", entries[i].displayname);
		e.length = entries[i].length;
		combined.push_back(e);
	}
}

static void PopulateClassicCombinedList()
{
	std::vector<CombinedEntry> combined;

	const char *folders[3] = { GCSettings.GBFolder, GCSettings.GBCFolder, GCSettings.GBAFolder };

	if (GCSettings.LoadMethod > 0 && ChangeInterface(GCSettings.LoadMethod, NOTSILENT))
	{
		for (int f = 0; f < 3; f++)
		{
			char topDir[MAXPATHLEN];
			snprintf(topDir, sizeof(topDir), "%s%s/", pathPrefix[GCSettings.LoadMethod], folders[f]);
			ScanFolderRecursive(topDir, "", combined, 0);
		}
	}

	// Sort across ALL three folders together by display name, not grouped
	// by which folder each entry came from - the whole point of Classic
	// mode showing a combined list is a single continuous A-Z, not GB's
	// A-Z followed by GBC's A-Z followed by GBA's A-Z.
	std::sort(combined.begin(), combined.end(), [](const CombinedEntry &a, const CombinedEntry &b) {
		return strcasecmp(a.displayname, b.displayname) < 0;
	});

	ResetBrowser();
	browser.dir[0] = 0;
	for (size_t i = 0; i < combined.size(); i++)
	{
		if (!AddBrowserEntry())
			break; // out of browser slots

		int idx = browser.numEntries;
		snprintf(browserList[idx].filename, MAXJOLIET, "%s", combined[i].fullpath);
		snprintf(browserList[idx].displayname, sizeof(browserList[idx].displayname), "%s", combined[i].displayname);
		browserList[idx].isdir = 0;
		browserList[idx].icon = ICON_NONE;
		browserList[idx].length = combined[i].length;
		browser.numEntries++;
	}
}

/****************************************************************************
 * MenuGameSelection
 *
 * Displays a list of games on the specified load device, and allows the user
 * to browse and select from this list.
 ***************************************************************************/
static char* getImageFolder()
{
	switch(GCSettings.PreviewImage)
	{
		case PREVIEWIMAGE_SCREENSHOT : return GCSettings.ScreenshotsFolder;
		case PREVIEWIMAGE_COVER : return GCSettings.CoverFolder;
		case PREVIEWIMAGE_ARTWORK : return GCSettings.ArtworkFolder;
		default : return GCSettings.CoverFolder;
	}
}

static int MenuGameSelection()
{
	int menu = MENU_NONE;
	bool res;
	int i;

	// ── Tab strip ────────────────────────────────────────────────────────
	enum { TAB_GB = 0, TAB_GBC = 1, TAB_GBA = 2, TAB_RECENT = 3 };
	static const char* kTabLabels[]  = { "GB", "GBC", "GBA", "Recent" };
	// Folder paths are user-configurable (Settings > Saving & Loading).
	// These pointers just mirror whatever is currently stored in GCSettings.
	// Only the first 3 tabs have a real backing folder - Recent is a
	// synthetic list built from GCSettings.RecentROMs (see
	// PopulateRecentList() above) and must never be used to index this.
	const char* kTabFolders[] = {
		GCSettings.GBFolder,
		GCSettings.GBCFolder,
		GCSettings.GBAFolder
	};
	static const int kNumFolderTabs = 3;
	static const int kNumTabs = 4;

	// Tab-list cache: GB/GBC/GBA's root-folder scan is otherwise the ~1
	// second stall switching tabs mid-session, same underlying cost
	// PopulateRecentList() used to pay per-entry and now doesn't - here the
	// cost is one real ParseDirectory() disk scan per tab instead of one
	// stat() per Recent entry, but the fix is the same shape: do the real
	// scan once, cache it, reuse the cache on every later visit to that
	// tab instead of re-scanning.
	//
	// Deliberately scoped to just each tab's ROOT listing - clicking into a
	// subfolder within a tab still does a live BrowserChangeFolder() scan
	// as before, uncached, and switching tabs has always reset back to the
	// tab's root (not wherever you last navigated within it), which this
	// preserves rather than changes.
	//
	// Not invalidated once populated - there's no realistic way for the SD/
	// USB device's contents to change out from under a running session on
	// real hardware, same assumption PopulateRecentList()'s caching above
	// already makes.
	struct TabCacheEntry {
		char filename[MAXJOLIET + 1];
		char displayname[MAXJOLIET + 1];
		bool isdir;
		int  icon;
		u64  length;
	};
	static std::vector<TabCacheEntry> tabCache[3]; // indexed by TAB_GB/TAB_GBC/TAB_GBA
	static char tabCacheDir[3][MAXPATHLEN];
	static bool tabCacheValid[3] = { false, false, false };

	auto PopulateTabList = [&](int t) {
		snprintf(GCSettings.LoadFolder, MAXPATHLEN, "%s", kTabFolders[t]);

		if (tabCacheValid[t])
		{
			ResetBrowser();
			snprintf(browser.dir, sizeof(browser.dir), "%s", tabCacheDir[t]);
			for (size_t i = 0; i < tabCache[t].size(); i++)
			{
				if (!AddBrowserEntry())
					break; // out of browser slots
				int idx = browser.numEntries;
				snprintf(browserList[idx].filename, MAXJOLIET, "%s", tabCache[t][i].filename);
				snprintf(browserList[idx].displayname, sizeof(browserList[idx].displayname), "%s", tabCache[t][i].displayname);
				browserList[idx].isdir = tabCache[t][i].isdir;
				browserList[idx].icon = tabCache[t][i].icon;
				browserList[idx].length = tabCache[t][i].length;
				browser.numEntries++;
			}
			return;
		}

		OpenGameList();

		tabCache[t].clear();
		tabCache[t].reserve(browser.numEntries);
		for (int i = 0; i < browser.numEntries; i++)
		{
			TabCacheEntry e;
			snprintf(e.filename, sizeof(e.filename), "%s", browserList[i].filename);
			snprintf(e.displayname, sizeof(e.displayname), "%s", browserList[i].displayname);
			e.isdir = browserList[i].isdir;
			e.icon = browserList[i].icon;
			e.length = browserList[i].length;
			tabCache[t].push_back(e);
		}
		snprintf(tabCacheDir[t], sizeof(tabCacheDir[t]), "%s", browser.dir);
		tabCacheValid[t] = true;
	};

	static bool restoredTabOnce = false;
	static int activeTab = (GCSettings.LastActiveTab >= 0 && GCSettings.LastActiveTab < kNumTabs)
		? GCSettings.LastActiveTab : 2; // default GBA if unset/out of range
	if (GCSettings.ClassicBrowser && activeTab == TAB_RECENT)
		activeTab = TAB_GB; // classic mode has no Recent tab - a stale value from a
		                    // previous tabbed session must not hit the TAB_RECENT
		                    // branches below (they assume browserList entries are
		                    // full device-qualified paths, which a plain classic
		                    // folder listing's entries are not)
	// Exact cursor position within the current tab's list, captured right
	// before jumping into Settings (see settingsBtn handling below) so that
	// coming back restores precisely where the user was scrolled to -
	// rather than always snapping back to whatever game was last loaded.
	// -1 means "nothing captured yet" (first-ever entry, or last exit
	// wasn't via the Settings button), in which case we fall back to the
	// existing LastFileLoaded-based restore below.
	static int savedTab = -1;
	static int savedSelIndex = -1;
	if (!GCSettings.ClassicBrowser) {
		if (!restoredTabOnce) {
			restoredTabOnce = true;
			if (activeTab != TAB_RECENT)
				snprintf(GCSettings.LoadFolder, MAXPATHLEN, "%s", kTabFolders[activeTab]);
		}

		// Snap to current LoadFolder on entry - but only among the real folder
		// tabs, and only if we didn't just restore straight into Recent above
		// (otherwise this would override that restore, since LoadFolder still
		// holds whatever folder tab was last active before Recent).
		if (activeTab != TAB_RECENT) {
			for (int t = 0; t < kNumFolderTabs; t++) {
				if (strcasecmp(GCSettings.LoadFolder, kTabFolders[t]) == 0) {
					activeTab = t;
					break;
				}
			}
		}
	}
	// ─────────────────────────────────────────────────────────────────────

	GuiText titleTxt("Choose Game", 26, (GXColor){255, 255, 255, 255});
	titleTxt.SetAlignment(ALIGN_LEFT, ALIGN_TOP);
	titleTxt.SetPosition(50, 50);

	GuiSound btnSoundOver(button_over_pcm, button_over_pcm_size, SOUND_PCM);
	GuiSound btnSoundClick(button_click_pcm, button_click_pcm_size, SOUND_PCM);
	GuiImageData iconHome(icon_home_png);
	GuiImageData iconSettings(icon_settings_png);
	GuiImageData btnOutline(button_long_png);
	GuiImageData btnOutlineOver(button_long_over_png);
	GuiImageData bgPreviewImg(bg_preview_png);

	// Use button_short assets so the tabs look like native UI buttons
	GuiImageData tabBtnImg(button_short_png);
	GuiImageData tabBtnImgOver(button_short_over_png);

	GuiTrigger trigHome;
	trigHome.SetButtonOnlyTrigger(-1, WPAD_BUTTON_HOME | WPAD_CLASSIC_BUTTON_HOME, 0, WIIDRC_BUTTON_HOME);

	// ── Bottom-bar buttons (Settings / Exit) ────────────────────────────
	GuiText settingsBtnTxt("Settings", 22, (GXColor){0, 0, 0, 255});
	GuiImage settingsBtnIcon(&iconSettings);
	settingsBtnIcon.SetAlignment(ALIGN_LEFT, ALIGN_MIDDLE);
	settingsBtnIcon.SetPosition(14, 0);
	GuiImage settingsBtnImg(&btnOutline);
	GuiImage settingsBtnImgOver(&btnOutlineOver);
	GuiButton settingsBtn(btnOutline.GetWidth(), btnOutline.GetHeight());
	settingsBtn.SetAlignment(ALIGN_LEFT, ALIGN_BOTTOM);
	settingsBtn.SetPosition(90, -35);
	settingsBtn.SetLabel(&settingsBtnTxt);
	settingsBtn.SetIcon(&settingsBtnIcon);
	settingsBtn.SetImage(&settingsBtnImg);
	settingsBtn.SetImageOver(&settingsBtnImgOver);
	settingsBtn.SetSoundOver(&btnSoundOver);
	settingsBtn.SetSoundClick(&btnSoundClick);
	settingsBtn.SetTrigger(trigA);
	settingsBtn.SetEffectGrow();

	GuiText exitBtnTxt("Exit", 22, (GXColor){0, 0, 0, 255});
	GuiImage exitBtnIcon(&iconHome);
	exitBtnIcon.SetAlignment(ALIGN_LEFT, ALIGN_MIDDLE);
	exitBtnIcon.SetPosition(14, 0);
	GuiImage exitBtnImg(&btnOutline);
	GuiImage exitBtnImgOver(&btnOutlineOver);
	GuiButton exitBtn(btnOutline.GetWidth(), btnOutline.GetHeight());
	exitBtn.SetAlignment(ALIGN_RIGHT, ALIGN_BOTTOM);
	exitBtn.SetPosition(-90, -35);
	exitBtn.SetLabel(&exitBtnTxt);
	exitBtn.SetIcon(&exitBtnIcon);
	exitBtn.SetImage(&exitBtnImg);
	exitBtn.SetImageOver(&exitBtnImgOver);
	exitBtn.SetSoundOver(&btnSoundOver);
	exitBtn.SetSoundClick(&btnSoundClick);
	exitBtn.SetTrigger(trigA);
	exitBtn.SetTrigger(&trigHome);
	exitBtn.SetEffectGrow();

	// buttonWindow contains ONLY the bottom-bar buttons — no tabs here so
	// GC controller B/D-pad navigation to Settings/Exit is unaffected.
	GuiWindow buttonWindow(screenwidth, screenheight);
	buttonWindow.Append(&settingsBtn);
	buttonWindow.Append(&exitBtn);

	// ── Tab strip window — separate so it never steals focus from the ───
	// ── file browser or the bottom-bar buttons via GC navigation.      ───
	//
	// Tabs sit directly above the GuiFileBrowser (which starts at y=98).
	// button_short is 152×42; we scale it down to fit four across 330px.
	// Each tab is 75px wide, 28px tall, with 5px gaps, starting at x=20.
	static const int TAB_W = 75;
	static const int TAB_H = 28;
	static const int TAB_Y = 67;   // just above the file browser at y=98
	static const int TAB_GAP = 5;

	GuiImage tabImg0(&tabBtnImg);    GuiImage tabImg0Over(&tabBtnImgOver);
	GuiImage tabImg1(&tabBtnImg);    GuiImage tabImg1Over(&tabBtnImgOver);
	GuiImage tabImg2(&tabBtnImg);    GuiImage tabImg2Over(&tabBtnImgOver);
	GuiImage tabImg3(&tabBtnImg);    GuiImage tabImg3Over(&tabBtnImgOver);

	GuiText tabTxt0(kTabLabels[0], 18, (GXColor){0,0,0,255});
	GuiText tabTxt1(kTabLabels[1], 18, (GXColor){0,0,0,255});
	GuiText tabTxt2(kTabLabels[2], 18, (GXColor){0,0,0,255});
	GuiText tabTxt3(kTabLabels[3], 16, (GXColor){0,0,0,255}); // slightly smaller - "Recent" is a wider label

	// Scale button_short images to fit our tab size
	float scaleX = (float)TAB_W / tabBtnImg.GetWidth();
	float scaleY = (float)TAB_H / tabBtnImg.GetHeight();
	tabImg0.SetScale(scaleX, scaleY);    tabImg0Over.SetScale(scaleX, scaleY);
	tabImg1.SetScale(scaleX, scaleY);    tabImg1Over.SetScale(scaleX, scaleY);
	tabImg2.SetScale(scaleX, scaleY);    tabImg2Over.SetScale(scaleX, scaleY);
	tabImg3.SetScale(scaleX, scaleY);    tabImg3Over.SetScale(scaleX, scaleY);

	GuiButton tabBtn0(TAB_W, TAB_H);
	GuiButton tabBtn1(TAB_W, TAB_H);
	GuiButton tabBtn2(TAB_W, TAB_H);
	GuiButton tabBtn3(TAB_W, TAB_H);
	GuiButton* tabBtns[4] = { &tabBtn0, &tabBtn1, &tabBtn2, &tabBtn3 };
	GuiText*   tabTxts[4] = { &tabTxt0, &tabTxt1, &tabTxt2, &tabTxt3 };
	GuiImage*  tabImgs[4] = { &tabImg0, &tabImg1, &tabImg2, &tabImg3 };
	GuiImage*  tabImgsOver[4] = { &tabImg0Over, &tabImg1Over, &tabImg2Over, &tabImg3Over };

	for (int t = 0; t < kNumTabs; t++) {
		tabBtns[t]->SetAlignment(ALIGN_LEFT, ALIGN_TOP);
		tabBtns[t]->SetPosition(20 + t * (TAB_W + TAB_GAP), TAB_Y);
		tabBtns[t]->SetLabel(tabTxts[t]);
		tabBtns[t]->SetImage(tabImgs[t]);
		tabBtns[t]->SetImageOver(tabImgsOver[t]);
		tabBtns[t]->SetSoundOver(&btnSoundOver);
		tabBtns[t]->SetSoundClick(&btnSoundClick);
		tabBtns[t]->SetTrigger(trigA);
		tabBtns[t]->SetTrigger(trig2);
		tabBtns[t]->SetEffectGrow();
		// Mark inactive tabs visually with reduced alpha on the image
		tabImgs[t]->SetAlpha(t == activeTab ? 255 : 160);
		tabTxts[t]->SetColor(t == activeTab
			? (GXColor){255, 255, 255, 255}
			: (GXColor){180, 180, 180, 200});
	}

	// tabWindow is appended to mainWindow but kept separate from buttonWindow
	// so GC controller navigation between gameBrowser ↔ buttonWindow works
	// exactly as it did before the tabs were added.
	GuiWindow tabWindow(screenwidth, screenheight);
	for (int t = 0; t < kNumTabs; t++)
		tabWindow.Append(tabBtns[t]);
	// Exclude tabWindow from GuiWindow::ToggleFocus()'s top-level B-button
	// focus cycle. GuiWindow defaults focus=0 (focusable), so without this,
	// pressing B cycled through gameBrowser -> buttonWindow -> tabWindow (no
	// visible highlight) -> back to gameBrowser, a 3-way cycle instead of
	// the intended 2-way one - hence needing an extra B press.
	tabWindow.SetFocus(-1);

	GuiFileBrowser gameBrowser(330, 268);
	gameBrowser.SetPosition(20, 98);
	ResetBrowser();

	GuiTrigger trigPlusMinus;
	trigPlusMinus.SetButtonOnlyTrigger(-1, WPAD_BUTTON_PLUS | WPAD_CLASSIC_BUTTON_PLUS, PAD_TRIGGER_Z, WIIDRC_BUTTON_PLUS);

	GuiImage bgPreview(&bgPreviewImg);
	bgPreview.SetPosition(365, 98);
	int previousPreviewImg = GCSettings.PreviewImage;
	
	GuiImage preview;
	preview.SetAlignment(ALIGN_CENTRE, ALIGN_MIDDLE);
	preview.SetPosition(174, -8);
	u8* imgBuffer = MEM_ALLOC(640 * 480 * 4);
	int  previousBrowserIndex = -1;
	char imagePath[MAXJOLIET + 1];
	const u32 PREVIEW_SETTLE_DELAY = 150000; // 150ms - wait for the cursor to settle before loading art
	u64 previewSettleTime = 0;
	bool previewPending = false;

	HaltGui();
	btnLogo->SetAlignment(ALIGN_RIGHT, ALIGN_TOP);
	btnLogo->SetPosition(-50, 24);
	mainWindow->Append(&titleTxt);
	mainWindow->Append(&gameBrowser);
	mainWindow->Append(&buttonWindow);
	if (!GCSettings.ClassicBrowser)
		mainWindow->Append(&tabWindow);     // tab window last so it draws on top
	mainWindow->Append(&bgPreview);
	mainWindow->Append(&preview);
	ResumeGui();

	#ifdef HW_RVL
	ShutoffRumble();
	#endif

	// populate initial directory listing
	selectLoadedFile = 1;
	if (GCSettings.ClassicBrowser)
		PopulateClassicCombinedList();
	else if (activeTab == TAB_RECENT)
		PopulateRecentList();
	else
		PopulateTabList(activeTab);

	gameBrowser.ResetState();
	{
		int restoreIndex = -1;

		// Prefer the exact position we were at before entering Settings,
		// if we're back on the same tab and it's still in range.
		if (savedTab == activeTab && savedSelIndex >= 0 && savedSelIndex < browser.numEntries) {
			restoreIndex = savedSelIndex;
		} else {
			// Restore previously-selected file if it's present in this
			// tab's list (GCSettings.LastFileLoaded, set right before a
			// game loads). Falls back to the first entry, matching prior
			// behavior, if not found (e.g. first run, or the file was
			// moved/deleted).
			if (GCSettings.LastFileLoaded[0] != '\0') {
				for (int bi = 0; bi < browser.numEntries; bi++) {
					if (strcasecmp(browserList[bi].filename, GCSettings.LastFileLoaded) == 0) {
						restoreIndex = bi;
						break;
					}
				}
			}
		}
		// Consumed - don't let it apply again until we explicitly capture
		// a fresh position on the way into Settings.
		savedTab = -1;
		savedSelIndex = -1;

		if (restoreIndex >= 0)
			browser.selIndex = restoreIndex;
		else
			gameBrowser.fileList[0]->SetState(STATE_SELECTED);
	}
	gameBrowser.TriggerUpdate();
	titleTxt.SetText(inSz ? szname : "Choose Game");

	// Helper: switch tab, reload file list, refresh visuals
	auto SwitchTab = [&](int t) {
		if (t < 0) t = kNumTabs - 1;
		if (t >= kNumTabs) t = 0;
		if (t == activeTab) return;
		activeTab = t;

		HaltGui();
		ResetBrowser();
		if (activeTab == TAB_RECENT) {
			PopulateRecentList();
		} else {
			PopulateTabList(activeTab);
		}
		gameBrowser.ResetState();
		if (browser.numEntries > 0)
			gameBrowser.fileList[0]->SetState(STATE_SELECTED);
		gameBrowser.TriggerUpdate();

		// Update tab visuals
		for (int tt = 0; tt < kNumTabs; tt++) {
			tabImgs[tt]->SetAlpha(tt == activeTab ? 255 : 160);
			tabTxts[tt]->SetColor(tt == activeTab
				? (GXColor){255, 255, 255, 255}
				: (GXColor){180, 180, 180, 200});
		}
		ResumeGui();
	};

	while(menu == MENU_NONE)
	{
		usleep(THREAD_SLEEP);

		// ── L/R tab cycling — poll pads directly so it works on GC      ──
		// controller regardless of which GUI element has focus.
		// We check btns_d (just-pressed) to get a single cycle per press.
		if (!GCSettings.ClassicBrowser) {
			for (int p = 0; p < 4; p++) {
				if (userInput[p].pad.btns_d & PAD_TRIGGER_L)
					SwitchTab(activeTab - 1);
				if (userInput[p].pad.btns_d & PAD_TRIGGER_R)
					SwitchTab(activeTab + 1);
#ifdef HW_RVL
				if (userInput[p].wpad->btns_d & WPAD_CLASSIC_BUTTON_FULL_L)
					SwitchTab(activeTab - 1);
				if (userInput[p].wpad->btns_d & WPAD_CLASSIC_BUTTON_FULL_R)
					SwitchTab(activeTab + 1);
#endif
			}
		}

		if(selectLoadedFile == 2)
		{
			selectLoadedFile = 0;
			mainWindow->ChangeFocus(&gameBrowser);
			gameBrowser.TriggerUpdate();
		}

		// update gameWindow based on arrow buttons
		// set MENU_EXIT if A button pressed on a game
		for(i=0; i < FILE_PAGESIZE; i++)
		{
			if(gameBrowser.fileList[i]->GetState() == STATE_CLICKED)
			{
				gameBrowser.fileList[i]->ResetState();
				
				// check corresponding browser entry
				if(browserList[browser.selIndex].isdir || IsSz())
				{	
					HaltGui();
					res = BrowserChangeFolder();
					if(res)
					{
						gameBrowser.ResetState();
						gameBrowser.fileList[0]->SetState(STATE_SELECTED);
						gameBrowser.TriggerUpdate();
						previousBrowserIndex = -1;			
					}
					else
					{
						menu = MENU_GAMESELECTION;
						break;
					}

					titleTxt.SetText(inSz ? szname : "Choose Game");
					
					ResumeGui();
				}
				else
				{
					#ifdef HW_RVL
					ShutoffRumble();
					#endif

					// Capture the full path of the ROM about to be loaded, for
					// the Recent Games list - a successful BrowserLoadFile()
					// below wipes browserList via ResetBrowser(), so this has
					// to happen first.
					char recentFullPath[MAXPATHLEN];
					if (activeTab == TAB_RECENT || GCSettings.ClassicBrowser)
					{
						// Recent-tab and Classic-combined-list entries both
						// store their full device-qualified path directly in
						// filename (they can each live in a different
						// folder, unlike a normal single-directory listing).
						snprintf(recentFullPath, MAXPATHLEN, "%s", browserList[browser.selIndex].filename);

						// Re-stage browser.dir/browserList[0] to look like a
						// normal single-file listing, so BrowserLoadFile()
						// (which assumes browser.dir + browserList[selIndex]
						// .filename is the full path) works unmodified.
						char stagedPath[MAXPATHLEN];
						snprintf(stagedPath, MAXPATHLEN, "%s", recentFullPath);
						char *slash = strrchr(stagedPath, '/');
						char base[MAXJOLIET + 1];
						snprintf(base, sizeof(base), "%s", slash ? slash + 1 : stagedPath);
						if (slash) slash[1] = '\0'; // truncate to just the directory, keep trailing '/'
						else stagedPath[0] = '\0';

						ResetBrowser();
						AddBrowserEntry();
						snprintf(browser.dir, sizeof(browser.dir), "%s", stagedPath);
						snprintf(browserList[0].filename, MAXJOLIET, "%s", base);
						browserList[0].isdir = 0;
						browser.numEntries = 1;
						browser.selIndex = 0;
					}
					else
					{
						snprintf(recentFullPath, MAXPATHLEN, "%s%s", browser.dir, browserList[browser.selIndex].filename);
					}

					// Remember tab + selection so relaunching the app restores
					// this position.
					GCSettings.LastActiveTab = activeTab;
					snprintf(GCSettings.LastFileLoaded, sizeof(GCSettings.LastFileLoaded), "%s",
						browserList[browser.selIndex].filename);

					mainWindow->SetState(STATE_DISABLED);
					if(BrowserLoadFile())
					{
						AddRecentROM(recentFullPath);
						menu = MENU_EXIT;
					}
					else
						mainWindow->SetState(STATE_DEFAULT);
				}
			}
		}
		
		//update gamelist image
		// Debounced: only reload artwork once the cursor has settled on an entry
		// for a short moment, instead of on every single index change. Doing a
		// synchronous disk read + PNG decode on every scroll step is what was
		// causing the ROM list to feel choppy - while art is being loaded here,
		// the file browser (which runs on the separate UpdateGUI thread) keeps
		// advancing selIndex, so the moment the decode finishes we'd immediately
		// block on another one, eating/bunching up subsequent input.
		if(previousBrowserIndex != browser.selIndex || previousPreviewImg != GCSettings.PreviewImage)
		{
			previousBrowserIndex = browser.selIndex;
			previousPreviewImg = GCSettings.PreviewImage;
			previewSettleTime = gettime();
			previewPending = true;
		}

		if(previewPending && diff_usec(previewSettleTime, gettime()) > PREVIEW_SETTLE_DELAY)
		{
			previewPending = false;

			// ensure selected index is valid. The Recent tab has no shared
			// browser.dir (each entry can live in a different folder/device -
			// see PopulateRecentList()) and no ".." parent-folder entry to
			// skip, so it needs its own, slightly looser validity check.
			bool validSelection;
			if (activeTab == TAB_RECENT)
				validSelection = (browser.numEntries > 0 && browser.selIndex >= 0 && browser.selIndex < browser.numEntries);
			else
				validSelection = (browser.dir[0] != 0 && GCSettings.LoadMethod > 0 && browser.numEntries > 0
					&& browser.selIndex > 0 && browser.selIndex < browser.numEntries);

			if(!validSelection)
			{
				HaltGui();
				preview.SetImage(NULL, 0, 0);
				ResumeGui();
			}
			else
			{
				// Screenshots/Covers/Artwork folders are shared across all
				// tabs, but a Recent entry's own device may differ from
				// whatever folder tab was last active - resolve it from the
				// entry's stored full path instead of assuming LoadMethod.
				int imgDevice = GCSettings.LoadMethod;
				if (activeTab == TAB_RECENT)
					FindDevice(browserList[browser.selIndex].filename, &imgDevice);

				snprintf(imagePath, MAXJOLIET, "%s%s/%s.png", pathPrefix[imgDevice], getImageFolder(), browserList[browser.selIndex].displayname);

				int width, height;
				if(ChangeInterface(imagePath, SILENT) && DecodePNGFromFile(imagePath, &width, &height, imgBuffer, 640, 480))
				{
					HaltGui();
					preview.SetImage(imgBuffer, width, height);
					preview.SetScale( MIN(225.0f / width, 235.0f / height) );
					ResumeGui();
				}
				else
				{
					HaltGui();
					preview.SetImage(NULL, 0, 0);
					ResumeGui();
				}
			}
		}

		if(settingsBtn.GetState() == STATE_CLICKED)
		{
			savedTab = activeTab;
			savedSelIndex = browser.selIndex;
			menu = MENU_SETTINGS;
		}
		else if(exitBtn.GetState() == STATE_CLICKED)
			ExitRequested = 1;

		// Tab A-button clicks (Wiimote pointer or GC A on a focused tab)
		for (int t = 0; t < kNumTabs; t++) {
			if (tabBtns[t]->GetState() == STATE_CLICKED) {
				tabBtns[t]->ResetState();
				SwitchTab(t);
				break;
			}
		}
	}

	HaltParseThread(); // halt parsing
	HaltGui();
	ResetBrowser();
	mainWindow->Remove(&titleTxt);
	mainWindow->Remove(&buttonWindow);
	mainWindow->Remove(&tabWindow);
	mainWindow->Remove(&gameBrowser);
	mainWindow->Remove(&bgPreview);
	mainWindow->Remove(&preview);
	MEM_DEALLOC(imgBuffer);
	return menu;
}

#ifdef HW_RVL
static int playerMappingChan = 0;

static void PlayerMappingWindowUpdate(void * ptr, int dir)
{
	GuiButton * b = (GuiButton *)ptr;
	if(b->GetState() == STATE_CLICKED)
	{
		playerMapping[playerMappingChan] += dir;

		if(playerMapping[playerMappingChan] > 3)
			playerMapping[playerMappingChan] = 0;
		if(playerMapping[playerMappingChan] < 0)
			playerMapping[playerMappingChan] = 3;

		char playerNumber[20];
		sprintf(playerNumber, "Player %d", playerMapping[playerMappingChan]+1);

		settingText->SetText(playerNumber);
		b->ResetState();
	}
}

static void PlayerMappingWindowLeftClick(void * ptr) { PlayerMappingWindowUpdate(ptr, -1); }
static void PlayerMappingWindowRightClick(void * ptr) { PlayerMappingWindowUpdate(ptr, +1); }

static void PlayerMappingWindow(int chan)
{
	playerMappingChan = chan;

	GuiWindow * w = new GuiWindow(300,250);
	w->SetAlignment(ALIGN_CENTRE, ALIGN_MIDDLE);

	GuiTrigger trigLeft;
	trigLeft.SetButtonOnlyInFocusTrigger(-1, WPAD_BUTTON_LEFT | WPAD_CLASSIC_BUTTON_LEFT, PAD_BUTTON_LEFT, WIIDRC_BUTTON_LEFT);

	GuiTrigger trigRight;
	trigRight.SetButtonOnlyInFocusTrigger(-1, WPAD_BUTTON_RIGHT | WPAD_CLASSIC_BUTTON_RIGHT, PAD_BUTTON_RIGHT, WIIDRC_BUTTON_RIGHT);

	GuiImageData arrowLeft(button_arrow_left_png);
	GuiImage arrowLeftImg(&arrowLeft);
	GuiImageData arrowLeftOver(button_arrow_left_over_png);
	GuiImage arrowLeftOverImg(&arrowLeftOver);
	GuiButton arrowLeftBtn(arrowLeft.GetWidth(), arrowLeft.GetHeight());
	arrowLeftBtn.SetImage(&arrowLeftImg);
	arrowLeftBtn.SetImageOver(&arrowLeftOverImg);
	arrowLeftBtn.SetAlignment(ALIGN_LEFT, ALIGN_MIDDLE);
	arrowLeftBtn.SetTrigger(trigA);
	arrowLeftBtn.SetTrigger(&trigLeft);
	arrowLeftBtn.SetSelectable(false);
	arrowLeftBtn.SetUpdateCallback(PlayerMappingWindowLeftClick);

	GuiImageData arrowRight(button_arrow_right_png);
	GuiImage arrowRightImg(&arrowRight);
	GuiImageData arrowRightOver(button_arrow_right_over_png);
	GuiImage arrowRightOverImg(&arrowRightOver);
	GuiButton arrowRightBtn(arrowRight.GetWidth(), arrowRight.GetHeight());
	arrowRightBtn.SetImage(&arrowRightImg);
	arrowRightBtn.SetImageOver(&arrowRightOverImg);
	arrowRightBtn.SetAlignment(ALIGN_RIGHT, ALIGN_MIDDLE);
	arrowRightBtn.SetTrigger(trigA);
	arrowRightBtn.SetTrigger(&trigRight);
	arrowRightBtn.SetSelectable(false);
	arrowRightBtn.SetUpdateCallback(PlayerMappingWindowRightClick);

	char playerNumber[20];
	sprintf(playerNumber, "Player %d", playerMapping[playerMappingChan]+1);

	settingText = new GuiText(playerNumber, 22, (GXColor){0, 0, 0, 255});

	w->Append(&arrowLeftBtn);
	w->Append(&arrowRightBtn);
	w->Append(settingText);

	char title[50];
	sprintf(title, "Player Mapping - Controller %d", chan+1);

	int previousPlayerMapping = playerMapping[playerMappingChan];

	if(!SettingWindow(title,w))
		playerMapping[playerMappingChan] = previousPlayerMapping; // undo changes

	delete(w);
	delete(settingText);
}
#endif

/****************************************************************************
 * MenuGame
 *
 * Menu displayed when returning to the menu from in-game.
 ***************************************************************************/
static void MenuGameCheats(); // defined below; forward-declared so MenuGame() can call it directly for the pause-menu Cheats button
static int MenuGame()
{
	int menu = MENU_NONE;

	GuiText titleTxt(ROMFilename, 22, (GXColor){255, 255, 255, 255});
	titleTxt.SetAlignment(ALIGN_LEFT, ALIGN_TOP);
	titleTxt.SetPosition(50,50);

	// Small, muted "Patch Applied" tag under the title - only shown when
	// LoadPatchForCurrentROM() (vbasupport.cpp) actually found and applied
	// an IPS/UPS/BPS patch for this ROM. Deliberately not bundled into
	// titleTxt itself (e.g. "GameName [Patched]") since that would grow/
	// shrink the title text depending on patch state and risk crowding a
	// long ROM name; a separate small line underneath never touches the
	// title's own space. Sits in the gap between the title (y=50) and the
	// first row of buttons (y=120), so it can't overlap either.
	GuiText patchIndicatorTxt("Patch Applied", 16, (GXColor){180, 180, 180, 255});
	patchIndicatorTxt.SetAlignment(ALIGN_LEFT, ALIGN_TOP);
	patchIndicatorTxt.SetPosition(50, 78);

	GuiSound btnSoundOver(button_over_pcm, button_over_pcm_size, SOUND_PCM);
	GuiSound btnSoundClick(button_click_pcm, button_click_pcm_size, SOUND_PCM);
	GuiImageData btnOutline(button_png);
	GuiImageData btnOutlineOver(button_over_png);
	GuiImageData btnCloseOutline(button_small_png);
	GuiImageData btnCloseOutlineOver(button_small_over_png);
	GuiImageData btnLargeOutline(button_large_png);
	GuiImageData btnLargeOutlineOver(button_large_over_png);
	GuiImageData iconGameSettings(icon_game_settings_png);
	GuiImageData iconLoad(icon_game_load_png);
	GuiImageData iconSave(icon_game_save_png);
	GuiImageData iconDelete(icon_game_delete_png);
	GuiImageData iconReset(icon_game_reset_png);

	GuiImageData battery(battery_png);
	GuiImageData batteryRed(battery_red_png);
	GuiImageData batteryBar(battery_bar_png);

	GuiTrigger trigHome;
	trigHome.SetButtonOnlyTrigger(-1, WPAD_BUTTON_HOME | WPAD_CLASSIC_BUTTON_HOME, 0, WIIDRC_BUTTON_HOME);

	GuiText saveBtnTxt("Save", 22, (GXColor){0, 0, 0, 255});
	GuiImage saveBtnImg(&btnLargeOutline);
	GuiImage saveBtnImgOver(&btnLargeOutlineOver);
	GuiImage saveBtnIcon(&iconSave);
	GuiButton saveBtn(btnLargeOutline.GetWidth(), btnLargeOutline.GetHeight());
	saveBtn.SetAlignment(ALIGN_CENTRE, ALIGN_TOP);
	saveBtn.SetPosition(-200, 120);
	saveBtn.SetLabel(&saveBtnTxt);
	saveBtn.SetImage(&saveBtnImg);
	saveBtn.SetImageOver(&saveBtnImgOver);
	saveBtn.SetIcon(&saveBtnIcon);
	saveBtn.SetSoundOver(&btnSoundOver);
	saveBtn.SetSoundClick(&btnSoundClick);
	saveBtn.SetTrigger(trigA);
	saveBtn.SetEffectGrow();

	GuiText loadBtnTxt("Load", 22, (GXColor){0, 0, 0, 255});
	GuiImage loadBtnImg(&btnLargeOutline);
	GuiImage loadBtnImgOver(&btnLargeOutlineOver);
	GuiImage loadBtnIcon(&iconLoad);
	GuiButton loadBtn(btnLargeOutline.GetWidth(), btnLargeOutline.GetHeight());
	loadBtn.SetAlignment(ALIGN_CENTRE, ALIGN_TOP);
	loadBtn.SetPosition(0, 120);
	loadBtn.SetLabel(&loadBtnTxt);
	loadBtn.SetImage(&loadBtnImg);
	loadBtn.SetImageOver(&loadBtnImgOver);
	loadBtn.SetIcon(&loadBtnIcon);
	loadBtn.SetSoundOver(&btnSoundOver);
	loadBtn.SetSoundClick(&btnSoundClick);
	loadBtn.SetTrigger(trigA);
	loadBtn.SetEffectGrow();

	GuiText deleteBtnTxt("Delete", 22, (GXColor){0, 0, 0, 255});
	GuiImage deleteBtnImg(&btnLargeOutline);
	GuiImage deleteBtnImgOver(&btnLargeOutlineOver);
	GuiImage deleteBtnIcon(&iconDelete);
	GuiButton deleteBtn(btnLargeOutline.GetWidth(), btnLargeOutline.GetHeight());
	deleteBtn.SetAlignment(ALIGN_CENTRE, ALIGN_TOP);
	deleteBtn.SetPosition(200, 120);
	deleteBtn.SetLabel(&deleteBtnTxt);
	deleteBtn.SetImage(&deleteBtnImg);
	deleteBtn.SetImageOver(&deleteBtnImgOver);
	deleteBtn.SetIcon(&deleteBtnIcon);
	deleteBtn.SetSoundOver(&btnSoundOver);
	deleteBtn.SetSoundClick(&btnSoundClick);
	deleteBtn.SetTrigger(trigA);
	deleteBtn.SetEffectGrow();
	
	// The Boktai Weather button used to live here; it's now in the
	// in-game Game Settings menu (MenuGameSettings(), below) instead - see
	// the comment there for why.

	GuiText resetBtnTxt("Reset", 22, (GXColor){0, 0, 0, 255});
	GuiImage resetBtnImg(&btnLargeOutline);
	GuiImage resetBtnImgOver(&btnLargeOutlineOver);
	GuiImage resetBtnIcon(&iconReset);
	GuiButton resetBtn(btnLargeOutline.GetWidth(), btnLargeOutline.GetHeight());
	resetBtn.SetAlignment(ALIGN_CENTRE, ALIGN_TOP);
	resetBtn.SetPosition(200, 250);
	resetBtn.SetLabel(&resetBtnTxt);
	resetBtn.SetImage(&resetBtnImg);
	resetBtn.SetImageOver(&resetBtnImgOver);
	resetBtn.SetIcon(&resetBtnIcon);
	resetBtn.SetSoundOver(&btnSoundOver);
	resetBtn.SetSoundClick(&btnSoundClick);
	resetBtn.SetTrigger(trigA);
	resetBtn.SetEffectGrow();

	// Direct access to the cheats screen from the pause menu, same idea as
	// Snes9x TX's MenuGame() - no icon asset for this exists in this
	// project yet (icon_game_cheats_png, the way snes9xtx has one), so
	// this renders as a plain bordered/text button like mainmenuBtn/
	// closeBtn below rather than SetIcon()'ing something that doesn't
	// exist. Drop in a real icon_game_cheats.png (regenerated through
	// this project's normal image->header asset pipeline) and add a
	// SetIcon(&cheatsBtnIcon) call here if you want it to match the
	// others visually.
	GuiText cheatsBtnTxt("Cheats", 22, (GXColor){0, 0, 0, 255});
	GuiImage cheatsBtnImg(&btnLargeOutline);
	GuiImage cheatsBtnImgOver(&btnLargeOutlineOver);
	GuiButton cheatsBtn(btnLargeOutline.GetWidth(), btnLargeOutline.GetHeight());
	cheatsBtn.SetAlignment(ALIGN_CENTRE, ALIGN_TOP);
	cheatsBtn.SetPosition(0, 250);
	cheatsBtn.SetLabel(&cheatsBtnTxt);
	cheatsBtn.SetImage(&cheatsBtnImg);
	cheatsBtn.SetImageOver(&cheatsBtnImgOver);
	cheatsBtn.SetSoundOver(&btnSoundOver);
	cheatsBtn.SetSoundClick(&btnSoundClick);
	cheatsBtn.SetTrigger(trigA);
	cheatsBtn.SetEffectGrow();

	GuiText gameSettingsBtnTxt("Game Settings", 22, (GXColor){0, 0, 0, 255});
	gameSettingsBtnTxt.SetWrap(true, btnLargeOutline.GetWidth()-30);
	GuiImage gameSettingsBtnImg(&btnLargeOutline);
	GuiImage gameSettingsBtnImgOver(&btnLargeOutlineOver);
	GuiImage gameSettingsBtnIcon(&iconGameSettings);
	GuiButton gameSettingsBtn(btnLargeOutline.GetWidth(), btnLargeOutline.GetHeight());
	gameSettingsBtn.SetAlignment(ALIGN_CENTRE, ALIGN_TOP);
	gameSettingsBtn.SetPosition(-200, 250);
	gameSettingsBtn.SetLabel(&gameSettingsBtnTxt);
	gameSettingsBtn.SetImage(&gameSettingsBtnImg);
	gameSettingsBtn.SetImageOver(&gameSettingsBtnImgOver);
	gameSettingsBtn.SetIcon(&gameSettingsBtnIcon);
	gameSettingsBtn.SetSoundOver(&btnSoundOver);
	gameSettingsBtn.SetSoundClick(&btnSoundClick);
	gameSettingsBtn.SetTrigger(trigA);
	gameSettingsBtn.SetEffectGrow();

	GuiText mainmenuBtnTxt("Main Menu", 22, (GXColor){0, 0, 0, 255});
	if(GCSettings.AutoloadGame) {
		mainmenuBtnTxt.SetText("Exit");
	}
	GuiImage mainmenuBtnImg(&btnOutline);
	GuiImage mainmenuBtnImgOver(&btnOutlineOver);
	GuiButton mainmenuBtn(btnOutline.GetWidth(), btnOutline.GetHeight());
	mainmenuBtn.SetAlignment(ALIGN_CENTRE, ALIGN_BOTTOM);
	mainmenuBtn.SetPosition(0, -35);
	mainmenuBtn.SetLabel(&mainmenuBtnTxt);
	mainmenuBtn.SetImage(&mainmenuBtnImg);
	mainmenuBtn.SetImageOver(&mainmenuBtnImgOver);
	mainmenuBtn.SetSoundOver(&btnSoundOver);
	mainmenuBtn.SetSoundClick(&btnSoundClick);
	mainmenuBtn.SetTrigger(trigA);
	mainmenuBtn.SetTrigger(trigB);
	mainmenuBtn.SetEffectGrow();

	GuiText closeBtnTxt("Close", 20, (GXColor){0, 0, 0, 255});
	GuiImage closeBtnImg(&btnCloseOutline);
	GuiImage closeBtnImgOver(&btnCloseOutlineOver);
	GuiButton closeBtn(btnCloseOutline.GetWidth(), btnCloseOutline.GetHeight());
	closeBtn.SetAlignment(ALIGN_RIGHT, ALIGN_TOP);
	closeBtn.SetPosition(-50, 35);
	closeBtn.SetLabel(&closeBtnTxt);
	closeBtn.SetImage(&closeBtnImg);
	closeBtn.SetImageOver(&closeBtnImgOver);
	closeBtn.SetSoundOver(&btnSoundOver);
	closeBtn.SetSoundClick(&btnSoundClick);
	closeBtn.SetTrigger(trigA);
	closeBtn.SetTrigger(trigB);
	closeBtn.SetTrigger(&trigHome);
	closeBtn.SetEffectGrow();

	#ifdef HW_RVL
	int i;
	char txt[3];
	bool status[4] = { false, false, false, false };
	int level[4] = { 0, 0, 0, 0 };
	bool newStatus;
	int newLevel;
	GuiText * batteryTxt[4];
	GuiImage * batteryImg[4];
	GuiImage * batteryBarImg[4];
	GuiButton * batteryBtn[4];

	for(i=0; i < 4; ++i)
	{
		sprintf(txt, "P%d", i+1);

		batteryTxt[i] = new GuiText(txt, 20, (GXColor){255, 255, 255, 255});
		batteryTxt[i]->SetAlignment(ALIGN_LEFT, ALIGN_MIDDLE);
		batteryImg[i] = new GuiImage(&battery);
		batteryImg[i]->SetAlignment(ALIGN_LEFT, ALIGN_MIDDLE);
		batteryImg[i]->SetPosition(30, 0);
		batteryBarImg[i] = new GuiImage(&batteryBar);
		batteryBarImg[i]->SetTile(0);
		batteryBarImg[i]->SetAlignment(ALIGN_LEFT, ALIGN_MIDDLE);
		batteryBarImg[i]->SetPosition(34, 0);

		batteryBtn[i] = new GuiButton(70, 20);
		batteryBtn[i]->SetLabel(batteryTxt[i]);
		batteryBtn[i]->SetImage(batteryImg[i]);
		batteryBtn[i]->SetIcon(batteryBarImg[i]);
		batteryBtn[i]->SetAlignment(ALIGN_LEFT, ALIGN_BOTTOM);
		batteryBtn[i]->SetTrigger(trigA);
		batteryBtn[i]->SetSoundOver(&btnSoundOver);
		batteryBtn[i]->SetSoundClick(&btnSoundClick);
		batteryBtn[i]->SetSelectable(false);
		batteryBtn[i]->SetState(STATE_DISABLED);
		batteryBtn[i]->SetAlpha(150);
	}

	batteryBtn[0]->SetPosition(45, -65);
	batteryBtn[1]->SetPosition(135, -65);
	batteryBtn[2]->SetPosition(45, -40);
	batteryBtn[3]->SetPosition(135, -40);
	#endif

	HaltGui();
	GuiWindow w(screenwidth, screenheight);
	w.Append(&titleTxt);
	if (PatchApplied)
		w.Append(&patchIndicatorTxt);
	w.Append(&saveBtn);
	w.Append(&loadBtn);
	w.Append(&deleteBtn);
	w.Append(&resetBtn);
	w.Append(&cheatsBtn);
	w.Append(&gameSettingsBtn);

	#ifdef HW_RVL
	w.Append(batteryBtn[0]);
	w.Append(batteryBtn[1]);
	w.Append(batteryBtn[2]);
	w.Append(batteryBtn[3]);
	#endif

	w.Append(&mainmenuBtn);
	w.Append(&closeBtn);

	btnLogo->SetAlignment(ALIGN_RIGHT, ALIGN_BOTTOM);
	btnLogo->SetPosition(-50, -40);
	mainWindow->Append(&w);

	if(lastMenu == MENU_NONE)
	{
		enterSound->Play();
		bgTopImg->SetEffect(EFFECT_SLIDE_TOP | EFFECT_SLIDE_IN, 35);
		closeBtn.SetEffect(EFFECT_SLIDE_TOP | EFFECT_SLIDE_IN, 35);
		titleTxt.SetEffect(EFFECT_SLIDE_TOP | EFFECT_SLIDE_IN, 35);
		mainmenuBtn.SetEffect(EFFECT_SLIDE_BOTTOM | EFFECT_SLIDE_IN, 35);
		bgBottomImg->SetEffect(EFFECT_SLIDE_BOTTOM | EFFECT_SLIDE_IN, 35);
		btnLogo->SetEffect(EFFECT_SLIDE_BOTTOM | EFFECT_SLIDE_IN, 35);
		#ifdef HW_RVL
		batteryBtn[0]->SetEffect(EFFECT_SLIDE_BOTTOM | EFFECT_SLIDE_IN, 35);
		batteryBtn[1]->SetEffect(EFFECT_SLIDE_BOTTOM | EFFECT_SLIDE_IN, 35);
		batteryBtn[2]->SetEffect(EFFECT_SLIDE_BOTTOM | EFFECT_SLIDE_IN, 35);
		batteryBtn[3]->SetEffect(EFFECT_SLIDE_BOTTOM | EFFECT_SLIDE_IN, 35);
		#endif

		w.SetEffect(EFFECT_FADE, 15);
	}

	ResumeGui();

	if(lastMenu == MENU_NONE)
	{
		if (GCSettings.AutoSave == 1)
		{
			// Previously called SaveBatteryOrStateAuto(FILE_SRAM, SILENT)
			// here. Removed - mGBA's own core keeps the battery save
			// continuously, safely synced to disk on its own (it's memory-
			// mapped directly onto the same file for the whole session -
			// see the comment in UnloadCore(), vbasupport.cpp). That
			// manual call opened a second, independent handle to the same
			// file the core still had live-mapped, which could corrupt or
			// zero it out - on every single pause, not just on exit. There
			// is nothing left to do for the battery half of this setting.
		}
		else if (GCSettings.AutoSave == 2)
		{
			if (WindowPrompt("Save", "Save State?", "Save", "Don't Save") )
				SaveBatteryOrStateAuto(FILE_SNAPSHOT, NOTSILENT); // save state
		}
		else if (GCSettings.AutoSave == 3)
		{
			if (WindowPrompt("Save", "Save SRAM and State?", "Save", "Don't Save") )
			{
				// See the AutoSave==1 comment just above for why the SRAM
				// half of this was removed - only the state save remains.
				SaveBatteryOrStateAuto(FILE_SNAPSHOT, NOTSILENT); // save state
			}
		}
	}

	while(menu == MENU_NONE)
	{
		usleep(THREAD_SLEEP);

		#ifdef HW_RVL
		for(i=0; i < 4; i++)
		{
			if(WPAD_Probe(i, NULL) == WPAD_ERR_NONE)
			{
				newStatus = true;
				newLevel = (userInput[i].wpad->battery_level / 100.0) * 4;
				if(newLevel > 4) newLevel = 4;
			}
			else
			{
				newStatus = false;
				newLevel = 0;
			}

			if(status[i] != newStatus || level[i] != newLevel)
			{
				if(newStatus == true) // controller connected
				{
					batteryBtn[i]->SetAlpha(255);
					batteryBtn[i]->SetState(STATE_DEFAULT);
					batteryBarImg[i]->SetTile(newLevel);

					if(newLevel == 0)
						batteryImg[i]->SetImage(&batteryRed);
					else
						batteryImg[i]->SetImage(&battery);
				}
				else // controller not connected
				{
					batteryBtn[i]->SetAlpha(150);
					batteryBtn[i]->SetState(STATE_DISABLED);
					batteryBarImg[i]->SetTile(0);
					batteryImg[i]->SetImage(&battery);
				}
				status[i] = newStatus;
				level[i] = newLevel;
			}
		}
		#endif

		if(saveBtn.GetState() == STATE_CLICKED)
		{
			menu = MENU_GAME_SAVE;
		}
		else if(loadBtn.GetState() == STATE_CLICKED)
		{
			menu = MENU_GAME_LOAD;
		}
		else if(deleteBtn.GetState() == STATE_CLICKED)
		{
			menu = MENU_GAME_DELETE;
		}
		else if(resetBtn.GetState() == STATE_CLICKED)
		{
			if (WindowPrompt("Reset Game", "Reset this game? Any unsaved progress will be lost.", "OK", "Cancel"))
			{
				emulator.emuReset();
				menu = MENU_EXIT;
			}
		}
		else if(cheatsBtn.GetState() == STATE_CLICKED)
		{
			// This button doesn't set `menu` (this screen re-shows once the
			// cheats modal closes - see below), so unlike every other
			// button here its click state is never cleared by this
			// function returning / the button going away. Without
			// resetting it explicitly, it was still STATE_CLICKED on the
			// very next loop iteration after MenuGameCheats() returned,
			// which instantly re-opened the cheats screen again - Go Back
			// looked like it did nothing.
			cheatsBtn.ResetState();

			// MenuGameCheats() is a self-contained blocking modal, not part
			// of the MENU_* state machine, so it doesn't set `menu` - this
			// screen (MenuGame) just re-shows once the cheats modal closes,
			// same as if nothing had been clicked.
			//
			// It draws its own full-screen title/buttons, so this screen's
			// `w` has to come off mainWindow (and lose focus) first - left
			// up, it used to leave its title/Main Menu button on screen
			// underneath the cheats screen's own title/Go Back button
			// (visibly overlapping both), and it kept input focus, which
			// meant B never reached the cheats screen's Go Back button and
			// there was no way to leave.
			HaltGui();
			mainWindow->Remove(&w);
			ResumeGui();

			MenuGameCheats();

			HaltGui();
			mainWindow->Append(&w);
			mainWindow->ChangeFocus(&w);
			ResumeGui();
		}
		else if(gameSettingsBtn.GetState() == STATE_CLICKED)
		{
			menu = MENU_GAMESETTINGS;
		}
#ifdef HW_RVL
		else if(batteryBtn[0]->GetState() == STATE_CLICKED)
		{
			PlayerMappingWindow(0);
		}
		else if(batteryBtn[1]->GetState() == STATE_CLICKED)
		{
			PlayerMappingWindow(1);
		}
		else if(batteryBtn[2]->GetState() == STATE_CLICKED)
		{
			PlayerMappingWindow(2);
		}
		else if(batteryBtn[3]->GetState() == STATE_CLICKED)
		{
			PlayerMappingWindow(3);
		}
#endif
		else if(mainmenuBtn.GetState() == STATE_CLICKED)
		{
			if (WindowPrompt("Quit Game", "Quit this game? Any unsaved progress will be lost.", "OK", "Cancel"))
			{
				HaltGui();
				if (gameScreenImg) {
					mainWindow->Remove(gameScreenImg);
					delete gameScreenImg;
					gameScreenImg = NULL;
				}
				if (gameScreenTexture) {
					free(gameScreenTexture);
					gameScreenTexture = NULL;
				}
				gameScreenIsBlurred = false;
				ClearScreenshot();
				if(GCSettings.AutoloadGame) {
					ExitApp();
				}
				else {
					gameScreenImg = new GuiImage(screenwidth, screenheight, (GXColor){236, 226, 238, 255});
					gameScreenImg->ColorStripe(10);
					mainWindow->Insert(gameScreenImg, 0);
					ResumeGui();
					#ifndef NO_SOUND
					bgMusic->Play(); // startup music
					#endif
					menu = MENU_GAMESELECTION;
				}
			}
		}
		else if(closeBtn.GetState() == STATE_CLICKED)
		{
			menu = MENU_EXIT;

			exitSound->Play();
			bgTopImg->SetEffect(EFFECT_SLIDE_TOP | EFFECT_SLIDE_OUT, 15);
			closeBtn.SetEffect(EFFECT_SLIDE_TOP | EFFECT_SLIDE_OUT, 15);
			titleTxt.SetEffect(EFFECT_SLIDE_TOP | EFFECT_SLIDE_OUT, 15);
			mainmenuBtn.SetEffect(EFFECT_SLIDE_BOTTOM | EFFECT_SLIDE_OUT, 15);
			bgBottomImg->SetEffect(EFFECT_SLIDE_BOTTOM | EFFECT_SLIDE_OUT, 15);
			btnLogo->SetEffect(EFFECT_SLIDE_BOTTOM | EFFECT_SLIDE_OUT, 15);
			#ifdef HW_RVL
			batteryBtn[0]->SetEffect(EFFECT_SLIDE_BOTTOM | EFFECT_SLIDE_OUT, 15);
			batteryBtn[1]->SetEffect(EFFECT_SLIDE_BOTTOM | EFFECT_SLIDE_OUT, 15);
			batteryBtn[2]->SetEffect(EFFECT_SLIDE_BOTTOM | EFFECT_SLIDE_OUT, 15);
			batteryBtn[3]->SetEffect(EFFECT_SLIDE_BOTTOM | EFFECT_SLIDE_OUT, 15);
			#endif

			w.SetEffect(EFFECT_FADE, -15);
			usleep(350000); // wait for effects to finish
		}
	}

	HaltGui();

	#ifdef HW_RVL
	for(i=0; i < 4; ++i)
	{
		delete batteryTxt[i];
		delete batteryImg[i];
		delete batteryBarImg[i];
		delete batteryBtn[i];
	}
	#endif

	mainWindow->Remove(&w);
	return menu;
}

/****************************************************************************
 * FindGameSaveNum
 *
 * Determines the save file number of the given file name
 * Returns -1 if none is found
 ***************************************************************************/
static int FindGameSaveNum(char * savefile, int method)
{
	int n = -1;
	int romlen = strlen(ROMFilename);
	int savelen = strlen(savefile);
	int diff = savelen-romlen;

	if(strncmp(savefile, ROMFilename, romlen) != 0)
		return -1;

	if(savefile[romlen] == ' ')
	{
		if(diff == 5 && strncmp(&savefile[romlen+1], "Auto", 4) == 0)
			n = 0; // found Auto save
		else if(diff == 2 || diff == 3)
			n = atoi(&savefile[romlen+1]);
	}

	if(n >= 0 && n < MAX_SAVES)
		return n;
	else
		return -1;
}

/****************************************************************************
 * MenuGameSaves
 *
 * Allows the user to load or save progress.
 ***************************************************************************/
static int MenuGameSaves(int action)
{
	SaveList saves;
	struct stat filestat;
	struct tm * timeinfo;

	int menu = MENU_NONE;
	int ret, result;
	int i, n, type, len, len2;
	int j = 0;

	char filepath[1024];
	char deletepath[1024];
	char scrfile[1024];
	char tmp[MAXJOLIET+1];

	int method = GCSettings.SaveMethod;

	if(!ChangeInterface(method, NOTSILENT))
		return MENU_GAME;

	GuiText titleTxt(NULL, 26, (GXColor){255, 255, 255, 255});
	titleTxt.SetAlignment(ALIGN_LEFT, ALIGN_TOP);
	titleTxt.SetPosition(50,50);

	if(action == 0)
		titleTxt.SetText("Load Game");
	else if (action == 2)
		titleTxt.SetText("Delete Saves");
	else
		titleTxt.SetText("Save Game");

	GuiSound btnSoundOver(button_over_pcm, button_over_pcm_size, SOUND_PCM);
	GuiSound btnSoundClick(button_click_pcm, button_click_pcm_size, SOUND_PCM);
	GuiImageData btnOutline(button_png);
	GuiImageData btnOutlineOver(button_over_png);
	GuiImageData btnCloseOutline(button_small_png);
	GuiImageData btnCloseOutlineOver(button_small_over_png);

	GuiTrigger trigHome;
	trigHome.SetButtonOnlyTrigger(-1, WPAD_BUTTON_HOME | WPAD_CLASSIC_BUTTON_HOME, 0, WIIDRC_BUTTON_HOME);

	GuiText backBtnTxt("Go Back", 22, (GXColor){0, 0, 0, 255});
	GuiImage backBtnImg(&btnOutline);
	GuiImage backBtnImgOver(&btnOutlineOver);
	GuiButton backBtn(btnOutline.GetWidth(), btnOutline.GetHeight());
	backBtn.SetAlignment(ALIGN_LEFT, ALIGN_BOTTOM);
	backBtn.SetPosition(50, -35);
	backBtn.SetLabel(&backBtnTxt);
	backBtn.SetImage(&backBtnImg);
	backBtn.SetImageOver(&backBtnImgOver);
	backBtn.SetSoundOver(&btnSoundOver);
	backBtn.SetSoundClick(&btnSoundClick);
	backBtn.SetTrigger(trigA);
	backBtn.SetTrigger(trigB);
	backBtn.SetEffectGrow();

	GuiText closeBtnTxt("Close", 20, (GXColor){0, 0, 0, 255});
	GuiImage closeBtnImg(&btnCloseOutline);
	GuiImage closeBtnImgOver(&btnCloseOutlineOver);
	GuiButton closeBtn(btnCloseOutline.GetWidth(), btnCloseOutline.GetHeight());
	closeBtn.SetAlignment(ALIGN_RIGHT, ALIGN_TOP);
	closeBtn.SetPosition(-50, 35);
	closeBtn.SetLabel(&closeBtnTxt);
	closeBtn.SetImage(&closeBtnImg);
	closeBtn.SetImageOver(&closeBtnImgOver);
	closeBtn.SetSoundOver(&btnSoundOver);
	closeBtn.SetSoundClick(&btnSoundClick);
	closeBtn.SetTrigger(trigA);
	closeBtn.SetTrigger(trigB);
	closeBtn.SetTrigger(&trigHome);
	closeBtn.SetEffectGrow();

	HaltGui();
	GuiWindow w(screenwidth, screenheight);
	w.Append(&backBtn);
	w.Append(&closeBtn);
	mainWindow->Append(&w);
	mainWindow->Append(&titleTxt);
	ResumeGui();

	memset(&saves, 0, sizeof(saves));

	len = strlen(ROMFilename);

	// find matching files
	AllocSaveBuffer();

	// Two passes: SaveFolder for .sav, StateFolder for .sgm + its preview
	// .png (they used to be the same folder - see MakeFilePath() in
	// filebrowser.cpp for where they actually diverge now). Both passes
	// share the same `saves`/j accumulator, same as when this was one scan.
	const char *scanFolders[2] = { GCSettings.SaveFolder, GCSettings.StateFolder };

	for (int pass = 0; pass < 2; pass++)
	{
		sprintf(browser.dir, "%s%s", pathPrefix[GCSettings.SaveMethod], scanFolders[pass]);
		ParseDirectory(true, false);

		for(i=0; i < browser.numEntries; i++)
		{
			len2 = strlen(browserList[i].filename);

			if(len2 < 6 || len2-len < 5)
				continue;

			if(strncmp(&browserList[i].filename[len2-4], ".sav", 4) == 0)
				type = FILE_SRAM;
			else if(strncmp(&browserList[i].filename[len2-4], ".sgm", 4) == 0)
				type = FILE_SNAPSHOT;
			else
				continue;

			strcpy(tmp, browserList[i].filename);
			tmp[len2-4] = 0;
			n = FindGameSaveNum(tmp, method);

			if(n >= 0)
			{
				saves.type[j] = type;
				saves.files[saves.type[j]][n] = 1;
				strcpy(saves.filename[j], browserList[i].filename);

				if(saves.type[j] == FILE_SNAPSHOT)
				{
					sprintf(scrfile, "%s%s/%s.png", pathPrefix[GCSettings.SaveMethod], GCSettings.StateFolder, tmp);

					memset(savebuffer, 0, SAVEBUFFERSIZE);
					if(LoadFile(scrfile, SILENT))
						saves.previewImg[j] = new GuiImageData(savebuffer, 64, 48);
				}
				snprintf(filepath, 1024, "%s%s/%s", pathPrefix[GCSettings.SaveMethod], scanFolders[pass], saves.filename[j]);
				if (stat(filepath, &filestat) == 0)
				{
					timeinfo = localtime(&filestat.st_mtime);
					strftime(saves.date[j], 20, "%a %b %d", timeinfo);
					strftime(saves.time[j], 10, "%I:%M %p", timeinfo);
				}
				++j;
			}
		}
	}

	FreeSaveBuffer();
	saves.length = j;

	if((saves.length == 0 && action == 0) || (saves.length == 0 && action == 2)) 
	{
		InfoPrompt("No game saves found.");
		menu = MENU_GAME;
	}

	GuiSaveBrowser saveBrowser(552, 248, &saves, action);
	saveBrowser.SetPosition(0, 108);
	saveBrowser.SetAlignment(ALIGN_CENTRE, ALIGN_TOP);

	HaltGui();
	mainWindow->Append(&saveBrowser);
	mainWindow->ChangeFocus(&saveBrowser);
	ResumeGui();

	while(menu == MENU_NONE)
	{
		usleep(THREAD_SLEEP);

		ret = saveBrowser.GetClickedSave();

		// load, save and delete save games
		if(ret > -3)
		{
			result = 0;

			if(action == 0) // load
			{
				MakeFilePath(filepath, saves.type[ret], saves.filename[ret]);
				switch(saves.type[ret])
				{
					case FILE_SRAM:
						result = LoadBatteryOrState(filepath, saves.type[ret], NOTSILENT);
						emulator.emuReset();
						break;
					case FILE_SNAPSHOT:
						result = LoadBatteryOrState(filepath, saves.type[ret], NOTSILENT);
						break;
				}
				if(result)
					menu = MENU_EXIT;
			}
			else if(action == 2) // delete SRAM/State
			{
				if (WindowPrompt("Delete File", "Delete this save file? Deleted files can not be restored.", "OK", "Cancel"))
				{
					MakeFilePath(filepath, saves.type[ret], saves.filename[ret]);
					switch(saves.type[ret])
					{
						case FILE_SRAM:
							strncpy(deletepath, filepath, 1024);
							deletepath[strlen(deletepath)-4] = 0;
							strcat(deletepath, ".sav");
							remove(deletepath); // Delete the *.srm file (Battery save file)
						break;
						case FILE_SNAPSHOT:
							strncpy(deletepath, filepath, 1024);
							deletepath[strlen(deletepath)-4] = 0;
							strcat(deletepath, ".png");
							remove(deletepath); // Delete the *.png file (Screenshot file)
							strncpy(deletepath, filepath, 1024);
							deletepath[strlen(deletepath)-4] = 0;
							strcat(deletepath, ".sgm");
							remove(deletepath); // Delete the *.frz file (Save State file)
						break;
					}							
				}
				menu = MENU_GAME_DELETE;
			
			
			}
			else // save
			{
				if(ret == -2) // new SRAM
				{
					for(i=1; i < 100; i++)
						if(saves.files[FILE_SRAM][i] == 0)
							break;

					if(i < 100)
					{
						MakeFilePath(filepath, FILE_SRAM, ROMFilename, i);
						SaveBatteryOrState(filepath, FILE_SRAM, NOTSILENT);
						menu = MENU_GAME_SAVE;
					}
				}
				else if(ret == -1) // new State
				{
					for(i=1; i < 100; i++)
						if(saves.files[FILE_SNAPSHOT][i] == 0)
							break;

					if(i < 100)
					{
						MakeFilePath(filepath, FILE_SNAPSHOT, ROMFilename, i);
						SaveBatteryOrState(filepath, FILE_SNAPSHOT, NOTSILENT);
						menu = MENU_GAME_SAVE;
					}
				}
				else // overwrite SRAM/State
				{
					MakeFilePath(filepath, saves.type[ret], saves.filename[ret]);
					switch(saves.type[ret])
					{
						case FILE_SRAM:
							SaveBatteryOrState(filepath, FILE_SRAM, NOTSILENT);
							break;
						case FILE_SNAPSHOT:
							SaveBatteryOrState(filepath, FILE_SNAPSHOT, NOTSILENT);
							break;
					}
					menu = MENU_GAME_SAVE;
				}
			}
		}

		if(backBtn.GetState() == STATE_CLICKED)
		{
			menu = MENU_GAME;
		}
		else if(closeBtn.GetState() == STATE_CLICKED)
		{
			menu = MENU_EXIT;

			exitSound->Play();
			bgTopImg->SetEffect(EFFECT_SLIDE_TOP | EFFECT_SLIDE_OUT, 15);
			closeBtn.SetEffect(EFFECT_SLIDE_TOP | EFFECT_SLIDE_OUT, 15);
			titleTxt.SetEffect(EFFECT_SLIDE_TOP | EFFECT_SLIDE_OUT, 15);
			backBtn.SetEffect(EFFECT_SLIDE_BOTTOM | EFFECT_SLIDE_OUT, 15);
			bgBottomImg->SetEffect(EFFECT_SLIDE_BOTTOM | EFFECT_SLIDE_OUT, 15);
			btnLogo->SetEffect(EFFECT_SLIDE_BOTTOM | EFFECT_SLIDE_OUT, 15);

			w.SetEffect(EFFECT_FADE, -15);

			usleep(350000); // wait for effects to finish
		}
	}

	HaltGui();

	for(i=0; i < saves.length; i++)
		if(saves.previewImg[i])
			delete saves.previewImg[i];

	mainWindow->Remove(&saveBrowser);
	mainWindow->Remove(&w);
	mainWindow->Remove(&titleTxt);
	ResetBrowser();
	return menu;
}


/****************************************************************************
 * MenuGameSettings
 ***************************************************************************/
// Shared by both the Weather button's initial label (at construction) and
// its click handler (after SunBars changes) - regardless of the weather
// setting, there should be no sun at night time.
static void FormatSunLabel(char *buf, size_t bufSize)
{
	time_t long_time;
	time(&long_time);
	struct tm *newtime = localtime(&long_time);
	if (newtime->tm_hour > 21 || newtime->tm_hour < 5)
		snprintf(buf, bufSize, "Weather: Night Time");
	else
		snprintf(buf, bufSize, "Weather: %d%% sun", SunBars * 10);
}

static int MenuGameSettings()
{
	int menu = MENU_NONE;
	char filepath[1024];

	// Weather button, only for Boktai games with a solar sensor cartridge.
	// RomIdCode is packed big-endian from the 4-char header game code
	// (code[0]<<24 | code[1]<<16 | code[2]<<8 | code[3] - see vbasupport.cpp's
	// LoadVBAROM()); every Boktai release's code starts with "U3" (Boktai =
	// U3IE/U3IP/U3IJ, Boktai 2 = U32E/U32P/U32J, Boktai 3 = U33J, JP only),
	// which lives in the top two bytes. This used to live in MenuGame() (the
	// in-game pause menu) as its own separate button anchored at (0, 380),
	// which put it directly underneath mainmenuBtn (anchored to the bottom
	// of that same screen) - overlapping, hard to hit, and mainmenuBtn was
	// appended after it so it also ate the touch/click in the overlap area.
	// Living here instead, as a normal grid button alongside Screenshot,
	// avoids the overlap entirely.
	bool isBoktai = (((RomIdCode >> 16) & 0xFFFF) == (((u32)'U' << 8) | '3'));
	char sunLabel[64];

	GuiText titleTxt("Game Settings", 26, (GXColor){255, 255, 255, 255});
	titleTxt.SetAlignment(ALIGN_LEFT, ALIGN_TOP);
	titleTxt.SetPosition(50,50);

	GuiSound btnSoundOver(button_over_pcm, button_over_pcm_size, SOUND_PCM);
	GuiSound btnSoundClick(button_click_pcm, button_click_pcm_size, SOUND_PCM);
	GuiImageData btnOutline(button_png);
	GuiImageData btnOutlineOver(button_over_png);
	GuiImageData btnLargeOutline(button_large_png);
	GuiImageData btnLargeOutlineOver(button_large_over_png);
	GuiImageData iconMappings(icon_settings_mappings_png);
	GuiImageData iconVideo(icon_settings_video_png);
	GuiImageData iconScreenshot(icon_settings_screenshot_png);
	GuiImageData btnCloseOutline(button_small_png);
	GuiImageData btnCloseOutlineOver(button_small_over_png);

	GuiTrigger trigHome;
	trigHome.SetButtonOnlyTrigger(-1, WPAD_BUTTON_HOME | WPAD_CLASSIC_BUTTON_HOME, 0, WIIDRC_BUTTON_HOME);

	GuiText mappingBtnTxt("Button Mappings", 22, (GXColor){0, 0, 0, 255});
	mappingBtnTxt.SetWrap(true, btnLargeOutline.GetWidth()-30);
	GuiImage mappingBtnImg(&btnLargeOutline);
	GuiImage mappingBtnImgOver(&btnLargeOutlineOver);
	GuiImage mappingBtnIcon(&iconMappings);
	GuiButton mappingBtn(btnLargeOutline.GetWidth(), btnLargeOutline.GetHeight());
	mappingBtn.SetAlignment(ALIGN_CENTRE, ALIGN_TOP);
	mappingBtn.SetPosition(-125, 120);
	mappingBtn.SetLabel(&mappingBtnTxt);
	mappingBtn.SetImage(&mappingBtnImg);
	mappingBtn.SetImageOver(&mappingBtnImgOver);
	mappingBtn.SetIcon(&mappingBtnIcon);
	mappingBtn.SetSoundOver(&btnSoundOver);
	mappingBtn.SetSoundClick(&btnSoundClick);
	mappingBtn.SetTrigger(trigA);
	mappingBtn.SetEffectGrow();

	GuiText videoBtnTxt("Video", 22, (GXColor){0, 0, 0, 255});
	videoBtnTxt.SetWrap(true, btnLargeOutline.GetWidth()-30);
	GuiImage videoBtnImg(&btnLargeOutline);
	GuiImage videoBtnImgOver(&btnLargeOutlineOver);
	GuiImage videoBtnIcon(&iconVideo);
	GuiButton videoBtn(btnLargeOutline.GetWidth(), btnLargeOutline.GetHeight());
	videoBtn.SetAlignment(ALIGN_CENTRE, ALIGN_TOP);
	videoBtn.SetPosition(125, 120);
	videoBtn.SetLabel(&videoBtnTxt);
	videoBtn.SetImage(&videoBtnImg);
	videoBtn.SetImageOver(&videoBtnImgOver);
	videoBtn.SetIcon(&videoBtnIcon);
	videoBtn.SetSoundOver(&btnSoundOver);
	videoBtn.SetSoundClick(&btnSoundClick);
	videoBtn.SetTrigger(trigA);
	videoBtn.SetEffectGrow();

	GuiText screenshotBtnTxt("Screenshot", 22, (GXColor){0, 0, 0, 255});
	GuiImage screenshotBtnImg(&btnLargeOutline);
	GuiImage screenshotBtnImgOver(&btnLargeOutlineOver);
	GuiImage screenshotBtnIcon(&iconScreenshot);
	GuiButton screenshotBtn(btnLargeOutline.GetWidth(), btnLargeOutline.GetHeight());
	screenshotBtn.SetAlignment(ALIGN_CENTRE, ALIGN_TOP);
	screenshotBtn.SetPosition(isBoktai ? -125 : 0, 250);
	screenshotBtn.SetLabel(&screenshotBtnTxt);
	screenshotBtn.SetImage(&screenshotBtnImg);
	screenshotBtn.SetImageOver(&screenshotBtnImgOver);
	screenshotBtn.SetIcon(&screenshotBtnIcon);
	screenshotBtn.SetSoundOver(&btnSoundOver);
	screenshotBtn.SetSoundClick(&btnSoundClick);
	screenshotBtn.SetTrigger(trigA);
	screenshotBtn.SetEffectGrow();

	// Weather button (see FormatSunLabel() above, and isBoktai above) - only
	// constructed/shown for Boktai games. Sits in the same row as
	// Screenshot rather than as a lone centered button, so it doesn't need
	// its own dedicated row.
	GuiText *sunBtnTxt = NULL;
	GuiImage *sunBtnImg = NULL;
	GuiImage *sunBtnImgOver = NULL;
	GuiButton *sunBtn = NULL;
	if (isBoktai)
	{
		FormatSunLabel(sunLabel, sizeof(sunLabel));
		sunBtnTxt = new GuiText(sunLabel, 22, (GXColor){0, 0, 0, 255});
		sunBtnTxt->SetWrap(true, btnLargeOutline.GetWidth()-30);
		sunBtnImg = new GuiImage(&btnLargeOutline);
		sunBtnImgOver = new GuiImage(&btnLargeOutlineOver);
		sunBtn = new GuiButton(btnLargeOutline.GetWidth(), btnLargeOutline.GetHeight());
		sunBtn->SetAlignment(ALIGN_CENTRE, ALIGN_TOP);
		sunBtn->SetPosition(125, 250);
		sunBtn->SetLabel(sunBtnTxt);
		sunBtn->SetImage(sunBtnImg);
		sunBtn->SetImageOver(sunBtnImgOver);
		sunBtn->SetSoundOver(&btnSoundOver);
		sunBtn->SetSoundClick(&btnSoundClick);
		sunBtn->SetTrigger(trigA);
		sunBtn->SetTrigger(trig2);
		sunBtn->SetEffectGrow();
	}
	
	GuiText closeBtnTxt("Close", 20, (GXColor){0, 0, 0, 255});
	GuiImage closeBtnImg(&btnCloseOutline);
	GuiImage closeBtnImgOver(&btnCloseOutlineOver);
	GuiButton closeBtn(btnCloseOutline.GetWidth(), btnCloseOutline.GetHeight());
	closeBtn.SetAlignment(ALIGN_RIGHT, ALIGN_TOP);
	closeBtn.SetPosition(-50, 35);
	closeBtn.SetLabel(&closeBtnTxt);
	closeBtn.SetImage(&closeBtnImg);
	closeBtn.SetImageOver(&closeBtnImgOver);
	closeBtn.SetSoundOver(&btnSoundOver);
	closeBtn.SetSoundClick(&btnSoundClick);
	closeBtn.SetTrigger(trigA);
	closeBtn.SetTrigger(trigB);
	closeBtn.SetTrigger(&trigHome);
	closeBtn.SetEffectGrow();

	GuiText backBtnTxt("Go Back", 22, (GXColor){0, 0, 0, 255});
	GuiImage backBtnImg(&btnOutline);
	GuiImage backBtnImgOver(&btnOutlineOver);
	GuiButton backBtn(btnOutline.GetWidth(), btnOutline.GetHeight());
	backBtn.SetAlignment(ALIGN_LEFT, ALIGN_BOTTOM);
	backBtn.SetPosition(50, -35);
	backBtn.SetLabel(&backBtnTxt);
	backBtn.SetImage(&backBtnImg);
	backBtn.SetImageOver(&backBtnImgOver);
	backBtn.SetSoundOver(&btnSoundOver);
	backBtn.SetSoundClick(&btnSoundClick);
	backBtn.SetTrigger(trigA);
	backBtn.SetTrigger(trigB);
	backBtn.SetEffectGrow();

	HaltGui();
	GuiWindow w(screenwidth, screenheight);
	w.Append(&titleTxt);
	w.Append(&mappingBtn);
	w.Append(&videoBtn);
	w.Append(&screenshotBtn);
	if (isBoktai)
		w.Append(sunBtn);
	w.Append(&closeBtn);
	w.Append(&backBtn);

	mainWindow->Append(&w);

	ResumeGui();

	while(menu == MENU_NONE)
	{
		usleep(THREAD_SLEEP);

		if(mappingBtn.GetState() == STATE_CLICKED)
		{
			menu = MENU_GAMESETTINGS_MAPPINGS;
		}
		else if(videoBtn.GetState() == STATE_CLICKED)
		{
			menu = MENU_GAMESETTINGS_VIDEO;
		}
		else if(screenshotBtn.GetState() == STATE_CLICKED)
		{
			if (WindowPrompt("Preview Screenshot", "Save a new Preview Screenshot? Current Screenshot image will be overwritten.", "OK", "Cancel"))
			{
				snprintf(filepath, 1024, "%s%s/%s", pathPrefix[GCSettings.LoadMethod], GCSettings.ScreenshotsFolder, ROMFilename);
				SavePreviewImg(filepath, SILENT); 
			}
		}
		else if(isBoktai && sunBtn->GetState() == STATE_CLICKED)
		{
			// This button doesn't set `menu` (clicking it should adjust the
			// level and keep this screen open, not close it) - same
			// situation as cheatsBtn above in MenuGame(), and the same fix:
			// without resetting it explicitly, it's still STATE_CLICKED on
			// the very next loop iteration, which re-runs this whole branch
			// again immediately - and keeps doing so, many times per
			// second, until something external clears it. That's what was
			// racing SunBars through several 10% steps on a single click
			// and landing wherever it happened to be when it finally
			// stopped - and since this is an else-if chain, it also meant
			// closeBtn/backBtn below were never even checked for as long as
			// this kept matching first, which is why the screen felt stuck.
			sunBtn->ResetState();

			// ResetState() above clears back to STATE_DEFAULT - including
			// the selected/highlighted look, not just the stuck click flag
			// - so without this, every single press bumped the percentage
			// AND silently dropped the cursor off the button, meaning
			// changing it twice took several presses just to navigate back
			// onto it before the next change could even register. Explicit
			// re-select keeps the highlight (and the cursor) right where
			// it was, so repeated presses just keep adjusting the value.
			sunBtn->SetState(STATE_SELECTED, -1);

			++SunBars;
			if (SunBars > 10) SunBars = 0;
			FormatSunLabel(sunLabel, sizeof(sunLabel));
			sunBtnTxt->SetText(sunLabel);
		}
		else if(closeBtn.GetState() == STATE_CLICKED)
		{
			menu = MENU_EXIT;

			exitSound->Play();
			bgTopImg->SetEffect(EFFECT_SLIDE_TOP | EFFECT_SLIDE_OUT, 15);
			closeBtn.SetEffect(EFFECT_SLIDE_TOP | EFFECT_SLIDE_OUT, 15);
			titleTxt.SetEffect(EFFECT_SLIDE_TOP | EFFECT_SLIDE_OUT, 15);
			backBtn.SetEffect(EFFECT_SLIDE_BOTTOM | EFFECT_SLIDE_OUT, 15);
			bgBottomImg->SetEffect(EFFECT_SLIDE_BOTTOM | EFFECT_SLIDE_OUT, 15);
			btnLogo->SetEffect(EFFECT_SLIDE_BOTTOM | EFFECT_SLIDE_OUT, 15);

			w.SetEffect(EFFECT_FADE, -15);

			usleep(350000); // wait for effects to finish
		}
		else if(backBtn.GetState() == STATE_CLICKED)
		{
			menu = MENU_GAME;
			// Persist changes made on this screen (SGB Border, GB Hardware,
			// Basic Palette, UTC offset) immediately - don't rely on the
			// user later also backing all the way out through the
			// top-level Settings hub, since that's a separate menu tree
			// reached differently (in-game vs. main menu).
			SavePrefs(SILENT);
		}
	}

	HaltGui();
	mainWindow->Remove(&w);
	if (isBoktai)
	{
		delete sunBtnTxt;
		delete sunBtnImg;
		delete sunBtnImgOver;
		delete sunBtn;
	}
	return menu;
}

/****************************************************************************
 * MenuSettingsMappings
 ***************************************************************************/
static int MenuSettingsMappings()
{
	int menu = MENU_NONE;

	GuiText titleTxt("Game Settings - Button Mappings", 26, (GXColor){255, 255, 255, 255});
	titleTxt.SetAlignment(ALIGN_LEFT, ALIGN_TOP);
	titleTxt.SetPosition(50,50);

	GuiSound btnSoundOver(button_over_pcm, button_over_pcm_size, SOUND_PCM);
	GuiSound btnSoundClick(button_click_pcm, button_click_pcm_size, SOUND_PCM);
	GuiImageData btnOutline(button_png);
	GuiImageData btnOutlineOver(button_over_png);
	GuiImageData btnLargeOutline(button_large_png);
	GuiImageData btnLargeOutlineOver(button_large_over_png);
	GuiImageData iconWiimote(icon_settings_wiimote_png);
	GuiImageData iconClassic(icon_settings_classic_png);
	GuiImageData iconGamecube(icon_settings_gamecube_png);
	GuiImageData iconNunchuk(icon_settings_nunchuk_png);
	GuiImageData iconWiiupro(icon_settings_wiiupro_png);
	GuiImageData iconDrc(icon_settings_drc_png);

	GuiText gamecubeBtnTxt("GameCube Controller", 22, (GXColor){0, 0, 0, 255});
	gamecubeBtnTxt.SetWrap(true, btnLargeOutline.GetWidth()-30);
	GuiImage gamecubeBtnImg(&btnLargeOutline);
	GuiImage gamecubeBtnImgOver(&btnLargeOutlineOver);
	GuiImage gamecubeBtnIcon(&iconGamecube);
	GuiButton gamecubeBtn(btnLargeOutline.GetWidth(), btnLargeOutline.GetHeight());
	gamecubeBtn.SetAlignment(ALIGN_CENTRE, ALIGN_TOP);
	gamecubeBtn.SetPosition(-125, 120);
	gamecubeBtn.SetLabel(&gamecubeBtnTxt);
	gamecubeBtn.SetImage(&gamecubeBtnImg);
	gamecubeBtn.SetImageOver(&gamecubeBtnImgOver);
	gamecubeBtn.SetIcon(&gamecubeBtnIcon);
	gamecubeBtn.SetSoundOver(&btnSoundOver);
	gamecubeBtn.SetSoundClick(&btnSoundClick);
	gamecubeBtn.SetTrigger(trigA);
	gamecubeBtn.SetEffectGrow();
	
	GuiText wiimoteBtnTxt("Wiimote", 22, (GXColor){0, 0, 0, 255});
	GuiImage wiimoteBtnImg(&btnLargeOutline);
	GuiImage wiimoteBtnImgOver(&btnLargeOutlineOver);
	GuiImage wiimoteBtnIcon(&iconWiimote);
	GuiButton wiimoteBtn(btnLargeOutline.GetWidth(), btnLargeOutline.GetHeight());
	wiimoteBtn.SetAlignment(ALIGN_CENTRE, ALIGN_TOP);
	wiimoteBtn.SetPosition(125, 120);
	wiimoteBtn.SetLabel(&wiimoteBtnTxt);
	wiimoteBtn.SetImage(&wiimoteBtnImg);
	wiimoteBtn.SetImageOver(&wiimoteBtnImgOver);
	wiimoteBtn.SetIcon(&wiimoteBtnIcon);
	wiimoteBtn.SetSoundOver(&btnSoundOver);
	wiimoteBtn.SetSoundClick(&btnSoundClick);
	wiimoteBtn.SetTrigger(trigA);
	wiimoteBtn.SetEffectGrow();

	GuiText drcBtnTxt("Wii U GamePad", 22, (GXColor){0, 0, 0, 255});
	drcBtnTxt.SetWrap(true, btnLargeOutline.GetWidth()-30);
	GuiImage drcBtnImg(&btnLargeOutline);
	GuiImage drcBtnImgOver(&btnLargeOutlineOver);
	GuiImage drcBtnIcon(&iconDrc);
	GuiButton drcBtn(btnLargeOutline.GetWidth(), btnLargeOutline.GetHeight());
	drcBtn.SetAlignment(ALIGN_CENTRE, ALIGN_TOP);
	drcBtn.SetPosition(200, 120);
	drcBtn.SetLabel(&drcBtnTxt);
	drcBtn.SetImage(&drcBtnImg);
	drcBtn.SetImageOver(&drcBtnImgOver);
	drcBtn.SetIcon(&drcBtnIcon);
	drcBtn.SetSoundOver(&btnSoundOver);
	drcBtn.SetSoundClick(&btnSoundClick);
	drcBtn.SetTrigger(trigA);
	drcBtn.SetEffectGrow();
	
	GuiText classicBtnTxt("Classic Controller", 22, (GXColor){0, 0, 0, 255});
	classicBtnTxt.SetWrap(true, btnLargeOutline.GetWidth()-30);
	GuiImage classicBtnImg(&btnLargeOutline);
	GuiImage classicBtnImgOver(&btnLargeOutlineOver);
	GuiImage classicBtnIcon(&iconClassic);
	GuiButton classicBtn(btnLargeOutline.GetWidth(), btnLargeOutline.GetHeight());
	classicBtn.SetAlignment(ALIGN_CENTRE, ALIGN_TOP);
	classicBtn.SetPosition(-200, 250);
	classicBtn.SetLabel(&classicBtnTxt);
	classicBtn.SetImage(&classicBtnImg);
	classicBtn.SetImageOver(&classicBtnImgOver);
	classicBtn.SetIcon(&classicBtnIcon);
	classicBtn.SetSoundOver(&btnSoundOver);
	classicBtn.SetSoundClick(&btnSoundClick);
	classicBtn.SetTrigger(trigA);
	classicBtn.SetEffectGrow();

	GuiText nunchukBtnTxt1("Wiimote", 22, (GXColor){0, 0, 0, 255});
	GuiText nunchukBtnTxt2("&", 18, (GXColor){0, 0, 0, 255});
	GuiText nunchukBtnTxt3("Nunchuk", 22, (GXColor){0, 0, 0, 255});
	nunchukBtnTxt1.SetPosition(0, -20);
	nunchukBtnTxt3.SetPosition(0, +20);
	GuiImage nunchukBtnImg(&btnLargeOutline);
	GuiImage nunchukBtnImgOver(&btnLargeOutlineOver);
	GuiImage nunchukBtnIcon(&iconNunchuk);
	GuiButton nunchukBtn(btnLargeOutline.GetWidth(), btnLargeOutline.GetHeight());
	nunchukBtn.SetAlignment(ALIGN_CENTRE, ALIGN_TOP);
	nunchukBtn.SetPosition(0, 250);
	nunchukBtn.SetLabel(&nunchukBtnTxt1, 0);
	nunchukBtn.SetLabel(&nunchukBtnTxt2, 1);
	nunchukBtn.SetLabel(&nunchukBtnTxt3, 2);
	nunchukBtn.SetImage(&nunchukBtnImg);
	nunchukBtn.SetImageOver(&nunchukBtnImgOver);
	nunchukBtn.SetIcon(&nunchukBtnIcon);
	nunchukBtn.SetSoundOver(&btnSoundOver);
	nunchukBtn.SetSoundClick(&btnSoundClick);
	nunchukBtn.SetTrigger(trigA);
	nunchukBtn.SetEffectGrow();

	GuiText wiiuproBtnTxt("Wii U Pro Controller", 22, (GXColor){0, 0, 0, 255});
	wiiuproBtnTxt.SetWrap(true, btnLargeOutline.GetWidth()-20);
	GuiImage wiiuproBtnImg(&btnLargeOutline);
	GuiImage wiiuproBtnImgOver(&btnLargeOutlineOver);
	GuiImage wiiuproBtnIcon(&iconWiiupro);
	GuiButton wiiuproBtn(btnLargeOutline.GetWidth(), btnLargeOutline.GetHeight());
	wiiuproBtn.SetAlignment(ALIGN_CENTRE, ALIGN_TOP);
	wiiuproBtn.SetPosition(200, 250);
	wiiuproBtn.SetLabel(&wiiuproBtnTxt);
	wiiuproBtn.SetImage(&wiiuproBtnImg);
	wiiuproBtn.SetImageOver(&wiiuproBtnImgOver);
	wiiuproBtn.SetIcon(&wiiuproBtnIcon);
	wiiuproBtn.SetSoundOver(&btnSoundOver);
	wiiuproBtn.SetSoundClick(&btnSoundClick);
	wiiuproBtn.SetTrigger(trigA);
	wiiuproBtn.SetEffectGrow();

	GuiText backBtnTxt("Go Back", 22, (GXColor){0, 0, 0, 255});
	GuiImage backBtnImg(&btnOutline);
	GuiImage backBtnImgOver(&btnOutlineOver);
	GuiButton backBtn(btnOutline.GetWidth(), btnOutline.GetHeight());
	backBtn.SetAlignment(ALIGN_LEFT, ALIGN_BOTTOM);
	backBtn.SetPosition(50, -35);
	backBtn.SetLabel(&backBtnTxt);
	backBtn.SetImage(&backBtnImg);
	backBtn.SetImageOver(&backBtnImgOver);
	backBtn.SetSoundOver(&btnSoundOver);
	backBtn.SetSoundClick(&btnSoundClick);
	backBtn.SetTrigger(trigA);
	backBtn.SetTrigger(trigB);
	backBtn.SetEffectGrow();

	HaltGui();
	GuiWindow w(screenwidth, screenheight);
	w.Append(&titleTxt);

	w.Append(&gamecubeBtn);
#ifdef HW_RVL
	w.Append(&wiimoteBtn);
	
	if(WiiDRC_Inited() && WiiDRC_Connected()) {
		gamecubeBtn.SetPosition(-200, 120);
		wiimoteBtn.SetPosition(0, 120);
		w.Append(&drcBtn);
	}
	
	w.Append(&classicBtn);
	w.Append(&nunchukBtn);
	w.Append(&wiiuproBtn);
#endif
	w.Append(&backBtn);

	mainWindow->Append(&w);

	ResumeGui();

	while(menu == MENU_NONE)
	{
		usleep(THREAD_SLEEP);

		if(wiimoteBtn.GetState() == STATE_CLICKED)
		{
			menu = MENU_GAMESETTINGS_MAPPINGS_MAP;
			mapMenuCtrl = CTRLR_WIIMOTE;
		}
		else if(nunchukBtn.GetState() == STATE_CLICKED)
		{
			menu = MENU_GAMESETTINGS_MAPPINGS_MAP;
			mapMenuCtrl = CTRLR_NUNCHUK;
		}
		else if(classicBtn.GetState() == STATE_CLICKED)
		{
			menu = MENU_GAMESETTINGS_MAPPINGS_MAP;
			mapMenuCtrl = CTRLR_CLASSIC;
		}
		else if(wiiuproBtn.GetState() == STATE_CLICKED)
		{
			menu = MENU_GAMESETTINGS_MAPPINGS_MAP;
			mapMenuCtrl = CTRLR_WUPC;
		}
		else if(drcBtn.GetState() == STATE_CLICKED)
		{
			menu = MENU_GAMESETTINGS_MAPPINGS_MAP;
			mapMenuCtrl = CTRLR_WIIDRC;
		}
		else if(gamecubeBtn.GetState() == STATE_CLICKED)
		{
			menu = MENU_GAMESETTINGS_MAPPINGS_MAP;
			mapMenuCtrl = CTRLR_GCPAD;
		}
		else if(backBtn.GetState() == STATE_CLICKED)
		{
			menu = MENU_GAMESETTINGS;
		}
	}
	HaltGui();
	mainWindow->Remove(&w);
	return menu;
}

/****************************************************************************
 * ButtonMappingWindow
 ***************************************************************************/
static u32
ButtonMappingWindow()
{
	GuiWindow promptWindow(448,288);
	promptWindow.SetAlignment(ALIGN_CENTRE, ALIGN_MIDDLE);
	promptWindow.SetPosition(0, -10);
	GuiSound btnSoundOver(button_over_pcm, button_over_pcm_size, SOUND_PCM);
	GuiSound btnSoundClick(button_click_pcm, button_click_pcm_size, SOUND_PCM);
	GuiImageData btnOutline(button_png);
	GuiImageData btnOutlineOver(button_over_png);

	GuiImageData dialogBox(dialogue_box_png);
	GuiImage dialogBoxImg(&dialogBox);

	GuiText titleTxt("Button Mapping", 26, (GXColor){255, 255, 255, 255});
	titleTxt.SetAlignment(ALIGN_CENTRE, ALIGN_TOP);
	titleTxt.SetPosition(0,14);

	char msg[200];

	switch(mapMenuCtrl)
	{
		case CTRLR_GCPAD:
			#ifdef HW_RVL
			sprintf(msg, "Press any button on the GameCube Controller now. Press Home or the C-Stick in any direction to clear the existing mapping.");
			#else
			sprintf(msg, "Press any button on the GameCube Controller now. Press the C-Stick in any direction to clear the existing mapping.");
			#endif
			break;
		case CTRLR_WIIMOTE:
			sprintf(msg, "Press any button on the Wiimote now. Press Home to clear the existing mapping.");
			break;
		case CTRLR_CLASSIC:
			sprintf(msg, "Press any button on the Classic Controller now. Press Home to clear the existing mapping.");
			break;
		case CTRLR_WUPC:
			sprintf(msg, "Press any button on the Wii U Pro Controller now. Press Home to clear the existing mapping.");
			break;
		case CTRLR_WIIDRC:
			sprintf(msg, "Press any button on the Wii U GamePad now. Press Home to clear the existing mapping.");
			break;
		case CTRLR_NUNCHUK:
			sprintf(msg, "Press any button on the Wiimote or Nunchuk now. Press Home to clear the existing mapping.");
			break;
	}

	GuiText msgTxt(msg, 26, (GXColor){0, 0, 0, 255});
	msgTxt.SetAlignment(ALIGN_CENTRE, ALIGN_MIDDLE);
	msgTxt.SetPosition(0,-20);
	msgTxt.SetWrap(true, 430);

	promptWindow.Append(&dialogBoxImg);
	promptWindow.Append(&titleTxt);
	promptWindow.Append(&msgTxt);

	HaltGui();
	mainWindow->SetState(STATE_DISABLED);
	mainWindow->Append(&promptWindow);
	mainWindow->ChangeFocus(&promptWindow);
	ResumeGui();

	u32 pressed = 0;

	while(pressed == 0)
	{
		usleep(THREAD_SLEEP);

		if(mapMenuCtrl == CTRLR_GCPAD)
		{
			pressed = userInput[0].pad.btns_d;


			if(userInput[0].pad.substickX < -70 ||
					userInput[0].pad.substickX > 70 ||
					userInput[0].pad.substickY < -70 ||
					userInput[0].pad.substickY > 70)
				pressed = WPAD_BUTTON_HOME;

			if(userInput[0].wpad->btns_d == WPAD_BUTTON_HOME)
				pressed = WPAD_BUTTON_HOME;
		}
		else if(mapMenuCtrl == CTRLR_WIIDRC)
		{
			pressed = userInput[0].wiidrcdata.btns_d;
		}
		else
		{
			pressed = userInput[0].wpad->btns_d;

			// always allow Home button to be pressed to cancel
			if(pressed != WPAD_BUTTON_HOME)
			{
				switch(mapMenuCtrl)
				{
					case CTRLR_WIIMOTE:
						if(pressed > 0x1000)
							pressed = 0; // not a valid input
						break;
					case CTRLR_CLASSIC:
						if(userInput[0].wpad->exp.type != WPAD_EXP_CLASSIC && userInput[0].wpad->exp.classic.type < 2)
							pressed = 0; // not a valid input
						else if(pressed <= 0x1000)
							pressed = 0;
						break;
					case CTRLR_WUPC:
						if(userInput[0].wpad->exp.type != WPAD_EXP_CLASSIC && userInput[0].wpad->exp.classic.type == 2)
							pressed = 0; // not a valid input
						else if(pressed <= 0x1000)
							pressed = 0;
						break;
					case CTRLR_NUNCHUK:
						if(userInput[0].wpad->exp.type != WPAD_EXP_NUNCHUK)
							pressed = 0; // not a valid input
						break;
				}
			}
		}
	}

	if(mapMenuCtrl == CTRLR_WIIDRC) {
		if(pressed == WIIDRC_BUTTON_HOME) {
			pressed = 0;
		}
	}
	else if(pressed == WPAD_BUTTON_HOME || pressed == WPAD_CLASSIC_BUTTON_HOME) {
		pressed = 0;
	}

	HaltGui();
	mainWindow->Remove(&promptWindow);
	mainWindow->SetState(STATE_DEFAULT);
	ResumeGui();

	return pressed;
}

static int MenuSettingsMappingsMap()
{
	int menu = MENU_NONE;
	int ret,i,j;
	bool firstRun = true;
	OptionList options;

	char menuTitle[100];
	char menuSubtitle[100];
	sprintf(menuTitle, "Game Settings - Button Mappings");

	GuiText titleTxt(menuTitle, 26, (GXColor){255, 255, 255, 255});
	titleTxt.SetAlignment(ALIGN_LEFT, ALIGN_TOP);
	titleTxt.SetPosition(50,30);

	sprintf(menuSubtitle, "%s", ctrlrName[mapMenuCtrl]);
	GuiText subtitleTxt(menuSubtitle, 20, (GXColor){255, 255, 255, 255});
	subtitleTxt.SetAlignment(ALIGN_LEFT, ALIGN_TOP);
	subtitleTxt.SetPosition(50,60);

	GuiSound btnSoundOver(button_over_pcm, button_over_pcm_size, SOUND_PCM);
	GuiSound btnSoundClick(button_click_pcm, button_click_pcm_size, SOUND_PCM);
	GuiImageData btnOutline(button_png);
	GuiImageData btnOutlineOver(button_over_png);
	GuiImageData btnShortOutline(button_short_png);
	GuiImageData btnShortOutlineOver(button_short_over_png);

	GuiText backBtnTxt("Go Back", 22, (GXColor){0, 0, 0, 255});
	GuiImage backBtnImg(&btnOutline);
	GuiImage backBtnImgOver(&btnOutlineOver);
	GuiButton backBtn(btnOutline.GetWidth(), btnOutline.GetHeight());
	backBtn.SetAlignment(ALIGN_LEFT, ALIGN_BOTTOM);
	backBtn.SetPosition(50, -35);
	backBtn.SetLabel(&backBtnTxt);
	backBtn.SetImage(&backBtnImg);
	backBtn.SetImageOver(&backBtnImgOver);
	backBtn.SetSoundOver(&btnSoundOver);
	backBtn.SetSoundClick(&btnSoundClick);
	backBtn.SetTrigger(trigA);
	backBtn.SetTrigger(trigB);
	backBtn.SetEffectGrow();

	GuiText resetBtnTxt("Reset Mappings", 22, (GXColor){0, 0, 0, 255});
	GuiImage resetBtnImg(&btnShortOutline);
	GuiImage resetBtnImgOver(&btnShortOutlineOver);
	GuiButton resetBtn(btnShortOutline.GetWidth(), btnShortOutline.GetHeight());
	resetBtn.SetAlignment(ALIGN_LEFT, ALIGN_BOTTOM);
	resetBtn.SetPosition(260, -35);
	resetBtn.SetLabel(&resetBtnTxt);
	resetBtn.SetImage(&resetBtnImg);
	resetBtn.SetImageOver(&resetBtnImgOver);
	resetBtn.SetSoundOver(&btnSoundOver);
	resetBtn.SetSoundClick(&btnSoundClick);
	resetBtn.SetTrigger(trigA);
	resetBtn.SetEffectGrow();

	i=0;
	sprintf(options.name[i++], "B");
	sprintf(options.name[i++], "A");
	sprintf(options.name[i++], "Select");
	sprintf(options.name[i++], "Start");
	sprintf(options.name[i++], "Up");
	sprintf(options.name[i++], "Down");
	sprintf(options.name[i++], "Left");
	sprintf(options.name[i++], "Right");
	sprintf(options.name[i++], "L");
	sprintf(options.name[i++], "R");
	// Not one of the 10 GBA buttons above (indices 0-9, backed by
	// btnmap[mapMenuCtrl][i]) - a separate action backed by
	// ffmap[mapMenuCtrl] instead (see vbagx.h). GC pad keeps its
	// hardcoded C-Stick-Right fast-forward and isn't remappable here -
	// see FastForwardHeld(), input.cpp.
	int idxFastForward = -1;
	if (mapMenuCtrl != CTRLR_GCPAD)
	{
		idxFastForward = i;
		sprintf(options.name[i++], "Fast Forward (Hold)");
	}
	options.length = i;

	for(i=0; i < options.length; i++)
		options.value[i][0] = 0;

	GuiOptionBrowser optionBrowser(552, 248, &options);
	optionBrowser.SetPosition(0, 108);
	optionBrowser.SetAlignment(ALIGN_CENTRE, ALIGN_TOP);
	optionBrowser.SetCol2Position(215);

	HaltGui();
	GuiWindow w(screenwidth, screenheight);
	w.Append(&backBtn);
	w.Append(&resetBtn);
	mainWindow->Append(&optionBrowser);
	mainWindow->Append(&w);
	mainWindow->Append(&titleTxt);
	mainWindow->Append(&subtitleTxt);
	ResumeGui();

	while(menu == MENU_NONE)
	{
		usleep(THREAD_SLEEP);

		if(backBtn.GetState() == STATE_CLICKED)
		{
			menu = MENU_GAMESETTINGS_MAPPINGS;
		}
		else if(resetBtn.GetState() == STATE_CLICKED)
		{
			resetBtn.ResetState();

			int choice = WindowPrompt(
				"Reset Mappings",
				"Are you sure that you want to reset your mappings?",
				"Yes",
				"No");

			if(choice == 1)
			{
				ResetControls(mapMenuCtrl);
				firstRun = true;
			}
		}

		ret = optionBrowser.GetClickedOption();

		if(ret >= 0 && ret == idxFastForward)
		{
			// Same "wait for next button press" capture as the GBA-button
			// rows below, just written into ffmap[mapMenuCtrl] instead of
			// btnmap[mapMenuCtrl][ret] - see vbagx.h's ffmap comment for
			// why this is a separate array. 0 (Home-to-clear) means
			// unassigned/off, same as any other cleared mapping.
			ffmap[mapMenuCtrl] = ButtonMappingWindow();
			firstRun = true; // force the display-value loop below to redraw this row
			ret = -1; // already handled - don't also fall into the btnmap path below
		}
		else if(ret >= 0)
		{
			// get a button selection from user
			btnmap[mapMenuCtrl][ret] = ButtonMappingWindow();
		}

		if(ret >= 0 || firstRun)
		{
			firstRun = false;

			for(i=0; i < options.length; i++)
			{
				if (i == idxFastForward)
				{
					// ffmap-backed row - same "look up the display name for
					// this raw button constant" lookup as the loop below,
					// just against ffmap[mapMenuCtrl] instead of
					// btnmap[mapMenuCtrl][i].
					options.value[i][0] = 0;
					if (ffmap[mapMenuCtrl] != 0)
					{
						for(j=0; j < ctrlr_def[mapMenuCtrl].num_btns; j++)
						{
							if(ffmap[mapMenuCtrl] == ctrlr_def[mapMenuCtrl].map[j].btn)
							{
								sprintf(options.value[i], ctrlr_def[mapMenuCtrl].map[j].name);
								break;
							}
						}
					}
					continue;
				}

				for(j=0; j < ctrlr_def[mapMenuCtrl].num_btns; j++)
				{
					if(btnmap[mapMenuCtrl][i] == 0)
					{
						options.value[i][0] = 0;
					}
					else if(btnmap[mapMenuCtrl][i] ==
						ctrlr_def[mapMenuCtrl].map[j].btn)
					{
						if(strcmp(options.value[i], ctrlr_def[mapMenuCtrl].map[j].name) != 0)
							sprintf(options.value[i], ctrlr_def[mapMenuCtrl].map[j].name);
						break;
					}
				}
			}
			optionBrowser.TriggerUpdate();
		}
	}

	HaltGui();
	mainWindow->Remove(&optionBrowser);
	mainWindow->Remove(&w);
	mainWindow->Remove(&titleTxt);
	mainWindow->Remove(&subtitleTxt);
	return menu;
}

/****************************************************************************
 * MenuSettingsVideo
 ***************************************************************************/

static void ScreenZoomWindowUpdate(void * ptr, float h, float v)
{
	GuiButton * b = (GuiButton *)ptr;
	if(b->GetState() == STATE_CLICKED)
	{
		char zoom[10], zoom2[10];
		
		if(IsGBAGame())
		{
			GCSettings.gbaZoomHor += h;
			GCSettings.gbaZoomVert += v;

			// Mirrors FixInvalidSettings()'s own gbaZoomHor/gbaZoomVert range
			// (preferences.cpp) - without this, zoom can be pushed past
			// [0.5, 1.6] here, display fine for the rest of this session,
			// and then get silently reset to 1.0 by FixInvalidSettings() the
			// moment SavePrefs() runs (every time this screen's Back button
			// is pressed), discarding whatever the user actually set. Same
			// clamp-on-every-click pattern ScreenPositionWindowUpdate()
			// already uses for xshift/yshift, just below.
			if(!(GCSettings.gbaZoomHor >= 0.5 && GCSettings.gbaZoomHor <= 1.6))
				GCSettings.gbaZoomHor = 1.0;
			if(!(GCSettings.gbaZoomVert >= 0.5 && GCSettings.gbaZoomVert <= 1.6))
				GCSettings.gbaZoomVert = 1.0;

			sprintf(zoom, "%.2f%%", GCSettings.gbaZoomHor*100);
			sprintf(zoom2, "%.2f%%", GCSettings.gbaZoomVert*100);
		}
		else
		{
			GCSettings.gbZoomHor += h;
			GCSettings.gbZoomVert += v;

			if(!(GCSettings.gbZoomHor >= 0.5 && GCSettings.gbZoomHor <= 1.6))
				GCSettings.gbZoomHor = 1.0;
			if(!(GCSettings.gbZoomVert >= 0.5 && GCSettings.gbZoomVert <= 1.6))
				GCSettings.gbZoomVert = 1.0;

			sprintf(zoom, "%.2f%%", GCSettings.gbZoomHor*100);
			sprintf(zoom2, "%.2f%%", GCSettings.gbZoomVert*100);
		}
		
		settingText->SetText(zoom);
		settingText2->SetText(zoom2);
		b->ResetState();
	}
}

static void ScreenZoomWindowLeftClick(void * ptr) { ScreenZoomWindowUpdate(ptr, -0.01, 0); }
static void ScreenZoomWindowRightClick(void * ptr) { ScreenZoomWindowUpdate(ptr, +0.01, 0); }
static void ScreenZoomWindowUpClick(void * ptr) { ScreenZoomWindowUpdate(ptr, 0, +0.01); }
static void ScreenZoomWindowDownClick(void * ptr) { ScreenZoomWindowUpdate(ptr, 0, -0.01); }

static void ScreenZoomWindow()
{
	GuiWindow * w = new GuiWindow(200,200);
	w->SetAlignment(ALIGN_CENTRE, ALIGN_MIDDLE);

	GuiTrigger trigLeft;
	trigLeft.SetButtonOnlyInFocusTrigger(-1, WPAD_BUTTON_LEFT | WPAD_CLASSIC_BUTTON_LEFT, PAD_BUTTON_LEFT, WIIDRC_BUTTON_LEFT);

	GuiTrigger trigRight;
	trigRight.SetButtonOnlyInFocusTrigger(-1, WPAD_BUTTON_RIGHT | WPAD_CLASSIC_BUTTON_RIGHT, PAD_BUTTON_RIGHT, WIIDRC_BUTTON_RIGHT);

	GuiTrigger trigUp;
	trigUp.SetButtonOnlyInFocusTrigger(-1, WPAD_BUTTON_UP | WPAD_CLASSIC_BUTTON_UP, PAD_BUTTON_UP, WIIDRC_BUTTON_UP);

	GuiTrigger trigDown;
	trigDown.SetButtonOnlyInFocusTrigger(-1, WPAD_BUTTON_DOWN | WPAD_CLASSIC_BUTTON_DOWN, PAD_BUTTON_DOWN, WIIDRC_BUTTON_DOWN);

	GuiImageData arrowLeft(button_arrow_left_png);
	GuiImage arrowLeftImg(&arrowLeft);
	GuiImageData arrowLeftOver(button_arrow_left_over_png);
	GuiImage arrowLeftOverImg(&arrowLeftOver);
	GuiButton arrowLeftBtn(arrowLeft.GetWidth(), arrowLeft.GetHeight());
	arrowLeftBtn.SetImage(&arrowLeftImg);
	arrowLeftBtn.SetImageOver(&arrowLeftOverImg);
	arrowLeftBtn.SetAlignment(ALIGN_CENTRE, ALIGN_MIDDLE);
	arrowLeftBtn.SetPosition(50, 0);
	arrowLeftBtn.SetTrigger(trigA);
	arrowLeftBtn.SetTrigger(&trigLeft);
	arrowLeftBtn.SetSelectable(false);
	arrowLeftBtn.SetUpdateCallback(ScreenZoomWindowLeftClick);

	GuiImageData arrowRight(button_arrow_right_png);
	GuiImage arrowRightImg(&arrowRight);
	GuiImageData arrowRightOver(button_arrow_right_over_png);
	GuiImage arrowRightOverImg(&arrowRightOver);
	GuiButton arrowRightBtn(arrowRight.GetWidth(), arrowRight.GetHeight());
	arrowRightBtn.SetImage(&arrowRightImg);
	arrowRightBtn.SetImageOver(&arrowRightOverImg);
	arrowRightBtn.SetAlignment(ALIGN_CENTRE, ALIGN_MIDDLE);
	arrowRightBtn.SetPosition(164, 0);
	arrowRightBtn.SetTrigger(trigA);
	arrowRightBtn.SetTrigger(&trigRight);
	arrowRightBtn.SetSelectable(false);
	arrowRightBtn.SetUpdateCallback(ScreenZoomWindowRightClick);

	GuiImageData arrowUp(button_arrow_up_png);
	GuiImage arrowUpImg(&arrowUp);
	GuiImageData arrowUpOver(button_arrow_up_over_png);
	GuiImage arrowUpOverImg(&arrowUpOver);
	GuiButton arrowUpBtn(arrowUp.GetWidth(), arrowUp.GetHeight());
	arrowUpBtn.SetImage(&arrowUpImg);
	arrowUpBtn.SetImageOver(&arrowUpOverImg);
	arrowUpBtn.SetAlignment(ALIGN_CENTRE, ALIGN_MIDDLE);
	arrowUpBtn.SetPosition(-76, -27);
	arrowUpBtn.SetTrigger(trigA);
	arrowUpBtn.SetTrigger(&trigUp);
	arrowUpBtn.SetSelectable(false);
	arrowUpBtn.SetUpdateCallback(ScreenZoomWindowUpClick);

	GuiImageData arrowDown(button_arrow_down_png);
	GuiImage arrowDownImg(&arrowDown);
	GuiImageData arrowDownOver(button_arrow_down_over_png);
	GuiImage arrowDownOverImg(&arrowDownOver);
	GuiButton arrowDownBtn(arrowDown.GetWidth(), arrowDown.GetHeight());
	arrowDownBtn.SetImage(&arrowDownImg);
	arrowDownBtn.SetImageOver(&arrowDownOverImg);
	arrowDownBtn.SetAlignment(ALIGN_CENTRE, ALIGN_MIDDLE);
	arrowDownBtn.SetPosition(-76, 27);
	arrowDownBtn.SetTrigger(trigA);
	arrowDownBtn.SetTrigger(&trigDown);
	arrowDownBtn.SetSelectable(false);
	arrowDownBtn.SetUpdateCallback(ScreenZoomWindowDownClick);

	GuiImageData screenPosition(screen_position_png);
	GuiImage screenPositionImg(&screenPosition);
	screenPositionImg.SetAlignment(ALIGN_CENTRE, ALIGN_MIDDLE);
	screenPositionImg.SetPosition(0, 0);

	settingText = new GuiText(NULL, 20, (GXColor){0, 0, 0, 255});
	settingText2 = new GuiText(NULL, 20, (GXColor){0, 0, 0, 255});
	char zoom[10], zoom2[10];
	float currentZoomHor, currentZoomVert;
	
	if(IsGBAGame())
	{
		sprintf(zoom, "%.2f%%", GCSettings.gbaZoomHor*100);
		sprintf(zoom2, "%.2f%%", GCSettings.gbaZoomVert*100);
		currentZoomHor = GCSettings.gbaZoomHor;
		currentZoomVert = GCSettings.gbaZoomVert;
	}
	else
	{
		sprintf(zoom, "%.2f%%", GCSettings.gbZoomHor*100);
		sprintf(zoom2, "%.2f%%", GCSettings.gbZoomVert*100);
		currentZoomHor = GCSettings.gbZoomHor;
		currentZoomVert = GCSettings.gbZoomVert;
	}

	settingText->SetText(zoom);
	settingText->SetPosition(108, 0);
	settingText2->SetText(zoom2);
	settingText2->SetPosition(-76, 0);

	w->Append(&arrowLeftBtn);
	w->Append(&arrowRightBtn);
	w->Append(&arrowUpBtn);
	w->Append(&arrowDownBtn);
	w->Append(&screenPositionImg);
	w->Append(settingText);
	w->Append(settingText2);
	
	char windowName[20];
	if(IsGBAGame())
		sprintf(windowName, "GBA Screen Zoom");
	else
		sprintf(windowName, "GB Screen Zoom");

	if(!SettingWindow(windowName,w))
	{
		// undo changes
		if(IsGBAGame())
		{
			GCSettings.gbaZoomHor = currentZoomHor;
			GCSettings.gbaZoomVert = currentZoomVert;
		}
		else
		{
			GCSettings.gbZoomHor = currentZoomHor;
			GCSettings.gbZoomVert = currentZoomVert;
		}
	}

	delete(w);
	delete(settingText);
	delete(settingText2);
}

static void ScreenPositionWindowUpdate(void * ptr, int x, int y)
{
	GuiButton * b = (GuiButton *)ptr;
	if(b->GetState() == STATE_CLICKED)
	{
		GCSettings.xshift += x;
		GCSettings.yshift += y;

		if(!(GCSettings.xshift > -50 && GCSettings.xshift < 50))
			GCSettings.xshift = 0;
		if(!(GCSettings.yshift > -50 && GCSettings.yshift < 50))
			GCSettings.yshift = 0;

		char shift[10];
		sprintf(shift, "%hd, %hd", GCSettings.xshift, GCSettings.yshift);
		settingText->SetText(shift);
		b->ResetState();
	}
}

static void ScreenPositionWindowLeftClick(void * ptr) { ScreenPositionWindowUpdate(ptr, -1, 0); }
static void ScreenPositionWindowRightClick(void * ptr) { ScreenPositionWindowUpdate(ptr, +1, 0); }
static void ScreenPositionWindowUpClick(void * ptr) { ScreenPositionWindowUpdate(ptr, 0, -1); }
static void ScreenPositionWindowDownClick(void * ptr) { ScreenPositionWindowUpdate(ptr, 0, +1); }

static void ScreenPositionWindow()
{
	GuiWindow * w = new GuiWindow(150,150);
	w->SetAlignment(ALIGN_CENTRE, ALIGN_MIDDLE);
	w->SetPosition(0, -10);

	GuiTrigger trigLeft;
	trigLeft.SetButtonOnlyInFocusTrigger(-1, WPAD_BUTTON_LEFT | WPAD_CLASSIC_BUTTON_LEFT, PAD_BUTTON_LEFT, WIIDRC_BUTTON_LEFT);

	GuiTrigger trigRight;
	trigRight.SetButtonOnlyInFocusTrigger(-1, WPAD_BUTTON_RIGHT | WPAD_CLASSIC_BUTTON_RIGHT, PAD_BUTTON_RIGHT, WIIDRC_BUTTON_RIGHT);

	GuiTrigger trigUp;
	trigUp.SetButtonOnlyInFocusTrigger(-1, WPAD_BUTTON_UP | WPAD_CLASSIC_BUTTON_UP, PAD_BUTTON_UP, WIIDRC_BUTTON_UP);

	GuiTrigger trigDown;
	trigDown.SetButtonOnlyInFocusTrigger(-1, WPAD_BUTTON_DOWN | WPAD_CLASSIC_BUTTON_DOWN, PAD_BUTTON_DOWN, WIIDRC_BUTTON_DOWN);

	GuiImageData arrowLeft(button_arrow_left_png);
	GuiImage arrowLeftImg(&arrowLeft);
	GuiImageData arrowLeftOver(button_arrow_left_over_png);
	GuiImage arrowLeftOverImg(&arrowLeftOver);
	GuiButton arrowLeftBtn(arrowLeft.GetWidth(), arrowLeft.GetHeight());
	arrowLeftBtn.SetImage(&arrowLeftImg);
	arrowLeftBtn.SetImageOver(&arrowLeftOverImg);
	arrowLeftBtn.SetAlignment(ALIGN_LEFT, ALIGN_MIDDLE);
	arrowLeftBtn.SetTrigger(trigA);
	arrowLeftBtn.SetTrigger(&trigLeft);
	arrowLeftBtn.SetSelectable(false);
	arrowLeftBtn.SetUpdateCallback(ScreenPositionWindowLeftClick);

	GuiImageData arrowRight(button_arrow_right_png);
	GuiImage arrowRightImg(&arrowRight);
	GuiImageData arrowRightOver(button_arrow_right_over_png);
	GuiImage arrowRightOverImg(&arrowRightOver);
	GuiButton arrowRightBtn(arrowRight.GetWidth(), arrowRight.GetHeight());
	arrowRightBtn.SetImage(&arrowRightImg);
	arrowRightBtn.SetImageOver(&arrowRightOverImg);
	arrowRightBtn.SetAlignment(ALIGN_RIGHT, ALIGN_MIDDLE);
	arrowRightBtn.SetTrigger(trigA);
	arrowRightBtn.SetTrigger(&trigRight);
	arrowRightBtn.SetSelectable(false);
	arrowRightBtn.SetUpdateCallback(ScreenPositionWindowRightClick);

	GuiImageData arrowUp(button_arrow_up_png);
	GuiImage arrowUpImg(&arrowUp);
	GuiImageData arrowUpOver(button_arrow_up_over_png);
	GuiImage arrowUpOverImg(&arrowUpOver);
	GuiButton arrowUpBtn(arrowUp.GetWidth(), arrowUp.GetHeight());
	arrowUpBtn.SetImage(&arrowUpImg);
	arrowUpBtn.SetImageOver(&arrowUpOverImg);
	arrowUpBtn.SetAlignment(ALIGN_CENTRE, ALIGN_TOP);
	arrowUpBtn.SetTrigger(trigA);
	arrowUpBtn.SetTrigger(&trigUp);
	arrowUpBtn.SetSelectable(false);
	arrowUpBtn.SetUpdateCallback(ScreenPositionWindowUpClick);

	GuiImageData arrowDown(button_arrow_down_png);
	GuiImage arrowDownImg(&arrowDown);
	GuiImageData arrowDownOver(button_arrow_down_over_png);
	GuiImage arrowDownOverImg(&arrowDownOver);
	GuiButton arrowDownBtn(arrowDown.GetWidth(), arrowDown.GetHeight());
	arrowDownBtn.SetImage(&arrowDownImg);
	arrowDownBtn.SetImageOver(&arrowDownOverImg);
	arrowDownBtn.SetAlignment(ALIGN_CENTRE, ALIGN_BOTTOM);
	arrowDownBtn.SetTrigger(trigA);
	arrowDownBtn.SetTrigger(&trigDown);
	arrowDownBtn.SetSelectable(false);
	arrowDownBtn.SetUpdateCallback(ScreenPositionWindowDownClick);

	GuiImageData screenPosition(screen_position_png);
	GuiImage screenPositionImg(&screenPosition);
	screenPositionImg.SetAlignment(ALIGN_CENTRE, ALIGN_MIDDLE);

	settingText = new GuiText(NULL, 20, (GXColor){0, 0, 0, 255});
	char shift[10];
	sprintf(shift, "%i, %i", GCSettings.xshift, GCSettings.yshift);
	settingText->SetText(shift);

	int currentX = GCSettings.xshift;
	int currentY = GCSettings.yshift;

	w->Append(&arrowLeftBtn);
	w->Append(&arrowRightBtn);
	w->Append(&arrowUpBtn);
	w->Append(&arrowDownBtn);
	w->Append(&screenPositionImg);
	w->Append(settingText);

	if(!SettingWindow("Screen Position",w))
	{
		// undo changes
		GCSettings.xshift = currentX;
		GCSettings.yshift = currentY;
	}

	delete(w);
	delete(settingText);
}

static int MenuSettingsVideo()
{
	int menu = MENU_NONE;
	int ret;
	int i = 0;
	bool firstRun = true;
	OptionList options;

	// gGbDmgMode reflects whether the currently loaded game is actually
	// rendering in real monochrome DMG mode right now (set in vbasupport.cpp
	// based on the resolved hardware model, not just the ROM's own color
	// capability - so a GBC game deliberately forced into DMG mode via the
	// Hardware setting correctly shows this too, while a GBC game running in
	// its native color mode correctly does not, since BasicPalette has no
	// effect on real color data).
	extern bool gGbDmgMode;
	bool isDmgGame = !IsGBAGame() && gGbDmgMode;
	int idxGbPalette = -1;
	int idxFilterMethod = -1;
	int idxColorEmulation = -1;
	int idxInterframeBlending = -1;

	sprintf(options.name[i++], "Rendering");
	sprintf(options.name[i++], "Scaling");
	if(IsGBAGame()) {
		sprintf(options.name[i++], "GBA Screen Zoom");
		sprintf(options.name[i++], "GBA Fixed Pixel Ratio");
	} else {
		sprintf(options.name[i++], "GB Screen Zoom");
		sprintf(options.name[i++], "GB Fixed Pixel Ratio");
	}
	sprintf(options.name[i++], "Screen Position");
	sprintf(options.name[i++], "Video Mode");
	idxFilterMethod = i;
	sprintf(options.name[i++], "Filter");
	if (isDmgGame) {
		idxGbPalette = i;
		sprintf(options.name[i++], "GB Color Emulation");
	} else if (IsGBAGame()) {
		idxColorEmulation = i;
		sprintf(options.name[i++], "GBA Color Emulation");
	} else {
		idxColorEmulation = i;
		sprintf(options.name[i++], "GBC Color Emulation");
	}
	idxInterframeBlending = i;
	sprintf(options.name[i++], "Interframe Blending");
	// Moved here from the pre-game Emulation settings menu, which isn't
	// reachable during gameplay - Frameskip is exactly the kind of thing
	// you want to be able to flip on/off in response to what's actually
	// happening on screen (e.g. a busy scene bogging down), not something
	// you'd only ever set once before starting.
	int idxFrameskip = i;
	sprintf(options.name[i++], "Frameskip");
	options.length = i;

	for(i=0; i < options.length; i++)
		options.value[i][0] = 0;

	GuiText titleTxt("Game Settings - Video", 26, (GXColor){255, 255, 255, 255});
	titleTxt.SetAlignment(ALIGN_LEFT, ALIGN_TOP);
	titleTxt.SetPosition(50,50);

	GuiSound btnSoundOver(button_over_pcm, button_over_pcm_size, SOUND_PCM);
	GuiSound btnSoundClick(button_click_pcm, button_click_pcm_size, SOUND_PCM);
	GuiImageData btnOutline(button_png);
	GuiImageData btnOutlineOver(button_over_png);

	GuiText backBtnTxt("Go Back", 22, (GXColor){0, 0, 0, 255});
	GuiImage backBtnImg(&btnOutline);
	GuiImage backBtnImgOver(&btnOutlineOver);
	GuiButton backBtn(btnOutline.GetWidth(), btnOutline.GetHeight());
	backBtn.SetAlignment(ALIGN_LEFT, ALIGN_BOTTOM);
	backBtn.SetPosition(50, -35);
	backBtn.SetLabel(&backBtnTxt);
	backBtn.SetImage(&backBtnImg);
	backBtn.SetImageOver(&backBtnImgOver);
	backBtn.SetSoundOver(&btnSoundOver);
	backBtn.SetSoundClick(&btnSoundClick);
	backBtn.SetTrigger(trigA);
	backBtn.SetTrigger(trigB);
	backBtn.SetEffectGrow();

	GuiOptionBrowser optionBrowser(552, 248, &options);
	optionBrowser.SetPosition(0, 108);
	optionBrowser.SetCol2Position(240);
	optionBrowser.SetAlignment(ALIGN_CENTRE, ALIGN_TOP);

	HaltGui();
	GuiWindow w(screenwidth, screenheight);
	w.Append(&backBtn);
	mainWindow->Append(&optionBrowser);
	mainWindow->Append(&w);
	mainWindow->Append(&titleTxt);
	ResumeGui();

	while(menu == MENU_NONE)
	{
		usleep(THREAD_SLEEP);

		ret = optionBrowser.GetClickedOption();

		switch (ret)
		{
			case 0:
				GCSettings.render++;
				if (GCSettings.render >= RENDER_LENGTH)
					GCSettings.render = RENDER_FILTERED;
				break;

			case 1:
				GCSettings.scaling++;
				if (GCSettings.scaling >= SCALING_LENGTH)
					GCSettings.scaling = SCALING_MAINTAIN_ASPECT;
				// disable Widescreen correction in Wii mode - determined automatically
				#ifdef HW_RVL
				if(GCSettings.scaling == SCALING_WIDESCREEN_CORRECTION)
					GCSettings.scaling = SCALING_MAINTAIN_ASPECT;
				#endif
				break;

			case 2:
				ScreenZoomWindow();
				break;

			case 3:
				if(IsGBAGame()) {
					GCSettings.gbaFixed++;
					if(GCSettings.gbaFixed > 3)
						GCSettings.gbaFixed = 0;
				} else {
					GCSettings.gbFixed++;
					if(GCSettings.gbFixed > 3)
						GCSettings.gbFixed = 0;
				}
				break;

			case 4:
				ScreenPositionWindow();
				break;

			case 5:
				GCSettings.videomode++;
				if(GCSettings.videomode >= VIDEOMODE_LENGTH)
					GCSettings.videomode = VIDEOMODE_AUTO;
				break;

			default:
				// ret == -1 (no click this poll - GetClickedOption()'s
				// idle sentinel) falls into this default case every
				// single loop iteration. idxGbPalette/idxColorEmulation
				// are only conditionally set above (whichever one doesn't
				// apply to the current game type is left at its -1 init
				// value) - without the "ret >= 0" guard, an idle -1 poll
				// would spuriously match a -1'd idx below and silently
                // increment that setting on every idle tick.
				if (ret >= 0 && ret == idxGbPalette) {
					GCSettings.BasicPalette++;
					if (GCSettings.BasicPalette > 3)
						GCSettings.BasicPalette = 0;
				} else if (ret >= 0 && ret == idxFilterMethod) {
					GCSettings.FilterMethod++;
					if (GCSettings.FilterMethod >= FILTER_LENGTH)
						GCSettings.FilterMethod = FILTER_NONE;
				} else if (ret >= 0 && ret == idxColorEmulation) {
					// Must stay in sync with vbasupport.cpp's kGBAMatrices/
					// kGBMatrices array length (7: Off/GBA/GBC/AGS-101/VBA/
					// PSP/NDS). Both lists contain every profile now, so a
					// GBA game can render through the GBC matrix and a GBC
					// game through the GBA one.
					if (IsGBAGame()) {
						GCSettings.GBAColorEmulation++;
						if (GCSettings.GBAColorEmulation >= 7)
							GCSettings.GBAColorEmulation = 0;
					} else {
						GCSettings.GBCColorEmulation++;
						if (GCSettings.GBCColorEmulation >= 7)
							GCSettings.GBCColorEmulation = 0;
					}
				} else if (ret >= 0 && ret == idxInterframeBlending) {
					GCSettings.InterframeBlending = !GCSettings.InterframeBlending;
				} else if (ret >= 0 && ret == idxFrameskip) {
					GCSettings.Frameskip++;
					if (GCSettings.Frameskip > 4)
						GCSettings.Frameskip = 0;
				}
				break;
		}

		if(ret >= 0 || firstRun)
		{
			firstRun = false;

			if (GCSettings.render == RENDER_FILTERED)
				sprintf (options.value[0], "Filtered (Auto)");
			else if (GCSettings.render == RENDER_UNFILTERED)
				sprintf (options.value[0], "Unfiltered");
			else if (GCSettings.render == RENDER_FILTERED_SHARP)
				sprintf (options.value[0], "Filtered (Sharp)");
			else if (GCSettings.render == RENDER_FILTERED_SOFT)
				sprintf (options.value[0], "Filtered (Soft)");

			if (GCSettings.scaling == SCALING_MAINTAIN_ASPECT)
				sprintf (options.value[1], "Maintain Aspect Ratio");
			else if (GCSettings.scaling == SCALING_PARTIAL_STRETCH)
				sprintf (options.value[1], "Partial Stretch");
			else if (GCSettings.scaling == SCALING_STRETCH_TO_FIT)
				sprintf (options.value[1], "Stretch to Fit");
			else if (GCSettings.scaling == SCALING_WIDESCREEN_CORRECTION)
				sprintf (options.value[1], "16:9 Correction");

			int fixed;
			if(IsGBAGame()) {
				sprintf (options.value[2], "%.2f%%, %.2f%%", GCSettings.gbaZoomHor*100, GCSettings.gbaZoomVert*100);
				fixed = GCSettings.gbaFixed;
			} else {
				sprintf (options.value[2], "%.2f%%, %.2f%%", GCSettings.gbZoomHor*100, GCSettings.gbZoomVert*100);
				fixed = GCSettings.gbFixed;
			}

			if (fixed) {
				int w = fixed / 10;
				int ratio = fixed % 10;
				const char* widescreen = w
					? "(16:9 Correction)"
					: "";
				
				sprintf (options.value[3], "%dx %s", ratio, widescreen);
			} else {
				sprintf (options.value[3], "Disabled");
			}

			sprintf (options.value[4], "%d, %d", GCSettings.xshift, GCSettings.yshift);

			switch(GCSettings.videomode)
			{
				case VIDEOMODE_AUTO:
					sprintf (options.value[5], "Automatic (Recommended)"); break;
				case VIDEOMODE_NTSC:
					sprintf (options.value[5], "NTSC (480i)"); break;
				case VIDEOMODE_PROGRESSIVE:
					sprintf (options.value[5], "NTSC (480p)"); break;
				case VIDEOMODE_PAL:
					sprintf (options.value[5], "PAL (576i)"); break;
				case VIDEOMODE_EURGB:
					sprintf (options.value[5], "European RGB (240i)"); break;
				case VIDEOMODE_240P:
					sprintf (options.value[5], "NTSC (240p)"); break;
				case VIDEOMODE_EURGB_240P:
					sprintf (options.value[5], "European RGB (240p)"); break;
			}

			// Takes effect immediately, live, on the next rendered frame -
			// no game reset needed. ApplyGBPalette() (vbasupport.cpp) reads
			// GCSettings.BasicPalette fresh every frame while gGbDmgMode is
			// true, so simply changing the value above is enough.
			if (idxGbPalette >= 0) {
				if (GCSettings.BasicPalette == 0)
					sprintf (options.value[idxGbPalette], "Green Screen");
				else if (GCSettings.BasicPalette == 1)
					sprintf (options.value[idxGbPalette], "Monochrome Screen");
				else if (GCSettings.BasicPalette == 2)
					sprintf (options.value[idxGbPalette], "Game Boy Pocket");
				else
					sprintf (options.value[idxGbPalette], "Game Boy Light");
			}

			// Takes effect immediately, live, every frame - configure_tev_pipeline()
			// (video.cpp) reads GCSettings.FilterMethod fresh at the top of every
			// GX_Render() call for the scanline filter (real GX TEV hardware work),
			// and GX_Render() itself reads it for Scale2x (a CPU-side pre-filter
			// applied before the frame reaches the border-compositing/swizzle
			// path - see GX_Render() in video.cpp for its current scope limits:
			// no SGB border, no Fixed Pixel Ratio yet). hq2x/2xBR/DDT from the
			// same VBA-GX 3.0.0 release aren't ported yet.
			if (idxFilterMethod >= 0) {
				sprintf (options.value[idxFilterMethod], "%s",
					GCSettings.FilterMethod == FILTER_SCANLINES ? "Scanlines" :
					GCSettings.FilterMethod == FILTER_SCALE2X ? "Scale2x" :
					GCSettings.FilterMethod == FILTER_SHARP_BILINEAR ? "Sharp Bilinear" : "None");
			}

			// Takes effect immediately, live, on the next rendered frame -
			// mgba_emuMain() (vbasupport.cpp) reads GCSettings.GBAColorEmulation/
			// GBCColorEmulation fresh every frame, same as GB Palette above.
			// Order/count must stay in sync with vbasupport.cpp's
			// kGBAMatrices/kGBMatrices arrays.
			if (idxColorEmulation >= 0) {
				int idx = IsGBAGame() ? GCSettings.GBAColorEmulation : GCSettings.GBCColorEmulation;
				const char *names[7] = {
					"Off",
					"GBA",
					"GBC",
					"GBA SP (AGS-101)",
					"VBA",
					"PSP",
					"NDS"
				};
				if (idx < 0 || idx >= 7) idx = 0; // defensive, shouldn't happen
				sprintf (options.value[idxColorEmulation], "%s", names[idx]);
			}

			// Takes effect immediately, live, next frame - mgba_emuMain()
			// (vbasupport.cpp) reads GCSettings.InterframeBlending fresh
			// every frame.
			if (idxInterframeBlending >= 0) {
				sprintf (options.value[idxInterframeBlending], "%s",
					GCSettings.InterframeBlending ? "On" : "Off");
			}

			if (GCSettings.Frameskip == 0)
				sprintf (options.value[idxFrameskip], "Off");
			else
				sprintf (options.value[idxFrameskip], "%d", GCSettings.Frameskip);

			optionBrowser.TriggerUpdate();
		}

		if(backBtn.GetState() == STATE_CLICKED)
		{
			menu = MENU_GAMESETTINGS;
			// Persist immediately - this screen is reached in-game, a
			// separate menu tree from the main pre-game Settings hub (see
			// MenuGameSettings()'s equivalent comment), so don't rely on
			// backing all the way out through that other tree to save.
			SavePrefs(SILENT);
		}
	}
	HaltGui();
	mainWindow->Remove(&optionBrowser);
	mainWindow->Remove(&w);
	mainWindow->Remove(&titleTxt);
	return menu;
}

/****************************************************************************
 * CheatsDeletePicker
 *
 * Small nested modal opened from MenuGameCheats() below: a plain list of
 * existing cheats (name only, no On/Off column) - clicking one confirms
 * via WindowPrompt(), then deletes it. Returns once the user backs out
 * or a delete happens (the caller rebuilds its own list either way).
 ***************************************************************************/
static void CheatsDeletePicker()
{
	// See MAX_CHEATS in vbagx.h - shared cap matching Snes9x TX's own
	// MAX_CHEATS convention (cheatmgr.cpp).
	const int MAX_CHEATS_SHOWN = MAX_CHEATS;

	int count = CheatCount();
	if (count <= 0)
		return;
	if (count > MAX_CHEATS_SHOWN)
		count = MAX_CHEATS_SHOWN;

	int menu = MENU_NONE;
	int ret, i;
	OptionList options;

	for (i = 0; i < count; i++)
	{
		char desc[64];
		bool enabled = false;
		CheatGetInfo(i, desc, sizeof(desc), &enabled);
		snprintf(options.name[i], sizeof(options.name[i]), "%s", desc);
		options.value[i][0] = 0;
	}
	options.length = i;

	GuiText titleTxt("Delete a Cheat", 26, (GXColor){255, 255, 255, 255});
	titleTxt.SetAlignment(ALIGN_LEFT, ALIGN_TOP);
	titleTxt.SetPosition(50, 50);

	GuiSound btnSoundOver(button_over_pcm, button_over_pcm_size, SOUND_PCM);
	GuiSound btnSoundClick(button_click_pcm, button_click_pcm_size, SOUND_PCM);
	GuiImageData btnOutline(button_png);
	GuiImageData btnOutlineOver(button_over_png);

	GuiText backBtnTxt("Go Back", 22, (GXColor){0, 0, 0, 255});
	GuiImage backBtnImg(&btnOutline);
	GuiImage backBtnImgOver(&btnOutlineOver);
	GuiButton backBtn(btnOutline.GetWidth(), btnOutline.GetHeight());
	backBtn.SetAlignment(ALIGN_LEFT, ALIGN_BOTTOM);
	backBtn.SetPosition(50, -35);
	backBtn.SetLabel(&backBtnTxt);
	backBtn.SetImage(&backBtnImg);
	backBtn.SetImageOver(&backBtnImgOver);
	backBtn.SetSoundOver(&btnSoundOver);
	backBtn.SetSoundClick(&btnSoundClick);
	backBtn.SetTrigger(trigA);
	backBtn.SetTrigger(trigB);
	backBtn.SetEffectGrow();

	GuiOptionBrowser optionBrowser(552, 248, &options);
	optionBrowser.SetPosition(0, 108);
	optionBrowser.SetAlignment(ALIGN_CENTRE, ALIGN_TOP);

	HaltGui();
	GuiWindow w(screenwidth, screenheight);
	w.Append(&backBtn);
	mainWindow->Append(&optionBrowser);
	mainWindow->Append(&w);
	mainWindow->Append(&titleTxt);
	ResumeGui();

	while (menu == MENU_NONE)
	{
		usleep(THREAD_SLEEP);
		ret = optionBrowser.GetClickedOption();

		if (ret >= 0 && ret < count)
		{
			char msg[96];
			snprintf(msg, sizeof(msg), "Delete \"%s\"?", options.name[ret]);
			if (WindowPrompt("Delete Cheat", msg, "Delete", "Cancel"))
			{
				CheatDelete(ret);
				menu = MENU_GAME; // any non-NONE value - just used to exit this loop
			}
		}

		if (backBtn.GetState() == STATE_CLICKED)
			menu = MENU_GAME;
	}

	HaltGui();
	mainWindow->Remove(&optionBrowser);
	mainWindow->Remove(&w);
	mainWindow->Remove(&titleTxt);
}

/****************************************************************************
 * MenuGameCheats
 *
 * Blocking modal, not part of the MENU_* state machine (the enum lives in
 * a header this fork doesn't have on hand - see the call site in
 * MenuSettingsEmulation() for the same reasoning already used for
 * OnScreenKeyboard()). One GuiOptionBrowser row per cheat for the current
 * ROM; clicking a cheat row toggles it On/Off in place, the same
 * click-to-toggle interaction Snes9x GX/Snes9x TX's cheat lists use. Row
 * 0 is always "Add New Cheat..." (two OnScreenKeyboard prompts -
 * description, then the code itself - handed to CheatAdd(), which relies
 * on mGBA's own cheat parser to auto-detect Game Genie vs GameShark/
 * Action Replay format from the code text, so there's no separate format
 * picker to build). Row 1 is "Delete a Cheat..." (only shown once at
 * least one cheat exists), opening CheatsDeletePicker() above.
 *
 * Adding/deleting a cheat changes the row COUNT, which this simple
 * fixed-OptionList screen doesn't support mutating in place - those two
 * actions just tail-recurse back into a fresh call of this function
 * instead of trying to patch the existing GuiOptionBrowser's rows.
 * Toggling only changes a value string, so that one updates in place.
 ***************************************************************************/
static void MenuGameCheats()
{
	// See MAX_CHEATS in vbagx.h - shared cap matching Snes9x TX's own
	// MAX_CHEATS convention (cheatmgr.cpp).
	const int MAX_CHEATS_SHOWN = MAX_CHEATS;

	int menu = MENU_NONE;
	int ret, i;
	OptionList options;

	int cheatCount = CheatCount();
	if (cheatCount > MAX_CHEATS_SHOWN)
		cheatCount = MAX_CHEATS_SHOWN;

	i = 0;
	int idxAddCheat = i;
	sprintf(options.name[i++], "Add New Cheat...");
	options.value[idxAddCheat][0] = 0;

	int idxDeleteCheat = -1;
	if (cheatCount > 0)
	{
		idxDeleteCheat = i;
		sprintf(options.name[i++], "Delete a Cheat...");
		options.value[idxDeleteCheat][0] = 0;
	}

	int cheatRowStart = i;
	for (int c = 0; c < cheatCount; c++)
	{
		char desc[64];
		bool enabled = false;
		CheatGetInfo(c, desc, sizeof(desc), &enabled);
		snprintf(options.name[i], sizeof(options.name[i]), "%s", desc);
		sprintf(options.value[i], "%s", enabled ? "On" : "Off");
		i++;
	}
	options.length = i;

	if (cheatCount == 0)
	{
		// Nothing to toggle yet - still show the screen (so "Add New
		// Cheat..." is reachable), just skip straight to it being empty.
	}

	// No title text here on purpose - this screen is only ever shown on top
	// of another screen (the pause menu, or the Settings menu) that's been
	// temporarily hidden by the caller, and that other screen already has
	// its own title (usually the game's title) sitting at this same
	// top-left spot. A second "Cheats" title here just overlapped it.

	GuiSound btnSoundOver(button_over_pcm, button_over_pcm_size, SOUND_PCM);
	GuiSound btnSoundClick(button_click_pcm, button_click_pcm_size, SOUND_PCM);
	GuiImageData btnOutline(button_png);
	GuiImageData btnOutlineOver(button_over_png);

	GuiText backBtnTxt("Go Back", 22, (GXColor){0, 0, 0, 255});
	GuiImage backBtnImg(&btnOutline);
	GuiImage backBtnImgOver(&btnOutlineOver);
	GuiButton backBtn(btnOutline.GetWidth(), btnOutline.GetHeight());
	backBtn.SetAlignment(ALIGN_LEFT, ALIGN_BOTTOM);
	backBtn.SetPosition(50, -35);
	backBtn.SetLabel(&backBtnTxt);
	backBtn.SetImage(&backBtnImg);
	backBtn.SetImageOver(&backBtnImgOver);
	backBtn.SetSoundOver(&btnSoundOver);
	backBtn.SetSoundClick(&btnSoundClick);
	backBtn.SetTrigger(trigA);
	backBtn.SetTrigger(trigB);
	backBtn.SetEffectGrow();

	GuiOptionBrowser optionBrowser(552, 248, &options);
	optionBrowser.SetPosition(0, 108);
	optionBrowser.SetAlignment(ALIGN_CENTRE, ALIGN_TOP);

	HaltGui();
	GuiWindow w(screenwidth, screenheight);
	w.Append(&backBtn);
	mainWindow->Append(&optionBrowser);
	mainWindow->Append(&w);
	mainWindow->ChangeFocus(&w);
	ResumeGui();

	bool needsRebuild = false;

	while (menu == MENU_NONE)
	{
		usleep(THREAD_SLEEP);
		ret = optionBrowser.GetClickedOption();

		if (ret == idxAddCheat)
		{
			char desc[64] = "";
			// OnScreenKeyboard() (this file, above) is void - it edits the
			// buffer in place and leaves it untouched if the user backs
			// out, same as every other settings field that uses it (e.g.
			// GCSettings.SaveFolder above). Empty after the call is
			// treated as "cancelled".
			OnScreenKeyboard(desc, sizeof(desc));
			if (desc[0] != 0)
			{
				char code[256] = "";
				OnScreenKeyboard(code, sizeof(code));
				if (code[0] != 0)
				{
					if (!CheatAdd(desc, code))
						InfoPrompt("Could not parse that cheat code.");
					needsRebuild = true;
					menu = MENU_GAME;
				}
			}
		}
		else if (idxDeleteCheat >= 0 && ret == idxDeleteCheat)
		{
			CheatsDeletePicker();
			needsRebuild = true;
			menu = MENU_GAME;
		}
		else if (ret >= cheatRowStart && ret < options.length)
		{
			CheatToggle(ret - cheatRowStart);
			bool enabled = false;
			char desc[64];
			CheatGetInfo(ret - cheatRowStart, desc, sizeof(desc), &enabled);
			sprintf(options.value[ret], "%s", enabled ? "On" : "Off");
			optionBrowser.TriggerUpdate();
		}

		if (backBtn.GetState() == STATE_CLICKED)
			menu = MENU_GAME;
	}

	HaltGui();
	mainWindow->Remove(&optionBrowser);
	mainWindow->Remove(&w);

	// Row count changed (add/delete) - rebuild the whole screen instead of
	// trying to mutate options.length on a live GuiOptionBrowser. Does NOT
	// recurse if the user just pressed Back with no changes.
	if (needsRebuild)
		MenuGameCheats();
}

/****************************************************************************
 * MenuSettingsEmulation
 ***************************************************************************/
static int MenuSettingsEmulation()
{
	int menu = MENU_NONE;
	int ret;
	int i = 0;
	bool firstRun = true;
	OptionList options;

	sprintf(options.name[i++], "Hardware (GB/GBC)");
	sprintf(options.name[i++], "Super Game Boy border");
	sprintf(options.name[i++], "Offset from UTC (hours)");
#ifdef HW_RVL
	sprintf(options.name[i++], "Wii Remote Tilt Control");
#endif
	int idxGBABorder = i;
	sprintf(options.name[i++], "Border (GBA)");
	int idxGBBorder = i;
	sprintf(options.name[i++], "Border (GB/GBC)");
	int idxFastForward = i;
	sprintf(options.name[i++], "Fast Forward Speed");
	options.length = i;

	// Scan the same "borders" folder LoadGBABorderIfEnabled() (vbasupport.cpp)
	// actually reads from, once, for the "None" + cycle-through-files list.
	// Reuses the browser/browserList/ParseDirectory mechanism the save-state
	// screen already uses to scan a folder into a flat filename list, rather
	// than introducing a second directory-scanning path.
	const int MAX_GBA_BORDERS = 64;
	static char gbaBorderList[64][MAXJOLIET+1];
	int gbaBorderCount = 0;
	int gbaBorderIndex = 0; // 0 = None

	{
		sprintf(browser.dir, "%s%s", pathPrefix[GCSettings.LoadMethod], GCSettings.GBABorderFolder);
		ParseDirectory(true, false);

		for(int bi=0; bi < browser.numEntries && gbaBorderCount < MAX_GBA_BORDERS; bi++)
		{
			if(browserList[bi].isdir) continue;

			int blen = strlen(browserList[bi].filename);
			if(blen <= 4) continue;
			if(strcasecmp(&browserList[bi].filename[blen-4], ".png") != 0 &&
			   strcasecmp(&browserList[bi].filename[blen-4], ".bor") != 0 &&
			   strcasecmp(&browserList[bi].filename[blen-4], ".bmp") != 0)
				continue;

			snprintf(gbaBorderList[gbaBorderCount], MAXJOLIET+1, "%s", browserList[bi].filename);
			gbaBorderCount++;
		}

		// Find the currently-selected file in the freshly scanned list, so
		// the displayed value matches GCSettings.GBABorderFile even if it
		// was set in a previous session (or the folder's contents changed).
		if(GCSettings.GBABorderFile[0] != 0)
		{
			for(int bi=0; bi < gbaBorderCount; bi++)
			{
				if(strcasecmp(gbaBorderList[bi], GCSettings.GBABorderFile) == 0)
				{
					gbaBorderIndex = bi + 1; // +1 because index 0 is "None"
					break;
				}
			}
		}
	}

	// Same scan, filtered to .png/.sgb/.bmp files, for the GB/GBC border
	// list (GCSettings.GBBorderFile). All three file types are cycled
	// together as one list - which format a given entry is gets sorted out
	// by extension at load time in LoadGBBorderFileIfEnabled()
	// (vbasupport.cpp), same as the GBA list above. Independent folder
	// from the GBA one (GCSettings.GBCBorderFolder vs GBABorderFolder).
	const int MAX_GB_BORDERS = 64;
	static char gbBorderList[64][MAXJOLIET+1];
	int gbBorderCount = 0;
	int gbBorderIndex = 0; // 0 = None

	{
		sprintf(browser.dir, "%s%s", pathPrefix[GCSettings.LoadMethod], GCSettings.GBCBorderFolder);
		ParseDirectory(true, false);

		for(int bi=0; bi < browser.numEntries && gbBorderCount < MAX_GB_BORDERS; bi++)
		{
			if(browserList[bi].isdir) continue;

			int blen = strlen(browserList[bi].filename);
			if(blen <= 4) continue;
			if(strcasecmp(&browserList[bi].filename[blen-4], ".png") != 0 &&
			   strcasecmp(&browserList[bi].filename[blen-4], ".sgb") != 0 &&
			   strcasecmp(&browserList[bi].filename[blen-4], ".bmp") != 0)
				continue;

			snprintf(gbBorderList[gbBorderCount], MAXJOLIET+1, "%s", browserList[bi].filename);
			gbBorderCount++;
		}

		if(GCSettings.GBBorderFile[0] != 0)
		{
			for(int bi=0; bi < gbBorderCount; bi++)
			{
				if(strcasecmp(gbBorderList[bi], GCSettings.GBBorderFile) == 0)
				{
					gbBorderIndex = bi + 1;
					break;
				}
			}
		}
	}

	for(i=0; i < options.length; i++)
		options.value[i][0] = 0;
	
	GuiText titleTxt("Game Settings - Emulation", 26, (GXColor){255, 255, 255, 255});
	titleTxt.SetAlignment(ALIGN_LEFT, ALIGN_TOP);
	titleTxt.SetPosition(50,50);

	GuiSound btnSoundOver(button_over_pcm, button_over_pcm_size, SOUND_PCM);
	GuiSound btnSoundClick(button_click_pcm, button_click_pcm_size, SOUND_PCM);
	GuiImageData btnOutline(button_png);
	GuiImageData btnOutlineOver(button_over_png);

	GuiText backBtnTxt("Go Back", 22, (GXColor){0, 0, 0, 255});
	GuiImage backBtnImg(&btnOutline);
	GuiImage backBtnImgOver(&btnOutlineOver);
	GuiButton backBtn(btnOutline.GetWidth(), btnOutline.GetHeight());
	backBtn.SetAlignment(ALIGN_LEFT, ALIGN_BOTTOM);
	backBtn.SetPosition(50, -35);
	backBtn.SetLabel(&backBtnTxt);
	backBtn.SetImage(&backBtnImg);
	backBtn.SetImageOver(&backBtnImgOver);
	backBtn.SetSoundOver(&btnSoundOver);
	backBtn.SetSoundClick(&btnSoundClick);
	backBtn.SetTrigger(trigA);
	backBtn.SetTrigger(trigB);
	backBtn.SetEffectGrow();

	GuiOptionBrowser optionBrowser(552, 248, &options);
	optionBrowser.SetPosition(0, 108);
	optionBrowser.SetCol2Position(240);
	optionBrowser.SetAlignment(ALIGN_CENTRE, ALIGN_TOP);

	HaltGui();
	GuiWindow w(screenwidth, screenheight);
	w.Append(&backBtn);
	mainWindow->Append(&optionBrowser);
	mainWindow->Append(&w);
	mainWindow->Append(&titleTxt);
	ResumeGui();

	while(menu == MENU_NONE)
	{
		usleep(THREAD_SLEEP);

		ret = optionBrowser.GetClickedOption();

		switch (ret)
		{
			case 0:
				GCSettings.GBHardware++;
				if (GCSettings.GBHardware > 5)
					GCSettings.GBHardware = 0;
				break;
			
			case 1:
				GCSettings.SGBBorder++;
				if (GCSettings.SGBBorder > 2)
					GCSettings.SGBBorder = 0;
				break;
			
			case 2:
				GCSettings.OffsetMinutesUTC += 15;
				if (GCSettings.OffsetMinutesUTC > 60*14) {
					GCSettings.OffsetMinutesUTC = -60*12;
				}
				break;
#ifdef HW_RVL
			case 3:
				GCSettings.MotionTilt ^= 1;
				break;
#endif
		}

		if (ret == idxGBABorder)
		{
			gbaBorderIndex++;
			if (gbaBorderIndex > gbaBorderCount)
				gbaBorderIndex = 0;

			if (gbaBorderIndex == 0)
				GCSettings.GBABorderFile[0] = 0; // None
			else
				snprintf(GCSettings.GBABorderFile, sizeof(GCSettings.GBABorderFile),
				         "%s", gbaBorderList[gbaBorderIndex-1]);
		}

		if (ret == idxGBBorder)
		{
			gbBorderIndex++;
			if (gbBorderIndex > gbBorderCount)
				gbBorderIndex = 0;

			if (gbBorderIndex == 0)
				GCSettings.GBBorderFile[0] = 0; // None
			else
				snprintf(GCSettings.GBBorderFile, sizeof(GCSettings.GBBorderFile),
				         "%s", gbBorderList[gbBorderIndex-1]);
		}

		if (ret == idxFastForward)
		{
			GCSettings.FastForwardSpeed++;
			if (GCSettings.FastForwardSpeed > 3)
				GCSettings.FastForwardSpeed = 0;
		}

		if(ret >= 0 || firstRun)
		{
			firstRun = false;

			if (GCSettings.GBHardware == 0)
				sprintf (options.value[0], "Auto");
			else if (GCSettings.GBHardware == 1)
				sprintf (options.value[0], "Game Boy Color");
			else if (GCSettings.GBHardware == 2)
				sprintf (options.value[0], "Super Game Boy");
			else if (GCSettings.GBHardware == 3)
				sprintf (options.value[0], "Super Game Boy 2");
			else if (GCSettings.GBHardware == 4)
				sprintf (options.value[0], "Game Boy");
			else if (GCSettings.GBHardware == 5)
				sprintf (options.value[0], "Game Boy Advance");
			
			if (GCSettings.SGBBorder == 0)
				sprintf (options.value[1], "Off");
			else if (GCSettings.SGBBorder == 1)
				sprintf (options.value[1], "From game (SGB only)");
			else if (GCSettings.SGBBorder == 2)
				sprintf (options.value[1], "From border file");
			
			sprintf (options.value[2], "%+.2f", GCSettings.OffsetMinutesUTC / 60.0);

#ifdef HW_RVL
			sprintf (options.value[3], "%s", GCSettings.MotionTilt ? "On" : "Off");
#endif
			sprintf (options.value[idxGBABorder], "%s",
			         gbaBorderIndex == 0 ? "None" : gbaBorderList[gbaBorderIndex-1]);

			sprintf (options.value[idxGBBorder], "%s",
			         gbBorderIndex == 0 ? "None" : gbBorderList[gbBorderIndex-1]);

			if (GCSettings.FastForwardSpeed == 0)
				sprintf (options.value[idxFastForward], "Off");
			else
				sprintf (options.value[idxFastForward], "%dx", GCSettings.FastForwardSpeed + 1);

			optionBrowser.TriggerUpdate();
		}

		if(backBtn.GetState() == STATE_CLICKED)
		{
			menu = MENU_SETTINGS;
			// Persist changes made here (GB Hardware, SGB Border, UTC
			// offset) immediately on leaving this screen.
			SavePrefs(SILENT);
		}
	}
	HaltGui();
	InitialisePalette();
	mainWindow->Remove(&optionBrowser);
	mainWindow->Remove(&w);
	mainWindow->Remove(&titleTxt);
	return menu;
}

/****************************************************************************
 * MenuSettings
 ***************************************************************************/
static int MenuSettings()
{
	int menu = MENU_NONE;

	GuiText titleTxt("Settings", 26, (GXColor){255, 255, 255, 255});
	titleTxt.SetAlignment(ALIGN_LEFT, ALIGN_TOP);
	titleTxt.SetPosition(50,50);

	GuiSound btnSoundOver(button_over_pcm, button_over_pcm_size, SOUND_PCM);
	GuiSound btnSoundClick(button_click_pcm, button_click_pcm_size, SOUND_PCM);
	GuiImageData btnOutline(button_long_png);
	GuiImageData btnOutlineOver(button_long_over_png);
	GuiImageData btnLargeOutline(button_large_png);
	GuiImageData btnLargeOutlineOver(button_large_over_png);
	GuiImageData iconFile(icon_settings_file_png);
	GuiImageData iconMenu(icon_settings_menu_png);
	GuiImageData iconEmulation(icon_game_settings_png);

	GuiText savingBtnTxt1("Saving", 22, (GXColor){0, 0, 0, 255});
	GuiText savingBtnTxt2("&", 18, (GXColor){0, 0, 0, 255});
	GuiText savingBtnTxt3("Loading", 22, (GXColor){0, 0, 0, 255});
	savingBtnTxt1.SetPosition(0, -20);
	savingBtnTxt3.SetPosition(0, +20);
	GuiImage savingBtnImg(&btnLargeOutline);
	GuiImage savingBtnImgOver(&btnLargeOutlineOver);
	GuiImage fileBtnIcon(&iconFile);
	GuiButton savingBtn(btnLargeOutline.GetWidth(), btnLargeOutline.GetHeight());
	savingBtn.SetAlignment(ALIGN_CENTRE, ALIGN_TOP);
	savingBtn.SetPosition(-125, 120);
	savingBtn.SetLabel(&savingBtnTxt1, 0);
	savingBtn.SetLabel(&savingBtnTxt2, 1);
	savingBtn.SetLabel(&savingBtnTxt3, 2);
	savingBtn.SetImage(&savingBtnImg);
	savingBtn.SetImageOver(&savingBtnImgOver);
	savingBtn.SetIcon(&fileBtnIcon);
	savingBtn.SetSoundOver(&btnSoundOver);
	savingBtn.SetSoundClick(&btnSoundClick);
	savingBtn.SetTrigger(trigA);
	savingBtn.SetEffectGrow();

	GuiText menuBtnTxt("Menu", 22, (GXColor){0, 0, 0, 255});
	menuBtnTxt.SetWrap(true, btnLargeOutline.GetWidth()-30);
	GuiImage menuBtnImg(&btnLargeOutline);
	GuiImage menuBtnImgOver(&btnLargeOutlineOver);
	GuiImage menuBtnIcon(&iconMenu);
	GuiButton menuBtn(btnLargeOutline.GetWidth(), btnLargeOutline.GetHeight());
	menuBtn.SetAlignment(ALIGN_CENTRE, ALIGN_TOP);
	menuBtn.SetPosition(125, 120);
	menuBtn.SetLabel(&menuBtnTxt);
	menuBtn.SetImage(&menuBtnImg);
	menuBtn.SetImageOver(&menuBtnImgOver);
	menuBtn.SetIcon(&menuBtnIcon);
	menuBtn.SetSoundOver(&btnSoundOver);
	menuBtn.SetSoundClick(&btnSoundClick);
	menuBtn.SetTrigger(trigA);
	menuBtn.SetEffectGrow();

	GuiText emulationBtnTxt("Emulation", 22, (GXColor){0, 0, 0, 255});
	GuiImage emulationBtnImg(&btnLargeOutline);
	GuiImage emulationBtnImgOver(&btnLargeOutlineOver);
	GuiImage emulationBtnIcon(&iconEmulation);
	GuiButton emulationBtn(btnLargeOutline.GetWidth(), btnLargeOutline.GetHeight());
	emulationBtn.SetAlignment(ALIGN_CENTRE, ALIGN_TOP);
	emulationBtn.SetPosition(-125, 250);
	emulationBtn.SetLabel(&emulationBtnTxt);
	emulationBtn.SetImage(&emulationBtnImg);
	emulationBtn.SetImageOver(&emulationBtnImgOver);
	emulationBtn.SetIcon(&emulationBtnIcon);
	emulationBtn.SetSoundOver(&btnSoundOver);
	emulationBtn.SetSoundClick(&btnSoundClick);
	emulationBtn.SetTrigger(trigA);
	emulationBtn.SetEffectGrow();

	// Cheats used to be a row buried inside Emulation settings - moved out
	// to its own top-level button here so it's reachable in one press. No
	// icon asset exists for this yet (see the pause-menu cheatsBtn comment
	// above MenuGame()'s own cheats button), so this is a plain text
	// button like the others without one.
	GuiText cheatsBtnTxt("Cheats", 22, (GXColor){0, 0, 0, 255});
	GuiImage cheatsBtnImg(&btnLargeOutline);
	GuiImage cheatsBtnImgOver(&btnLargeOutlineOver);
	GuiButton cheatsBtn(btnLargeOutline.GetWidth(), btnLargeOutline.GetHeight());
	cheatsBtn.SetAlignment(ALIGN_CENTRE, ALIGN_TOP);
	cheatsBtn.SetPosition(125, 250);
	cheatsBtn.SetLabel(&cheatsBtnTxt);
	cheatsBtn.SetImage(&cheatsBtnImg);
	cheatsBtn.SetImageOver(&cheatsBtnImgOver);
	cheatsBtn.SetSoundOver(&btnSoundOver);
	cheatsBtn.SetSoundClick(&btnSoundClick);
	cheatsBtn.SetTrigger(trigA);
	cheatsBtn.SetEffectGrow();

	GuiText backBtnTxt("Go Back", 22, (GXColor){0, 0, 0, 255});
	GuiImage backBtnImg(&btnOutline);
	GuiImage backBtnImgOver(&btnOutlineOver);
	GuiButton backBtn(btnOutline.GetWidth(), btnOutline.GetHeight());
	backBtn.SetAlignment(ALIGN_LEFT, ALIGN_BOTTOM);
	backBtn.SetPosition(90, -35);
	backBtn.SetLabel(&backBtnTxt);
	backBtn.SetImage(&backBtnImg);
	backBtn.SetImageOver(&backBtnImgOver);
	backBtn.SetSoundOver(&btnSoundOver);
	backBtn.SetSoundClick(&btnSoundClick);
	backBtn.SetTrigger(trigA);
	backBtn.SetTrigger(trigB);
	backBtn.SetEffectGrow();

	GuiText resetBtnTxt("Reset Settings", 22, (GXColor){0, 0, 0, 255});
	GuiImage resetBtnImg(&btnOutline);
	GuiImage resetBtnImgOver(&btnOutlineOver);
	GuiButton resetBtn(btnOutline.GetWidth(), btnOutline.GetHeight());
	resetBtn.SetAlignment(ALIGN_RIGHT, ALIGN_BOTTOM);
	resetBtn.SetPosition(-90, -35);
	resetBtn.SetLabel(&resetBtnTxt);
	resetBtn.SetImage(&resetBtnImg);
	resetBtn.SetImageOver(&resetBtnImgOver);
	resetBtn.SetSoundOver(&btnSoundOver);
	resetBtn.SetSoundClick(&btnSoundClick);
	resetBtn.SetTrigger(trigA);
	resetBtn.SetEffectGrow();

	HaltGui();
	GuiWindow w(screenwidth, screenheight);
	w.Append(&titleTxt);
	w.Append(&savingBtn);
	w.Append(&menuBtn);
	w.Append(&emulationBtn);
	w.Append(&cheatsBtn);
	w.Append(&backBtn);
	w.Append(&resetBtn);

	mainWindow->Append(&w);

	ResumeGui();

	while(menu == MENU_NONE)
	{
		usleep(THREAD_SLEEP);

		if(savingBtn.GetState() == STATE_CLICKED)
		{
			menu = MENU_SETTINGS_FILE;
		}
		else if(menuBtn.GetState() == STATE_CLICKED)
		{
			menu = MENU_SETTINGS_MENU;
		}
		else if(emulationBtn.GetState() == STATE_CLICKED)
		{
			menu = MENU_SETTINGS_EMULATION;
		}
		else if(cheatsBtn.GetState() == STATE_CLICKED)
		{
			// See MenuGame()'s identical cheatsBtn handling for why this
			// reset is required - without it, Go Back instantly re-opens
			// the cheats screen instead of returning here.
			cheatsBtn.ResetState();

			// MenuGameCheats() is a self-contained blocking modal, not part
			// of the MENU_* state machine, so it doesn't set `menu` - this
			// screen just re-shows once the cheats modal closes. It draws
			// its own full-screen title/buttons, so this screen's `w` has
			// to come off mainWindow (and lose focus) first, the same way
			// the pause menu's cheats button does - otherwise this
			// screen's own buttons stay on screen underneath the cheats
			// screen's, overlapping them, and keep input focus so B can't
			// back out of the cheats screen.
			HaltGui();
			mainWindow->Remove(&w);
			ResumeGui();

			MenuGameCheats();

			HaltGui();
			mainWindow->Append(&w);
			mainWindow->ChangeFocus(&w);
			ResumeGui();
		}
		else if(backBtn.GetState() == STATE_CLICKED)
		{
			menu = MENU_GAMESELECTION;

			// Persist any settings changes made while in the Settings area
			// (e.g. Load/Save Device, folder paths) now that the user is
			// leaving it. Without this, changes such as switching Load/Save
			// Device to USB only ever live in memory for the current
			// session - the next boot's LoadPrefs() re-reads the old
			// settings.xml from disk (still SD, or whatever was last
			// actually saved), which looks like the USB choice "keeps
			// getting reset to SD".
			CreateMissingDirectories();
			SavePrefs(SILENT);
		}
		else if(resetBtn.GetState() == STATE_CLICKED)
		{
			resetBtn.ResetState();

			int choice = WindowPrompt(
				"Reset Settings",
				"Are you sure that you want to reset your settings?",
				"Yes",
				"No");

			if(choice == 1) {
				DefaultSettings();
				autoSaveMethod(SILENT);
				autoLoadMethod(SILENT);
			}
		}
	}

	HaltGui();
	mainWindow->Remove(&w);
	return menu;
}

/****************************************************************************
 * MenuSettingsFile
 ***************************************************************************/

static int MenuSettingsFile()
{
	int menu = MENU_NONE;
	int ret;
	int i = 0;
	bool firstRun = true;
	OptionList options;
	sprintf(options.name[i++], "Load Device");
	sprintf(options.name[i++], "Save Device");
	sprintf(options.name[i++], "Save Folder");
	sprintf(options.name[i++], "State Folder");
	sprintf(options.name[i++], "GB Folder");
	sprintf(options.name[i++], "GBC Folder");
	sprintf(options.name[i++], "GBA Folder");
	sprintf(options.name[i++], "Screenshots Folder");
	sprintf(options.name[i++], "Covers Folder");
	sprintf(options.name[i++], "Artwork Folder");
	sprintf(options.name[i++], "GBA Border Folder");
	sprintf(options.name[i++], "GBC Border Folder");
	sprintf(options.name[i++], "Cheats Folder");
	sprintf(options.name[i++], "Auto Load");
	sprintf(options.name[i++], "Auto Save");
	// Applies to both the .sav (SRAM) AND .sgm (save state) auto-save slot,
	// not just .sav files - see the switch case below and its comment.
	// Shortened from "Append 'Auto' to Auto-Save Filenames" (36 chars) -
	// every other label in this list tops out at 18 chars ("Screenshots
	// Folder"), and the long version overlapped the On/Off value column.
	sprintf(options.name[i++], "Auto-Save Suffix");
	options.length = i;

	for(i=0; i < options.length; i++)
		options.value[i][0] = 0;

	GuiText titleTxt("Settings - Saving & Loading", 26, (GXColor){255, 255, 255, 255});
	titleTxt.SetAlignment(ALIGN_LEFT, ALIGN_TOP);
	titleTxt.SetPosition(50,50);

	GuiSound btnSoundOver(button_over_pcm, button_over_pcm_size, SOUND_PCM);
	GuiSound btnSoundClick(button_click_pcm, button_click_pcm_size, SOUND_PCM);
	GuiImageData btnOutline(button_long_png);
	GuiImageData btnOutlineOver(button_long_over_png);

	GuiText backBtnTxt("Go Back", 22, (GXColor){0, 0, 0, 255});
	GuiImage backBtnImg(&btnOutline);
	GuiImage backBtnImgOver(&btnOutlineOver);
	GuiButton backBtn(btnOutline.GetWidth(), btnOutline.GetHeight());
	backBtn.SetAlignment(ALIGN_LEFT, ALIGN_BOTTOM);
	backBtn.SetPosition(90, -35);
	backBtn.SetLabel(&backBtnTxt);
	backBtn.SetImage(&backBtnImg);
	backBtn.SetImageOver(&backBtnImgOver);
	backBtn.SetSoundOver(&btnSoundOver);
	backBtn.SetSoundClick(&btnSoundClick);
	backBtn.SetTrigger(trigA);
	backBtn.SetTrigger(trigB);
	backBtn.SetEffectGrow();

	GuiOptionBrowser optionBrowser(552, 248, &options);
	optionBrowser.SetPosition(0, 108);
	optionBrowser.SetAlignment(ALIGN_CENTRE, ALIGN_TOP);
	optionBrowser.SetCol2Position(215);

	HaltGui();
	GuiWindow w(screenwidth, screenheight);
	w.Append(&backBtn);
	mainWindow->Append(&optionBrowser);
	mainWindow->Append(&w);
	mainWindow->Append(&titleTxt);
	ResumeGui();

	while(menu == MENU_NONE)
	{
		usleep(THREAD_SLEEP);

		ret = optionBrowser.GetClickedOption();

		switch (ret)
		{
			case 0:
				GCSettings.LoadMethod++;
				break;

			case 1:
				GCSettings.SaveMethod++;
				break;

			case 2:
				OnScreenKeyboard(GCSettings.SaveFolder, MAXPATHLEN);
				break;

			case 3:
				OnScreenKeyboard(GCSettings.StateFolder, MAXPATHLEN);
				break;

			case 4:
				OnScreenKeyboard(GCSettings.GBFolder, MAXPATHLEN);
				break;

			case 5:
				OnScreenKeyboard(GCSettings.GBCFolder, MAXPATHLEN);
				break;

			case 6:
				OnScreenKeyboard(GCSettings.GBAFolder, MAXPATHLEN);
				break;

			case 7:
				OnScreenKeyboard(GCSettings.ScreenshotsFolder, MAXPATHLEN);
				break;

			case 8:
				OnScreenKeyboard(GCSettings.CoverFolder, MAXPATHLEN);
				break;

			case 9:
				OnScreenKeyboard(GCSettings.ArtworkFolder, MAXPATHLEN);
				break;
			
			case 10:
				OnScreenKeyboard(GCSettings.GBABorderFolder, MAXPATHLEN);
				break;

			case 11:
				OnScreenKeyboard(GCSettings.GBCBorderFolder, MAXPATHLEN);
				break;

			case 12:
				OnScreenKeyboard(GCSettings.CheatFolder, MAXPATHLEN);
				break;

			case 13:
				GCSettings.AutoLoad++;
				if (GCSettings.AutoLoad > 2)
					GCSettings.AutoLoad = 0;
				break;

			case 14:
				GCSettings.AutoSave++;
				if (GCSettings.AutoSave > 3)
					GCSettings.AutoSave = 0;
				break;

			case 15:
				GCSettings.AppendAuto++;
				if (GCSettings.AppendAuto > 1)
					GCSettings.AppendAuto = 0;
				break;
		}

		if(ret >= 0 || firstRun)
		{
			firstRun = false;

			// some load/save methods are not implemented - here's where we skip them
			// they need to be skipped in the order they were enumerated

			// no SD/USB ports on GameCube
			#ifdef HW_DOL
			if(GCSettings.LoadMethod == DEVICE_SD)
				GCSettings.LoadMethod++;
			if(GCSettings.SaveMethod == DEVICE_SD)
				GCSettings.SaveMethod++;
			if(GCSettings.LoadMethod == DEVICE_USB)
				GCSettings.LoadMethod++;
			if(GCSettings.SaveMethod == DEVICE_USB)
				GCSettings.SaveMethod++;
			#endif

			// saving to DVD is impossible
			if(GCSettings.SaveMethod == DEVICE_DVD)
				GCSettings.SaveMethod++;

			// Network (SMB) is disabled for now - not yet tested/supported
			if(GCSettings.LoadMethod == DEVICE_SMB)
				GCSettings.LoadMethod++;
			if(GCSettings.SaveMethod == DEVICE_SMB)
				GCSettings.SaveMethod++;

			// skip GameCube devices on Wii
			#ifdef HW_RVL
			if(GCSettings.LoadMethod == DEVICE_SD_SLOTA)
				GCSettings.LoadMethod++;
			if(GCSettings.SaveMethod == DEVICE_SD_SLOTA)
				GCSettings.SaveMethod++;
			if(GCSettings.LoadMethod == DEVICE_SD_SLOTB)
				GCSettings.LoadMethod++;
			if(GCSettings.SaveMethod == DEVICE_SD_SLOTB)
				GCSettings.SaveMethod++;
			if(GCSettings.LoadMethod == DEVICE_SD_PORT2)
				GCSettings.LoadMethod++;
			if(GCSettings.SaveMethod == DEVICE_SD_PORT2)
				GCSettings.SaveMethod++;
			if(GCSettings.LoadMethod == DEVICE_SD_GCLOADER)
				GCSettings.LoadMethod++;
			if(GCSettings.SaveMethod == DEVICE_SD_GCLOADER)
				GCSettings.SaveMethod++;
			#endif

			// correct load/save methods out of bounds
			if(GCSettings.LoadMethod >= DEVICE_LENGTH)
				GCSettings.LoadMethod = DEVICE_AUTO;
			if(GCSettings.SaveMethod >= DEVICE_LENGTH)
				GCSettings.SaveMethod = DEVICE_AUTO;

			if (GCSettings.LoadMethod == DEVICE_AUTO) sprintf (options.value[0],"Auto Detect");
			else if (GCSettings.LoadMethod == DEVICE_SD) sprintf (options.value[0],"SD");
			else if (GCSettings.LoadMethod == DEVICE_USB) sprintf (options.value[0],"USB");
			else if (GCSettings.LoadMethod == DEVICE_DVD) sprintf (options.value[0],"DVD");
			else if (GCSettings.LoadMethod == DEVICE_SD_SLOTA) sprintf (options.value[0],"SD Gecko Slot A");
			else if (GCSettings.LoadMethod == DEVICE_SD_SLOTB) sprintf (options.value[0],"SD Gecko Slot B");
			else if (GCSettings.LoadMethod == DEVICE_SD_PORT2) sprintf (options.value[0],"SD in SP2");
			else if (GCSettings.LoadMethod == DEVICE_SD_GCLOADER) sprintf (options.value[0],"GC Loader");

			if (GCSettings.SaveMethod == DEVICE_AUTO) sprintf (options.value[1],"Auto Detect");
			else if (GCSettings.SaveMethod == DEVICE_SD) sprintf (options.value[1],"SD");
			else if (GCSettings.SaveMethod == DEVICE_USB) sprintf (options.value[1],"USB");
			else if (GCSettings.SaveMethod == DEVICE_SD_SLOTA) sprintf (options.value[1],"SD Gecko Slot A");
			else if (GCSettings.SaveMethod == DEVICE_SD_SLOTB) sprintf (options.value[1],"SD Gecko Slot B");
			else if (GCSettings.SaveMethod == DEVICE_SD_PORT2) sprintf (options.value[1],"SD in SP2");
			else if (GCSettings.SaveMethod == DEVICE_SD_GCLOADER) sprintf (options.value[1],"GC Loader");

			snprintf (options.value[2], 35, "%s", GCSettings.SaveFolder);
			snprintf (options.value[3], 35, "%s", GCSettings.StateFolder);
			snprintf (options.value[4], 35, "%s", GCSettings.GBFolder);
			snprintf (options.value[5], 35, "%s", GCSettings.GBCFolder);
			snprintf (options.value[6], 35, "%s", GCSettings.GBAFolder);
			snprintf (options.value[7], 35, "%s", GCSettings.ScreenshotsFolder);
			snprintf (options.value[8], 35, "%s", GCSettings.CoverFolder);
			snprintf (options.value[9], 35, "%s", GCSettings.ArtworkFolder);
			snprintf (options.value[10], 35, "%s", GCSettings.GBABorderFolder);
			snprintf (options.value[11], 35, "%s", GCSettings.GBCBorderFolder);
			snprintf (options.value[12], 35, "%s", GCSettings.CheatFolder);

			if (GCSettings.AutoLoad == 0) sprintf (options.value[13],"Off");
			else if (GCSettings.AutoLoad == 1) sprintf (options.value[13],"SRAM");
			else if (GCSettings.AutoLoad == 2) sprintf (options.value[13],"State");

			if (GCSettings.AutoSave == 0) sprintf (options.value[14],"Off");
			else if (GCSettings.AutoSave == 1) sprintf (options.value[14],"SRAM");
			else if (GCSettings.AutoSave == 2) sprintf (options.value[14],"State");
			else if (GCSettings.AutoSave == 3) sprintf (options.value[14],"Both");

			if (GCSettings.AppendAuto == 0) sprintf (options.value[15],"Off");
			else if (GCSettings.AppendAuto == 1) sprintf (options.value[15],"On");

			optionBrowser.TriggerUpdate();
		}

		if(backBtn.GetState() == STATE_CLICKED)
		{
			menu = MENU_SETTINGS;
			autoSaveMethod(SILENT);
			autoLoadMethod(SILENT);
		}
	}
	HaltGui();
	mainWindow->Remove(&optionBrowser);
	mainWindow->Remove(&w);
	mainWindow->Remove(&titleTxt);
	// Persist changes made on this screen (Load/Save Device, folders,
	// Auto Load/Save, etc.) as soon as the user leaves it, rather than
	// only when they later also back out of the top-level Settings hub.
	SavePrefs(SILENT);
	return menu;
}

/****************************************************************************
 * MenuSettingsMenu
 ***************************************************************************/
static int MenuSettingsMenu()
{
	int menu = MENU_NONE;
	int ret;
	int i = 0;
	bool firstRun = true;
	OptionList options;
	currentLanguage = GCSettings.language;

	sprintf(options.name[i++], "Exit Action");
	sprintf(options.name[i++], "Wiimote Orientation");
	sprintf(options.name[i++], "Music Volume");
	sprintf(options.name[i++], "Sound Effects Volume");
	sprintf(options.name[i++], "Rumble");
	sprintf(options.name[i++], "Language");
	sprintf(options.name[i++], "Preview Image");
	sprintf(options.name[i++], "Classic (Non-Tabbed) Game List");
	options.length = i;

	for(i=0; i < options.length; i++)
		options.value[i][0] = 0;

	GuiText titleTxt("Settings - Menu", 26, (GXColor){255, 255, 255, 255});
	titleTxt.SetAlignment(ALIGN_LEFT, ALIGN_TOP);
	titleTxt.SetPosition(50,50);

	GuiSound btnSoundOver(button_over_pcm, button_over_pcm_size, SOUND_PCM);
	GuiSound btnSoundClick(button_click_pcm, button_click_pcm_size, SOUND_PCM);
	GuiImageData btnOutline(button_long_png);
	GuiImageData btnOutlineOver(button_long_over_png);

	GuiText backBtnTxt("Go Back", 22, (GXColor){0, 0, 0, 255});
	GuiImage backBtnImg(&btnOutline);
	GuiImage backBtnImgOver(&btnOutlineOver);
	GuiButton backBtn(btnOutline.GetWidth(), btnOutline.GetHeight());
	backBtn.SetAlignment(ALIGN_LEFT, ALIGN_BOTTOM);
	backBtn.SetPosition(90, -35);
	backBtn.SetLabel(&backBtnTxt);
	backBtn.SetImage(&backBtnImg);
	backBtn.SetImageOver(&backBtnImgOver);
	backBtn.SetSoundOver(&btnSoundOver);
	backBtn.SetSoundClick(&btnSoundClick);
	backBtn.SetTrigger(trigA);
	backBtn.SetTrigger(trigB);
	backBtn.SetEffectGrow();

	GuiOptionBrowser optionBrowser(552, 248, &options);
	optionBrowser.SetPosition(0, 108);
	optionBrowser.SetAlignment(ALIGN_CENTRE, ALIGN_TOP);
	optionBrowser.SetCol2Position(275);

	HaltGui();
	GuiWindow w(screenwidth, screenheight);
	w.Append(&backBtn);
	mainWindow->Append(&optionBrowser);
	mainWindow->Append(&w);
	mainWindow->Append(&titleTxt);
	ResumeGui();

	while(menu == MENU_NONE)
	{
		usleep(THREAD_SLEEP);

		ret = optionBrowser.GetClickedOption();

		switch (ret)
		{
			case 0:
				GCSettings.ExitAction++;
				#ifdef HW_RVL
				if(GCSettings.ExitAction >= EXITACTION_WII_LENGTH)
					GCSettings.ExitAction = EXITACTION_WII_AUTO;
				#else
				if(GCSettings.ExitAction >= EXITACTION_GC_LENGTH)
					GCSettings.ExitAction = EXITACTION_GC_RETURN_TO_LOADER;
				#endif
				break;
			case 1:
				GCSettings.WiimoteOrientation ^= 1;
				break;
			case 2:
				GCSettings.MusicVolume += 10;
				if(GCSettings.MusicVolume > 100)
					GCSettings.MusicVolume = 0;
				bgMusic->SetVolume(GCSettings.MusicVolume);
				break;
			case 3:
				GCSettings.SFXVolume += 10;
				if(GCSettings.SFXVolume > 100)
					GCSettings.SFXVolume = 0;
				break;
			case 4:
				GCSettings.Rumble ^= 1;
				break;
			case 5:
				GCSettings.language++;
				
				if(GCSettings.language == LANG_TRAD_CHINESE) // skip (not supported)
					GCSettings.language = LANG_KOREAN;
				else if(GCSettings.language >= LANG_LENGTH)
					GCSettings.language = LANG_JAPANESE;
				break;			
			case 6:
				GCSettings.PreviewImage++;
				if(GCSettings.PreviewImage >= PREVIEWIMAGE_LENGTH)
					GCSettings.PreviewImage = PREVIEWIMAGE_SCREENSHOT;
				break;
			case 7:
				GCSettings.ClassicBrowser ^= 1;
				break;
		}

		if(ret >= 0 || firstRun)
		{
			firstRun = false;

			#ifdef HW_RVL
			if (GCSettings.ExitAction == EXITACTION_WII_RETURN_TO_MENU)
				sprintf (options.value[0], "Return to Wii Menu");
			else if (GCSettings.ExitAction == EXITACTION_WII_POWER_OFF)
				sprintf (options.value[0], "Power off Wii");
			else if (GCSettings.ExitAction == EXITACTION_WII_RETURN_TO_LOADER)
				sprintf (options.value[0], "Return to Loader");
			else
				sprintf (options.value[0], "Auto");
			#else // GameCube
			if (GCSettings.ExitAction == EXITACTION_GC_RETURN_TO_LOADER)
				sprintf (options.value[0], "Return to Loader");
			else
				sprintf (options.value[0], "Reboot");

			options.name[1][0] = 0; // Wiimote
			options.name[2][0] = 0; // Music
			options.name[3][0] = 0; // Sound Effects
			options.name[4][0] = 0; // Rumble
			#endif

			if (GCSettings.WiimoteOrientation == WIIMOTEORIENTATION_VERTICAL)
				sprintf (options.value[1], "Vertical");
			else if (GCSettings.WiimoteOrientation == WIIMOTEORIENTATION_HORIZONTAL)
				sprintf (options.value[1], "Horizontal");

			if(GCSettings.MusicVolume > 0)
				sprintf(options.value[2], "%d%%", GCSettings.MusicVolume);
			else
				sprintf(options.value[2], "Mute");

			if(GCSettings.SFXVolume > 0)
				sprintf(options.value[3], "%d%%", GCSettings.SFXVolume);
			else
				sprintf(options.value[3], "Mute");

			if (GCSettings.Rumble == 1)
				sprintf (options.value[4], "Enabled");
			else
				sprintf (options.value[4], "Disabled");

			switch(GCSettings.language)
			{
				case LANG_JAPANESE:		sprintf(options.value[5], "Japanese"); break;
				case LANG_ENGLISH:		sprintf(options.value[5], "English"); break;
				case LANG_GERMAN:		sprintf(options.value[5], "German"); break;
				case LANG_FRENCH:		sprintf(options.value[5], "French"); break;
				case LANG_SPANISH:		sprintf(options.value[5], "Spanish"); break;
				case LANG_ITALIAN:		sprintf(options.value[5], "Italian"); break;
				case LANG_DUTCH:		sprintf(options.value[5], "Dutch"); break;
				case LANG_SIMP_CHINESE:	sprintf(options.value[5], "Chinese (Simplified)"); break;
				case LANG_TRAD_CHINESE:	sprintf(options.value[5], "Chinese (Traditional)"); break;
				case LANG_KOREAN:		sprintf(options.value[5], "Korean"); break;
				case LANG_PORTUGUESE:	sprintf(options.value[5], "Portuguese"); break;
				case LANG_BRAZILIAN_PORTUGUESE: sprintf(options.value[5], "Brazilian Portuguese"); break;
				case LANG_CATALAN:		sprintf(options.value[5], "Catalan"); break;
				case LANG_TURKISH:		sprintf(options.value[5], "Turkish"); break;
				case LANG_SWEDISH:		sprintf(options.value[5], "Swedish"); break;
			}
	
			switch(GCSettings.PreviewImage)
			{
				case 0:	
					sprintf(options.value[6], "Screenshots"); 
					break; 
				case 1:	
					sprintf(options.value[6], "Covers");	  
					break; 
				case 2:	
					sprintf(options.value[6], "Artwork");
					break; 
			}

			sprintf(options.value[7], "%s", GCSettings.ClassicBrowser ? "On" : "Off");
			optionBrowser.TriggerUpdate();
		}

		if(backBtn.GetState() == STATE_CLICKED)
		{
			menu = MENU_SETTINGS;
			// Persist changes made here (Preview Image, Wiimote
			// Orientation, Exit Action, volumes, Rumble, language, etc.)
			// immediately on leaving this screen.
			SavePrefs(SILENT);
		}
	}
	ChangeLanguage();
	HaltGui();
	mainWindow->Remove(&optionBrowser);
	mainWindow->Remove(&w);
	mainWindow->Remove(&titleTxt);
	return menu;
}

/****************************************************************************
 * MainMenu
 ***************************************************************************/
void
MainMenu (int menu)
{
	static bool firstRun = true;
	int currentMenu = menu;
	lastMenu = MENU_NONE;
	
	if(firstRun)
	{
		#ifdef HW_RVL
		pointer[0] = new GuiImageData(player1_point_png);
		pointer[1] = new GuiImageData(player2_point_png);
		pointer[2] = new GuiImageData(player3_point_png);
		pointer[3] = new GuiImageData(player4_point_png);
		#endif

		trigA = new GuiTrigger;
		trigA->SetSimpleTrigger(-1, WPAD_BUTTON_A | WPAD_CLASSIC_BUTTON_A, PAD_BUTTON_A, WIIDRC_BUTTON_A);
		trig2 = new GuiTrigger;
		trig2->SetSimpleTrigger(-1, WPAD_BUTTON_2, 0, 0);

		// trigB is deliberately separate from trig2: trig2 is a focus-
		// requiring SimpleTrigger covering only the Wiimote's "2" button
		// (Classic Controller/GameCube pad/Wii U Gamepad users had no
		// equivalent at all), which is why B previously just acted as a
		// second A on whatever was focused rather than a dedicated
		// "go back" - same class of behavior Snes9x-RX's changelog
		// describes fixing ("Pressing B will now Go Back from all
		// menus"). SetButtonOnlyTrigger (same mechanism already used for
		// trigHome above) fires regardless of which button currently has
		// cursor focus, and covers B on every controller type, so it can
		// be attached only to each screen's back/cancel/close button and
		// still work no matter what's highlighted.
		trigB = new GuiTrigger;
		trigB->SetButtonOnlyTrigger(-1, WPAD_BUTTON_B | WPAD_CLASSIC_BUTTON_B, PAD_BUTTON_B, WIIDRC_BUTTON_B);
	}

	mainWindow = new GuiWindow(screenwidth, screenheight);

	if(menu == MENU_GAME)
	{
		gameScreenTexture = DecodePNGToRGB565(gameScreenPng.pngData, gameScreenPng.pngSize, &gameScreenTexW, &gameScreenTexH);

		if (gameScreenTexture)
		{
			// gameScreenPng.viewX/Y/W/H are fractions (0.0-1.0) of the GAME's
			// own real video mode dimensions at capture time - convert to
			// real pixels on THIS (the menu's) canvas first. This is the
			// actual fix for the pause-background squish bug: the game can
			// run in a different real video mode than the menu (which
			// always forces auto/preferred regardless of GCSettings.
			// videomode), so reusing the game's captured pixels directly
			// against the menu's own differently-sized canvas was wrong -
			// see video.h's GameScreenPng comment.
			float viewPxX = gameScreenPng.viewX * screenwidth;
			float viewPxY = gameScreenPng.viewY * screenheight;
			float viewPxW = gameScreenPng.viewW * screenwidth;
			float viewPxH = gameScreenPng.viewH * screenheight;

			// Scale from the PNG's real content size (nativeW/nativeH,
			// border-inclusive when a border is present) to the size the
			// game was actually being shown at on screen when captured
			// (now in real menu-canvas pixels, viewPxW/viewPxH).
			//
			// This MUST be nativeW/nativeH, not a bare-game-only reference
			// - verified against video.cpp's UpdateScaling(): the live
			// quad's own on-screen size (which viewPxW/viewPxH derive
			// from, via liveVW/liveVH) is itself computed from
			// GameboyAspectRatio = vwidth/vheight, and vwidth/vheight are
			// the same real per-frame, border-inclusive dimensions as
			// nativeW/nativeH. So the live quad is ALREADY sized to fit
			// the whole bordered picture whenever a border is present -
			// there's no separate "game-only" quad to account for. An
			// earlier version of this code divided by the bare game
			// native size instead (reasoning that the border would
			// otherwise get shrunk to fit inside the game's own area) -
			// that was wrong for the bordered case: it used a
			// too-small denominator, inflating the scale and making
			// SGB-bordered games specifically appear zoomed in on the
			// pause screen (non-bordered games were unaffected, since for
			// them nativeW/H and the bare game size are the same number).
			// gameScreenTexW/gameScreenTexH (the decoded texture's padded
			// dimensions) are NOT used for the scale calculation, only
			// passed to Menu_DrawImg565 itself, since PNGU may round them
			// up past the real image size (see DecodePNGToRGB565's header
			// comment).
			gameScreenScaleX = (gameScreenPng.nativeW > 0) ? (viewPxW / (float)gameScreenPng.nativeW) : 1.0f;
			gameScreenScaleY = (gameScreenPng.nativeH > 0) ? (viewPxH / (float)gameScreenPng.nativeH) : 1.0f;

			// No margin offset needed - viewPxX/viewPxY (from liveVX/
			// liveVY) already mark the full picture's own top-left corner,
			// border included when present, per the same reasoning above.
			gameScreenDrawX = viewPxX;
			gameScreenDrawY = viewPxY;

			printf("[blur] screenwidth=%d screenheight=%d | viewPxX=%.2f viewPxY=%.2f viewPxW=%.2f viewPxH=%.2f | nativeW=%d nativeH=%d | scaleX=%.4f scaleY=%.4f | drawX=%.2f drawY=%.2f drawW=%.2f drawH=%.2f\n",
				screenwidth, screenheight, viewPxX, viewPxY, viewPxW, viewPxH,
				gameScreenPng.nativeW, gameScreenPng.nativeH,
				gameScreenScaleX, gameScreenScaleY, gameScreenDrawX, gameScreenDrawY,
				gameScreenPng.nativeW * gameScreenScaleX, gameScreenPng.nativeH * gameScreenScaleY);

			// Build the full-screen blurred composite once, here - not
			// per-frame. See BuildBlurredPauseScreen()'s own header comment
			// for the full pipeline (unswizzle -> upscale+composite ->
			// box blur -> overlay tint -> re-swizzle).
			BuildBlurredPauseScreen();
			gameScreenIsBlurred = (gameScreenBlurred != NULL);

			if (!gameScreenIsBlurred) {
				// Blur build failed (allocation failure) - fall back to
				// the plain backdrop rather than drawing nothing.
				gameScreenImg = new GuiImage(screenwidth, screenheight, (GXColor){236, 226, 238, 255});
				gameScreenImg->ColorStripe(10);
			}

			// No GuiImage/mainWindow involvement for this case - drawn
			// manually via Menu_DrawImg565() from UpdateGUI() (right
			// before mainWindow->Draw(), matching the old
			// Insert(gameScreenImg, 0) background z-order) and from the
			// credits-screen loop. gameScreenImg stays NULL here; it's
			// only used below for the solid-color fallback case.
			//
			// The old GuiImage path's ColorStripe() darkening is now
			// provided by BuildBlurredPauseScreen()'s overlay-tint blend
			// step instead (baked into gameScreenBlurred itself, not a
			// separate per-frame effect).
		}
		else
		{
			// Decode failed - fall back to a plain backdrop rather than a
			// null-image crash.
			gameScreenIsBlurred = false;
			gameScreenImg = new GuiImage(screenwidth, screenheight, (GXColor){236, 226, 238, 255});
			gameScreenImg->ColorStripe(10);
		}
	}
	else
	{
		gameScreenIsBlurred = false;
		gameScreenImg = new GuiImage(screenwidth, screenheight, (GXColor){236, 226, 238, 255});
		gameScreenImg->ColorStripe(10);
	}

	if (!gameScreenIsBlurred)
		mainWindow->Append(gameScreenImg);

	GuiSound btnSoundOver(button_over_pcm, button_over_pcm_size, SOUND_PCM);
	GuiSound btnSoundClick(button_click_pcm, button_click_pcm_size, SOUND_PCM);
	GuiImageData bgTop(bg_top_png);
	bgTopImg = new GuiImage(&bgTop);
	GuiImageData bgBottom(bg_bottom_png);
	bgBottomImg = new GuiImage(&bgBottom);
	bgBottomImg->SetAlignment(ALIGN_LEFT, ALIGN_BOTTOM);
	GuiImageData logo(logo_png);
	GuiImage logoImg(&logo);
	GuiImageData logoOver(logo_over_png);
	GuiImage logoImgOver(&logoOver);
	GuiText logoTxt(APPVERSION, 18, (GXColor){255, 255, 255, 255});
	logoTxt.SetAlignment(ALIGN_RIGHT, ALIGN_TOP);
	logoTxt.SetPosition(0, 4);
	btnLogo = new GuiButton(logoImg.GetWidth(), logoImg.GetHeight());
	btnLogo->SetAlignment(ALIGN_RIGHT, ALIGN_TOP);
	btnLogo->SetPosition(-50, 24);
	btnLogo->SetImage(&logoImg);
	btnLogo->SetImageOver(&logoImgOver);
	btnLogo->SetLabel(&logoTxt);
	btnLogo->SetSoundOver(&btnSoundOver);
	btnLogo->SetSoundClick(&btnSoundClick);
	btnLogo->SetTrigger(trigA);
	btnLogo->SetTrigger(trig2);
	btnLogo->SetUpdateCallback(WindowCredits);

	mainWindow->Append(bgTopImg);
	mainWindow->Append(bgBottomImg);
	mainWindow->Append(btnLogo);

	if(currentMenu == MENU_GAMESELECTION)
		ResumeGui();

	if(firstRun) {
		// If LoadPrefs() fails to find settings.xml this boot (e.g. a USB
		// drive that hadn't finished mounting yet at this exact point in
		// boot - see chat history for a retry-loop attempt at this that
		// caused a hardware-only hang and was reverted), autoLoadMethod()/
		// autoSaveMethod() below will fall back to auto-detecting a device
		// (which tends to land on SD, since it's checked first and mounts
		// faster than USB). We must NOT then immediately save that
		// fallback back to settings.xml - doing so permanently overwrites
		// the user's real saved device/folder choice just because of a
		// one-off timing miss, which is exactly the "USB setting keeps
		// getting reset to SD" bug. Only persist here if we actually found
		// and loaded the real settings this boot; otherwise just proceed
		// with in-memory defaults for this session and let the normal
		// save-on-exit/save-on-change paths persist things once the app
		// is in a state we're confident about.
		bool prefsLoaded = LoadPrefs();
		autoSaveMethod(SILENT);
		autoLoadMethod(SILENT);

		CreateMissingDirectories();
		if (prefsLoaded)
			SavePrefs(SILENT);
	}

#ifdef HW_RVL
	if(firstRun)
	{
		u32 ios = IOS_GetVersion();

		if(!SupportedIOS(ios))
			ErrorPrompt("The current IOS is unsupported. Functionality and/or stability may be adversely affected.");
		else if(!SaneIOS(ios))
			ErrorPrompt("The current IOS has been altered (fake-signed). Functionality and/or stability may be adversely affected.");
	}
#endif

	#ifndef NO_SOUND
	if(firstRun) {
		bgMusic = new GuiSound(bg_music, bg_music_size, SOUND_OGG);
		bgMusic->SetVolume(GCSettings.MusicVolume);
		bgMusic->SetLoop(true);
		enterSound = new GuiSound(enter_ogg, enter_ogg_size, SOUND_OGG);
		enterSound->SetVolume(GCSettings.SFXVolume);
		exitSound = new GuiSound(exit_ogg, exit_ogg_size, SOUND_OGG);
		exitSound->SetVolume(GCSettings.SFXVolume);
	}

	if(currentMenu == MENU_GAMESELECTION)
		bgMusic->Play(); // startup music
	#endif

	firstRun = false;

	while(currentMenu != MENU_EXIT || !ROMLoaded)
	{
		switch (currentMenu)
		{
			case MENU_GAMESELECTION:
				currentMenu = MenuGameSelection();
				break;
			case MENU_GAME:
				currentMenu = MenuGame();
				break;
			case MENU_GAME_LOAD:
				currentMenu = MenuGameSaves(0);
				break;
			case MENU_GAME_SAVE:
				currentMenu = MenuGameSaves(1);
				break;
			case MENU_GAME_DELETE:
				currentMenu = MenuGameSaves(2);
				break;	
			case MENU_GAMESETTINGS:
				currentMenu = MenuGameSettings();
				break;
			case MENU_GAMESETTINGS_MAPPINGS:
				currentMenu = MenuSettingsMappings();
				break;
			case MENU_GAMESETTINGS_MAPPINGS_MAP:
				currentMenu = MenuSettingsMappingsMap();
				break;
			case MENU_GAMESETTINGS_VIDEO:
				currentMenu = MenuSettingsVideo();
				break;
			case MENU_SETTINGS:
				currentMenu = MenuSettings();
				break;
			case MENU_SETTINGS_FILE:
				currentMenu = MenuSettingsFile();
				break;
			case MENU_SETTINGS_MENU:
				currentMenu = MenuSettingsMenu();
				break;
			case MENU_SETTINGS_EMULATION:
				currentMenu = MenuSettingsEmulation();
				break;
			default: // unrecognized menu
				currentMenu = MenuGameSelection();
				break;
		}
		lastMenu = currentMenu;
		usleep(THREAD_SLEEP);
	}

	#ifdef HW_RVL
	ShutoffRumble();
	#endif

	CancelAction();
	HaltGui();

	delete btnLogo;
	delete gameScreenImg;
	gameScreenImg = NULL;
	delete bgTopImg;
	delete bgBottomImg;
	delete mainWindow;

	mainWindow = NULL;

	if (gameScreenTexture) {
		free(gameScreenTexture);
		gameScreenTexture = NULL;
	}
	if (gameScreenBlurred) {
		free(gameScreenBlurred);
		gameScreenBlurred = NULL;
	}
	gameScreenIsBlurred = false;

	ClearScreenshot();

	// wait for keys to be depressed
	while(MenuRequested())
	{
		UpdatePads();
		usleep(THREAD_SLEEP);
	}
}