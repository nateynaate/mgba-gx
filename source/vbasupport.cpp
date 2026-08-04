/****************************************************************************
 * mGBA GX - vbasupport.cpp
 *
 * Drop-in replacement for vbagx's vbasupport.cpp.
 * Drives mGBA instead of VBA-M, exports the same interface.
 * Audio uses the same architecture as mGBA's official Wii port:
 * mAudioResampler -> ring buffer -> double-buffered AUDIO DMA.
 ***************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>
#include <errno.h>
#include <math.h>
#include <fcntl.h>
#include <malloc.h>
#include <sys/param.h>
#include <sys/stat.h>

#include <ogc/gx.h>
#include <ogc/audio.h>
#include <ogc/cache.h>
#include <ogc/system.h>
#include <ogc/lwp_watchdog.h>
#include <asndlib.h>

#include <mgba/core/core.h>
#include <mgba/core/interface.h>
#include <mgba/core/log.h>
#include <mgba/core/serialize.h>
#include <mgba-util/vfs.h>
#include <mgba-util/patch.h>
#include <mgba-util/audio-buffer.h>
#include <mgba-util/audio-resampler.h>
#include <mgba-util/configuration.h>
#include <mgba/internal/gba/gba.h>
// GB palette is written via DMG I/O registers using core->rawWrite8:
//   0xFF47 = BGP  (background palette)
//   0xFF48 = OBP0 (sprite palette 0)
//   0xFF49 = OBP1 (sprite palette 1)
// Each register is a packed byte: bits[7:6]=shade3, [5:4]=shade2,
// [3:2]=shade1, [1:0]=shade0. shade 0=lightest, 3=darkest.
// Default DMG: 0xE4 = 11100100 = shade3=3,shade2=2,shade1=1,shade0=0
// The actual rendered colors for each shade index come from mGBA's
// internal dmgPalette table, which IS set from the gb.pal0-3 config
// keys during GBReset(). Since config isn't working, we bypass it by
// writing the palette registers directly after reset() — this triggers
// the same GBVideoWritePalette path that the hardware register writes
// use at runtime, mapping shade indices to whatever mGBA's default
// color table provides. For a custom color palette we'd need to patch
// the dmgPalette table, but for green/mono selection we can instead
// write pre-converted RGB565 colors directly to the palette[] array
// inside struct GBVideo via GBVideoWritePalette.
// Since mgba/internal/gb/gb.h is not in portlibs, we declare the
// function ourselves — it IS compiled into the binary (confirmed in map).

#include "vbagx.h"
#include "menu.h"
#include "vbasupport.h"
#include "filebrowser.h"
#include "gameinput.h"
#include "audio.h"
#include "video.h"
#include "fileop.h"
#include "input.h"
#include "utils/pngu.h"

/* -------------------------------------------------------------------------
 * Globals expected by vbagx
 * ---------------------------------------------------------------------- */
int   cartridgeType = CARTRIDGE_NONE;
int   SunBars       = 0;
u32   RomIdCode     = 0;
bool  TiltSideways  = false;
char  RomTitle[17]  = {0};
struct EmulatedSystem emulator = {0};
bool  PatchApplied  = false;
char  PatchFilename[MAXPATHLEN] = {0};

/* mGBA state */
static struct mCore *core             = NULL;

/* -------------------------------------------------------------------------
 * Save-type detection
 *
 * mGBA's own built-in override table (src/gba/overrides.c, wired up via
 * mCoreLoadConfig() below) forces the correct save type for a handful of
 * well-known problem carts - Pokemon Ruby/Sapphire/Emerald among them -
 * because runtime heuristics alone (guessing from which memory region the
 * game's first save-related access touches) can settle on the wrong
 * type and then SILENTLY RESIZE the existing save file to match once
 * gameplay starts. Real, destructive data loss - not just a "fails to
 * load" cosmetic issue.
 *
 * Rather than only patching in per-game entries by hand as each broken
 * cart is discovered, DetectSaveTypeFromROM() below scans the ROM's own
 * bytes for the save-library ID strings the GBA SDK's linker embeds
 * verbatim into any cart that uses it - "EEPROM_V", "SRAM_V", "SRAM_F_V",
 * "FLASH_V", "FLASH512_V", "FLASH1M_V" (each followed by a 3-digit
 * version number, e.g. "FLASH1M_V102"). This is the same technique
 * VBA/VBA-M and other GBA emulators use as their PRIMARY save-type
 * detection: since the string is part of the actual save-driver code the
 * game was linked against, it's a fact about the cart, not a probabilistic
 * guess from observed behavior - and it correctly disambiguates
 * Flash64k vs Flash128k (FLASH_V vs FLASH1M_V are distinct strings) up
 * front, before any gameplay has happened.
 *
 * This does NOT replace mGBA's own built-in override table - that one
 * still applies via mCoreLoadConfig() for carts it already knows about.
 * It supplements it: any cart with a recognizable ID string gets a
 * correct, general answer without ever being individually special-cased.
 *
 * Carts that hit neither mGBA's table nor a recognizable string (usually
 * because their code was hand-optimized or built without linking the
 * stock SDK save library, so no ID string survives in the binary) fall
 * through to knownSaveTypeOverrides[] below, same as before - but that
 * table now only needs to hold the rare, CONFIRMED-via-hardware-dump
 * exceptions, not every cart in general.
 *
 * Both paths write into the SAME Configuration table that mCoreLoadConfig()
 * below already populates from any on-disk config.ini, mirroring mGBA's
 * own per-game config.ini override mechanism (GBAOverrideFind(),
 * src/gba/overrides.c): a section named "override.<4-char game code>"
 * with a "savetype" key. Writing directly into that table's
 * "override.XXXX" section here has the exact same effect as if the user
 * had hand-edited a config.ini with:
 *     [override.B24E]
 *     savetype=FLASH1M
 * but doesn't depend on this Wii port actually having a writable/loaded
 * config.ini at a well-known path (mCoreInitConfig(core, NULL) above is
 * given a NULL port name, so there's no guarantee one exists here).
 *
 * IMPORTANT: struct mCoreConfig's fields (configTable, port, etc.) are
 * used directly elsewhere in this file only via the mCoreConfigSet*()
 * wrapper functions - this is the first place touching config.configTable
 * directly. If your local mgba/core/config.h doesn't expose configTable
 * as a plain (non-opaque) struct member, this won't compile - the fix in
 * that case is to instead write an actual config.ini file to whatever
 * path mCoreInitConfig(core, NULL) reads from, with the same
 * [override.XXXX] section shown above.
 *
 * Only add entries to knownSaveTypeOverrides[] that are CONFIRMED via a
 * hardware save-type dump (e.g. gbhwdb.gekkio.fi) - guessing wrong there
 * reintroduces the exact "silently resizes/corrupts your save" failure
 * mode this is meant to fix.
 */
struct SaveTypeOverrideEntry {
    char gameCode[5];   // 4 chars + NUL, matches GBA header offset 0xAC
    const char *saveType; // one of: SRAM, FLASH512, FLASH1M, EEPROM, EEPROM512, NONE
};

static const struct SaveTypeOverrideEntry knownSaveTypeOverrides[] = {
    // Pokemon Mystery Dungeon: Red Rescue Team (US/AU) - confirmed 1M FLASH
    // via hardware dump; NOT in mGBA's built-in overrides.c table. Its
    // binary also doesn't carry a scannable FLASH1M_V string (hand-tuned
    // save code, no stock SDK library linked in), so DetectSaveTypeFromROM()
    // can't find it either - this entry is the fallback for exactly that
    // "no string, no built-in override" gap.
    { "B24E", "FLASH1M" },
};

/* Scans romBuffer for a GBA SDK save-library ID string and returns the
 * matching mGBA savetype config value, or NULL if none was found. Order
 * matters: FLASH512_V/FLASH1M_V must be checked before the bare FLASH_V,
 * since some libraries additionally emit a generic "FLASH_V" string
 * alongside the size-specific one and we want the more precise match. */
static const char *DetectSaveTypeFromROM(const u8 *rom, size_t romSize)
{
    static const struct { const char *needle; const char *saveType; } signatures[] = {
        { "EEPROM_V",   "EEPROM"   },
        { "SRAM_F_V",   "SRAM"     },
        { "SRAM_V",     "SRAM"     },
        { "FLASH512_V", "FLASH512" },
        { "FLASH1M_V",  "FLASH1M"  },
        { "FLASH_V",    "FLASH512" }, // bare FLASH_V historically means the 64K/512kbit part
    };

    for (size_t s = 0; s < sizeof(signatures) / sizeof(signatures[0]); s++) {
        size_t needleLen = strlen(signatures[s].needle);
        if (needleLen >= romSize) continue;
        for (size_t i = 0; i + needleLen <= romSize; i++) {
            if (memcmp(rom + i, signatures[s].needle, needleLen) == 0)
                return signatures[s].saveType;
        }
    }
    return NULL;
}

static void ForceKnownSaveTypeOverrides(struct mCore *core, const u8 *romBuffer, size_t romSize)
{
    if (!core || cartridgeType != CARTRIDGE_GBA || romSize < 0xB0)
        return;

    char gameCode[5];
    memcpy(gameCode, romBuffer + 0xAC, 4);
    gameCode[4] = '\0';

    char sectionName[16];
    snprintf(sectionName, sizeof(sectionName), "override.%s", gameCode);

    // 1. Real detection first: scan the ROM's own bytes for a save-library
    //    ID string. This is general - it works for any cart, not just ones
    //    we've hand-listed - and it's a fact about the binary, not a guess.
    const char *detected = DetectSaveTypeFromROM((const u8 *)romBuffer, romSize);
    if (detected) {
        ConfigurationSetValue(&core->config.configTable, sectionName, "savetype", detected);
        printf("[mGBA] Detected save type for %s from ROM signature: savetype=%s\n",
               gameCode, detected);
        return;
    }

    // 2. No scannable string (game shipped with hand-tuned save code, no
    //    stock SDK library linked in) - fall back to the hand-confirmed
    //    per-game table for the rare exceptions that hit this gap.
    for (size_t i = 0; i < sizeof(knownSaveTypeOverrides) / sizeof(knownSaveTypeOverrides[0]); i++) {
        if (memcmp(gameCode, knownSaveTypeOverrides[i].gameCode, 4) == 0) {
            ConfigurationSetValue(&core->config.configTable, sectionName, "savetype",
                                   knownSaveTypeOverrides[i].saveType);
            printf("[mGBA] Forced save-type override for %s (no ROM signature found): savetype=%s\n",
                   gameCode, knownSaveTypeOverrides[i].saveType);
            return;
        }
    }

    // 3. Neither path matched - leave detection to mGBA's own built-in
    //    overrides.c table (already wired up via mCoreLoadConfig() below)
    //    and, failing that, its runtime heuristic.
}
static u16          *videoBuf         = NULL;
// Color-correction output buffer, same size as videoBuf, allocated/freed
// alongside it. NEVER written to by mGBA's core - only by
// ApplyColorCorrection() below. This exists because videoBuf is not
// necessarily fully rewritten every frame: mGBA's own software renderer
// skips redrawing tiles/rows it considers unchanged from its OWN last
// write for performance. Applying the color-correction matrix IN PLACE
// onto videoBuf meant any pixel mGBA didn't redraw a given frame still
// held LAST frame's already-corrected value, which then got corrected
// AGAIN on top of that - and again on subsequent unchanged frames. Since
// this matrix's rows each sum to 1.0, gray is an exact fixed point but
// everything else gets pulled toward gray a little on every application -
// so a pixel sitting unchanged for many frames (a static menu screen,
// motionless background tiles, etc.) would desaturate more and more the
// longer it stayed on screen, and looked like flicker/shimmer wherever
// "just redrawn by mGBA" (correct) pixels sat next to "stale, N times
// corrected" (desaturated) ones. Writing corrected output to this
// separate buffer instead - always freshly recomputed in full from
// videoBuf's current (genuine, mGBA-owned) contents every frame - makes
// that compounding impossible, since we never read back our own prior
// output as input again.
static u16          *correctedVideoBuf = NULL;
// Previous (pre-blend) frame, used by ApplyInterframeBlending() below to
// implement the "Simple" 50/50 interframe blend mode from mGBA's own
// frontends (mCoreOptions.interframeBlending / renderer.interframeBlending
// in mgba's SDL/Qt code - see src/platform/sdl/main.c). This port doesn't
// use mGBA's own video renderer objects (it feeds GX_Render directly from
// whichever of videoBuf/correctedVideoBuf is current), so the blend is
// done here at the GX pipeline level instead of via a core config option -
// same approach already used for color correction and the GB palette.
static u16          *prevVideoBuf     = NULL;
static bool          coreRunning      = false;
static bool          videoInited      = false;

/* Screen dimensions */
#define GBA_SCREEN_W  240
#define GBA_SCREEN_H  160
#define GB_SCREEN_W   160
#define GB_SCREEN_H   144
#define SCREEN_W (cartridgeType == CARTRIDGE_GBA ? GBA_SCREEN_W : GB_SCREEN_W)
#define SCREEN_H (cartridgeType == CARTRIDGE_GBA ? GBA_SCREEN_H : GB_SCREEN_H)

/* Actual per-frame output dimensions, queried from the core after ROM load.
 * Equal to SCREEN_W/H normally, but 256x224 for GB/GBC games when the
 * Super Game Boy border ("From game") setting is active. */
static int gbCoreW = GBA_SCREEN_W;
static int gbCoreH = GBA_SCREEN_H;

/* ROM buffer — required by mGBA's FIXED_ROM_BUFFER Wii platform code.
 * Allocated from MEM2 Arena2, exactly as the official Wii port does. */
uint32_t *romBuffer     = NULL;
size_t    romBufferSize = 0;

/* emulating flag read by vbagx main loop */
int emulating = 0;

/* -------------------------------------------------------------------------
 * Wii Remote tilt-sensor peripheral (mGBA's mRotationSource interface)
 *
 * A handful of GB/GBC and GBA carts have a built-in motion sensor instead
 * of (or alongside) normal buttons: Kirby Tilt 'n' Tumble and Koro Koro
 * Puzzle Happy Panechu are GB/GBC MBC7 carts (2-axis accelerometer);
 * WarioWare Twisted (GBA, built-in gyro) and Yoshi Topsy-Turvy / Yoshi's
 * Universal Gravitation (GBA, 2-axis accelerometer) are GBA carts with
 * their own sensor hardware. mGBA already emulates all of these chips
 * internally; it just needs something to feed it live tilt/rotation values
 * every frame via mCore::setPeripheral(mPERIPH_ROTATION, ...). We feed it
 * the Wii Remote's own accelerometer-derived orientation (GetWiimoteTilt()
 * in input.cpp), which is a very natural match for games that were
 * designed to be tilted/twisted in your hands.
 *
 * Scale: verified directly against mGBA's own reference implementation in
 * src/platform/libretro/libretro.c, which is the authoritative source for
 * what raw int32_t range readTiltX/readTiltY actually expect:
 *     tiltX = sensorGetCallback(0, RETRO_SENSOR_ACCELEROMETER_X) *  3e8f;
 *     tiltY = sensorGetCallback(0, RETRO_SENSOR_ACCELEROMETER_Y) * -3e8f;
 * There, the accelerometer input is normalized to roughly -1.0..+1.0 (+-1g
 * per axis) before that 3e8f (300,000,000) scale is applied. Our previous
 * WII_TILT_SCALE of 40 was off by about 7 orders of magnitude - regardless
 * of whether the orientation was being read correctly, multiplying by 40
 * produced a value so close to zero that no game's tilt threshold would
 * ever register it as anything but dead flat.
 *
 * GetWiimoteTilt() returns roughly -90..+90 degrees, which we normalize to
 * the same -1.0..+1.0 range libretro uses (treating +-90 degrees as the
 * full +-1g range) before applying the same 3e8f scale. The Y-axis sign
 * flip mirrors libretro's own (screen/device "up" vs. accelerometer "up"
 * convention) - if in-game tilt ends up feeling inverted on hardware,
 * that's the sign to flip back, not the overall scale. */
#define WII_TILT_SCALE 3e8f

// Scale derived the same way as WII_TILT_SCALE above - starting from
// mGBA's own reference implementation (src/platform/libretro/libretro.c):
//   gyroZ = sensorGetCallback(GYROSCOPE_Z) * -5.5e8f;
// where the libretro gyroscope input is in rad/s. Converting our
// degrees-per-frame yaw delta to rad/s at 60Hz (GB/GBA's native frame
// rate): degrees_per_frame * 60 * (pi/180) ~= degrees_per_frame * 1.047,
// then applying the same -5.5e8f scale gives ~ -5.76e8f as the combined
// per-degree-per-frame constant. Unverified on real hardware - if gyro
// sensitivity doesn't feel right, this is an experimental starting point,
// not a confirmed-correct value like WII_TILT_SCALE.
#define WII_GYRO_SCALE -5.76e8f

// input.cpp - real WiiMotion Plus-backed yaw and the function to request
// Motion Plus fusion be enabled. input.h isn't available to add these to
// directly, hence the direct extern here - same pattern as
// GetCurrentTVFrameRate() above (video.cpp).
extern void GetWiimoteYaw(float *yaw);
extern void EnableWiimoteMotionPlus(bool enable);

static void TiltSample(struct mRotationSource *source) { (void)source; }

static int32_t TiltReadX(struct mRotationSource *source)
{
	(void)source;
	float tx, ty;
	GetWiimoteTilt(&tx, &ty);
	return (int32_t)((tx / 90.0f) * WII_TILT_SCALE);
}

static int32_t TiltReadY(struct mRotationSource *source)
{
	(void)source;
	float tx, ty;
	GetWiimoteTilt(&tx, &ty);
	return (int32_t)((ty / 90.0f) * -WII_TILT_SCALE);
}

