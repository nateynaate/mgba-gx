/****************************************************************************
 * mGBA-GX
 *
 * Fork of Visual Boy Advance GX (Tantric, 2008-2023)
 * mGBA-GX modifications 2026
 *
 * vbagx.h
 *
 * This file controls overall program flow. Most things start and end here!
 ***************************************************************************/
#ifndef _VBAGX_H_
#define _VBAGX_H_

#include <unistd.h>
#include <sys/param.h>

#include "filelist.h"
#include "utils/FreeTypeGX.h"

#define APPNAME 		"mGBA-GX"
#define APPVERSION 		"1.0.1"
#define APPFOLDER 		"mgbagx"
#define PREF_FILE_NAME 	"settings.xml"

#define NOTSILENT 0
#define SILENT 1

const char pathPrefix[10][11] =
{ "", "sd:/", "usb:/", "dvd:/", "smb:/", "carda:/", "cardb:/", "port2:/", "gcloader:/" };

enum 
{
	DEVICE_AUTO = 0,
	DEVICE_SD,
	DEVICE_USB,
	DEVICE_DVD,
	DEVICE_SMB,
	DEVICE_SD_SLOTA,
	DEVICE_SD_SLOTB,
	DEVICE_SD_PORT2,
	DEVICE_SD_GCLOADER,
	DEVICE_LENGTH
};

enum {
    SAVEFOLDER_SAVES = 0,
    SAVEFOLDER_LENGTH
};

enum {
    LOADFOLDER_ROMS = 0,
    LOADFOLDER_SCREENSHOTS,
    LOADFOLDER_COVERS,
    LOADFOLDER_ARTWORK,
	LOADFOLDER_BORDERS_GBA,
	LOADFOLDER_BORDERS_GBC,
	LOADFOLDER_GB,
	LOADFOLDER_GBC,
	LOADFOLDER_GBA,
	LOADFOLDER_CHEATS,
    LOADFOLDER_LENGTH
};

#define MAX_RECENT_ROMS 8

typedef struct {
    int id;
    const char *name;
} FolderDef;

const FolderDef saveFolder[] = {
    { SAVEFOLDER_SAVES,  "saves" }
};

const FolderDef loadFolder[] = {
    { LOADFOLDER_ROMS,        "roms" },
    { LOADFOLDER_SCREENSHOTS, "screenshots" },
    { LOADFOLDER_COVERS,      "covers" },
    { LOADFOLDER_ARTWORK,     "artwork" },
	{ LOADFOLDER_BORDERS_GBA, "borders/gba" },
	{ LOADFOLDER_BORDERS_GBC, "borders/gbc" },
	{ LOADFOLDER_GB,          "roms/gb" },
	{ LOADFOLDER_GBC,         "roms/gbc" },
	{ LOADFOLDER_GBA,         "roms/gba" },
	{ LOADFOLDER_CHEATS,      "cheats" }
};

enum 
{
	FILE_SRAM,
	FILE_SNAPSHOT,
	FILE_ROM
};

enum {
	PREVIEWIMAGE_SCREENSHOT = 0,
	PREVIEWIMAGE_COVER,
	PREVIEWIMAGE_ARTWORK,
	PREVIEWIMAGE_LENGTH
};

enum {
	RENDER_FILTERED = 1,
	RENDER_UNFILTERED,
	RENDER_FILTERED_SOFT,
	RENDER_FILTERED_SHARP,
	RENDER_LENGTH
};

// Ported from VBA-GX 3.0.0's video filter system - only the GX-hardware
// scanline effect so far (a small repeating I8 texture multiplied against
// the game texture via a second TEV stage), not the CPU-side HQ2X/Scale2x/
// 2xBR/DDT scaling filters that share this same enum upstream.
enum {
	FILTER_NONE = 0,
	FILTER_SCANLINES,
	FILTER_SCALE2X,
	FILTER_LENGTH
};

