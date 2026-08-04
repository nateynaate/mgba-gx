/****************************************************************************
 * Visual Boy Advance GX
 *
 * Tantric 2008-2023
 *
 * audio.cpp
 *
 * Head and tail audio mixer
 ***************************************************************************/

#include <gccore.h>
#include <ogcsys.h>
#include <ogc/lwp_watchdog.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <asndlib.h>

#include "audio.h"

extern int ConfigRequested;

// Set once per ROM load (see AudioSetPlatform() below, called from
// vbasupport.cpp's LoadMGBAROM()/InitMGBAAudio() using core->platform()).
// Selects which of the two independent driver implementations in this file
// actually gets wired to the hardware DMA callback: GBA keeps the original
// head/tail mixerdata ring below unchanged; GB/GBC uses the newer 16-buffer
// dynamic-rate-control queue (ported from upstream VBA-GX's audio.cpp,
// see the GBC_* block further down) since it directly targets the small
// sustained rate mismatch GB/GBC was measured hitting - continuous gentle
// pitch-bending against a hysteresis threshold, with click-free fades on
// starvation, instead of only reacting after a shortfall already happened.
static bool s_isGBPlatform = false;

/** Locals **/
// head is written by the emulator thread (write) and read by the DMA
// callback (interrupt context); tail is the reverse. They MUST be volatile
// so the compiler does not cache them in registers across the thread/ISR
// boundary, otherwise the consumer never observes the producer's updates.
static volatile int head = 0;
static volatile int tail = 0;
static int gameType = 0;

#define MIXBUFFSIZE 0x10000
// Accessed as u32 below, and (indirectly) feeds DMA, so require 32-byte
// alignment. Without this the u32 casts rely on undefined alignment.
static u8 mixerdata[MIXBUFFSIZE] ATTRIBUTE_ALIGN(32);

u8* GetMixerDataPtr() { return mixerdata; }
volatile int* GetMixerHeadPtr() { return &head; }
volatile int* GetMixerTailPtr() { return &tail; }
#define MIXERMASK ((MIXBUFFSIZE >> 2) - 1)
#define SWAP(x) (((x)>>16) | ((x)<<16)) // for reversing stereo channels

// One DMA frame is 3200 bytes (800 stereo 16-bit frames). The hardware
// buffer is sized to 3840 with 32-byte-aligned length headroom.
#define DMA_BYTES 3200

static u8 soundbuffer[2][3840] ATTRIBUTE_ALIGN(32);
static int whichab = 0;
// Read by the emulator thread (write) and written by the DMA callback.
static volatile int IsPlaying = 0;

// Forward declaration - AudioSetPlatform() (GBC_* block below) needs to
// reference this before its actual definition further down the file.
static void AudioPlayer();

/****************************************************************************
 * GBC_* — GB/GBC-only Dynamic Rate Control driver
 *
 * Ported from upstream VBA-GX's audio.cpp (Daryl Borth). Runs alongside,
 * not instead of, the GBA path above - AudioSetPlatform() at the bottom of
 * this file picks exactly one of the two to actually register as the
 * hardware DMA callback, based on core->platform() at ROM load. GBA never
 * touches any of this block.
 *
 * Upstream's version gets fed directly by VBA's own Sound.cpp mixer via
 * SoundWii::write()/getDynamicRate() - a hook mGBA-GX doesn't have, since
 * PushAudio() (vbasupport.cpp) pulls already-resampled audio out of mGBA's
 * resampler itself rather than being pushed into by the core. So instead
 * of SoundWii::write(), PushAudio() calls GBC_AudioCanWrite() /
 * GBC_AudioGetWriteBuffer() / GBC_AudioCommitWrite() directly once it has
 * drained a DMA_BYTES-sized chunk out of resamplerDest, and multiplies the
 * chunk it's about to commit by GBC_AudioGetDynamicRate() as it goes (see
 * ResampleChunkForDynamicRate() below) - producing the same continuous,
 * hysteresis-gated pitch-bend behavior upstream gets, without ever calling
 * mAudioResamplerSetSource() (i.e. without the per-reset phase discontinuity
 * that was the original source of the GB/GBC crackle).
 ***************************************************************************/

// BUFFERCOUNT must be a power of two so the ring index can advance with a cheap bitwise mask
#define GBC_BUFFERCOUNT 16
#define GBC_MAX_QUEUED_BUFFERS 12 // Leave a 4-buffer safety zone to prevent input lag