static int32_t TiltReadGyroZ(struct mRotationSource *source)
{
	(void)source;
	// Real gyroscope data, backed by WiiMotion Plus (built-in or the
	// standalone accessory) - see GetWiimoteYaw()'s own comment (input.cpp)
	// for why a plain Wii Remote's accelerometer fundamentally can't
	// provide this axis at all, gyro-approximated or otherwise. This
	// replaces the old accelerometer-roll-delta stand-in that used to live
	// here - EnableWiimoteMotionPlus() (see the call site in
	// mgba_emuLoadFile() below) has to have actually been called for
	// orient.yaw to be gyro-accurate; without it wiiuse still returns
	// *something* for yaw, just not a meaningfully accurate one, so this
	// degrades the same way it always did on hardware with no Motion Plus
	// attached, rather than failing outright.
	//
	// orient.yaw is a cumulative ANGLE (-180..180 degrees), not a rate, so
	// this still needs a frame-to-frame delta to get the angular velocity
	// readGyroZ expects - same shape as the old approximation, just fed
	// from a real gyro instead of accelerometer roll. Unlike accelerometer
	// roll (which never exceeds +-90), yaw can wrap from +180 to -180
	// during an actual fast spin - exactly the motion this axis exists to
	// detect - so the delta is normalized to the shortest angular distance
	// before scaling, or a full spin would register as one enormous
	// spurious spike instead of a smooth continuous rotation rate.
	//
	// Scale: same WII_GYRO_SCALE derivation as before this change (see its
	// own definition above) - degrees-per-frame at GB/GBA's native ~60Hz,
	// converted to rad/s, then mGBA's own libretro.c reference scale
	// applied. That derivation didn't depend on which sensor the degrees
	// came from, so it still applies unchanged here.
	static float lastYaw = 0.0f;
	float yaw;
	GetWiimoteYaw(&yaw);
	float delta = yaw - lastYaw;
	lastYaw = yaw;
	if (delta > 180.0f) delta -= 360.0f;
	else if (delta < -180.0f) delta += 360.0f;
	return (int32_t)(delta * WII_GYRO_SCALE);
}

// Field-assigned (not aggregate-initialized) to avoid depending on the
// exact declared field order in mgba/core/interface.h - confirmed from
// mGBA's own libretro.c that struct mRotationSource has exactly these 4
// function-pointer fields (no userdata pointer).
static struct mRotationSource gTiltSource;

static void InitTiltSource()
{
	gTiltSource.sample = TiltSample;
	gTiltSource.readTiltX = TiltReadX;
	gTiltSource.readTiltY = TiltReadY;
	gTiltSource.readGyroZ = TiltReadGyroZ;
}

/* -------------------------------------------------------------------------
 * Cart rumble peripheral (mGBA's mRumbleIntegrator helper)
 *
 * A few carts have a built-in rumble motor - Pokemon Pinball (GBC) and
 * Pokemon Pinball: Ruby & Sapphire (GBA) are the well-known ones. The old
 * VBA core had its own direct hook into this project's Wii-side rumble
 * driver (systemCartridgeRumble() in input.cpp, which handles the actual
 * WPAD_Rumble() calls, debouncing, and the "Rumble" GCSettings toggle);
 * that hook was VBA-specific and got orphaned when the VBA core was
 * removed, leaving the Wii-side driver correct but never actually called -
 * this reconnects mGBA to that same existing driver rather than
 * reimplementing the debouncing/timing logic from scratch.
 *
 * mRumbleIntegrator (vs. the raw mRumble interface) is mGBA's own helper
 * for exactly this case - it handles the raw interface's timing-based
 * setRumble(enable, sinceLast)/integrate(period) calls internally and
 * exposes a simple setRumble(struct mRumbleIntegrator*, float level)
 * callback instead. This matches mGBA's own reference implementation in
 * src/platform/libretro/libretro.c exactly (down to registering &gRumble
 * itself, not &gRumble.d - mRumbleIntegrator::d is its first member, so
 * they're the same address; mCore::setPeripheral expects a struct
 * mRumble*, which this same-address trick satisfies). */
static struct mRumbleIntegrator gRumble;

static void RumbleSetLevel(struct mRumbleIntegrator *rumble, float level)
{
	(void)rumble;
	systemCartridgeRumble(level > 0.0f);
}

static void InitRumbleSource()
{
	mRumbleIntegratorInit(&gRumble);
	gRumble.setRumble = RumbleSetLevel;
}

/* -------------------------------------------------------------------------
 * Solar sensor peripheral (mGBA's GBALuminanceSource interface)
 *
 * Boktai / Boktai 2: Solar Boy Django / Boktai 3: Sabata's Counterattack
 * (GBA) have a built-in light sensor cartridge. mGBA's GBA core emulates
 * the sensor hardware itself, but - unlike rotationSource/rumble above -
 * nothing in this port ever registered a GBALuminanceSource for it. Boktai
 * 2's intro includes a mandatory solar-calibration screen that polls the
 * sensor in a loop waiting for a reading before it will continue; with no
 * light source attached, that poll is never satisfied and the game hangs
 * right there - this is what was causing the freeze partway through the
 * intro dialogue. (Games that don't have this cart hardware simply never
 * call readLuminance, so registering this unconditionally for every GBA
 * game is harmless, same reasoning as rotationSource above.)
 *
 * The Wii has no ambient light sensor to read a real value from, so like
 * mGBA's own reference frontends (e.g. the shared GBAGUIRunner's
 * _readLux(), which this mirrors) we expose an adjustable level instead of
 * a live reading - driven by the existing SunBars global (0-10, cycled by
 * the in-game Weather button in menu.cpp's pause menu). GBA_LUX_LEVELS is
 * mGBA's own 10-entry step table (extern const int[10], declared in
 * mgba/gba/interface.h - do not redeclare it locally, it's already
 * exported by the portlib) - the same one the reference GUI runner reads
 * from. A *lower* register value means *brighter* on real hardware, hence
 * the 0xFF inversion. */
static void SolarSample(struct GBALuminanceSource *lux) { (void)lux; }

static uint8_t SolarReadLuminance(struct GBALuminanceSource *lux)
{
	(void)lux;
	int value = 0x16;
	if (SunBars > 0)
		value += GBA_LUX_LEVELS[(SunBars > 10 ? 10 : SunBars) - 1];
	return 0xFF - value;
}

static struct GBALuminanceSource gSolarSource;

static void InitSolarSource()
{
	gSolarSource.sample = SolarSample;
	gSolarSource.readLuminance = SolarReadLuminance;
}

/* -------------------------------------------------------------------------
 * Per-game settings memory
 * ---------------------------------------------------------------------- */
// Remembers GBHardware / SGBBorder / BasicPalette per game, so switching
// between different carts that need different hardware modes doesn't
// require manually re-toggling settings every time. Keyed by the game's
// title as stored in its own ROM header (GB: offset 0x134, 16 bytes; GBA:
// offset 0xA0, 12 bytes) - extracted directly from the raw ROM bytes we
// already have in romBuffer, so this works even before/without the core
// being initialized.
#define MAX_GAME_SETTINGS 128
struct PerGameSettings {
    char title[17];
    int  GBHardware;
    int  SGBBorder;
    int  BasicPalette;
};

static void ExtractGameKey(const void *romData, size_t romSize, int cartType, char *out, size_t outSize)
{
    memset(out, 0, outSize);
    if (!romData) return;

    size_t offset, len;
    if (cartType == CARTRIDGE_GB)       { offset = 0x134; len = 16; }
    else if (cartType == CARTRIDGE_GBA) { offset = 0xA0;  len = 12; }
    else return;

    if (romSize < offset + len) return;
    len = (len < outSize - 1) ? len : outSize - 1;
    memcpy(out, (const u8 *)romData + offset, len);

    // trim trailing spaces/control bytes so keys compare cleanly
    for (int i = (int)strlen(out) - 1; i >= 0; i--) {
        if ((unsigned char)out[i] <= ' ')
            out[i] = '\0';
        else
            break;
    }
}

static void GetGameSettingsPath(char *path, size_t sz)
{
    snprintf(path, sz, "%s%s/gamesettings.dat", pathPrefix[GCSettings.SaveMethod], GCSettings.SaveFolder);
}

static bool LoadPerGameSettings(const char *key, int *hw, int *border, int *pal)
{
    if (!key || key[0] == '\0') return false;
    char path[MAXPATHLEN];
    GetGameSettingsPath(path, sizeof(path));

    FILE *f = fopen(path, "rb");
    if (!f) return false;

    struct PerGameSettings rec;
    bool found = false;
    while (fread(&rec, sizeof(rec), 1, f) == 1) {
        if (strncmp(rec.title, key, sizeof(rec.title)) == 0) {
            *hw = rec.GBHardware;
            *border = rec.SGBBorder;
            *pal = rec.BasicPalette;
            found = true;
            break;
        }
    }
    fclose(f);
    return found;
}

static void SavePerGameSettings(const char *key, int hw, int border, int pal)
{
    if (!key || key[0] == '\0') return;
    char path[MAXPATHLEN];
    GetGameSettingsPath(path, sizeof(path));

    static struct PerGameSettings records[MAX_GAME_SETTINGS];
    int count = 0;
    FILE *f = fopen(path, "rb");
    if (f) {
        count = (int)fread(records, sizeof(struct PerGameSettings), MAX_GAME_SETTINGS, f);
        fclose(f);
    }

    int idx = -1;
    for (int i = 0; i < count; i++) {
        if (strncmp(records[i].title, key, sizeof(records[i].title)) == 0) { idx = i; break; }
    }
    if (idx == -1) {
        if (count < MAX_GAME_SETTINGS) idx = count++;
        else idx = 0; // table full - simplest eviction: overwrite the oldest slot
    }

    strncpy(records[idx].title, key, sizeof(records[idx].title) - 1);
    records[idx].title[sizeof(records[idx].title) - 1] = '\0';
    records[idx].GBHardware = hw;
    records[idx].SGBBorder = border;
    records[idx].BasicPalette = pal;

    f = fopen(path, "wb");
    if (f) {
        fwrite(records, sizeof(struct PerGameSettings), count, f);
        fclose(f);
    }
}

// Key for whatever game is CURRENTLY loaded (or was, right up until
// UnloadCore() clears romBuffer's contents by overwriting them with the
// next ROM). Recomputed at load time; read again at unload time before the
// new ROM's bytes overwrite the buffer.
static char currentGameKey[17] = {0};

/* Actual per-frame stride (in pixels) mGBA is writing into videoBuf, so
 * video.cpp's texture-tiling code can use the real value instead of
 * assuming a fixed convention for every resolution. */
int gGbVideoStride = 0;

/* True native console resolution (160x144 GB/GBC, 240x160 GBA), regardless
 * of whether an SGB border is active. Used by video.cpp to crop screenshots
 * to just the game viewport instead of the full bordered canvas. */
int gGbNativeW = 0;
int gGbNativeH = 0;

/* True only when the resolved hardware model is genuine DMG (not
 * CGB/SGB/SGB2/AGB) - the GB Screen Palette setting only makes sense in
 * this mode, since CGB/SGB games supply their own real color data. Set in
 * LoadVBAROM() once the model is resolved; read every frame by
 * ApplyGBPalette() in mgba_emuMain(). */
bool gGbDmgMode = false;

extern void GX_Render(int w, int h, u8 *buf);
extern void GX_Render_Init(int width, int height);

/* -------------------------------------------------------------------------
 * Logger
 * ---------------------------------------------------------------------- */
static void GXMGBALog(struct mLogger *logger, int category,
                      enum mLogLevel level, const char *fmt, va_list args)
{
    (void)logger; (void)category; (void)level;
    vprintf(fmt, args); printf("\n");
}
static struct mLogger gxLogger = { .log = GXMGBALog };

/* -------------------------------------------------------------------------
 * Audio — reverted back to a polling model, called once per emulated frame
 * from mgba_emuMain() below, instead of the mAVStream/setAVStream callback
 * registration this briefly went through.
 *
 * Why: core->setAVStream() could never be confirmed to be a real, correct
 * mCore API for primary gameplay audio. mGBA's own documented frontend
 * pattern for audio is the pull-based getAudioBuffer()/setAudioBufferSize()
 * model, and mAVStream itself looks like it's actually the AV
 * capture/recording interface (GIF/video export - postVideoFrame,
 * videoDimensionsChanged), not the playback path. Rather than ship an
 * architecture that's likely solving this with the wrong subsystem on top
 * of an unconfirmed API, this goes back to a known-shape polling call.
 *
 * The resampler setup itself (mAudioResampler, mINTERPOLATOR_COSINE, 48000
 * destination) is kept as-is - that part was never in question, only how
 * often/how it gets pumped.
 * ---------------------------------------------------------------------- */
extern u8* GetMixerDataPtr();
extern volatile int* GetMixerHeadPtr();
extern volatile int* GetMixerTailPtr();

// video.cpp - returns the real Hz mgba_emuMain()'s VSync loop is currently
// pacing to (50.0 for true PAL, ~59.94 for everything else). See that
// function's own comment for why only true PAL differs.
extern double GetCurrentTVFrameRate();

// audio.cpp - re-primes the DMA chain if SwitchAudioMode(1) handed audio to
// ASND while the menu was open. Called from PushAudio() below, once per
// emulated frame.
extern void RestartAudioDMA();

#define MIXBUFFSIZE_LOCAL 0x10000
#define MIXERMASK_LOCAL   ((MIXBUFFSIZE_LOCAL >> 2) - 1)

static bool audioInitialized = false;
static struct mAudioBuffer    resamplerDest;
static struct mAudioResampler resampler;
static double fpsRatio = 1.0;
static unsigned lastCoreRate = 0;  // last core->audioSampleRate() seen; mAudioResamplerSetSource()
                                    // is only ever called again when this actually changes (e.g. GBA
                                    // SOUNDBIAS mid-game). GB/GBC never changes this mid-game.

// --- Periodic real-time recalibration ------------------------------------
// fpsRatio (InitMGBAAudio(), below) is computed once per ROM load from
// GetCurrentTVFrameRate()'s NOMINAL value (50.0/59.94) divided into the
// core's native FPS. That assumes mgba_emuMain() actually achieves that
// many real callbacks per second. In testing this doesn't hold on GB/GBC:
// per-frame video cost (color LUT application, interframe blending,
// texture upload) plus the vsync wait in GX_Render() means the ACHIEVED
// callback rate sits measurably below the nominal TV rate. Because the
// resampler's source rate is set once from the nominal assumption, it
// keeps handing the DMA path fewer real samples per real second than the
// DMA (a fixed 48kHz hardware clock) drains - not a one-off stall, but a
// small constant deficit on essentially every DMA callback, which is
// exactly the "short by 65/800 words, every callback" pattern seen in
// testing rather than the occasional-big-gap pattern a rare GPU stall
// would produce.
//
// --- Periodic real-time recalibration (superseded, see below) -----------
// An earlier pass here added a "measure the real achieved frame rate and
// periodically recompute the resampler source rate from it" scheme, aimed
// at the same sustained-deficit pattern found in the underrun log. That's
// now handled more directly for GB/GBC by the ported VBA-GX dynamic-rate-
// control driver (GBC_AudioGetDynamicRate(), audio.cpp) - a continuous,
// hysteresis-gated correction driven by actual DMA-queue occupancy rather
// than an inferred frame-rate estimate, applied via lightweight linear
// interpolation on already-resampled output (ResampleChunkForDynamicRate()
// below) instead of a periodic mAudioResamplerSetSource() call. That
// approach directly measures the thing that actually matters (is the DMA
// queue draining faster than it's being filled) instead of inferring it
// from frame timing, so the wall-clock recalibration scaffolding was
// removed rather than layered on top of it.

// --- Why there's no per-frame rate nudging here anymore ------------------
// An earlier version of this function re-called mAudioResamplerSetSource()
// whenever a smoothed FIFO-occupancy estimate said the source rate should
// be nudged by a fraction of a percent, to keep resamplerDest from slowly
// drifting empty or full relative to its halfway mark. That reasoning was
// sound, but mAudioResamplerSetSource() resets the cosine interpolator's
// internal phase every time it is called - there is no phase-preserving
// "just nudge the ratio" entry point in mGBA's resampler API. Because the
// occupancy estimate wobbles by a small amount essentially every frame,
// even a deadbanded version of that nudge still re-armed (and therefore
// phase-reset) far too often. Each reset is a small discontinuity in the
// output stream; on GB/GBC's hard-edged PSG square/pulse waves that is
// audible as crackle/harshness, especially on a sustained tone (the
// DuckTales title-screen synth lead that flagged this). GBA's Direct
// Sound output is smoother/noisier already and mostly masks the same
// discontinuities, which is why this only stood out on GB/GBC.
//
// The fix is to stop treating "FIFO is a little off-center" as a reason
// to reset resampler phase at all. The source rate is set once per ROM
// load (InitMGBAAudio(), from the core's nominal audioSampleRate() times
// fpsRatio) and only re-armed here if the core itself reports a genuinely
// different sample rate. Any slow FIFO drift from imperfect frame pacing
// is absorbed by resamplerDest's headroom (16384 samples - see
// InitMGBAAudio()'s comment) instead of being fought sample-by-sample;
// that headroom is exactly what's needed to make occasional under/over
// fill inaudible without ever touching the resampler's phase.

// --- GB/GBC dynamic-rate chunk resample -----------------------------------
// --- GB/GBC dynamic-rate resample: continuous-phase, not per-chunk -------
// The previous version of this stage resampled each 800-frame DMA chunk
// independently (fresh 0..N mapping every call), which resets the linear
// interpolator's fractional phase at every chunk boundary - the same class
// of bug as the mAudioResamplerSetSource() phase-reset issue this whole
// GBC driver was built to avoid, just reintroduced one layer up. Measured
// against a real hardware mGBA recording, that showed up as a genuinely
// elevated noise floor from ~12kHz up (aliasing/imaging from the resample,
// not clipping) across nearly the entire clip, plus the whole recording
// finishing 0.77% early - meaning the DMA queue was in DRAINING almost
// continuously, so the stretch was active on nearly every chunk, not
// occasionally.
//
// Fix: track a persistent fractional read position into an accumulation
// buffer across PushAudio() calls, so consecutive chunks are phase-
// continuous - a small streaming resampler instead of N independent ones.
static s16   s_gbcAccum[20480 * 2];
static int   s_gbcAccumCount = 0;   // valid stereo frames currently in s_gbcAccum, starting at index 0
static double s_gbcReadPos = 0.0;   // fractional read position into s_gbcAccum, persists across calls

// Push audio-quality diagnostics: running average of the dynamic-rate
// multiplier actually applied, printed periodically over telnet. If this
// sits consistently above/below 1.0 rather than oscillating around it,
// that's the DMA queue chronically running in one direction (draining or
// filling) rather than just absorbing normal jitter - a sign the root
// cause is a genuine average rate mismatch upstream (fpsRatio / achieved
// frame rate), not something this corrector should be fully compensating
// for on its own.
static double s_gbcRateSum = 0.0;
static u32    s_gbcRateSamples = 0;
static u64    s_gbcLastRatePrint = 0;