enum {
	VIDEOMODE_AUTO = 0,
	VIDEOMODE_NTSC,
	VIDEOMODE_PROGRESSIVE,
	VIDEOMODE_PAL,
	VIDEOMODE_EURGB,
	VIDEOMODE_240P,
	VIDEOMODE_EURGB_240P,
	VIDEOMODE_LENGTH
};

enum {
	WIIMOTEORIENTATION_VERTICAL = 0,
	WIIMOTEORIENTATION_HORIZONTAL,
	WIIMOTEORIENTATION_LENGTH
};

enum {
	SCALING_MAINTAIN_ASPECT = 0,
	SCALING_PARTIAL_STRETCH,
	SCALING_STRETCH_TO_FIT,
	SCALING_WIDESCREEN_CORRECTION,
	SCALING_LENGTH
};

enum {
	EXITACTION_WII_AUTO = 0,
	EXITACTION_WII_RETURN_TO_MENU,
	EXITACTION_WII_POWER_OFF,
	EXITACTION_WII_RETURN_TO_LOADER,
	EXITACTION_WII_LENGTH
};

enum {
	EXITACTION_GC_RETURN_TO_LOADER = 0,
	EXITACTION_GC_REBOOT,
	EXITACTION_GC_LENGTH
};

enum 
{
	LANG_JAPANESE = 0,
	LANG_ENGLISH,
	LANG_GERMAN,
	LANG_FRENCH,
	LANG_SPANISH,
	LANG_ITALIAN,
	LANG_DUTCH,
	LANG_SIMP_CHINESE,
	LANG_TRAD_CHINESE,
	LANG_KOREAN,
	LANG_PORTUGUESE,
	LANG_BRAZILIAN_PORTUGUESE,
	LANG_CATALAN,
	LANG_TURKISH,
	LANG_SWEDISH,
	LANG_LENGTH
};

struct SGCSettings
{
	float	gbaZoomHor;    // GBA horizontal zoom amount
	float	gbaZoomVert;   // GBA vertical zoom amount
	float	gbZoomHor;     // GB horizontal zoom amount
	float	gbZoomVert;    // GB vertical zoom amount
	int		gbFixed;
	int		gbaFixed;
	int		AutoLoad;
	int		AutoSave;
	int		LoadMethod;    // For ROMS: Auto, SD, DVD, USB, Network (SMB)
	int		SaveMethod;    // For SRAM, Freeze, Prefs: Auto, SD, USB, SMB
	int		AppendAuto;    // 0 - no, 1 - yes
	int		videomode;     // 0 - automatic, 1 - NTSC (480i), 2 - Progressive (480p), 3 - PAL (50Hz), 4 - PAL (60Hz)
	int		scaling;       // 0 - default, 1 - partial stretch, 2 - stretch to fit, 3 - widescreen correction
	int		render;		   // 0 - original, 1 - filtered, 2 - unfiltered
	int		FilterMethod;  // 0 - none, 1 - scanlines (see FILTER_* enum)
	int		xshift;		   // video output shift
	int		yshift;
	int		WiimoteOrientation;
	int		ExitAction;
	int		MusicVolume;
	int		SFXVolume;
	int		Rumble;
	int 	language;
	int		PreviewImage;
	int		AutoloadGame;
	
	int		OffsetMinutesUTC; // Used for clock on MBC3 and TAMA5
	int 	GBHardware;    // Mapped to gbEmulatorType in VBA
	int 	SGBBorder;
	// "Color Emulation" (renamed from "Color Correction"): picks which
	// real handheld's screen color response to emulate for this system.
	// Separate fields per system since a game can choose ANY profile
	// regardless of its own native hardware (GBA games can use the GBC/
	// Gambatte profile and vice versa) - the field just says which system
	// the ROM actually is, not which profile is applied to it. Applied as
	// a CPU-side per-pixel matrix pipeline in vbasupport.cpp
	// (BuildColorLUT()/ApplyColorCorrection()). Values (same index order
	// for both fields, kGBAMatrices/kGBMatrices in mgba_emuMain()):
	//   0 - Off
	//   1 - GBA (hunterk/Pokefan531 "Color Mangler", public domain,
	//       libretro/glsl-shaders handheld/shaders/color/gba-color.glsl)
	//   2 - GBC/Gambatte (same lineage, gbc-color.glsl)
	//   3 - GBA SP (AGS-101), sourced from the real sp101-color.slang shader - see kMatrixAGS101
	//   4 - VBA-style
	//   5 - PSP-style
	//   6 - NDS-style
	// 0/1 match the old on/off GBAColorCorrection/GBCColorCorrection
	// values exactly, so existing settings.xml files keep working.
	int		GBAColorEmulation;
	int		GBCColorEmulation;

