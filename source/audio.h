/****************************************************************************
 * Visual Boy Advance GX
 *
 * Tantric 2008-2023
 *
 * audio.h
 *
 * Head and tail audio mixer
 ***************************************************************************/

#ifndef __AUDIOMIXER__
#define __AUDIOMIXER__

#include "vba/common/SoundDriver.h"

void InitialiseSound();
void StopAudio();
void SetAudioRate(int type);
void SwitchAudioMode(int mode);
void ShutdownAudio();
void RestartAudioDMA();

// Called once per ROM load (see LoadMGBAROM() in vbasupport.cpp) once
// core->platform() is known, to select which DMA driver is actually wired
// up: the original GBA head/tail ring, or the GB/GBC dynamic-rate-control
// queue ported from upstream VBA-GX. See audio.cpp for the full picture.
void AudioSetPlatform(bool isGB);

// GB/GBC-only producer API, used from PushAudio() (vbasupport.cpp) in
// place of the GBA path's GetMixerHeadPtr()/GetMixerTailPtr() ring push.
// Mirrors the shape of VBA-GX's SoundWii::canWrite()/getWriteBuffer()/
// commitWrite()/getDynamicRate(), since PushAudio() doesn't go through the
// SoundDriver interface at all - see the GBC_* block in audio.cpp.
bool   GBC_AudioCanWrite();
u8*    GBC_AudioGetWriteBuffer();
void   GBC_AudioCommitWrite();
double GBC_AudioGetDynamicRate();
void   GBC_AudioStart();
int    GBC_AudioGetUnplayed(); // -1 before DMA has primed
void   GBC_AudioGetFadeStats(u32* starveEvents, u32* fadeInEvents); // TEST diagnostic - see audio.cpp

class SoundWii: public SoundDriver
{
public:
	SoundWii();
	virtual ~SoundWii();

	virtual bool init(long sampleRate);
	virtual void pause();
	virtual void reset();
	virtual void resume();
	virtual void write(u16 * finalWave, int length);
};

#endif