static void PushAudioGBC(const int16_t* fresh, size_t freshCount)
{
    // Append the freshly-resampled frames onto the accumulation buffer.
    size_t room = (sizeof(s_gbcAccum) / (2 * sizeof(s16))) - s_gbcAccumCount;
    if (freshCount > room) freshCount = room; // clamp - should not realistically be hit
    memcpy(&s_gbcAccum[s_gbcAccumCount * 2], fresh, freshCount * 2 * sizeof(s16));
    s_gbcAccumCount += (int)freshCount;

    while (GBC_AudioCanWrite())
    {
        double rate = GBC_AudioGetDynamicRate();
        s_gbcRateSum += rate;
        s_gbcRateSamples++;

        // Do we have enough lookahead to produce a full 800-frame chunk at
        // this rate without running past the data we actually have?
        double endPos = s_gbcReadPos + rate * 799.0;
        if ((int)endPos + 1 >= s_gbcAccumCount) break; // not enough source yet - wait for the next PushAudio() call

        s16* dst = (s16*)GBC_AudioGetWriteBuffer();
        double pos = s_gbcReadPos;
        for (int i = 0; i < 800; i++) {
            int i0 = (int)pos;
            double frac = pos - i0;
            s16 l0 = s_gbcAccum[i0*2],     r0 = s_gbcAccum[i0*2+1];
            s16 l1 = s_gbcAccum[(i0+1)*2], r1 = s_gbcAccum[(i0+1)*2+1];
            dst[i*2]   = (s16)(l0 + (l1 - l0) * frac);
            dst[i*2+1] = (s16)(r0 + (r1 - r0) * frac);
            pos += rate;
        }
        GBC_AudioCommitWrite();
        s_gbcReadPos = pos;
    }

    // Compact: drop whole frames already consumed off the front so the
    // accumulation buffer doesn't grow unbounded, adjusting the fractional
    // read position to match instead of resetting it (that reset is
    // exactly the phase discontinuity this rewrite is fixing).
    int consumedWhole = (int)s_gbcReadPos;
    if (consumedWhole > 0) {
        int remaining = s_gbcAccumCount - consumedWhole;
        if (remaining > 0)
            memmove(s_gbcAccum, &s_gbcAccum[consumedWhole * 2], (size_t)remaining * 2 * sizeof(s16));
        s_gbcAccumCount = remaining;
        s_gbcReadPos -= consumedWhole;
    }

    // Periodic diagnostic: average applied rate over the last ~1s. A
    // sustained bias away from 1.0 here (rather than the average hovering
    // right around it) points at the upstream production/consumption rate
    // still being systematically off, not just needing this corrector's
    // hysteresis to smooth out normal jitter.
    if (s_gbcRateSamples >= 60) {
        u64 now = gettime();
        if (s_gbcLastRatePrint == 0 || ticks_to_microsecs(now - s_gbcLastRatePrint) > 1000000) {
            // TEST: starvation/fade-in deltas over the same ~1s window, to
            // check whether the ring is going empty far more often than
            // the smoothed unplayed=N figure alone would suggest.
            static u32 lastStarve = 0, lastFadeIn = 0;
            u32 starveNow, fadeInNow;
            GBC_AudioGetFadeStats(&starveNow, &fadeInNow);
            printf("[audio][gbc] avg dynamic rate=%.5f over %lu chunks (unplayed=%d) starve+%lu fadein+%lu\n",
                s_gbcRateSum / s_gbcRateSamples, (unsigned long)s_gbcRateSamples, GBC_AudioGetUnplayed(),
                (unsigned long)(starveNow - lastStarve), (unsigned long)(fadeInNow - lastFadeIn));
            lastStarve = starveNow;
            lastFadeIn = fadeInNow;
            s_gbcRateSum = 0.0;
            s_gbcRateSamples = 0;
            s_gbcLastRatePrint = now;
        }
    }
}

// Called once per emulated frame from mgba_emuMain() below. Pulls whatever
// the core has produced through the resampler and drains the result into
// vbagx's existing mixerdata ring buffer (audio.cpp) - the consumer side
// (MIXER_GetSamples/AudioPlayer's DMA callback) is untouched.
static void PushAudio()
{
    if (!core || !audioInitialized) return;

    // Only re-point the resampler's source when the core's reported rate
    // actually changes (GBA's SOUNDBIAS can do this mid-game; GB/GBC
    // normally won't). This is now the ONLY condition that calls
    // mAudioResamplerSetSource() outside of InitMGBAAudio() - see the
    // block comment above for why per-frame drift nudging was removed
    // entirely rather than just throttled.
    unsigned srcRate = core->audioSampleRate(core);
    if (!srcRate) srcRate = 32768;
    if (srcRate != lastCoreRate) {
        lastCoreRate = srcRate;
        mAudioResamplerSetSource(&resampler, core->getAudioBuffer(core),
            (unsigned)(srcRate * fpsRatio), true);
    }

    RestartAudioDMA();

    mAudioResamplerProcess(&resampler);
    size_t avail = mAudioBufferAvailable(&resamplerDest);
    if (avail == 0) return;
    static int16_t tmp[16384 * 2];
    if (avail > 16384) avail = 16384;
    mAudioBufferRead(&resamplerDest, tmp, avail);

    if (cartridgeType == CARTRIDGE_GB) {
        PushAudioGBC(tmp, avail);
        return;
    }

    // GBA: unchanged continuous ring push into audio.cpp's mixerdata.
    u32 *src = (u32 *)tmp;
    u32 *dst = (u32 *)GetMixerDataPtr();
    volatile int *headPtr = GetMixerHeadPtr();
    volatile int *tailPtr = GetMixerTailPtr();
    int localHead = *headPtr;
    int consumer  = *tailPtr;
    for (size_t i = 0; i < avail; i++) {
        int next = (localHead + 1) & MIXERMASK_LOCAL;
        if (next == consumer) break;
        u32 v = src[i];
        dst[localHead] = ((v >> 16) | (v << 16));
        localHead = next;
    }
    *headPtr = localHead;
}

static void InitMGBAAudio()
{
    // Must run before SwitchAudioMode(0) below - it decides which DMA
    // callback (GBA ring vs GBC dynamic-rate queue) SwitchAudioMode(0)
    // actually registers. cartridgeType is already resolved by this point
    // (set from core->platform() earlier in LoadMGBAROM(), before init).
    AudioSetPlatform(cartridgeType == CARTRIDGE_GB);

    // SwitchAudioMode(0) must run on every ROM load, not just the first —
    // the menu calls SwitchAudioMode(1) each time it opens, handing the AI
    // hardware back to ASND. If this only ran once, every ROM load after the
    // first menu session would push audio into a ring buffer that nobody was
    // draining (DMA chain stopped, ASND has the hardware). The resampler
    // buffers only need to be initialized once, so that part stays guarded.
    SwitchAudioMode(0);

    if (!audioInitialized)
    {
        // 16384 (4x the old 4096) - see PushAudio()'s block comment on
        // GBInterrupt()'s early-exit backpressure mechanism for why a
        // too-small buffer here specifically hurts GB/GBC: it's cheap on
        // Wii's RAM (16384 stereo s16 samples = 64KB) and gives real
        // headroom against a single runFrame() call producing more audio
        // than one frame's nominal amount before we get back to drain it.
        mAudioBufferInit(&resamplerDest, 16384, 2);
        // FIX: mINTERPOLATOR_COSINE (mGBA's own Wii-port default) has weak
        // stopband rejection on the core-rate->48000 resample and was the
        // confirmed source of a broadband noise floor audible under game
        // audio. Measured via matched-gain recordings, comparing genuine
        // silent passages: cosine floor ~-56dBFS, sinc floor ~-73 to
        // -83dBFS across 8-24kHz (~17dB improvement, near the 16-bit
        // theoretical floor of ~-96dBFS). All other candidates (DMA queue
        // draining, volume scalar, the custom linear interpolator in
        // PushAudioGBC(), fpsRatio TV/native-rate compensation, fade-in/
        // starvation ramps) were individually tested and ruled out first -
        // see audio.cpp / vbasupport.cpp comments near
        // mAudioResamplerInit()/mAudioResamplerProcess() for that trail.
        mAudioResamplerInit(&resampler, mINTERPOLATOR_SINC);
        mAudioResamplerSetDestination(&resampler, &resamplerDest, 48000);

        audioInitialized = true;
    }

    // Everything below runs on every ROM load, not just the first - core is
    // a fresh instance each time (see UnloadCore()/LoadVBAROM()), so its
    // own resampler source and fpsRatio both need re-establishing per-ROM
    // even though the resampler buffers themselves are reused.
    if (!core) return;

    double nativeFPS = 60.0; // matches the reference's own flat fallback (fps = 60.0/1.001)
    if (core->frequency(core) > 0 && core->frameCycles(core) > 0)
        nativeFPS = (double)core->frequency(core) / (double)core->frameCycles(core);
    fpsRatio = GetCurrentTVFrameRate() / nativeFPS;

    unsigned srcRate = core->audioSampleRate(core);
    if (!srcRate) srcRate = 32768;
    lastCoreRate = srcRate;
    mAudioResamplerSetSource(&resampler, core->getAudioBuffer(core),
        (unsigned)(srcRate * fpsRatio), true);
}
/* -------------------------------------------------------------------------
 * Teardown
 * ---------------------------------------------------------------------- */
static void *lastRomData = NULL;

static void UnloadCore()
{
    // No explicit SRAM flush here anymore - see core->unloadROM() just
    // below. mGBA's own GBA core memory-maps the .sav file directly onto
    // the same VFile handed to core->loadSave() at ROM-load time
    // (GBASavedataInitFlash/SRAM/EEPROM -> vf->map(), src/gba/savedata.c)
    // and keeps that mapping live for the entire session - the game's
    // actual battery-save writes go straight into it continuously, no
    // separate flush step needed. It's written back to disk precisely
    // when core->unloadROM() closes savedata.realVf, right below.
    //
    // This used to also call SaveBatteryOrStateAuto(FILE_SRAM, SILENT)
    // here - opening a SECOND, independent file handle on that exact same
    // path with O_TRUNC, while the FIRST handle (the one just described)
    // was still live-mapped and about to be flushed/closed by
    // core->unloadROM() a few lines later. Two handles to the same file,
    // one of them truncating it out from under the other's active
    // mapping, is exactly the kind of thing that silently corrupts or
    // zeroes a save - this was very likely the actual root cause of saves
    // being cleared, not just an edge case in the write logic itself.

    ShutoffRumble(); // don't let cart rumble carry over across a game switch

    coreRunning = false;
    emulating   = 0;
    if (core) { core->unloadROM(core); core->deinit(core); core = NULL; }
    if (videoBuf) { free(videoBuf); videoBuf = NULL; }
    if (correctedVideoBuf) { free(correctedVideoBuf); correctedVideoBuf = NULL; }
    if (prevVideoBuf) { free(prevVideoBuf); prevVideoBuf = NULL; }
    cartridgeType = CARTRIDGE_NONE;
    RomIdCode = 0;
    RomTitle[0] = '\0';
}

/* -------------------------------------------------------------------------
 * GB Screen Palette (Green Screen / Monochrome Screen)
 * ---------------------------------------------------------------------- */
// Applied as a pixel-level post-process rather than through the core, since
// mgba/internal/gb/gb.h (needed for direct struct access to the real
// palette table) isn't available in this portlibs package, and the public
// register-write path (core->rawWrite8 on BGP/OBP0/OBP1) can only reassign
// which of the GB's fixed shade slots applies to a pixel value - it can't
// inject arbitrary custom RGB colors, since real GB hardware doesn't
// support that either.
//
// Instead: DMG output is always achromatic (R==G==B, four shades of pure
// gray). Any pixel that ISN'T achromatic is real color content (shouldn't
// happen when gGbDmgMode is true, but checking defensively costs nothing)
// and is left untouched. This sidesteps needing to know mGBA's exact
// internal default gray values, since we detect "is this pixel grayscale"
// generically rather than matching specific hardcoded RGB565 constants.
static void ApplyGBPalette(u16 *buf, int width, int height, int stride)
{
    // RGB565 target colors, lightest to darkest.
    // greenShades is the authentic classic DMG-01 LCD palette
    // (#9BBC0F, #8BAC0F, #306230, #0F380F - the real Game Boy green),
    // converted precisely to RGB565.
    static const uint16_t greenShades[4]  = { 0x9DE1, 0x8D61, 0x3306, 0x09C1 };
    static const uint16_t monoShades[4]   = { 0xFFFF, 0xAD75, 0x52AA, 0x0000 }; // neutral grayscale ramp
    // pocketShades: matches the "PocketGB" palette by retroadamshow (Lospec),
    // sourced from photography of a real Game Boy Pocket unit - #AEA691,
    // #887B6A, #605444, #4E3F2A, converted precisely to RGB565. This is a
    // warmer/taupe-ish ramp rather than neutral gray, reflecting how the
    // Pocket's unlit reflective LCD actually looks under real lighting
    // (see https://lospec.com/palette-list/pocketgb).
    static const uint16_t pocketShades[4] = { 0xAD32, 0x8BCD, 0x62A8, 0x49E5 };
    // lightShades: the Game Boy Light (MGB-101, Japan-only) with its front
    // light on, giving a cool cyan/teal backlit tint. Matches the "gbli"
    // (Game Boy Light) entry by HerrZatacke from the gb-palettes reference
    // collection on GitHub - #1DDECE, #19C7B3, #16A596, #0B7A6D, converted
    // precisely to RGB565.
    static const uint16_t lightShades[4]  = { 0x1EF9, 0x1E36, 0x1532, 0x0BCD };
    const uint16_t *shades = (GCSettings.BasicPalette == 3) ? lightShades
                            : (GCSettings.BasicPalette == 2) ? pocketShades
                            : (GCSettings.BasicPalette == 1) ? monoShades
                            : greenShades;

    for (int y = 0; y < height; y++) {
        u16 *row = buf + (size_t)y * stride;
        for (int x = 0; x < width; x++) {
            u16 px = row[x];
            int r5 = (px >> 11) & 0x1F;
            int g6 = (px >> 5)  & 0x3F;
            int b5 =  px        & 0x1F;
            int r8 = (r5 << 3) | (r5 >> 2);
            int g8 = (g6 << 2) | (g6 >> 4);
            int b8 = (b5 << 3) | (b5 >> 2);

            int maxc = r8 > g8 ? (r8 > b8 ? r8 : b8) : (g8 > b8 ? g8 : b8);
            int minc = r8 < g8 ? (r8 < b8 ? r8 : b8) : (g8 < b8 ? g8 : b8);
            if (maxc - minc > 12) continue; // not grayscale - leave real color content alone

            int brightness = (r8 + g8 + b8) / 3;
            int shade = (brightness >= 192) ? 0 : (brightness >= 128) ? 1 : (brightness >= 64) ? 2 : 3;
            row[x] = shades[shade];
        }
    }
}

/* -------------------------------------------------------------------------
 * GBA/GBC Color Emulation
 *
 * (Renamed from "Color Correction" -> "Color Emulation" throughout the UI
 * and settings, since these presets now also let a GBA game render through
 * the GBC/Gambatte matrix and vice versa - "correction" implied a single
 * canonical fix per system, "emulation" better describes picking which
 * real screen's color response to emulate.)
 *
 * NOTE: an earlier version of this file cited "Extrems' emgba
 * (source/gx_preview.c)" as the source of these matrices. That file does
 * not exist in the real extremscorner/emgba repo - the only color/video
 * pipeline files there are source/gx.c, gx_packed.c and gx_planar.c, none
 * of which contain a 3x3 color-primaries matrix (gx.c only does per-
 * channel gamma/brightness/contrast via a TLUT; gx_packed.c/gx_planar.c
 * are frame-blend and scaling filters). That citation - and the integer
 * coefficients that went with it - was wrong and has been replaced.
 *
 * These coefficients instead come from the real, well-known "Color
 * Mangler" shader (gba-color.glsl / gbc-color.glsl) written by hunterk and
 * refined by Pokefan531, public domain, shipped in libretro/glsl-shaders
 * under handheld/shaders/color/ (see also the libretro forum thread "Real
 * GBA and DS-Phat colors"). This is the same color-correction lineage
 * mGBA's own PC build, RetroArch's Gambatte/mGBA cores, and emgba's named
 * --matrix=gba/gbc presets all ultimately derive from. GBA and GBC use the
 * identical 3x3 matrix in the reference shaders - they only differ in a
 * pre-matrix exposure tweak, which this port doesn't use (see below).
 *
 * DELIBERATE DEVIATION FROM THE REFERENCE SHADER: the reference shader
 * applies this matrix in linear light (decode with pow(c, 2.2), matrix,
 * re-encode with pow(c, 1/2.2)). That blows up on dark, saturated colors
 * because gamma-decoding compresses small values a lot more than large
 * ones, so a channel that's small-but-nonzero pre-decode can become
 * negligible relative to a larger channel post-decode - and this matrix
 * has a negative coefficient (row 0's blue term, -0.06). Concretely, a
 * dark blue-leaning shadow/overlay tint like (10,10,40):
 *   - in linear light: red's contribution gets swamped by -0.06*blue and
 *     clips to 0, green gets pushed up by blue bleeding in -> (0,21,34).
 *     That's a visible hue-shift/crush, not just darkening - this is what
 *     was showing up as broken shadows/fades/water/dialogue-box
 *     translucency with color correction on.
 *   - applied directly to the stored (nonlinear) values, no gamma round-
 *     trip: (8,16,32) - a mild, sane shift, nothing clips.
 * So this port applies the matrix directly to the stored RGB565 values
 * instead. It's simpler, cheaper (no powf() per LUT entry), and doesn't
 * clip dark saturated colors. Neutral gray is still exact (each row sums
 * to 1.0) regardless of which space the matrix runs in, since that
 * property is just linear algebra and doesn't depend on gamma.
 * ---------------------------------------------------------------------- */
struct ColorMatrix {
    float row[3][3];   // row-major; row i, col j = contribution of input channel j to output channel i. Each row sums to 1.0 so neutral gray is preserved.
};

static const struct ColorMatrix kMatrixGBA = {{
    { 0.820f, 0.240f, -0.060f },
    { 0.125f, 0.665f,  0.210f },
    { 0.195f, 0.075f,  0.730f },
}};

static const struct ColorMatrix kMatrixGambatte = {{  // GBC - same matrix as GBA, see block comment above
    { 0.820f, 0.240f, -0.060f },
    { 0.125f, 0.665f,  0.210f },
    { 0.195f, 0.075f,  0.730f },
}};