/** Dynamic Rate Control (Hysteresis Pitch Bending) **/
#define GBC_UNPLAYED_HIGH_WATER 8         // Above this we are building latency, slow down
#define GBC_UNPLAYED_HIGH_RELEASE 6       // Stay slow until the queue drains back to here
#define GBC_UNPLAYED_LOW_RELEASE 6        // Stay fast until the queue fills back to here
#define GBC_UNPLAYED_LOW_WATER 4          // Below this we risk an underrun, speed up
#define GBC_UNPLAYED_CRITICAL 1           // At/below this we are one stall away from an audible dropout
#define GBC_UNPLAYED_START_LEVEL 6        // Queue at least this many buffers before starting DMA
#define GBC_RATE_SLOW_DOWN 1.005          // Emit samples slightly slower to drain the queue
#define GBC_RATE_SPEED_UP 0.995           // Emit samples slightly faster to fill the queue
#define GBC_RATE_EMERGENCY_SPEED_UP 0.985 // Harder pull-back only when we're on the brink
#define GBC_UNPLAYED_HIGH_CRITICAL 11     // mirrors CRITICAL's 3-buffer margin from MAX_QUEUED_BUFFERS
#define GBC_RATE_EMERGENCY_SLOW_DOWN 1.015
#define GBC_RATE_NEUTRAL 1.0

enum GBCRateState {
	GBC_RATE_STATE_NEUTRAL,
	GBC_RATE_STATE_DRAINING,  // running slow to shrink an over-full queue
	GBC_RATE_STATE_FILLING,   // running fast to grow an under-full queue
};

// Number of stereo frames over which we ramp to/from zero when the ring
// runs genuinely dry (~2ms - long enough to kill the click, short enough
// to stay inaudible as added latency).
#define GBC_FADE_FRAMES 96

static u8 gbcSoundbuffer[GBC_BUFFERCOUNT][DMA_BYTES] ATTRIBUTE_ALIGN(32);
static u8 gbcSilence[DMA_BYTES] ATTRIBUTE_ALIGN(32);
static u8 gbcFadeBuffer[DMA_BYTES] ATTRIBUTE_ALIGN(32);

static volatile int gbcPlayab = 0;
static volatile int gbcNextab = 0;
static bool gbcDmaStarted = false;
static GBCRateState gbcRateState = GBC_RATE_STATE_NEUTRAL;

static s16 gbcLastL = 0;
static s16 gbcLastR = 0;
static bool gbcWasStarved = false;

// TEST: cumulative counters for actual starvation/fade events, exposed via
// GBC_AudioGetFadeStats() below. The periodic "avg dynamic rate" telnet
// line only samples `unplayed` once a second - it can't show brief dips
// to 0 that recover before the next sample. These counters catch every
// single occurrence instead, to check whether GBC_ApplyFadeIn()/the
// fade-out silence substitution are firing far more often than the
// smoothed unplayed log suggests - frequent short (96-frame/~2ms) fade
// ramps could plausibly read as a broadband floor rather than isolated
// clicks. Remove once this hypothesis is checked.
static u32 gbcStarveEvents = 0;   // count of GBC_AudioPlayer() calls where unplayed==0
static u32 gbcFadeInEvents = 0;   // count of GBC_ApplyFadeIn() calls (recovery from starvation)

static inline int GBC_NextIndex(int current) {
	return (current + 1) & (GBC_BUFFERCOUNT - 1);
}

static inline int GBC_GetUnplayed() {
	return (gbcNextab - gbcPlayab + GBC_BUFFERCOUNT) & (GBC_BUFFERCOUNT - 1);
}

static void GBC_BuildFadeOutBuffer()
{
	s16* out = (s16*)gbcFadeBuffer;
	int const frames = DMA_BYTES / 4;
	int const n = (frames < GBC_FADE_FRAMES) ? frames : GBC_FADE_FRAMES;

	for (int i = 0; i < n; i++) {
		out[i * 2]     = (s16)(((s32)gbcLastL * (n - i)) / n);
		out[i * 2 + 1] = (s16)(((s32)gbcLastR * (n - i)) / n);
	}
	for (int i = n; i < frames; i++) {
		out[i * 2] = 0;
		out[i * 2 + 1] = 0;
	}
	DCFlushRange(gbcFadeBuffer, DMA_BYTES);
}