	// Interframe blending: averages each displayed frame 50/50 with the
	// previous one (mGBA's own "Simple" blend mode), for games that render
	// transparency effects by dithering between two alternating frames
	// instead of true alpha blending (F-Zero: Maximum Velocity's map
	// overlay is the classic example - without this it visibly flickers
	// between the two source frames instead of looking translucent).
	// Global rather than per-game for now, unlike GBHardware/SGBBorder/
	// BasicPalette above. Applied in vbasupport.cpp
	// (ApplyInterframeBlending(), mgba_emuMain()). 0 = Off, 1 = On.
	int		InterframeBlending;

	// Fast-forward multiplier, toggled from a menu option (Game Settings -
	// Emulation) for a PERSISTENT speed. Independently, FastForwardHeld()
	// (input.cpp) also fast-forwards at at least 2x for as long as any
	// controller's default fast-forward input is held - GameCube pad's
	// C-Stick Right, or Minus on Wiimote/Classic Controller/Wii U Pro
	// Controller/Wii U GamePad (see that function's own comment for the
	// exact field-level mapping - confirmed against this project's real
	// input.cpp, not guessed). Whichever of the menu setting or a held
	// button implies the higher speed wins; they don't stack. 0 = Off
	// (1x), 1 = 2x, 2 = 3x, 3 = 4x. Applied in mgba_emuMain()
	// (vbasupport.cpp) by running the core (effective speed) extra times
	// per displayed frame - see that function's own comment for why
	// audio is NOT muted during the skipped frames (the resulting
	// pitched-up audio is expected turbo-mode behavior, same as other
	// emulators).
	int		FastForwardSpeed;

	int		BasicPalette;	// 0 - Green   1 - Monochrome   2 - GB Pocket   3 - GB Light
	int		MotionTilt;		// Wii Remote tilt control for tilt-sensor GB/GBC games (0/1)

	// If set, MenuGameSelection() shows the pre-tabs interface instead:
	// no GB/GBC/GBA tab strip, no Recent tab, just a single folder listing
	// (GCSettings.LoadFolder, which already defaults to the shared "roms"
	// parent of roms/gb, roms/gbc, roms/gba) that the existing folder-
	// navigation (click a folder to enter it, ".." to go back up) handles
	// for free - no separate merged-listing code needed. For people who
	// don't want the tabbed browser. 0 = tabbed (default), 1 = classic.
	int		ClassicBrowser;

	// Game Boy Player-style border for GBA games, analogous to SGBBorder's
	// "From border file" mode (== 2) but GBA has no native in-core border
	// rendering to offer an equivalent of SGBBorder's mode 1. Unlike
	// SGBBorder this isn't remembered per-game - it's a single global
	// choice ("Border (GBA)" in Emulation settings cycles through every
	// .png/.bor/.bmp file actually present in GBABorderFolder, plus "None"), since
	// GBP borders are decorative frames rather than something tied to a
	// specific game the way SGB borders are. Empty string = None/disabled.
	// Supports 320x240 PNG or BMP (same PNGU_DecodeTo4x4RGB565 /
	// DecodeBMP24ToRGB565 paths as the GBC loader below) or a raw 320x240
	// RGBA8888 .bor dump with no header (the format the MiSTer-GBA-Borders
	// community pack ships). Lives under GCSettings.GBABorderFolder
	// (independently path-editable, separate from the GB/GBC folder below,
	// since GBA borders are a different pixel size). See
	// LoadGBABorderIfEnabled() in vbasupport.cpp.
	char	GBABorderFile[MAXPATHLEN];	// filename only (with extension), relative to GBABorderFolder; "" = None