// Sourced from the real "sp101-color.slang" shader (Color Mangler family,
// hunterk/Pokefan531, public domain) - the same lineage as kMatrixGBA/
// kMatrixGambatte above, this time specifically modeling the backlit GBA
// SP (AGS-101) panel rather than the front-lit AGS-001/original GBA one.
// The shader exposes three selectable target gamuts (sRGB/DCI/Rec2020,
// via its "Color Profile" parameter); this port uses the sRGB matrix
// (mode 1, and the shader's own default) since that's the convention the
// existing GBA/GBC matrices above follow and this pipeline doesn't expose
// a gamut picker. Raw shader values (SP1_sRGB, R/G/B rows only - the
// shader's 4th "alpha/lum" row is a separate pre-matrix brightness
// multiplier the shader applies in linear light before the matrix; like
// kMatrixGBA/kMatrixGambatte above, this port skips the gamma round-trip
// entirely and applies the matrix directly to stored RGB565, so that
// term doesn't carry over - see the DELIBERATE DEVIATION note above):
//   R: 0.96, 0.0325, 0.001   (sum 0.9935)
//   G: 0.11, 0.89, -0.03     (sum 0.97)
//   B: -0.07, 0.0775, 1.029  (sum 1.0365)
// normalized here so each row sums to 1.0, same as every other matrix in
// this file.
static const struct ColorMatrix kMatrixAGS101 = {{
    { 0.966f, 0.033f,  0.001f },
    { 0.113f, 0.918f, -0.031f },
    { -0.068f, 0.075f, 0.993f },
}};

// Sourced from libretro's vba-color.glsl (Pokefan531/hunterk, public
// domain): raw R/G/B rows {0.73,0.085,0.085}/{0.27,0.675,0.24}/
// {0.0,0.24,0.675}, normalized here so each row sums to 1.0 (see block
// comment above for why - matches this file's existing kMatrixGBA/
// kMatrixGambatte convention rather than the shader's own gamma-round-trip
// approach, which this port doesn't replicate).
static const struct ColorMatrix kMatrixVBA = {{
    { 0.811f, 0.094f, 0.094f },
    { 0.228f, 0.570f, 0.203f },
    { 0.000f, 0.262f, 0.738f },
}};

// Sourced from libretro's psp-color.glsl (Pokefan531/hunterk, public
// domain): raw rows {0.98,0.04,0.01}/{0.20,0.795,0.01}/{-0.18,0.165,0.98},
// normalized to row-sum 1.0 as above.
static const struct ColorMatrix kMatrixPSP = {{
    { 0.951f, 0.039f,  0.010f },
    { 0.199f, 0.791f,  0.010f },
    {-0.187f, 0.171f,  1.016f },
}};

// Sourced from libretro's lcd1x_nds.glsl (hunterk/Pokefan531, public
// domain): raw CC_R/RG/RB etc rows {0.87,0.10,0.10}/{0.255,0.645,0.17}/
// {-0.125,0.255,0.73}. The shader also applies a separate CC_LUM=0.89
// pre-scale, but that's a uniform per-row scalar and cancels out entirely
// once each row is normalized to sum to 1.0, so it's omitted here.
static const struct ColorMatrix kMatrixNDS = {{
    { 0.813f, 0.093f,  0.093f },
    { 0.238f, 0.603f,  0.159f },
    {-0.145f, 0.297f,  0.849f },
}};

// Trivial passthrough - useful as a wiring sanity check (output should be
// pixel-identical to no correction at all) and as an explicit "off with a
// matrix selected" option if this becomes a picker rather than on/off.
static const struct ColorMatrix kMatrixIdentity = {{
    { 1.0f, 0.0f, 0.0f },
    { 0.0f, 1.0f, 0.0f },
    { 0.0f, 0.0f, 1.0f },
}};

static u16 gColorLUT[65536];
static bool gColorLUTBuilt = false;
static const struct ColorMatrix *gColorLUTMatrix = NULL;

static void BuildColorLUT(const struct ColorMatrix *m)
{
    for (int px = 0; px < 65536; px++) {
        int r5 = (px >> 11) & 0x1F, g6 = (px >> 5) & 0x3F, b5 = px & 0x1F;
        int r8 = (r5 << 3) | (r5 >> 2);
        int g8 = (g6 << 2) | (g6 >> 4);
        int b8 = (b5 << 3) | (b5 >> 2);

        int r = (int)(m->row[0][0]*r8 + m->row[0][1]*g8 + m->row[0][2]*b8 + 0.5f);
        int g = (int)(m->row[1][0]*r8 + m->row[1][1]*g8 + m->row[1][2]*b8 + 0.5f);
        int b = (int)(m->row[2][0]*r8 + m->row[2][1]*g8 + m->row[2][2]*b8 + 0.5f);

        r = (r < 0) ? 0 : (r > 255) ? 255 : r;
        g = (g < 0) ? 0 : (g > 255) ? 255 : g;
        b = (b < 0) ? 0 : (b > 255) ? 255 : b;

        gColorLUT[px] = (u16)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    }
    gColorLUTBuilt = true;
}

static void ApplyColorCorrection(const u16 *src, u16 *dst, int width, int height, int stride)
{
    for (int y = 0; y < height; y++) {
        const u16 *srow = src + (size_t)y * stride;
        u16       *drow = dst + (size_t)y * stride;
        for (int x = 0; x < width; x++)
            drow[x] = gColorLUT[srow[x]];
    }
}

/* Averages `cur` and `prev` per-channel (RGB565) into `dst`, a straight
 * 50/50 mix matching mGBA's "Simple" interframe blending mode. Operates on
 * R/G/B independently by masking/adding/halving each channel's bits
 * separately (rather than adding the raw u16 words, which would carry
 * across channel boundaries and corrupt adjacent channels). */
static void ApplyInterframeBlending(const u16 *cur, const u16 *prev, u16 *dst, int width, int height, int stride)
{
    for (int y = 0; y < height; y++) {
        const u16 *crow = cur  + (size_t)y * stride;
        const u16 *prow = prev + (size_t)y * stride;
        u16       *drow = dst  + (size_t)y * stride;
        for (int x = 0; x < width; x++) {
            u16 c = crow[x], p = prow[x];
            u16 r = (u16)((((c >> 11) & 0x1F) + ((p >> 11) & 0x1F)) >> 1);
            u16 g = (u16)((((c >> 5)  & 0x3F) + ((p >> 5)  & 0x3F)) >> 1);
            u16 b = (u16)(((c & 0x1F) + (p & 0x1F)) >> 1);
            drow[x] = (r << 11) | (g << 5) | b;
        }
    }
}

/* -------------------------------------------------------------------------
 * Per-frame emulation
 * ---------------------------------------------------------------------- */
static void mgba_emuMain(int count)
{
    (void)count;
    if (!core || !coreRunning) return;

    extern void UpdatePads();
    extern u32  GetJoy(int);
    UpdatePads();
    u16 keys = (u16)GetJoy(0);

    core->setKeys(core, keys);

    // Fast-forward: GCSettings.FastForwardSpeed (0=Off/1x .. 3=4x) is the
    // persistent, menu-toggled speed (Game Settings - Emulation - Fast
    // Forward Speed). On top of that, FastForwardHeld() (input.cpp) fast-
    // forwards at at least 2x for as long as any controller's default
    // fast-forward input is held - GameCube pad's C-Stick Right, or Minus
    // on Wiimote/Classic Controller/Wii U Pro Controller/Wii U GamePad
    // (see that function's own comment for exactly how each maps and why
    // it's safe to check all of them with two field reads). Whichever of
    // the menu setting or the held button implies the higher speed wins;
    // they don't stack.
    extern bool FastForwardHeld();
    int heldFF = FastForwardHeld() ? 1 : 0; // 1 = 2x

    int effectiveFF = GCSettings.FastForwardSpeed;
    if (heldFF > effectiveFF)
        effectiveFF = heldFF;

    if (effectiveFF >= 1 && effectiveFF <= 3) {
        for (int i = 0; i < effectiveFF; i++) {
            core->runFrame(core);
            PushAudio();
        }
    }

    core->runFrame(core);
    PushAudio();

    // Index 0 = off (matches old on/off meaning exactly, so existing saved
    // settings.xml values of 0/1 keep working unchanged). Indices 2+ are
    // reachable via the "GBA/GBC Color Emulation" menu option (menu.cpp).
    // Both lists now include every profile - GBA games can render through
    // the GBC/Gambatte matrix and GBC games through the GBA one, per
    // request - plus the AGS-101 approximation (see kMatrixAGS101 above).
    static const struct ColorMatrix *kGBAMatrices[] = {
        NULL, &kMatrixGBA, &kMatrixGambatte, &kMatrixAGS101, &kMatrixVBA, &kMatrixPSP, &kMatrixNDS
    };
    static const struct ColorMatrix *kGBMatrices[] = {
        NULL, &kMatrixGambatte, &kMatrixGBA, &kMatrixAGS101, &kMatrixVBA, &kMatrixPSP, &kMatrixNDS
    };
    #define MATRIX_COUNT(arr) (int)(sizeof(arr) / sizeof(arr[0]))

    // Frameskip: GCSettings.Frameskip (0 = Off, N = skip N out of every
    // N+1 frames) is unrelated to FastForwardSpeed above - it doesn't touch
    // how many times core->runFrame() runs each real frame, which stays
    // at normal 1x cadence here. It only skips the video-side work below
    // (color correction, interframe blending, GX_Render's texture upload
    // and draw) on the skipped frames, trading displayed smoothness for
    // freed-up CPU/GX time each real frame - the same tradeoff EmGBA's
    // Frameskip setting makes, and why it can visibly help audio/gameplay
    // smoothness on content that's borderline for the Wii's CPU even at
    // normal speed.
    static int frameskipCounter = 0;
    bool skipVideoThisFrame = false;
    if (GCSettings.Frameskip > 0) {
        frameskipCounter++;
        if (frameskipCounter <= GCSettings.Frameskip) {
            skipVideoThisFrame = true;
        } else {
            frameskipCounter = 0;
        }
    } else {
        frameskipCounter = 0;
    }

    if (skipVideoThisFrame) {
        return;
    }

    const u16 *renderBuf = videoBuf; // default: no color emulation, render mGBA's buffer directly

    if (gGbDmgMode) {
        ApplyGBPalette(videoBuf, gbCoreW, gbCoreH, gGbVideoStride);
    } else {
        const struct ColorMatrix *wantMatrix = NULL;
        if (cartridgeType == CARTRIDGE_GBA) {
            int idx = GCSettings.GBAColorEmulation;
            if (idx > 0 && idx < MATRIX_COUNT(kGBAMatrices))
                wantMatrix = kGBAMatrices[idx];
        } else if (cartridgeType == CARTRIDGE_GB) {
            int idx = GCSettings.GBCColorEmulation;
            if (idx > 0 && idx < MATRIX_COUNT(kGBMatrices))
                wantMatrix = kGBMatrices[idx];
        }

        if (wantMatrix) {
            if (!gColorLUTBuilt || gColorLUTMatrix != wantMatrix) {
                BuildColorLUT(wantMatrix);
                gColorLUTMatrix = wantMatrix;
            }
            // Always read fresh from videoBuf (mGBA's own, untouched-by-us
            // buffer) and write into correctedVideoBuf - see that buffer's
            // declaration comment for why this can't be done in place.
            ApplyColorCorrection(videoBuf, correctedVideoBuf, gbCoreW, gbCoreH, gGbVideoStride);
            renderBuf = correctedVideoBuf;
        }
    }

    // Interframe blending ("Simple" mode from mGBA's own frontends - see
    // prevVideoBuf's declaration comment above): average this frame 50/50
    // with the previous one before displaying, so games that rely on
    // alternating-frame dithering to fake transparency (F-Zero's map
    // overlay, Chikyuu Kaihou Gun ZAS, etc.) render as intended instead of
    // visibly flickering between the two source frames. Blending happens
    // AFTER color emulation/GB palette above so the two stay consistent
    // with each other, and reads/writes go through a dedicated blend
    // buffer for the same "don't compound frame-over-frame" reason
    // correctedVideoBuf is kept separate from videoBuf.
    static u16 *blendedVideoBuf = NULL;
    if (GCSettings.InterframeBlending) {
        if (!blendedVideoBuf)
            // Same bound as MAX_BUF_W/MAX_BUF_H used at buffer-allocation
            // time (256+2 x 224 - the largest frame this port ever
            // produces, SGB-bordered GB/GBC) - that pair is a local const
            // inside the init function, not visible here, so it's
            // duplicated as a literal rather than shared.
            blendedVideoBuf = (u16 *)memalign(32, (256 + 2) * 224 * sizeof(u16));

        if (blendedVideoBuf) {
            ApplyInterframeBlending(renderBuf, prevVideoBuf, blendedVideoBuf, gbCoreW, gbCoreH, gGbVideoStride);
            // prevVideoBuf keeps this frame's PRE-blend content (renderBuf),
            // not the blended output, so the blend is always "current frame
            // + last genuine frame", never compounding across more than 2
            // frames.
            for (int y = 0; y < gbCoreH; y++)
                memcpy(prevVideoBuf + (size_t)y * gGbVideoStride,
                       renderBuf    + (size_t)y * gGbVideoStride,
                       gbCoreW * sizeof(u16));
            renderBuf = blendedVideoBuf;
        }
    }

    if (!videoInited) {
        GX_Render_Init(gbCoreW, gbCoreH);
        videoInited = true;
    }
    GX_Render(gbCoreW, gbCoreH, (u8 *)renderBuf);
    // Audio is pumped earlier in this function, right after each
    // core->runFrame() call - before the frameskip early-return above -
    // so it keeps flowing every emulated frame regardless of whether video
    // gets skipped this frame.
}

static void mgba_emuReset()
{
    if (core) core->reset(core);
}

void InitEmulator()
{
    videoInited = false;
    memset(&emulator, 0, sizeof(emulator));
    emulator.emuMain  = mgba_emuMain;
    emulator.emuReset = mgba_emuReset;
    emulator.emuCount = 1;
}

/* -------------------------------------------------------------------------
 * ApplyIPSPatchInPlace
 *
 * Applies an IPS patch directly into buf, with NO second buffer - unlike
 * mgba's own _IPSApplyPatch (patch-ips.c), which requires non-overlapping
 * `in`/`out` buffers (see its `restrict` qualifiers) and therefore always
 * needs a full second ROM-sized buffer to write into. On this build,
 * Arena2 (MEM2) has essentially no room left for that second buffer once
 * romBuffer's own 32MB reservation and everything else mGBA-GX keeps
 * resident there are accounted for - confirmed by "Not enough free MEM2"
 * showing up even for a scratch buffer barely larger than the ROM itself.
 *
 * IPS records only ever rewrite specific byte ranges (or occasionally
 * extend past the end) - there's no structural reason a record can't be
 * written straight into the same buffer it's read from, so long as the
 * capacity check happens before each write. This reimplements just enough
 * of the format (see patch-ips.c upstream for the format this mirrors) to
 * do that, in place, against romBuffer directly.
 *
 * buf/bufCapacity: the buffer to patch in place (romBuffer/romBufferSize).
 * ioSize: in - the ROM's current size; out - the highest offset the patch
 * actually touched (i.e. the real new size, growing only if the patch
 * legitimately extends past the original data - most don't).
 * Returns false on a truncated/corrupt patch file or a record that would
 * write past bufCapacity; buf may be partially modified in that case
 * (same as mgba's own applyPatch on failure).
 * ---------------------------------------------------------------------- */
static bool ApplyIPSPatchInPlace(struct VFile *vf, uint8_t *buf, size_t bufCapacity, size_t *ioSize)
{
    char header[5];
    vf->seek(vf, 0, SEEK_SET);
    if (vf->read(vf, header, 5) != 5 || memcmp(header, "PATCH", 5) != 0)
        return false;

    size_t highestTouched = *ioSize;

    while (true)
    {
        uint8_t offBytes[3];
        if (vf->read(vf, offBytes, 3) != 3)
            return false; // truncated - no EOF marker found

        if (offBytes[0] == 'E' && offBytes[1] == 'O' && offBytes[2] == 'F')
            break;

        uint32_t offset = ((uint32_t)offBytes[0] << 16) | ((uint32_t)offBytes[1] << 8) | offBytes[2];

        uint8_t sizeBytes[2];
        if (vf->read(vf, sizeBytes, 2) != 2)
            return false;
        uint16_t size = ((uint16_t)sizeBytes[0] << 8) | sizeBytes[1];

        if (size == 0)
        {
            // RLE record: 2-byte repeat count + 1 fill byte
            uint8_t rleBytes[2];
            if (vf->read(vf, rleBytes, 2) != 2)
                return false;
            uint16_t rleSize = ((uint16_t)rleBytes[0] << 8) | rleBytes[1];
            uint8_t fillByte;
            if (vf->read(vf, &fillByte, 1) != 1)
                return false;
            if ((size_t)offset + rleSize > bufCapacity)
                return false;
            memset(&buf[offset], fillByte, rleSize);
            if ((size_t)offset + rleSize > highestTouched)
                highestTouched = (size_t)offset + rleSize;
        }
        else
        {
            if ((size_t)offset + size > bufCapacity)
                return false;
            if (vf->read(vf, &buf[offset], size) != size)
                return false;
            if ((size_t)offset + size > highestTouched)
                highestTouched = (size_t)offset + size;
        }
    }

    *ioSize = highestTouched;
    return true;
}

/* -------------------------------------------------------------------------
 * LoadPatchForCurrentROM
 *
 * If a patch file with the same base name as the ROM sits alongside it in
 * the ROM directory (e.g. "Pokemon Red.gb" next to "Pokemon Red.ips"),
 * this applies it directly to our own already-loaded romBuffer, BEFORE the
 * ROM is ever handed to core->loadROM(). Tries .ips first (the common case
 * for GB/GBA romhacks), then .ups/.bps for the same softpatching workflow
 * tools like Lunar IPS/Floating IPS/beat produce. The patch FORMAT itself
 * is auto-detected from the file's own magic header via loadPatch()
 * (mgba-util/patch.h), not the extension - the extension here is only
 * used to find the file. Only the first match is applied.
 *
 * This used to just call core->loadPatch(core, patchVF) after the ROM was
 * already loaded into the core - simpler, but it routes through mGBA's own
 * GBApplyPatch()/GBAApplyPatch(), which unconditionally calls
 * anonymousMemoryMap() (src/util/memory.c) for the patched-output buffer,
 * sized to the format's worst-case ceiling (GBA_SIZE_ROM0, 32MB, since
 * IPS's own outputSize() can't know the real patched size up front).
 * anonymousMemoryMap() on this platform is a plain calloc() - a normal
 * heap allocation competing with every other malloc'd buffer in the app -
 * NOT the same MEM2/Arena2 pool romBuffer itself was deliberately carved
 * out of below. A fresh 32MB calloc() there can fail (crashing on the null
 * return, even after guarding the immediate dereference, since a failed
 * patch apply left downstream code assuming success) in exactly the cases
 * where mGBA-GX's larger memory footprint (custom borders, extra video
 * buffers for interframe blending, etc.) leaves less general-heap room
 * than standalone mGBA needs for the same call.
 *
 * Since we already fully control romBuffer (loaded a few lines up in
 * LoadVBAROM, straight from MEM2/Arena2, before this runs), we can apply
 * the patch ourselves against it directly, borrowing a same-sized scratch
 * buffer from that SAME Arena2 pool just long enough to run the patch
 * (mgba's patch appliers require non-overlapping `in`/`out` buffers - see
 * the `restrict` qualifiers in _IPSApplyPatch - so patching romBuffer onto
 * itself isn't an option), then copying the result back into romBuffer
 * and immediately returning the scratch space to Arena2 rather than
 * holding a second permanent reservation.
 *
 * Must be called after romBuffer is fully populated with the raw ROM
 * bytes and before VFileFromConstMemory()/core->loadROM() wrap it - not
 * after, the way core->loadPatch() required.
 *
 * Returns the ROM size to actually use going forward: the patched size on
 * success, or the original romSize unchanged if there's no patch file, or
 * if parsing/applying it failed for any reason.
 * ---------------------------------------------------------------------- */