static void GBC_ApplyFadeIn(u8* buf)
{
	s16* s = (s16*)buf;
	int const frames = DMA_BYTES / 4;
	int const n = (frames < GBC_FADE_FRAMES) ? frames : GBC_FADE_FRAMES;

	for (int i = 0; i < n; i++) {
		s[i * 2]     = (s16)(((s32)s[i * 2]     * i) / n);
		s[i * 2 + 1] = (s16)(((s32)s[i * 2 + 1] * i) / n);
	}
	DCFlushRange(buf, DMA_BYTES);
}

// Raw unplayed-buffer count for diagnostics/telnet logging, mirroring the
// GBA path's underrun printf above. Returns -1 before DMA has primed.
int GBC_AudioGetUnplayed()
{
	if (!gbcDmaStarted) return -1;
	return GBC_GetUnplayed();
}

// TEST: cumulative starvation/fade-event counts since boot, for the
// starvation-frequency hypothesis check (see counters' declaration above).
// Not reset between calls - caller (vbasupport.cpp) computes its own
// deltas between prints.
void GBC_AudioGetFadeStats(u32* starveEvents, u32* fadeInEvents)
{
	*starveEvents = gbcStarveEvents;
	*fadeInEvents = gbcFadeInEvents;
}

/****************************************************************************
 * GBC_AudioPlayer (ISR) — hardware DMA callback for GB/GBC, interrupt context
 ***************************************************************************/
static void GBC_AudioPlayer()
{
	if (ConfigRequested) {
		return;
	}

	int unplayed = GBC_GetUnplayed();

	if (unplayed == 0) {
		gbcStarveEvents++;
		if (!gbcWasStarved) {
			GBC_BuildFadeOutBuffer();
			gbcWasStarved = true;
		}
		AUDIO_InitDMA((u32)gbcFadeBuffer, DMA_BYTES);
	}
	else {
		u8* buf = gbcSoundbuffer[gbcPlayab];

		if (gbcWasStarved) {
			gbcFadeInEvents++;
			GBC_ApplyFadeIn(buf);
			gbcWasStarved = false;
		}

		AUDIO_InitDMA((u32)buf, DMA_BYTES);

		s16* s = (s16*)buf;
		int const frames = DMA_BYTES / 4;
		gbcLastL = s[(frames - 1) * 2];
		gbcLastR = s[(frames - 1) * 2 + 1];

		gbcPlayab = GBC_NextIndex(gbcPlayab);
	}
}

/****************************************************************************
 * GBC_AudioStart — reset the ring & hysteresis state cleanly
 ***************************************************************************/
void GBC_AudioStart()
{
	gbcNextab = 0;
	gbcPlayab = 0;
	gbcDmaStarted = false;
	gbcRateState = GBC_RATE_STATE_NEUTRAL;
	gbcWasStarved = false;
	gbcLastL = 0;
	gbcLastR = 0;
}

// Pure capacity query: is there room in the ring for one more buffer?
bool GBC_AudioCanWrite()
{
	if (ConfigRequested) {
		AUDIO_StopDMA();
		GBC_AudioStart();
		return false;
	}
	return GBC_GetUnplayed() < GBC_MAX_QUEUED_BUFFERS;
}

// Hysteresis-gated pitch-bend multiplier - see the #defines above for the
// thresholds. Called once per chunk from PushAudio() (vbasupport.cpp) and
// applied via ResampleChunkForDynamicRate() there, not here, since that's
// where the raw resampler output actually lives.
double GBC_AudioGetDynamicRate()
{
	int unplayed = GBC_GetUnplayed();

	if (gbcRateState == GBC_RATE_STATE_DRAINING && unplayed <= GBC_UNPLAYED_HIGH_RELEASE) {
		gbcRateState = GBC_RATE_STATE_NEUTRAL;
	}
	else if (gbcRateState == GBC_RATE_STATE_FILLING && unplayed >= GBC_UNPLAYED_LOW_RELEASE) {
		gbcRateState = GBC_RATE_STATE_NEUTRAL;
	}

	if (unplayed > GBC_UNPLAYED_HIGH_WATER) {
		gbcRateState = GBC_RATE_STATE_DRAINING;
	}
	else if (unplayed < GBC_UNPLAYED_LOW_WATER) {
		gbcRateState = GBC_RATE_STATE_FILLING;
	}

	if (gbcRateState == GBC_RATE_STATE_DRAINING) {
		return (unplayed >= GBC_UNPLAYED_HIGH_CRITICAL) ? GBC_RATE_EMERGENCY_SLOW_DOWN : GBC_RATE_SLOW_DOWN;
	}
	else if (gbcRateState == GBC_RATE_STATE_FILLING) {
		return (unplayed <= GBC_UNPLAYED_CRITICAL) ? GBC_RATE_EMERGENCY_SPEED_UP : GBC_RATE_SPEED_UP;
	}

	return GBC_RATE_NEUTRAL;
}