	// Selected GB/GBC border overlay, used when SGBBorder == 2 ("From
	// border file"). Cycled in menu.cpp through a single combined listing
	// of GBCBorderFolder/*.png, *.sgb and *.bmp - which decoder runs is
	// picked by file extension in LoadGBBorderFileIfEnabled()
	// (vbasupport.cpp). "" = None.
	//
	// .png/.bmp must be 256x224 RGB (same PNGU_DecodeTo4x4RGB565 /
	// DecodeBMP24ToRGB565 paths as the GBA loader above). .sgb is the
	// MiSTer-FPGA-style border overlay format, confirmed against a real
	// sample file (border_sgb1.sgb, 10112 bytes): 256 tiles of SGB/SNES
	// 4bpp planar tile data (8192 bytes), a 32x28 tilemap with palette/flip
	// bits (1792 bytes), then 4 palettes of 16 RGB555 colors (128 bytes) -
	// see DecodeSGBFileToRGB565() for the exact byte layout.
	char	GBBorderFile[MAXPATHLEN];	// filename only (with extension), relative to GBCBorderFolder; "" = None
	
	char	LoadFolder[MAXPATHLEN];  // Path to game files
	char	LastFileLoaded[MAXPATHLEN]; //Last file loaded filename
	char	SaveFolder[MAXPATHLEN];  // Path to save files
	char	ScreenshotsFolder[MAXPATHLEN]; //Path to screenshots files
	char	CoverFolder[MAXPATHLEN]; 	//Path to cover files
	char	ArtworkFolder[MAXPATHLEN]; 	//Path to artwork files
	char	GBABorderFolder[MAXPATHLEN]; // Path to GBA (Game Boy Player-style) border files; independently changeable from GBCBorderFolder.
	char	GBCBorderFolder[MAXPATHLEN]; // Path to GB/GBC border files (.png/.sgb/.bmp); independently changeable.
	char	CheatFolder[MAXPATHLEN]; // Path to per-game .cheats files (mGBA's own mCheatParseFile/mCheatSaveFile format - see CheatsLoadForCurrentROM()/CheatsSaveForCurrentROM(), vbasupport.cpp)
	char	GBFolder[MAXPATHLEN];   // Path to GB rom files
	char	GBCFolder[MAXPATHLEN];  // Path to GBC rom files
	char	GBAFolder[MAXPATHLEN];  // Path to GBA rom files

	// Recently played ROMs: '|'-delimited list of full paths, most recent
	// first, capped at MAX_RECENT_ROMS entries. Kept as a single string field
	// so it reuses the existing single-string XML setting mechanism rather
	// than needing new indexed/array XML support.
	char	RecentROMs[MAXPATHLEN * MAX_RECENT_ROMS];

	// Remember last-selected ROM browser tab (0=GB,1=GBC,2=GBA,3=Recent), so
	// relaunching the app restores it. Cursor position reuses the existing
	// GCSettings.LastFileLoaded field below.
	int		LastActiveTab;
};

void ExitApp();
void ShutdownWii();
bool SupportedIOS(u32 ios);
bool SaneIOS(u32 ios);
extern struct SGCSettings GCSettings;