static size_t LoadPatchForCurrentROM(size_t romSize)
{
    // Reset first, unconditionally - this must always reflect the ROM
    // that's loading right now, not leftover state from whatever was
    // loaded before (e.g. a patched game followed by an unpatched one).
    PatchApplied = false;
    PatchFilename[0] = '\0';

    static const char *patchExts[] = { ".ips", ".ups", ".bps" };

    char patchPath[MAXPATHLEN + 1];
    for (size_t i = 0; i < sizeof(patchExts) / sizeof(patchExts[0]); i++)
    {
        snprintf(patchPath, sizeof(patchPath), "%s%s%s", browser.dir, ROMFilename, patchExts[i]);

        struct VFile *patchVF = VFileOpen(patchPath, O_RDONLY);
        if (!patchVF)
            continue; // no patch with this extension - not an error, most ROMs don't have one

        bool isIPS = (strcmp(patchExts[i], ".ips") == 0);

        if (isIPS)
        {
            // No second buffer needed at all - see ApplyIPSPatchInPlace's
            // header comment for why this completely sidesteps the Arena2
            // exhaustion that broke the generic loadPatch()/applyPatch()
            // path below for every attempted scratch size, including ones
            // barely larger than the ROM itself.
            size_t newSize = romSize;
            if (ApplyIPSPatchInPlace(patchVF, (uint8_t*)romBuffer, romBufferSize, &newSize))
            {
                printf("[mGBA] Applied patch: %s\n", patchPath);
                PatchApplied = true;
                snprintf(PatchFilename, sizeof(PatchFilename), "%s%s", ROMFilename, patchExts[i]);
                romSize = newSize;
            }
            else
            {
                printf("[mGBA] Failed to apply patch (bad/corrupt file, or it writes past the "
                       "%u byte ROM buffer): %s\n", (unsigned)romBufferSize, patchPath);
            }

            patchVF->close(patchVF);
            return romSize;
        }

        // .ups/.bps: no in-place applier for these yet, so this still
        // goes through mgba's own loadPatch()/applyPatch(), which needs a
        // full second buffer and may hit the same "Not enough free MEM2"
        // limitation .ips just avoided. Flagged here rather than silently
        // reusing the (currently non-functional in practice) scratch-
        // buffer path without comment.
        struct Patch patch;
        if (!loadPatch(patchVF, &patch)) {
            printf("[mGBA] Failed to parse patch (bad/corrupt file?): %s\n", patchPath);
            patchVF->close(patchVF);
            return romSize;
        }

        size_t patchedSize = patch.outputSize(&patch, romSize);
        if (patchedSize > romBufferSize) {
            printf("[mGBA] Patched ROM would be too large (%u bytes, max %u)\n",
                   (unsigned)patchedSize, (unsigned)romBufferSize);
            patchVF->close(patchVF);
            return romSize;
        }

        size_t attemptSizes[2];
        int numAttempts = 0;
        attemptSizes[numAttempts++] = (romSize + 0x10000 <= romBufferSize) ? romSize + 0x10000 : romBufferSize;
        if (patchedSize > attemptSizes[0])
            attemptSizes[numAttempts++] = patchedSize;

        bool ok = false;
        bool arena2TooSmall = false;
        size_t successfulSize = 0;
        for (int attempt = 0; attempt < numAttempts && !ok; attempt++)
        {
            size_t scratchSize = attemptSizes[attempt];

            // Borrow a scratch buffer from the same Arena2 pool romBuffer
            // itself comes from, just for the duration of the patch apply
            // below - see the big comment above LoadPatchForCurrentROM for
            // why this can't just be a calloc()/anonymousMemoryMap() call,
            // and why romBuffer can't be patched onto itself.
            void *arena2LoBefore = SYS_GetArena2Lo();
            void *scratch = arena2LoBefore;
            void *newArena2Lo = (void *)((intptr_t)scratch + scratchSize);
            if ((intptr_t)newArena2Lo > (intptr_t)SYS_GetArena2Hi()) {
                arena2TooSmall = true;
                break; // a bigger attempt won't fit either - no point retrying
            }
            SYS_SetArena2Lo(newArena2Lo);

            ok = patch.applyPatch(&patch, romBuffer, romSize, scratch, scratchSize);
            if (ok) {
                // Only copy back scratchSize bytes - NOT patchedSize (the
                // flat worst-case ceiling) - since scratch itself is only
                // scratchSize bytes of actually-reserved Arena2 space on
                // this attempt. Reading further would walk off the end of
                // what we just borrowed into memory we don't own.
                memcpy(romBuffer, scratch, scratchSize);
                successfulSize = scratchSize;
            }

            // Give the scratch space back immediately - this was only
            // ever a brief loan for the patch apply above, not a second
            // permanent reservation alongside romBuffer's own.
            SYS_SetArena2Lo(arena2LoBefore);
        }

        if (ok) {
            printf("[mGBA] Applied patch: %s\n", patchPath);
            PatchApplied = true;
            snprintf(PatchFilename, sizeof(PatchFilename), "%s%s", ROMFilename, patchExts[i]);
            romSize = successfulSize;
        } else if (arena2TooSmall) {
            printf("[mGBA] Not enough free MEM2 to apply patch\n");
        } else {
            printf("[mGBA] Failed to apply patch (bad/corrupt file?): %s\n", patchPath);
        }

        patchVF->close(patchVF);
        return romSize; // first match wins - don't also look for the other extensions
    }

    return romSize;
}

/* -------------------------------------------------------------------------
 * LoadVBAROM — called by vbagx's BrowserLoadFile
 * ---------------------------------------------------------------------- */
void CheatsLoadForCurrentROM(void); // defined below, in the Cheats section; forward-declared for use in LoadVBAROM()