u8* GBC_AudioGetWriteBuffer()
{
	return gbcSoundbuffer[gbcNextab];
}

void GBC_AudioCommitWrite()
{
	DCFlushRange(gbcSoundbuffer[gbcNextab], DMA_BYTES);
	gbcNextab = GBC_NextIndex(gbcNextab);

	if (!gbcDmaStarted && GBC_GetUnplayed() >= GBC_UNPLAYED_START_LEVEL)
	{
		AUDIO_InitDMA((u32)gbcSoundbuffer[gbcPlayab], DMA_BYTES);
		gbcPlayab = GBC_NextIndex(gbcPlayab);
		AUDIO_StartDMA();
		gbcDmaStarted = true;
	}
}

/****************************************************************************
 * AudioSetPlatform
 *
 * Called once per ROM load, right after core->platform() is known (see
 * LoadMGBAROM() in vbasupport.cpp). Registers whichever DMA callback
 * actually matches the loaded platform. Must run before SwitchAudioMode(0)
 * / RestartAudioDMA() so the correct callback is already in place when the
 * DMA chain is (re)started.
 ***************************************************************************/
void AudioSetPlatform(bool isGB)
{
	s_isGBPlatform = isGB;
	if (isGB) {
		memset(gbcSilence, 0, DMA_BYTES);
		DCFlushRange(gbcSilence, DMA_BYTES);
		GBC_AudioStart();
	}
	#ifndef NO_SOUND
	AUDIO_RegisterDMACallback(isGB ? GBC_AudioPlayer : AudioPlayer);
	#endif
}

/****************************************************************************
 * MIXER_GetSamples
 *
 * Drains up to maxlen bytes from the ring buffer into dstbuffer. Any space
 * not filled (a buffer underrun) is left as silence by the initial memset.
 * Returns the number of bytes the caller should hand to the DMA engine,
 * which is always a fixed-size, 32-byte-aligned frame.
 ***************************************************************************/
// DIAGNOSTIC: counts underrun events (ring buffer ran dry before filling
// the requested chunk) to test whether this is the source of the reported
// discontinuities/glitches. Printed at most once per ~1 second (not once
// per occurrence - a real underrun period could otherwise fire this every
// callback and flood the console the same way the earlier mLOG spam did).
static u32 s_underrunCount = 0;
static u64 s_lastUnderrunPrint = 0;

static int MIXER_GetSamples(u8 *dstbuffer, int maxlen)
{
	u32 *src = (u32 *)mixerdata;
	u32 *dst = (u32 *)dstbuffer;
	u32 intlen = maxlen >> 2;
	u32 requested = intlen;

	memset(dstbuffer, 0, maxlen);

	// Snapshot the producer index once. head is volatile and updated from
	// the emulator thread; re-reading it in the loop condition could let the
	// consumer chase a moving target and over-read.
	int producer = head;

	while ( ( producer != tail ) && intlen )
	{
		*dst++ = src[tail];
		tail = (tail + 1) & MIXERMASK;
		intlen--;
	}

	// intlen > 0 here means the ring buffer ran dry before this DMA chunk
	// was fully filled - the remainder stays as the silence the initial
	// memset already wrote. Silence-then-signal (or signal-then-silence)
	// right at this boundary is exactly a discontinuity.
	if (intlen > 0)
	{
		s_underrunCount++;
		u64 now = gettime();
		if (s_lastUnderrunPrint == 0 || ticks_to_microsecs(now - s_lastUnderrunPrint) > 1000000)
		{
			printf("[audio] UNDERRUN: short by %lu/%lu words, total=%lu\n",
				(unsigned long)intlen, (unsigned long)requested, (unsigned long)s_underrunCount);
			s_lastUnderrunPrint = now;
		}
	}

	return maxlen;
}

/****************************************************************************
 * AudioPlayer
 ***************************************************************************/