// Per-controller-type, user-assignable "hold to fast-forward" button, one
// slot per CTRLR_* type (same 6-wide indexing as btnmap[6][10] in
// input.cpp). Unlike btnmap, this is NOT one of the 10 GBA face/d-pad/
// shoulder buttons - it's a separate, independent action, so it isn't
// folded into that array. Defaults to 0 (unassigned/off) for every
// Wiimote-family controller (see ResetControls(), input.cpp) rather than
// hardcoding Minus, since Minus already doubles as the in-game Select
// button on those controllers - holding it to fast-forward while it's
// also bound to Select meant Select-holding games (or just holding it to
// re-map Select) silently triggered fast-forward too. The GameCube
// controller keeps its own hardcoded C-Stick-Right fast-forward (see
// FastForwardHeld(), input.cpp) since it has no such conflict - GC pad's
// default btnmap never uses the C-Stick for anything. ffmap[CTRLR_GCPAD]
// is unused. See FastForwardHeld() (input.cpp) and the "Fast Forward
// (Hold)" row in MenuSettingsMappingsMap() (menu.cpp) for where this is
// read/written.
extern u32 ffmap[6];
extern int ScreenshotRequested;
extern int ConfigRequested;
extern int ShutdownRequested;
extern int ExitRequested;
extern char appPath[];

extern FreeTypeGX *fontSystem[];
extern bool isWiiVC;

// Matches Snes9x TX's own MAX_CHEATS cap (cheatmgr.cpp) - shared
// convention across this dev's GX-family ports. Bounds how many cheat
// rows menu.cpp's MenuGameCheats()/CheatsDeletePicker() will build in one
// OptionList; mGBA's own cheat storage has no such limit itself.
#define MAX_CHEATS 150

/* -----------------------------------------------------------------------
 * Cheats bridge (vbasupport.cpp <-> menu.cpp)
 *
 * Thin wrapper around mGBA's own struct mCheatDevice (mgba/core/cheats.h)
 * so menu.cpp doesn't need to touch mCore internals directly. One cheat
 * set per loaded ROM is used (created on demand), each cheat set holding
 * one "line" per cheat the user added - simpler than exposing mGBA's
 * full multi-set structure, and all this port's UI needs.
 *
 * NOTE ON API CONFIDENCE: the vbasupport.cpp implementation of these is
 * written against my recollection of mGBA's public mCheatDevice/mCheatSet
 * API (mCheatSetCreate, mCheatAddSet, mCheatAddLine, mCheatParseFile,
 * mCheatSaveFile, mCheatRemoveSet, the mCheatSet.enabled/name fields) -
 * NOT verified against your actual mgba/core/cheats.h the way the .sgb
 * border format and the AGS-101 matrix were confirmed against real
 * sample files earlier. If it doesn't compile as-is, the header's real
 * field/function names are almost certainly just slightly different from
 * what's written here - send me mgba/core/cheats.h and I'll correct it. */
int  CheatCount(void);                                              // number of cheats for the current ROM
bool CheatGetInfo(int index, char *descOut, int descOutSize, bool *enabledOut); // false if index out of range
void CheatToggle(int index);                                        // flips enabled on/off, saves to disk
bool CheatAdd(const char *description, const char *code);           // parses+adds one cheat line, saves to disk
void CheatDelete(int index);                                        // removes one cheat, saves to disk

/* -----------------------------------------------------------------------
 * Patch indicator (vbasupport.cpp <-> menu.cpp)
 *
 * Set by LoadPatchForCurrentROM() (vbasupport.cpp) whenever an IPS/UPS/BPS
 * patch was found alongside the current ROM and applied via mGBA's own
 * core->loadPatch(). Reset at the top of every ROM load, so it always
 * reflects the CURRENTLY loaded ROM, not whatever was loaded previously.
 * menu.cpp reads these to show a small "Patch applied" indicator on the
 * pause screen - see MenuGame(). */
extern bool PatchApplied;
extern char PatchFilename[MAXPATHLEN]; // just the patch's own filename (no dir), e.g. "Pokemon Red.ips"

static inline bool IsWiiU(void)
{
	return ((*(vu16*)0xCD8005A0 == 0xCAFE) || isWiiVC);
}
static inline bool IsWiiUFastCPU(void)
{
	return ((*(vu16*)0xCD8005A0 == 0xCAFE) && ((*(vu32*)0xCD8005B0 & 0x20) == 0));
}

#endif