bool LoadVBAROM()
{
    UnloadCore();

    /* romBuffer from MEM2 Arena2 — allocated once, reused across loads.
     * Bounds-checked against the current Arena2Hi boundary (already pulled
     * down by InitMem2Manager()'s LWP heap reservation - see mem2.cpp) so
     * a future change to either size can't silently reintroduce the
     * heap-corruption bug this was chasing: these two allocators know
     * nothing about each other and share the same physical Arena2 region. */
    if (!romBuffer) {
        romBufferSize = 0x2000000; /* 32MB, matches GBA_SIZE_ROM0 - must match mem2.cpp's ROM_BUFFER_RESERVE */
        romBuffer = (uint32_t *)SYS_GetArena2Lo();
        void *newLo = (void*)((intptr_t)romBuffer + romBufferSize);
        if ((intptr_t)newLo > (intptr_t)SYS_GetArena2Hi()) {
            printf("[LoadVBAROM] FATAL: romBuffer (32MB) would overlap the MEM2 heap "
                   "(Arena2Lo+32MB=%p > Arena2Hi=%p) - refusing to load\n",
                   newLo, SYS_GetArena2Hi());
            return false;
        }
        SYS_SetArena2Lo(newLo);
    }

    char romPath[MAXPATHLEN + 1];
    snprintf(romPath, sizeof(romPath), "%s%s",
             browser.dir, browserList[browser.selIndex].filename);

    FILE *fp = fopen(romPath, "rb");
    if (!fp) { printf("[mGBA] fopen failed: %s\n", romPath); return false; }
    fseek(fp, 0, SEEK_END);
    size_t romSize = (size_t)ftell(fp);
    rewind(fp);
    if (romSize > romBufferSize) {
        fclose(fp);
        printf("[mGBA] ROM too large: %u bytes\n", (unsigned)romSize);
        return false;
    }
    fread(romBuffer, 1, romSize, fp);
    fclose(fp);

    // Must happen here - before VFileFromConstMemory()/core->loadROM()
    // below - not after, the way this used to work via core->loadPatch().
    // See LoadPatchForCurrentROM()'s own header comment for why.
    romSize = LoadPatchForCurrentROM(romSize);

    /* VFileFromConstMemory: mGBA reads it but does NOT take ownership /
     * does NOT try to free() it — critical since romBuffer is MEM2, not
     * heap memory. Using VFileFromMemory here corrupts the heap. */
    struct VFile *romVF = VFileFromConstMemory(romBuffer, romSize);
    if (!romVF) return false;

    core = mCoreFindVF(romVF);
    if (!core) {
        printf("[mGBA] Unsupported ROM: %s\n", romPath);
        romVF->close(romVF);
        return false;
    }

    mCoreInitConfig(core, NULL);

    /* core->platform() just reports which core implementation was matched
     * (GB vs GBA) and is safe to call before core->init(). We need
     * cartridgeType (and the ROM-header SGB check below) resolved BEFORE
     * init, because mGBA's own docs mark the Game Boy model option as
     * "requires restart" - meaning it's only read and cached during
     * core->init(), not afterward. Setting config after init (as this code
     * previously did) had no effect, which is why the hardware/palette
     * settings never worked. */
    switch (core->platform(core)) {
        case mPLATFORM_GBA: cartridgeType = CARTRIDGE_GBA; break;
        case mPLATFORM_GB:  cartridgeType = CARTRIDGE_GB;  break;
        default:            cartridgeType = CARTRIDGE_NONE; break;
    }

    gGbNativeW = SCREEN_W;
    gGbNativeH = SCREEN_H;

    /* GCSettings.GBHardware ("Hardware (GB/GBC)" menu setting) selects which
     * hardware model mGBA's GB core should identify as: 0=Auto, 1=Game Boy
     * Color, 2=Super Game Boy, 3=Super Game Boy 2, 4=Game Boy, 5=Game Boy
     * Advance. sgb.model is mGBA's real hardware-model selector (the "sgb."
     * prefix is just a naming artifact from where the option lives in
     * mGBA's own settings UI).
     *
     * The SGB border itself only ever makes sense when the resolved model is
     * SGB or SGB2, and (for Auto) only when the ROM actually declares SGB
     * support in its header (byte 0x146 == 0x03 and byte 0x14B == 0x33 - the
     * standard SGB-flag check; real SGB hardware doesn't add a border to
     * plain GB/GBC games it doesn't recognize as SGB-enhanced). */
    bool romDeclaresSGB = (cartridgeType == CARTRIDGE_GB && romSize > 0x14B &&
                            ((u8*)romBuffer)[0x146] == 0x03 &&
                            ((u8*)romBuffer)[0x14B] == 0x33);

    /* ROM's actual CGB support is a fixed hardware fact for this cart,
     * used below for GBHardware Auto mode's model selection. */
    bool romSupportsCGB = (cartridgeType == CARTRIDGE_GB && romSize > 0x143 &&
                            (((u8*)romBuffer)[0x143] & 0x80) != 0);

    const char *sgbModelStr = NULL;
    bool wantSGBHardware = false;
    if (cartridgeType == CARTRIDGE_GB) {
        switch (GCSettings.GBHardware) {
            case 1: sgbModelStr = "CGB"; break;                              // Game Boy Color
            case 2: sgbModelStr = "SGB";  wantSGBHardware = true; break;     // Super Game Boy
            case 3: sgbModelStr = "SGB2"; wantSGBHardware = true; break;     // Super Game Boy 2
            case 4: sgbModelStr = "DMG"; break;                              // Game Boy
            case 5: sgbModelStr = "AGB"; break;                              // Game Boy Advance (GBC compat)
            case 0: default:                                                // Auto
                if (romDeclaresSGB && GCSettings.SGBBorder == 1) {
                    sgbModelStr = "SGB2";
                    wantSGBHardware = true;
                } else if (romSupportsCGB) {
                    sgbModelStr = "CGB";
                    wantSGBHardware = false;
                } else {
                    sgbModelStr = "DMG";
                    wantSGBHardware = false;
                }
                break;
        }
    }

    if (sgbModelStr) {
        mCoreConfigSetValue(&core->config, "sgb.model", sgbModelStr);
    }

    gGbDmgMode = (cartridgeType == CARTRIDGE_GB && sgbModelStr && strcmp(sgbModelStr, "DMG") == 0);

    bool enableBorder = wantSGBHardware && GCSettings.SGBBorder == 1;
    mCoreConfigSetIntValue(&core->config, "sgb.borders", enableBorder ? 1 : 0);
    mCoreConfigSetIntValue(&core->config, "sgb.borderCrop", 1); // crop non-SGB games to 160x144

    /* GB Screen Palette (GCSettings.BasicPalette: 0 = Green Screen, 1 =
     * Monochrome Screen, 2 = Game Boy Pocket, 3 = Game Boy Light). mGBA's DMG
     * background palette is set via the gb.pal0..gb.pal3 config keys
     * (confirmed from an actual mGBA config dump) - four 24-bit RGB values,
     * brightest to darkest. This only affects the DMG rendering path;
     * CGB/SGB games using their own real color data are unaffected.
     *
     * IMPORTANT: this is intentionally always set to neutral grayscale here,
     * NOT to the selected palette. ApplyGBPalette() (see above) re-tints
     * achromatic output to GCSettings.BasicPalette live, every frame, which
     * is what lets the palette be changed on the fly without reloading the
     * ROM. If this were instead baked in as the selected tint at load time,
     * the core's output pixels would no longer be neutral gray, so
     * ApplyGBPalette's grayscale detection would treat them as "real color
     * content" and skip re-tinting them - meaning changing the palette
     * setting mid-game would silently do nothing until the next ROM load. */
    if (cartridgeType == CARTRIDGE_GB) {
        static const uint32_t neutralPalette[4] = { 0xFFFFFF, 0xAAAAAA, 0x555555, 0x000000 };
        char key[16];
        for (int i = 0; i < 4; i++) {
            snprintf(key, sizeof(key), "gb.pal%d", i);
            mCoreConfigSetUIntValue(&core->config, key, neutralPalette[i]);
        }
    }

    core->init(core);

    /* Default audio volume - see mCoreLoadConfig()'s own comment just below
     * for why core->opts (which the "volume" key feeds into) needs an
     * explicit default before that call, not just an override table.
     * core->opts.volume is zeroed by _GBACoreInit()'s initial memset and
     * NEVER GETS SET to anything else unless a "volume" key exists in
     * config - it's not implicitly "full volume by default" the way you'd
     * expect. Every real mGBA frontend (SDL, Qt, the official Wii port)
     * sets this explicitly for exactly that reason. Without it, the GBA/GB
     * core mixes every sample against a volume of 0 - real audio data
     * flows through the entire buffer/resampler/DMA pipeline exactly like
     * it should (correct sample counts, correct timing), it's just
     * silent, since the actual PCM values are scaled to zero before they
     * ever leave the core. 0x100 matches mGBA's own GBA_AUDIO_VOLUME_MAX
     * (src/gba/audio.c) - "1.0x", not a boosted or attenuated value. Must
     * run before mCoreLoadConfig() so that call actually picks it up. */
    int defaultVolume = 0x100;
    // TEST: GB's 0xC0 anti-clipping attenuation temporarily removed to
    // isolate whether it's the source of the flat, broadband ~-70dB noise
    // floor heard in GB/GBC audio (see audio.cpp for the spectral
    // analysis). If that floor disappears with defaultVolume=0x100 for
    // GB too, the 0xC0 multiply (applied with unknown internal precision
    // inside the core's mixer) is confirmed as the cause, and a
    // non-lossy way to get clipping headroom - if still needed - should
    // be found instead of reintroducing a blind attenuation here.
    mCoreConfigSetDefaultIntValue(&core->config, "volume", defaultVolume);

    /* This is what actually wires up mGBA's built-in per-game override
     * table (src/gba/overrides.c) - things like forcing Pokemon Ruby to
     * its real Flash1M (128KB) save type instead of leaving detection to
     * the GBA core's own runtime heuristic (which infers save type lazily
     * from which memory region the game's first save-related access
     * touches, and can guess wrong). core->loadConfig() is what populates
     * the override-lookup table (GBACore's `overrides` field via
     * mCoreConfigGetOverridesConst()) that core->reset() consults via
     * GBAOverrideApplyDefaults() every time the core resets - without
     * ever calling this, that table stays NULL and every game runs on
     * pure heuristic auto-detection with no safety net.
     *
     * A misdetected save type doesn't just fail to load existing saves -
     * the core actively RESIZES the save file to match whatever type it
     * (mis)detects once the game starts running, which is how a real
     * 128KB Flash save ends up silently truncated down to 8KB (EEPROM's
     * size) the moment gameplay starts. This is real, destructive data
     * loss, not just a "reads as fresh" cosmetic issue - it's the actual
     * root cause of saves silently not carrying over.
     *
     * Must be called after core->init() (config loading touches
     * core->board's config-dependent fields) and before core->reset()
     * (GBAOverrideApplyDefaults()/_GBCoreLoadConfig's GB equivalent both
     * run there). mCoreLoadConfig() (vs core->loadConfig() directly) also
     * pulls in any user-editable config.ini overrides on top of the
     * built-in table, which is the standard way every other mGBA-based
     * frontend does this. */
    ForceKnownSaveTypeOverrides(core, (const u8 *)romBuffer, romSize);
    mCoreLoadConfig(core);

    // 16384, up from 4096 - see PushAudio()'s comment. If this fills up
    // mid-frame, GBAudioSample() (src/gb/audio.c) calls GBInterrupt(),
    // which force-exits core->runFrame() before the frame's cycles are
    // actually done - the frontend is expected to drain audio and call
    // runFrame() again, which mgba_emuMain() below does not do. Making
    // this buffer big enough that a single frame's audio production can't
    // fill it is the cheap fix that doesn't require restructuring the
    // frame loop to call runFrame() in a drain-and-retry cycle like the
    // reference frontends do.
    core->setAudioBufferSize(core, 16384);

    /* Wii Remote tilt control (see gTiltSource above) - only meaningful for
     * GB/GBC carts, and only some of those actually have a tilt sensor to
     * read it (mGBA's own per-ROM mapper detection handles that; carts
     * without one simply never call readTiltX/Y, so this is harmless to
     * register unconditionally for every GB/GBC game). */
    /* Wii Remote tilt control (see gTiltSource above) - meaningful for both
     * GB/GBC MBC7 carts (Kirby Tilt 'n' Tumble, Koro Koro Puzzle Happy
     * Panechu - 2-axis accelerometer, read via readTiltX/readTiltY) AND
     * GBA carts with a built-in motion sensor (WarioWare Twisted - gyro,
     * read via readGyroZ; Yoshi Topsy-Turvy/Universal Gravitation - 2-axis
     * accelerometer, same as MBC7). WarioWare Twisted and Yoshi Topsy-Turvy
     * are GBA games, not GB/GBC - registering this only for CARTRIDGE_GB
     * meant the peripheral was never hooked up for them at all. mGBA's own
     * GPIO/MBC7 code null-checks core->rotationSource before using it (see
     * GBAHardwareInitGyro in src/gba/cart/gpio.c), so registering
     * unconditionally for every GB/GBC/GBA game is harmless for carts
     * without a sensor to read. */
    if ((cartridgeType == CARTRIDGE_GB || cartridgeType == CARTRIDGE_GBA) && GCSettings.MotionTilt) {
        InitTiltSource();
        core->setPeripheral(core, mPERIPH_ROTATION, &gTiltSource);

        // Request WiiMotion Plus fusion (see EnableWiimoteMotionPlus's own
        // comment, input.cpp) so TiltReadGyroZ() above gets real gyro-
        // accurate orient.yaw instead of wiiuse's inaccurate non-Plus
        // fallback - this is what actually fixes WarioWare Twisted's
        // twist detection; readTiltX/readTiltY (MBC7/Yoshi accelerometer
        // games) don't need this and are unaffected either way.
        EnableWiimoteMotionPlus(true);
    }

    /* Cart rumble (see gRumble above) - Pokemon Pinball (GB) and Pokemon
     * Pinball: Ruby & Sapphire (GBA) are the notable carts with a built-in
     * rumble motor. Registered unconditionally for GB/GBA carts (not
     * gated on GCSettings.MotionTilt, which is specifically the Wii Remote
     * tilt-control setting for motion-sensor games, not this) - carts
     * without a rumble motor simply never call setRumble, and
     * systemCartridgeRumble()/updateRumble() already respect the separate
     * GCSettings.Rumble on/off setting themselves. */
    if (cartridgeType == CARTRIDGE_GB || cartridgeType == CARTRIDGE_GBA) {
        InitRumbleSource();
        core->setPeripheral(core, mPERIPH_RUMBLE, &gRumble);
    }

    /* Solar sensor (see gSolarSource above) - Boktai series only exists on
     * GBA, so this is gated to CARTRIDGE_GBA rather than registered for
     * GB/GBC too. */
    if (cartridgeType == CARTRIDGE_GBA) {
        InitSolarSource();
        core->setPeripheral(core, mPERIPH_GBA_LUMINANCE, &gSolarSource);
    }

    // Push all config values into the now-initialized core.
    // reloadConfigOption() is required after init() — init() sets up internal
    // state but only reads config values that have been explicitly reloaded.
    if (sgbModelStr) {
        core->reloadConfigOption(core, "sgb.model",      &core->config);
        core->reloadConfigOption(core, "sgb.borders",    &core->config);
        core->reloadConfigOption(core, "sgb.borderCrop", &core->config);
    }
    if (cartridgeType == CARTRIDGE_GB) {
        core->reloadConfigOption(core, "gb.pal0", &core->config);
        core->reloadConfigOption(core, "gb.pal1", &core->config);
        core->reloadConfigOption(core, "gb.pal2", &core->config);
        core->reloadConfigOption(core, "gb.pal3", &core->config);
    }

    // Allocate video buffer at the maximum possible size (256+2 x 224) so
    // we never need to reallocate. The true output dimensions are determined
    // after loadROM()+reset() once the core has processed the ROM header.
    bool sgbBordered = enableBorder;
    static const int MAX_BUF_W = 256 + 2;
    static const int MAX_BUF_H = 224;

    videoBuf = (u16 *)memalign(32, MAX_BUF_W * MAX_BUF_H * sizeof(u16));
    if (!videoBuf) { core->deinit(core); core = NULL; return false; }
    memset(videoBuf, 0, MAX_BUF_W * MAX_BUF_H * sizeof(u16));
    core->setVideoBuffer(core, (mColor*)videoBuf, MAX_BUF_W);

    // Same size as videoBuf, never touched by the core - see the
    // correctedVideoBuf declaration comment for why this exists.
    correctedVideoBuf = (u16 *)memalign(32, MAX_BUF_W * MAX_BUF_H * sizeof(u16));
    if (!correctedVideoBuf) { free(videoBuf); videoBuf = NULL; core->deinit(core); core = NULL; return false; }
    memset(correctedVideoBuf, 0, MAX_BUF_W * MAX_BUF_H * sizeof(u16));

    prevVideoBuf = (u16 *)memalign(32, MAX_BUF_W * MAX_BUF_H * sizeof(u16));
    if (!prevVideoBuf) { free(videoBuf); free(correctedVideoBuf); videoBuf = NULL; correctedVideoBuf = NULL; core->deinit(core); core = NULL; return false; }
    memset(prevVideoBuf, 0, MAX_BUF_W * MAX_BUF_H * sizeof(u16));

    if (!core->loadROM(core, romVF)) {
        printf("[mGBA] Failed to load ROM\n");
        UnloadCore();
        return false;
    }

    // Patch application now happens earlier, directly against romBuffer,
    // before VFileFromConstMemory()/core->loadROM() above - see
    // LoadPatchForCurrentROM()'s header comment for why. Nothing left to
    // do here.

    if (cartridgeType == CARTRIDGE_GBA) {
        struct GBA *gba = (struct GBA *)core->board;
        memcpy(RomTitle, (u8 *)gba->memory.rom + 0xA0, 12);
        RomTitle[12] = '\0';
        u8 *code = (u8 *)gba->memory.rom + 0xAC;
        RomIdCode = ((u32)code[0] << 24) | ((u32)code[1] << 16) |
                    ((u32)code[2] <<  8) |  (u32)code[3];
    }

    /* Auto-load SRAM: GCSettings.AutoLoad (0=Off, 1=SRAM, 2=State) already
     * existed as a menu setting, and LoadBatteryOrStateAuto() already
     * existed below, but nothing ever actually called it when a ROM boots
     * - only the manual "Load Game" menu option triggered a load. Auto-save
     * on exit was already working fine (ExitApp() calls
     * SaveBatteryOrStateAuto), which is why saves were being written but
     * never restored on the next launch.
     *
     * This MUST run before core->reset(), not after: core->reset() is what
     * drives GBAOverrideApplyDefaults()/GBASavedataInit() for this cart
     * (see the comment above core->setVideoBuffer() below), which
     * establishes the live save-data state from whatever VFile is
     * currently attached. If no VFile is attached yet - i.e. reset() runs
     * first - it initializes an empty save area, and attaching the real
     * .sav file afterward via core->loadSave() does not reliably get its
     * contents copied into that already-reset state: the open/read
     * succeeds (correct folder, correct file size) but the game still
     * boots as if it had no save. Every mGBA reference frontend (see
     * src/platform, e.g. sdl/main.c) calls mCoreLoadFile -> mCoreAutoloadSave
     * -> reset() in that order for exactly this reason; ROMFilename is
     * already populated by this point (set in filebrowser.cpp when the
     * ROM was selected), so ordering it here costs nothing.
     *
     * FILE_SNAPSHOT (save states) is intentionally NOT moved: a state
     * restores serialized RTC/full-machine state on top of a clean reset,
     * so it still needs to run after core->reset(), further below. */
    if (GCSettings.AutoLoad == 1)
        LoadBatteryOrStateAuto(FILE_SRAM, SILENT);

    core->reset(core);

    // Now that the ROM is loaded and reset, use currentVideoSize() - NOT
    // baseVideoSize(), which always reports the platform's maximum/static
    // canvas (256x224 for GB, regardless of model or border state - this is
    // exactly why the debug prompt showed 256x224 even with model=CGB and
    // sgbBordered=0). currentVideoSize() is the function that actually
    // reflects the current resolution once the ROM's mode is resolved:
    //   256x224  — SGB border is active (SGB-enhanced game on SGB hardware)
    //   160x144  — GB/GBC game
    //   240x160  — GBA
    {
        unsigned coreW = SCREEN_W, coreH = SCREEN_H;
        core->currentVideoSize(core, &coreW, &coreH);
        gbCoreW = (int)coreW;
        gbCoreH = (int)coreH;
        // Update stride and re-point the video buffer with the true dimensions.
        int trueStride = (sgbBordered && gbCoreW == 256) ? gbCoreW : (gbCoreW + 2);
        gGbVideoStride = trueStride;
        core->setVideoBuffer(core, (mColor*)videoBuf, trueStride);
        printf("[mGBA] Video: %dx%d stride=%d sgbBordered=%d\n",
               gbCoreW, gbCoreH, trueStride, (int)sgbBordered);
    }

    // GB Screen Palette (Green Screen / Monochrome Screen) is applied as a
    // pixel-level post-process on the rendered frame buffer - see
    // ApplyGBPalette() below, called from mgba_emuMain() every frame while
    // gGbDmgMode is true. This avoids needing mgba/internal/gb/gb.h (not
    // available in this portlibs package) or guessing at internal mCore
    // config keys, since it operates purely on pixel data we already own.

    coreRunning = true;
    InitEmulator();
    InitMGBAAudio();

    /* Clear any border left over from whatever was loaded previously
     * before evaluating either border path below. Both the SGB and GBA
     * loaders only ever SET InitialBorder when their condition is true -
     * neither ever cleared it when the condition was false (wrong
     * cartridge type, or border disabled) - so a border loaded for one
     * game kept being drawn (at the wrong dimensions) after loading a
     * different game that shouldn't have one. This reset makes "no
     * border" the actual default state for every load, not just an
     * accident of whatever ran last. */
    if (InitialBorder) { free(InitialBorder); InitialBorder = NULL; }
    InitialBorderWidth = 0;
    InitialBorderHeight = 0;
    SGBBorderLoadedFromGame = false;

    /* Manually-selected GB/GBC border (GCSettings.GBBorderFile, cycled in
     * menu.cpp's Game Settings - Emulation screen through a combined
     * .png+.sgb+.bmp listing of GCSettings.GBCBorderFolder) always wins over SGBBorder's
     * "From game" live capture, mirroring GBABorderFile's precedence
     * below. See LoadGBBorderFileIfEnabled(). */
    if (cartridgeType == CARTRIDGE_GB)
        LoadGBBorderFileIfEnabled();

    /* Game Boy Player-style border for GBA games - see
     * LoadGBABorderIfEnabled()'s own header comment (defined further below,
     * near the SGB border helpers) for the file formats supported. Must
     * also run after reset() so RomTitle is populated, same as the SGB
     * PNG block above. */
    LoadGBABorderIfEnabled(RomTitle);

    /* Load any saved cheats for this ROM (CheatFolder/ROMFilename.cheats,
     * mGBA's own mCheatParseFile format) - see CheatsLoadForCurrentROM()
     * below. Must run after RomTitle/ROMFilename are populated, same
     * requirement as the border loaders above. */
    CheatsLoadForCurrentROM();

    printf("[mGBA] Loaded: %s  type=%d  title=%s\n",
           romPath, cartridgeType, RomTitle);

    /* Auto-load save STATE only, here. SRAM autoload already happened
     * above, before core->reset() - see the comment there for why. A save
     * state is different: it's a full serialized snapshot (memory + RTC +
     * metadata) meant to be restored on top of a freshly-reset core, so it
     * correctly stays after core->reset(). */
    if (GCSettings.AutoLoad == 2)
        LoadBatteryOrStateAuto(FILE_SNAPSHOT, SILENT);

    /* Always ensure the core has a valid save VFile associated, even for
     * a brand-new game with no .sav file yet, and regardless of the
     * AutoLoad setting above. Leaving core->savedata's vf permanently
     * NULL for carts that genuinely have battery-backed save hardware
     * (SRAM/RTC) appears to be what's crashing fresh Pokemon Gold/Silver/
     * Crystal saves: the GB core's own RTC-write path may not have the
     * same defensive "if (!vf) return;" null-check its GBA-core
     * equivalent (GBASavedataRTCWrite) does, and Pokemon Gold reads its
     * RTC clock right at boot for the day/night cycle - early enough to
     * explain a crash on a fresh game, before any explicit save/load of
     * ours ever runs.
     *
     * This now mirrors mGBA's own mCoreAutoloadSave() (src/core/core.c)
     * exactly: open O_CREAT|O_RDWR - even if that means a brand-new,
     * zero-byte file - and hand it straight to core->loadSave(). The
     * core's own savedata growth path (GBResizeSram and friends) safely
     * grows a too-small/empty file with real writes as needed.
     *
     * Previously this hand-rolled the same idea via fopen("wb") + fseek()
     * to (expectedSize-1) + fputc(0) to "pre-size" the file before
     * reopening it O_RDONLY. That relies on sparse-file zero-fill - a
     * guarantee plain fseek()/fputc() doesn't actually give you, and
     * libfat on real Wii hardware doesn't honor it: the gap between the
     * old EOF and that single written byte can be leftover garbage from
     * the SD card rather than zeros. That's a very natural way to feed
     * corrupted RTC/SRAM data into the core on a truly fresh save - and
     * it only ever showed up on real hardware, never in Dolphin, because
     * Dolphin runs on a host OS filesystem that happens to zero-fill
     * sparse regions. Letting the core do its own growth via real writes
     * sidesteps the whole sparse-file assumption.
     *
     * If AutoLoad==1 already found and loaded an existing file above, skip
     * re-opening/loading it a second time here. */
    if (GCSettings.AutoLoad != 1) {
        char filepath[MAXPATHLEN];
        if (MakeFilePath(filepath, FILE_SRAM, ROMFilename, 0)) {
            struct VFile *vf = VFileOpen(filepath, O_CREAT | O_RDWR);
            if (vf)
                core->loadSave(core, vf); /* mGBA owns vf now; do not close */
        }
    }

    return true;
}

/* -------------------------------------------------------------------------
 * Cheats
 *
 * Bridge around mGBA's own struct mCheatDevice (mgba/core/cheats.h) for
 * menu.cpp - see the Cheat*() declarations in vbagx.h. One mCheatSet PER
 * CHEAT (not one shared set with many lines) - mGBA's enable/disable and
 * naming both live at the set level (struct mCheatSet.enabled/.name),
 * confirmed against the real header, so a 1:1 mapping of "one set == one
 * on/off row in the UI" is what the API actually models.
 *
 * API fully confirmed against a real mgba/core/cheats.h:
 *   core->cheatDevice(core), mCheatParseFile()/mCheatSaveFile(),
 *   device->createSet(device, name), mCheatAddSet()/mCheatRemoveSet(),
 *   mCheatAddLine(set, line, type), mCheatDeviceClear(device), and the
 *   struct mCheatSet.enabled/.name fields and struct mCheatDevice.cheats
 *   (type mCheatSets - vector accessors mCheatSetsSize()/
 *   mCheatSetsGetPointer() per the DECLARE_VECTOR(mCheatSets, ...)
 *   pattern the header uses).
 *
 * Still not independently confirmed: the cheat type constant passed to
 * mCheatAddLine() as `type` is a plain `int`, not the generic
 * enum mCheatType this header declares (that enum is the per-CHEAT
 * OPERATION semantics - CHEAT_ASSIGN/CHEAT_ADD/CHEAT_IF_EQ/etc, used
 * internally once a line is parsed - not the code-FORMAT selector
 * addLine's `type` param actually wants). The format selector is a
 * platform-specific enum (GBA's is GBA_CHEAT_AUTODETECT/CODEBREAKER/
 * GAMESHARK/PRO_ACTION_REPLAY/VBA, confirmed via gba/cheats.c) that lives
 * in a header this file set still doesn't have. This uses the literal 0,
 * relying on GBA_CHEAT_AUTODETECT being the first (0) enum case
 * (confirmed) and assuming the GB-side format enum also puts its own
 * autodetect constant at 0 (unconfirmed - gb/cheats.c isn't in this file
 * set).
 * ------------------------------------------------------------------- */
#include <mgba/core/cheats.h>

// Matches Snes9x TX's own MAX_CHEATS cap (cheatmgr.cpp) - shared
// convention across this dev's GX-family ports rather than an arbitrary
// number. Purely a safety bound on how many rows MenuGameCheats()
// (menu.cpp) will build in one OptionList - mGBA's own cheat storage
// (mCheatSets, a DECLARE_VECTOR) has no such limit and keeps growing
// past it if a .cheats file somehow has more. Shared via vbagx.h so
// menu.cpp uses the same number.

static char gCheatsFilePath[MAXPATHLEN];

static void BuildCheatsFilePath(void)
{
    snprintf(gCheatsFilePath, sizeof(gCheatsFilePath), "%s%s/%s.cheats",
             pathPrefix[GCSettings.LoadMethod], GCSettings.CheatFolder, ROMFilename);
}

/* Called once per ROM load, after RomTitle/ROMFilename are populated (see
 * call site above, alongside the border loaders). Silently does nothing
 * if no .cheats file exists yet for this ROM - same "missing file is
 * fine" handling as saves/borders, not an error. */
void CheatsLoadForCurrentROM(void)
{
    if (!core) return;

    // Erase whatever cheats belonged to the PREVIOUS ROM before loading
    // this one's, mirroring Snes9x TX's WiiSetupCheats() comment ("Erases
    // any preexisting cheats, loads cheats from a cheat file"). Without
    // this, if the same core object gets reused across ROM loads in one
    // session (unloadROM()+loadROM() rather than a full core teardown),
    // the previous game's cheat sets would still be sitting in
    // device->cheats and get applied to the new game too.
    struct mCheatDevice *device = core->cheatDevice(core);
    if (device)
        mCheatDeviceClear(device);

    BuildCheatsFilePath();

    struct VFile *vf = VFileOpen(gCheatsFilePath, O_RDONLY);
    if (!vf) return;

    if (device)
        mCheatParseFile(device, vf);
    vf->close(vf);
}

/* Every Add/Toggle/Delete below calls this immediately afterward - cheats
 * are persisted the moment they change, same "always write back" pattern
 * SaveBatteryOrState() already uses, rather than needing an explicit
 * "Save Cheats" step in the UI. */