static void AudioPlayer()
{
	if (!ConfigRequested)
	{
		whichab ^= 1;
		int len = MIXER_GetSamples(soundbuffer[whichab], DMA_BYTES);
		DCFlushRange(soundbuffer[whichab],len);
		AUDIO_InitDMA((u32)soundbuffer[whichab],len);
		IsPlaying = 1;
	}
	else
		IsPlaying = 0;
}

/****************************************************************************
 * RestartAudioDMA
 *
 * Lightweight re-prime of the DMA chain for use by PushAudio()
 * (vbasupport.cpp) after SwitchAudioMode(1) has handed the audio hardware
 * back to ASND (menu mode). Unlike SwitchAudioMode(0) this doesn't tear
 * down ASND — by the time PushAudio() is running, the emulator loop is
 * already active, so if ASND is still alive we just need to stop it cleanly
 * and hand the AI back to the DMA path. Mirrors the restart logic that
 * SoundWii::write() used to do inline:
 *   if (IsPlaying == 0) { ConfigRequested = 0; AudioPlayer(); }
 * but with a full re-init of the callback and DMA since SwitchAudioMode(1)
 * may have torn them down.
 ***************************************************************************/
void RestartAudioDMA()
{
	if (IsPlaying)
		return; // DMA chain is already running, nothing to do

	// ConfigRequested gates AudioPlayer() (the DMA callback) - see that
	// function above. It gets set to 1 in a few places (e.g. video.cpp,
	// after a screenshot) as a "menu/config UI wants the audio hardware"
	// signal, and needs to be cleared here so the DMA-driven emulator
	// audio path can actually resume. This used to only be cleared inside
	// the old VBA-era SoundWii::write() function, which nothing calls
	// anymore now that audio is fed via PushAudio() (vbasupport.cpp) -
	// meaning ConfigRequested could get set once and then never cleared
	// for the rest of the session, permanently silencing audio (the DMA
	// callback would immediately re-disable itself on every subsequent
	// tick even after this function re-armed IsPlaying). Clearing it here
	// instead, in the function that's actually the live "resume real
	// audio playback" entry point, fixes that for good.
	ConfigRequested = 0;

	// Mirror SwitchAudioMode(0)'s full sequence - see that function's own
	// comment for why AUDIO_Init + AUDIO_SetDSPSampleRate are both needed.
	#ifndef NO_SOUND
	ASND_Pause(1);
	ASND_End();
	AUDIO_StopDMA();
	AUDIO_RegisterDMACallback(NULL);
	DSP_Halt();
	AUDIO_Init(NULL);
	AUDIO_SetDSPSampleRate(AI_SAMPLERATE_48KHZ);
	AUDIO_RegisterDMACallback(s_isGBPlatform ? GBC_AudioPlayer : AudioPlayer);
	#endif

	if (s_isGBPlatform) {
		// GBC's DMA chain is primed by GBC_AudioCommitWrite() itself once
		// enough buffers are queued (GBC_UNPLAYED_START_LEVEL) - starting
		// it here with nothing queued would just play silence/garbage.
		// AUDIO_StartDMA() is intentionally NOT called in this branch.
		GBC_AudioStart();
		IsPlaying = 1; // "the DMA path is active", not "already spinning" - guards re-entry above
		return;
	}

	memset(soundbuffer[0],0,3840);
	memset(soundbuffer[1],0,3840);
	DCFlushRange(soundbuffer[0],3840);
	DCFlushRange(soundbuffer[1],3840);
	AUDIO_InitDMA((u32)soundbuffer[whichab],3200);
	AUDIO_StartDMA();
	IsPlaying = 1;
}

/****************************************************************************
 * StopAudio
 ***************************************************************************/

void StopAudio()
{
	AUDIO_StopDMA();
	IsPlaying = 0;
	gbcDmaStarted = false;
}

/****************************************************************************
 * SetAudioRate
 ***************************************************************************/

void SetAudioRate(int type)
{
	gameType = type;
}

/****************************************************************************
 * SwitchAudioMode
 *
 * Switches between menu sound and emulator sound
 ***************************************************************************/
