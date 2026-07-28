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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <asndlib.h>

#include "audio.h"

extern int ConfigRequested;

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

/****************************************************************************
 * MIXER_GetSamples
 *
 * Drains up to maxlen bytes from the ring buffer into dstbuffer. Any space
 * not filled (a buffer underrun) is left as silence by the initial memset.
 * Returns the number of bytes the caller should hand to the DMA engine,
 * which is always a fixed-size, 32-byte-aligned frame.
 ***************************************************************************/
static int MIXER_GetSamples(u8 *dstbuffer, int maxlen)
{
	u32 *src = (u32 *)mixerdata;
	u32 *dst = (u32 *)dstbuffer;
	u32 intlen = maxlen >> 2;

	memset(dstbuffer, 0, maxlen);

	// Snapshot the producer index once. head is volatile and updated from
	// the emulator thread; re-reading it in the loop condition could let the
	// consumer chase a moving target and over-read.
	int producer = head;

	while ( ( producer != tail ) && intlen )
	{
		*dst++ = src[tail++];
		tail &= MIXERMASK;
		intlen--;
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
	AUDIO_RegisterDMACallback(AudioPlayer);
	#endif
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
		AUDIO_RegisterDMACallback(AudioPlayer);
		#endif
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