static void CheatsSaveForCurrentROM(void)
{
    if (!core) return;
    struct mCheatDevice *device = core->cheatDevice(core);
    if (!device) return;

    BuildCheatsFilePath();
    struct VFile *vf = VFileOpen(gCheatsFilePath, O_CREAT | O_TRUNC | O_RDWR);
    if (!vf) return;
    mCheatSaveFile(device, vf);
    vf->close(vf);
}

int CheatCount(void)
{
    if (!core) return 0;
    struct mCheatDevice *device = core->cheatDevice(core);
    if (!device) return 0;
    return (int)mCheatSetsSize(&device->cheats);
}

bool CheatGetInfo(int index, char *descOut, int descOutSize, bool *enabledOut)
{
    if (!core || index < 0) return false;
    struct mCheatDevice *device = core->cheatDevice(core);
    if (!device) return false;
    if ((size_t)index >= mCheatSetsSize(&device->cheats)) return false;

    struct mCheatSet *set = *mCheatSetsGetPointer(&device->cheats, index);
    if (descOut && descOutSize > 0)
        snprintf(descOut, descOutSize, "%s", (set->name && set->name[0]) ? set->name : "Cheat");
    if (enabledOut)
        *enabledOut = set->enabled;
    return true;
}

void CheatToggle(int index)
{
    if (!core || index < 0) return;
    struct mCheatDevice *device = core->cheatDevice(core);
    if (!device) return;
    if ((size_t)index >= mCheatSetsSize(&device->cheats)) return;

    struct mCheatSet *set = *mCheatSetsGetPointer(&device->cheats, index);
    set->enabled = !set->enabled;
    CheatsSaveForCurrentROM();
}

bool CheatAdd(const char *description, const char *code)
{
    if (!core || !description || !code || !code[0]) return false;
    struct mCheatDevice *device = core->cheatDevice(core);
    if (!device) return false;

    struct mCheatSet *set = device->createSet(device, description);
    if (!set) return false;

    // 0 = autodetect - see this section's header comment for the
    // confidence caveat on this constant.
    if (!mCheatAddLine(set, code, 0)) {
        // Parse failed. The set was never registered with the device
        // (mCheatAddSet() below hasn't run yet), so it's not safe to
        // assume mCheatRemoveSet() handles an unregistered set the same
        // way as a registered one - rather than risk calling unverified
        // cleanup on a half-built object, this accepts a small one-time
        // leak on the (rare) invalid-code-entry path instead of risking
        // a crash. Flagging in case your real cheats.h has a safe
        // "destroy an unregistered set" call to use here instead.
        return false;
    }

    mCheatAddSet(device, set);
    set->enabled = true;
    CheatsSaveForCurrentROM();
    return true;
}

void CheatDelete(int index)
{
    if (!core || index < 0) return;
    struct mCheatDevice *device = core->cheatDevice(core);
    if (!device) return;
    if ((size_t)index >= mCheatSetsSize(&device->cheats)) return;

    struct mCheatSet *set = *mCheatSetsGetPointer(&device->cheats, index);
    mCheatRemoveSet(device, set); // assumed to also free `set` - see header comment
    CheatsSaveForCurrentROM();
}

/* -------------------------------------------------------------------------
 * Save / Load
 * ---------------------------------------------------------------------- */
bool SavePreviewImg(char *filepath, bool silent); // defined below; forward-declared for use in SaveBatteryOrState()

bool SaveBatteryOrState(char *filepath, int action, bool silent)
{
    if (!core) return false;

    // For the SRAM/battery path specifically: clone the savedata BEFORE
    // opening the destination file at all. VFileOpen(..., O_TRUNC) below
    // truncates the file the instant it's opened - before we've written
    // a single byte back. Previously this file was opened first, and only
    // afterward did we check whether core->savedataClone() actually gave
    // us anything (if (sram && size > 0)). If it ever came back empty -
    // save-type detection not fully settled yet, an interrupted exit
    // sequence, any edge case - that check correctly skipped the WRITE,
    // but the OPEN had already zeroed the file out from under whatever
    // was previously saved there. Cloning first means a failed/empty
    // clone just skips the save entirely, leaving the existing file
    // completely untouched - worse to skip a save than to silently
    // destroy a good one.
    //
    // NOTE: this function must never be called with the "Auto" slot
    // (filenum 0) path for FILE_SRAM while a ROM is loaded - see the much
    // bigger issue documented at the UnloadCore() and MenuGame() call
    // sites that used to do exactly that.
    if (action != FILE_SNAPSHOT) {
        void *sram = NULL;
        size_t size = core->savedataClone(core, &sram);
        if (!sram || size == 0) {
            if (!silent) printf("[mGBA] savedataClone() returned nothing - skipping save, existing file left untouched: %s\n", filepath);
            if (sram) free(sram);
            return false;
        }

        struct VFile *vf = VFileOpen(filepath, O_WRONLY | O_CREAT | O_TRUNC);
        if (!vf) {
            if (!silent) printf("[mGBA] Cannot open for write: %s\n", filepath);
            free(sram);
            return false;
        }
        bool ok = (vf->write(vf, sram, size) == (ssize_t)size);
        free(sram);
        vf->close(vf);
        return ok;
    }

    struct VFile *vf = VFileOpen(filepath, O_WRONLY | O_CREAT | O_TRUNC);
    if (!vf) {
        if (!silent) printf("[mGBA] Cannot open for write: %s\n", filepath);
        return false;
    }
    bool ok = false;
    {
        // Capture the current frame into gameScreenPng BEFORE writing the
        // state, so SavePreviewImg() below has something to actually save.
        // Previously nothing ever called this, so the preview thumbnail
        // step was silently a no-op every time (gameScreenPng.pngData was
        // whatever stale/empty value it last held, if anything).
        TakeScreenshot();
        ok = mCoreSaveStateNamed(core, vf,
             SAVESTATE_SAVEDATA | SAVESTATE_RTC | SAVESTATE_METADATA);
    }
    vf->close(vf);

    // Save states (not battery/SRAM saves) also get a matching .png preview
    // thumbnail alongside the state file, so save/load-state UI can show
    // what the game looked like at that point. Best-effort only - a failed
    // thumbnail write shouldn't fail the state save itself, since the state
    // data (the part that actually matters) is already safely on disk by
    // this point.
    if (ok && action == FILE_SNAPSHOT)
        SavePreviewImg(filepath, silent);

    return ok;
}

bool LoadBatteryOrState(char *filepath, int action, bool silent)
{
    if (!core) return false;

    if (action != FILE_SNAPSHOT) {
        // Previously asked core->savedataClone() how large this cart's
        // savedata SHOULD be, then hand pre-sized the file via fopen("r+b")
        // + fseek() to (expectedSize-1) + fputc(0) before reopening it
        // O_RDONLY. Removed for the same reason as the auto-create block
        // above mgba_emuLoadFile()'s "Always ensure the core has a valid
        // save VFile" comment: that pre-sizing trick relies on sparse-file
        // zero-fill, which libfat doesn't guarantee on real Wii hardware,
        // risking leftover garbage bytes instead of zeros feeding into the
        // core's RTC/SRAM state. Opening O_CREAT|O_RDWR directly and
        // handing the VFile straight to core->loadSave() - mirroring
        // mGBA's own mCoreAutoloadSave() (src/core/core.c) - lets the
        // core's own growth path (real writes, not a seek-past-EOF) safely
        // handle both a too-small existing file and a brand-new one.
        struct VFile *vf = VFileOpen(filepath, O_CREAT | O_RDWR);
        if (!vf) {
            if (!silent) printf("[mGBA] Cannot open for read/write: %s\n", filepath);
            return false;
        }

        // Root cause of saves silently not loading (2026-xx-xx): this
        // function was being reached correctly - right folder, right file,
        // right size - but was being called *after* core->reset() from the
        // boot path, which had already initialized an empty save area. See
        // the autoload comment in LoadVBAROM(), above core->reset(), for
        // the fix. Kept as a normal silent-gated diagnostic below.
        if (!silent) {
            ssize_t existingSize = vf->size(vf);
            printf("[mGBA] Loading save: %s (SaveMethod=%d SaveFolder=%s, existing size=%d bytes)\n",
                   filepath, GCSettings.SaveMethod, GCSettings.SaveFolder, (int)existingSize);
        }

        bool loadOk = core->loadSave(core, vf); /* mGBA owns vf now; do not close */
        if (!silent) printf("[mGBA] core->loadSave() returned %s\n", loadOk ? "true" : "false");
        return loadOk;
    }

    struct VFile *vf = VFileOpen(filepath, O_RDONLY);
    if (!vf) {
        if (!silent) printf("[mGBA] Cannot open for read: %s\n", filepath);
        return false;
    }
    bool ok = mCoreLoadStateNamed(core, vf,
         SAVESTATE_SAVEDATA | SAVESTATE_RTC | SAVESTATE_METADATA);
    vf->close(vf);
    return ok;
}

bool SaveBatteryOrStateAuto(int action, bool silent)
{
    // Previously hand-built the path as SaveFolder+ext directly, with no
    // ROM-specific filename component at all - every game's auto-save
    // collided into the exact same shared file (e.g. "sd:/mgbagx/saves.sav"
    // for every ROM, regardless of which one was loaded). MakeFilePath()
    // is the established, correct convention used everywhere else in the
    // codebase (see filebrowser.cpp) - SaveFolder + "/" + ROMFilename +
    // ext, with filenum=0 giving the "Auto" save slot (governed by
    // GCSettings.AppendAuto, same as the manual Save/Load Game menu).
    char filepath[MAXPATHLEN];
    if (!MakeFilePath(filepath, action, ROMFilename, 0))
        return false;
    return SaveBatteryOrState(filepath, action, silent);
}

bool LoadBatteryOrStateAuto(int action, bool silent)
{
    char filepath[MAXPATHLEN];
    if (!MakeFilePath(filepath, action, ROMFilename, 0))
        return false;
    return LoadBatteryOrState(filepath, action, silent);
}

/* -------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------- */
bool IsGameboyGame()      { return cartridgeType == CARTRIDGE_GB; }
bool IsGBAGame()          { return cartridgeType == CARTRIDGE_GBA; }
void ResetTiltAndCursor() { TiltSideways = false; }
void InitialisePalette()  {}
bool SavePreviewImg(char *filepath, bool silent)
{
	if (!gameScreenPng.pngData || gameScreenPng.pngSize <= 0)
	{
		if (!silent) printf("[Screenshot] No screenshot data available\n");
		return false;
	}

	// gameScreenPng includes the SGB border when one is present (needed
	// for the pause-menu blur - see CropGameScreenBorderForSave()'s header
	// comment in video.cpp) but the saved preview thumbnail should show
	// just the bare game content. Crop it back out here if there's a
	// border to crop; CropGameScreenBorderForSave() returns NULL (nothing
	// to free) when there isn't one, in which case the raw capture is
	// already exactly what we want.
	int croppedSize = 0;
	u8 *croppedData = CropGameScreenBorderForSave(&croppedSize);
	u8 *dataToSave = croppedData ? croppedData : gameScreenPng.pngData;
	int sizeToSave = croppedData ? croppedSize : gameScreenPng.pngSize;

	// Build the full .png path: filepath already contains the base path + ROM
	// name WITH its .sgm extension (it's the same path the state file was
	// just written to) - replace that extension with .png rather than
	// appending, to match the convention menu.cpp's save-list scan loop and
	// delete-file logic already use (both strip the last 4 chars of the
	// .sgm filename and add ".png"). Appending here instead of replacing
	// produced e.g. "Zelda.sgm.png" while the reader looked for
	// "Zelda.png" - the two never matched, so the thumbnail silently never
	// loaded no matter how many times the save screen was reopened.
	char pngPath[1024];
	snprintf(pngPath, sizeof(pngPath), "%s", filepath);
	size_t pngPathLen = strlen(pngPath);
	if (pngPathLen > 4 && strcmp(pngPath + pngPathLen - 4, ".sgm") == 0)
		pngPath[pngPathLen - 4] = '\0';
	strncat(pngPath, ".png", sizeof(pngPath) - strlen(pngPath) - 1);

	// Ensure the directory exists
	char dir[1024];
	snprintf(dir, sizeof(dir), "%s", pngPath);
	char *slash = strrchr(dir, '/');
	if (slash)
	{
		*slash = '\0';
		mkdir(dir, 0777); // ignore error if it already exists
	}

	// Write the PNG data to disk
	FILE *f = fopen(pngPath, "wb");
	if (!f)
	{
		if (!silent) printf("[Screenshot] Failed to open for write: %s\n", pngPath);
		if (croppedData) free(croppedData);
		return false;
	}

	bool ok = (fwrite(dataToSave, 1, (size_t)sizeToSave, f) == (size_t)sizeToSave);
	fclose(f);
	if (croppedData) free(croppedData);

	if (!silent)
	{
		if (ok) printf("[Screenshot] Saved: %s\n", pngPath);
		else    printf("[Screenshot] Write failed: %s\n", pngPath);
	}
	return ok;
}

/* -------------------------------------------------------------------------
 * VBA-M stubs — referenced by vbagx GUI/input code we kept
 * ---------------------------------------------------------------------- */
extern "C" {
void InitCrcTable(void) {}
void SzArDbExInit(void *db) { (void)db; }
int  SzArchiveOpen(void *s, void *db, void *am, void *at)
     { (void)s; (void)db; (void)am; (void)at; return 1; }
void SzArDbExFree(void *db, void *f) { (void)db; (void)f; }
}

/* -------------------------------------------------------------------------
 * SGB Border helpers
 * ---------------------------------------------------------------------- */

/* Returns a malloc'd path string "LoadMethod/<folder>/name", or NULL.
 * Caller must free() the returned string. sub is "gba" or "gbc", selecting
 * GCSettings.GBABorderFolder or GCSettings.GBCBorderFolder respectively -
 * see the comment above GCSettings.GBABorderFile/GBBorderFile in vbagx.h
 * for why the two systems get separate, independently path-editable
 * folders: GBA and GB/GBC borders can both be .png/.bmp files but at
 * different pixel dimensions (320x240 vs 256x224), so a shared folder
 * would list a GBC-sized image in the GBA cycle (and vice versa) with no
 * way to tell them apart until the decode step rejects it. */
static char *AllocAndGetBorderSubPath(const char *sub, const char *name)
{
    char *path = (char *)malloc(MAXPATHLEN + 64);
    if (!path) return NULL;
    const char *folder = (strcmp(sub, "gba") == 0) ? GCSettings.GBABorderFolder : GCSettings.GBCBorderFolder;
    snprintf(path, MAXPATHLEN + 64, "%s%s/%s",
             pathPrefix[GCSettings.LoadMethod], folder, name);
    return path;
}

/* Kept for compatibility with any other caller expecting the old
 * per-title PNG match path; menu.cpp/vbasupport.cpp no longer use this
 * for the GB/GBC border list (see LoadGBBorderFileIfEnabled below), which
 * now lets the user pick any file directly instead of relying on an
 * auto-matched RomTitle.png. Returns "LoadMethod/GBCBorderFolder/title.png". */
char *AllocAndGetPNGBorderPath(const char *title)
{
    char nameWithExt[MAXPATHLEN];
    snprintf(nameWithExt, sizeof(nameWithExt), "%s.png", title);
    return AllocAndGetBorderSubPath("gbc", nameWithExt);
}

/* Decodes a MiSTer-FPGA-style .sgb border overlay into a linear RGB565
 * buffer (caller-owned, malloc'd, W*H*2 bytes; NOT yet GX-tiled).
 *
 * Format confirmed against a real sample (border_sgb1.sgb, exactly 10112
 * bytes = 8192 + 1792 + 128, matching this layout with no header):
 *
 *   offset 0      : 256 tiles of SNES/SGB 4bpp planar tile data, 32 bytes
 *                    each (8192 bytes total). Each tile is 8x8 pixels:
 *                    rows 0-7 give bitplanes 0/1 interleaved 2 bytes/row
 *                    (16 bytes), then rows 0-7 again give bitplanes 2/3
 *                    the same way (16 bytes) - the standard SNES 4bpp
 *                    "2bpp doubled" planar layout, NOT 2bpp-per-row-pair
 *                    GB tile format.
 *   offset 8192   : 32x28 tilemap, 2 bytes/entry, little-endian (1792
 *                    bytes total). Bits 0-9 = tile index (0-255 in
 *                    practice), bits 10-12 = palette (0-3 used), bit 13 =
 *                    priority (ignored here), bit 14 = X-flip, bit 15 =
 *                    Y-flip. 32*8=256 x 28*8=224 matches the existing
 *                    256x224 SGB border canvas used elsewhere in this
 *                    file (SaveSGBBorderIfNoneExists).
 *   offset 9984   : 4 palettes of 16 colors, RGB555 (bit15 unused),
 *                    little-endian, 2 bytes/color (128 bytes total).
 *
 * Color 0 of each palette is treated as transparent (SGB border
 * convention) is NOT special-cased here - the border sits behind the
 * game image and DrawBorderAndGetDest draws the game on top of whatever
 * is in InitialBorder regardless, so an opaque decode of every index is
 * fine; there's no alpha channel to preserve in RGB565 anyway.
 *
 * Returns NULL on a size/sanity mismatch.
 */