void
SwitchAudioMode(int mode)
{
	if(mode == 0) // emulator
	{
		// See RestartAudioDMA()'s comment on why this needs clearing here
		// too - this is the other real entry point back into DMA-driven
		// emulator audio (called by InitMGBAAudio() on every ROM load).
		ConfigRequested = 0;

		#ifndef NO_SOUND
		ASND_Pause(1);
		ASND_End();
		AUDIO_StopDMA();
		AUDIO_RegisterDMACallback(NULL);
		DSP_Halt();
		// Re-initialize the AI hardware after ASND tears it down. ASND_End()
		// leaves the AI in an indeterminate state on real Wii hardware
		// (Dolphin is more forgiving). The NO_SOUND path in InitialiseSound()
		// already shows the correct complete sequence for raw DMA mode:
		// AUDIO_Init → AUDIO_SetDSPSampleRate → register callback. Without
		// both of these calls after ASND_End(), the DMA chain starts but the
		// hardware plays silence on real hardware.
		AUDIO_Init(NULL);
		AUDIO_SetDSPSampleRate(AI_SAMPLERATE_48KHZ);
		AUDIO_RegisterDMACallback(s_isGBPlatform ? GBC_AudioPlayer : AudioPlayer);
		#endif

		if (s_isGBPlatform) {
			// See RestartAudioDMA()'s matching branch - GBC_AudioCommitWrite()
			// starts the DMA chain itself once it has enough buffers queued.
			GBC_AudioStart();
			IsPlaying = 1;
			return;
		}

		memset(soundbuffer[0],0,3840);
		memset(soundbuffer[1],0,3840);
		DCFlushRange(soundbuffer[0],3840);
		DCFlushRange(soundbuffer[1],3840);
		AUDIO_InitDMA((u32)soundbuffer[whichab],3200);
		AUDIO_StartDMA();
		IsPlaying = 1;
	}
	else // menu
	{
		IsPlaying = 0;
		#ifndef NO_SOUND
		AUDIO_StopDMA();
		AUDIO_RegisterDMACallback(NULL);
		DSP_Unhalt();
		ASND_Init();
		ASND_Pause(0);
		#else
		AUDIO_StopDMA();
		#endif
	}
}

/****************************************************************************
 * InitialiseSound
 ***************************************************************************/

void InitialiseSound()
{
	#ifdef NO_SOUND
	AUDIO_Init (NULL);
	AUDIO_SetDSPSampleRate(AI_SAMPLERATE_48KHZ);
	AUDIO_RegisterDMACallback(AudioPlayer);
	#else
	ASND_Init();
	#endif
}

/****************************************************************************
 * ShutdownAudio
 *
 * Shuts down audio subsystem. Useful to avoid unpleasant sounds if a
 * crash occurs during shutdown.
 ***************************************************************************/
void ShutdownAudio()
{
	AUDIO_StopDMA();
}

/****************************************************************************
 * SoundDriver
 ***************************************************************************/

SoundWii::SoundWii()
{
	memset(soundbuffer, 0, 3840*2);
	memset(mixerdata, 0, MIXBUFFSIZE);
}

/****************************************************************************
* SoundWii::write  —  VESTIGIAL / DEAD CODE, kept only for link compatibility
*
* This was the old VBA-era audio path: a fixed-point nearest-neighbor
* resample (despite the "linear interpolate" comment it used to carry, it
* never blended between samples - it snapped fixofs>>16 straight to the
* nearest source index) feeding this file's own mixerdata ring directly.
*
* Nothing calls this anymore. Real audio output today goes through mGBA's
* own resampler (mAudioResampler, mINTERPOLATOR_COSINE - the same
* interpolator mGBA's own official Wii port uses) in PushAudio()
* (vbasupport.cpp), which writes into this same mixerdata ring via
* GetMixerHeadPtr()/GetMixerTailPtr() instead of going through this method.
* That's a strictly higher-quality resample than what this function ever
* did, so there's no audio-quality reason to keep this implementation
* around - it's left as a documented no-op rather than deleted outright
* since audio.h (not in hand at the time of this edit) may still declare
* SoundWii::write() as part of the class, and something elsewhere may still
* reference the type even though nothing calls this function in practice.
* If a future pass confirms no remaining references to the SoundWii class
* at all, this method (and the class) can be removed entirely.
****************************************************************************/

void SoundWii::write(u16 * finalWave, int length)
{
	(void)finalWave;
	(void)length;
}

bool SoundWii::init(long sampleRate)
{
	return true;
}

SoundWii::~SoundWii()
{
}

void SoundWii::pause()
{
}

void SoundWii::resume()
{
}

void SoundWii::reset()
{
}