static u16 *DecodeSGBFileToRGB565(const u8 *data, size_t size, int *outW, int *outH)
{
    const int TILE_COUNT = 256;
    const int MAP_W = 32, MAP_H = 28;
    const size_t TILEDATA_SIZE = TILE_COUNT * 32;       // 8192
    const size_t TILEMAP_SIZE  = MAP_W * MAP_H * 2;     // 1792
    const size_t PALETTE_SIZE  = 4 * 16 * 2;            // 128
    const size_t EXPECTED_SIZE = TILEDATA_SIZE + TILEMAP_SIZE + PALETTE_SIZE; // 10112

    if (size != EXPECTED_SIZE)
    {
        printf("[SGB] .sgb file wrong size: expected %u, got %u\n",
               (unsigned)EXPECTED_SIZE, (unsigned)size);
        return NULL;
    }

    const u8 *tileData = data;
    const u8 *tileMap   = data + TILEDATA_SIZE;
    const u8 *palData   = data + TILEDATA_SIZE + TILEMAP_SIZE;

    const int W = MAP_W * 8, H = MAP_H * 8; // 256x224

    // Convert all 4 palettes (RGB555 -> RGB565) up front.
    u16 palettes[4][16];
    for (int p = 0; p < 4; p++)
    {
        for (int c = 0; c < 16; c++)
        {
            int off = (p * 16 + c) * 2;
            u16 raw = palData[off] | (palData[off+1] << 8); // little-endian RGB555
            u8 r5 = raw & 0x1F;
            u8 g5 = (raw >> 5) & 0x1F;
            u8 b5 = (raw >> 10) & 0x1F;
            // RGB555 -> RGB565: green gets an extra bit (duplicate top bit)
            u8 g6 = (g5 << 1) | (g5 >> 4);
            palettes[p][c] = (r5 << 11) | (g6 << 5) | b5;
        }
    }

    u16 *out = (u16 *)malloc((size_t)W * H * 2);
    if (!out) return NULL;

    for (int ty = 0; ty < MAP_H; ty++)
    {
        for (int tx = 0; tx < MAP_W; tx++)
        {
            int mapOff = (ty * MAP_W + tx) * 2;
            u16 entry = tileMap[mapOff] | (tileMap[mapOff+1] << 8);

            int tileIdx = entry & 0x03FF;
            int palIdx  = (entry >> 10) & 0x07;
            if (palIdx > 3) palIdx = 3; // only 4 palettes present in the file
            bool xflip = (entry & 0x4000) != 0;
            bool yflip = (entry & 0x8000) != 0;

            if (tileIdx >= TILE_COUNT) tileIdx = 0;
            const u8 *tile = tileData + tileIdx * 32;

            for (int py = 0; py < 8; py++)
            {
                int srcY = yflip ? (7 - py) : py;
                u8 bp0 = tile[srcY * 2 + 0];
                u8 bp1 = tile[srcY * 2 + 1];
                u8 bp2 = tile[16 + srcY * 2 + 0];
                u8 bp3 = tile[16 + srcY * 2 + 1];

                for (int px = 0; px < 8; px++)
                {
                    int srcX = xflip ? px : (7 - px); // bit 7 = leftmost pixel
                    int bit = srcX;
                    int idx = ((bp0 >> bit) & 1)
                            | (((bp1 >> bit) & 1) << 1)
                            | (((bp2 >> bit) & 1) << 2)
                            | (((bp3 >> bit) & 1) << 3);

                    int outX = tx * 8 + px;
                    int outY = ty * 8 + py;
                    out[outY * W + outX] = palettes[palIdx][idx];
                }
            }
        }
    }

    *outW = W;
    *outH = H;
    return out;
}

/* Decodes an uncompressed 24bpp or 32bpp Windows BMP (BITMAPFILEHEADER +
 * BITMAPINFOHEADER, BI_RGB only - no RLE, no 16/8/4/1bpp, no OS/2 headers)
 * into a linear RGB565 buffer (caller-owned, malloc'd, W*H*2 bytes; NOT
 * yet GX-tiled), for the border loaders' .bmp support. Handles both
 * bottom-up (positive height, BMP's normal row order) and top-down
 * (negative height) row storage, and BMP's per-row 4-byte padding.
 * Returns NULL on any header/format mismatch (wrong size, unsupported
 * bit depth, compressed, etc). Doesn't validate exact expectedW/H itself -
 * callers check the returned *outW / *outH against what the border slot
 * requires. */
static u16 *DecodeBMPToRGB565(const u8 *data, size_t size, int *outW, int *outH)
{
    if (size < 54 || data[0] != 'B' || data[1] != 'M')
        return NULL;

    u32 dataOffset  = data[10] | (data[11] << 8) | (data[12] << 16) | ((u32)data[13] << 24);
    u32 dibHeaderSz = data[14] | (data[15] << 8) | (data[16] << 16) | ((u32)data[17] << 24);
    if (dibHeaderSz < 40 || size < 14 + dibHeaderSz)
        return NULL; // only BITMAPINFOHEADER (or newer, same leading layout) supported

    int32_t rawW = (int32_t)(data[18] | (data[19] << 8) | (data[20] << 16) | ((u32)data[21] << 24));
    int32_t rawH = (int32_t)(data[22] | (data[23] << 8) | (data[24] << 16) | ((u32)data[25] << 24));
    u16 bitCount    = data[28] | (data[29] << 8);
    u32 compression = data[30] | (data[31] << 8) | (data[32] << 16) | ((u32)data[33] << 24);

    if (compression != 0 /* BI_RGB */ || (bitCount != 24 && bitCount != 32))
    {
        printf("[Border] .bmp must be uncompressed 24 or 32bpp\n");
        return NULL;
    }

    bool topDown = (rawH < 0);
    int W = rawW;
    int H = topDown ? -rawH : rawH;
    if (W <= 0 || H <= 0) return NULL;

    int bytesPerPixel = bitCount / 8;
    size_t srcStride = (((size_t)W * bytesPerPixel + 3) / 4) * 4; // rows padded to 4 bytes
    if (size < (size_t)dataOffset + srcStride * H)
    {
        printf("[Border] .bmp truncated (expected at least %u bytes, got %u)\n",
               (unsigned)(dataOffset + srcStride * H), (unsigned)size);
        return NULL;
    }

    u16 *out = (u16 *)malloc((size_t)W * H * 2);
    if (!out) return NULL;

    for (int y = 0; y < H; y++)
    {
        // BMP rows are bottom-up by default (last row = top of image);
        // topDown (negative height) means first row IS the top already.
        int srcRow = topDown ? y : (H - 1 - y);
        const u8 *row = data + dataOffset + (size_t)srcRow * srcStride;
        u16 *dstRow = out + (size_t)y * W;
        for (int x = 0; x < W; x++)
        {
            const u8 *px = row + (size_t)x * bytesPerPixel;
            u8 b = px[0], g = px[1], r = px[2]; // BMP stores BGR(A)
            dstRow[x] = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
        }
    }

    *outW = W;
    *outH = H;
    return out;
}

/* Unified GB/GBC manual border loader - GCSettings.GBBorderFile is cycled
 * in menu.cpp through a combined listing of GCSettings.GBCBorderFolder's
 * *.png, *.sgb and *.bmp files, so which decode path runs is picked here
 * purely by file extension. Mirrors LoadGBABorderIfEnabled()'s structure
 * below (PNG via PNGU straight to 4x4-tiled RGB565; the raw/derived
 * formats go through a linear decode then SwizzleLinearToGXTiled). "" =
 * None. */
void LoadGBBorderFileIfEnabled(void)
{
    if (GCSettings.GBBorderFile[0] == 0)
        return;

    const int W = 256, H = 224;
    size_t nameLen = strlen(GCSettings.GBBorderFile);
    bool isPNG = (nameLen > 4 && strcasecmp(GCSettings.GBBorderFile + nameLen - 4, ".png") == 0);
    bool isSGB = (nameLen > 4 && strcasecmp(GCSettings.GBBorderFile + nameLen - 4, ".sgb") == 0);
    bool isBMP = (nameLen > 4 && strcasecmp(GCSettings.GBBorderFile + nameLen - 4, ".bmp") == 0);
    if (!isPNG && !isSGB && !isBMP)
    {
        printf("[SGB] Unrecognized border file extension: %s\n", GCSettings.GBBorderFile);
        return;
    }

    char *fullPath = AllocAndGetBorderSubPath("gbc", GCSettings.GBBorderFile);
    if (!fullPath) return;

    if (isPNG)
    {
        size_t pngSize = LoadFile((char *)savebuffer, fullPath, 0, SAVEBUFFERSIZE, SILENT);
        free(fullPath);
        if (pngSize == 0) return;

        IMGCTX ctx = PNGU_SelectImageFromBuffer(savebuffer);
        if (!ctx) return;

        PNGUPROP props;
        PNGU_GetImageProperties(ctx, &props);

        if (props.imgWidth != W || props.imgHeight != H)
        {
            printf("[SGB] Border PNG must be %dx%d, got %dx%d\n",
                   W, H, props.imgWidth, props.imgHeight);
            PNGU_ReleaseImageContext(ctx);
            return;
        }

        u16 *tiledBuf = (u16 *)memalign(32, W * H * 2);
        if (tiledBuf && PNGU_DecodeTo4x4RGB565(ctx, W, H, tiledBuf) == PNGU_OK)
        {
            if (InitialBorder) free(InitialBorder);
            InitialBorder        = tiledBuf;
            InitialBorderWidth   = W;
            InitialBorderHeight  = H;
            SGBBorderLoadedFromGame = true;
            printf("[SGB] Border loaded from PNG: %s\n", GCSettings.GBBorderFile);
        }
        else if (tiledBuf)
        {
            free(tiledBuf);
            printf("[SGB] PNGU decode failed for %s\n", GCSettings.GBBorderFile);
        }
        PNGU_ReleaseImageContext(ctx);
        return;
    }

    // --- .sgb (MiSTer-FPGA-style, see DecodeSGBFileToRGB565) or .bmp ---
    size_t gotSize = LoadFile((char *)savebuffer, fullPath, 0, SAVEBUFFERSIZE, SILENT);
    free(fullPath);
    if (gotSize == 0) return;

    int decW = 0, decH = 0;
    u16 *linear = isSGB
        ? DecodeSGBFileToRGB565((const u8 *)savebuffer, gotSize, &decW, &decH)
        : DecodeBMPToRGB565((const u8 *)savebuffer, gotSize, &decW, &decH);
    if (!linear) return;

    if (isBMP && (decW != W || decH != H))
    {
        printf("[SGB] Border BMP must be %dx%d, got %dx%d\n", W, H, decW, decH);
        free(linear);
        return;
    }

    u16 *tiledBuf = (u16 *)memalign(32, (size_t)decW * decH * 2);
    if (!tiledBuf) { free(linear); return; }

    SwizzleLinearToGXTiled((const u8 *)linear, (u8 *)tiledBuf, decW, decH, decW * 2);
    free(linear);

    if (InitialBorder) free(InitialBorder);
    InitialBorder        = tiledBuf;
    InitialBorderWidth   = decW;
    InitialBorderHeight  = decH;
    SGBBorderLoadedFromGame = true;
    printf("[SGB] Border loaded from %s: %s\n", isSGB ? ".sgb" : ".bmp", GCSettings.GBBorderFile);
}

/* Game Boy Player-style border for GBA games. GCSettings.GBABorderFile is
 * a single global choice (filename with extension, relative to
 * GCSettings.GBABorderFolder) set by cycling through "Border (GBA)" in the
 * Emulation settings menu, which lists whatever .png/.bor files actually
 * exist there. Empty string = None/disabled.
 *
 * Supports 320x240 PNG (same PNGU_DecodeTo4x4RGB565 path as the GBC PNG
 * loader) or a raw 320x240 RGBA8888 dump with no header - the format the
 * MiSTer-GBA-Borders community pack ships (confirmed against a real
 * sample: 320*240*4 = 307200 bytes, 4 bytes/pixel R,G,B,A with A always
 * 0xFF). The raw path converts to linear RGB565 and hands it to
 * SwizzleLinearToGXTiled() (video.cpp/video.h) - the same tiling routine
 * GX_Render uses every frame - rather than re-deriving the 4x4 tile
 * addressing math a third time in this file.
 *
 * Sets InitialBorder/InitialBorderWidth/InitialBorderHeight exactly like
 * the GBC loader; DrawBorderAndGetDest()/WriteFrameToTextureMemory() in
 * video.cpp are already generic over cartridge type - they just check
 * "if (InitialBorder)" - so no render-path changes are needed for this to
 * display once InitialBorder is populated. */
void LoadGBABorderIfEnabled(const char *title)
{
    (void)title; // no longer per-ROM - kept in the signature for call-site stability

    if (cartridgeType != CARTRIDGE_GBA || GCSettings.GBABorderFile[0] == 0)
        return;

    const int W = 320, H = 240;
    size_t nameLen = strlen(GCSettings.GBABorderFile);
    bool isPNG = (nameLen > 4 && strcasecmp(GCSettings.GBABorderFile + nameLen - 4, ".png") == 0);
    bool isBOR = (nameLen > 4 && strcasecmp(GCSettings.GBABorderFile + nameLen - 4, ".bor") == 0);
    bool isBMP = (nameLen > 4 && strcasecmp(GCSettings.GBABorderFile + nameLen - 4, ".bmp") == 0);
    if (!isPNG && !isBOR && !isBMP)
    {
        printf("[GBP] Unrecognized border file extension: %s\n", GCSettings.GBABorderFile);
        return;
    }

    char fullPath[MAXPATHLEN + 64];
    snprintf(fullPath, sizeof(fullPath), "%s%s/%s",
             pathPrefix[GCSettings.LoadMethod], GCSettings.GBABorderFolder, GCSettings.GBABorderFile);

    if (isPNG)
    {
        size_t pngSize = LoadFile((char *)savebuffer, fullPath, 0, SAVEBUFFERSIZE, SILENT);
        if (pngSize == 0) return;

        IMGCTX ctx = PNGU_SelectImageFromBuffer(savebuffer);
        if (!ctx) return;

        PNGUPROP props;
        PNGU_GetImageProperties(ctx, &props);

        if (props.imgWidth != W || props.imgHeight != H)
        {
            printf("[GBP] Border PNG must be %dx%d, got %dx%d\n",
                   W, H, props.imgWidth, props.imgHeight);
            PNGU_ReleaseImageContext(ctx);
            return;
        }

        u16 *tiledBuf = (u16 *)memalign(32, W * H * 2);
        if (tiledBuf && PNGU_DecodeTo4x4RGB565(ctx, W, H, tiledBuf) == PNGU_OK)
        {
            if (InitialBorder) free(InitialBorder);
            InitialBorder        = tiledBuf;
            InitialBorderWidth   = W;
            InitialBorderHeight  = H;
            SGBBorderLoadedFromGame = true; // also gates GBA's DrawBorderAndGetDest path
            printf("[GBP] Border loaded from PNG: %s\n", GCSettings.GBABorderFile);
        }
        else if (tiledBuf)
        {
            free(tiledBuf);
            printf("[GBP] PNGU decode failed for %s\n", GCSettings.GBABorderFile);
        }
        PNGU_ReleaseImageContext(ctx);
        return;
    }

    if (isBOR)
    {
        // --- .bor: raw 320x240 RGBA8888, no header ---
        const size_t rawSize = (size_t)W * H * 4;
        size_t gotSize = LoadFile((char *)savebuffer, fullPath, 0, SAVEBUFFERSIZE, SILENT);
        if (gotSize != rawSize)
        {
            if (gotSize > 0)
                printf("[GBP] Border .bor must be exactly %u bytes (320x240 RGBA8888), got %u\n",
                       (unsigned)rawSize, (unsigned)gotSize);
            return;
        }

        u16 *linear = (u16 *)memalign(32, W * H * 2);
        if (!linear) return;

        const u8 *src = (const u8 *)savebuffer;
        for (int i = 0; i < W * H; i++)
        {
            u8 r = src[i*4 + 0], g = src[i*4 + 1], b = src[i*4 + 2];
            linear[i] = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
        }

        u16 *tiledBuf = (u16 *)memalign(32, W * H * 2);
        if (!tiledBuf) { free(linear); return; }

        SwizzleLinearToGXTiled((const u8 *)linear, (u8 *)tiledBuf, W, H, W * 2);
        free(linear);

        if (InitialBorder) free(InitialBorder);
        InitialBorder        = tiledBuf;
        InitialBorderWidth   = W;
        InitialBorderHeight  = H;
        SGBBorderLoadedFromGame = true;
        printf("[GBP] Border loaded from .bor: %s\n", GCSettings.GBABorderFile);
        return;
    }

    // --- .bmp: uncompressed 24/32bpp Windows BMP (see DecodeBMPToRGB565) ---
    {
        size_t gotSize = LoadFile((char *)savebuffer, fullPath, 0, SAVEBUFFERSIZE, SILENT);
        if (gotSize == 0) return;

        int decW = 0, decH = 0;
        u16 *linear = DecodeBMPToRGB565((const u8 *)savebuffer, gotSize, &decW, &decH);
        if (!linear) return;

        if (decW != W || decH != H)
        {
            printf("[GBP] Border BMP must be %dx%d, got %dx%d\n", W, H, decW, decH);
            free(linear);
            return;
        }

        u16 *tiledBuf = (u16 *)memalign(32, (size_t)W * H * 2);
        if (!tiledBuf) { free(linear); return; }

        SwizzleLinearToGXTiled((const u8 *)linear, (u8 *)tiledBuf, W, H, W * 2);
        free(linear);

        if (InitialBorder) free(InitialBorder);
        InitialBorder        = tiledBuf;
        InitialBorderWidth   = W;
        InitialBorderHeight  = H;
        SGBBorderLoadedFromGame = true;
        printf("[GBP] Border loaded from .bmp: %s\n", GCSettings.GBABorderFile);
    }
}

/* Called by video.cpp's ProcessSGBBorder when a live SGB frame with a
 * non-empty border is detected (gbWidth==256, gbHeight==224).
 *
 * IMPORTANT: InitialBorder must contain GX 4x4-TILED RGB565 data, NOT a
 * linear pixel buffer, because DrawBorderAndGetDest memcpy's it directly
 * into texturemem which the GX sampler reads as tiled.
 *
 * We achieve this by allocating a tiled-size buffer and calling
 * WriteFrameToTextureMemory(), which runs the same PowerPC swizzle that
 * GX_Render uses every frame — so the result is pixel-perfect. */
void SaveSGBBorderIfNoneExists(const void *data)
{
    if (SGBBorderLoadedFromGame) return;   // already have a border
    if (!data) return;

    const int W = InitialBorderWidth  > 0 ? InitialBorderWidth  : 256;
    const int H = InitialBorderHeight > 0 ? InitialBorderHeight : 224;

    // Allocate a 32-byte aligned buffer sized for the tiled texture
    // (GX tiled size = W * H * 2 bytes, same as linear, but arranged in 4x4 tiles)
    u16 *tiledBuf = (u16 *)memalign(32, W * H * 2);
    if (!tiledBuf) return;

    // Swizzle the linear mGBA output into GX 4x4 tiled format.
    // mGBA's buffer has stride (W+2)*2 bytes per row due to its internal +2 padding.
    // SwizzleLinearToGXTiled reads exactly W pixels per row, skipping the padding.
    int srcStride = (W + 2) * 2;
    SwizzleLinearToGXTiled((const u8 *)data, (u8 *)tiledBuf, W, H, srcStride);

    // Free any previous border buffer before replacing
    if (InitialBorder) free(InitialBorder);

    InitialBorder        = tiledBuf;
    InitialBorderWidth   = W;
    InitialBorderHeight  = H;
    SGBBorderLoadedFromGame = true;
    printf("[SGB] Border captured and tiled (%dx%d)\n", W, H);
}
void systemGbBorderOn() {}

u16  systemColorMap16[0x10000] = {0};
static u32 _colorMap32[0x10000] = {0};
u32 *systemColorMap32 = _colorMap32;
int  systemColorDepth = 16;
int  systemSaveUpdateCounter = 0;
u32  systemGetClock() { return 0; }
bool CalibrateWario = false;
u16  systemGbPalette[24] = {0};