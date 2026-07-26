/****************************************************************************
 * Visual Boy Advance GX
 *
 * Tantric 2008-2023
 *
 * input.h
 *
 * Wii/Gamecube controller management
 ***************************************************************************/

#ifndef _INPUT_H_
#define _INPUT_H_

#include <gccore.h>
#include <wiiuse/wpad.h>
#include "utils/wiidrc.h"

#define PADCAL				50
#define WIIDRCCAL			20
#define MAXJP 				10 // # of mappable controller buttons

#define VBA_BUTTON_A		1
#define VBA_BUTTON_B		2
#define VBA_BUTTON_SELECT	4
#define VBA_BUTTON_START	8
#define VBA_RIGHT			16
#define VBA_LEFT			32
#define VBA_UP				64
#define VBA_DOWN			128
#define VBA_BUTTON_R		256
#define VBA_BUTTON_L		512
#define VBA_SPEED			1024
#define VBA_CAPTURE			2048

extern int rumbleRequest[4];
extern int playerMapping[4];
extern u32 btnmap[6][10];

void ResetControls(int wc = -1);
void ShutoffRumble();
void DoRumble(int i);
void systemGameRumble(int RumbleForFrames);
void systemGameRumbleOnlyFor(int OnlyRumbleForFrames);
void updateRumbleFrame();
u32 GetJoy(int which);
bool MenuRequested();
void SetupPads();
void UpdatePads();

// Called from vbasupport.cpp's mRumbleIntegrator hookup whenever the
// emulated cart's own rumble motor (Pokemon Pinball, Pokemon Pinball:
// Ruby & Sapphire, etc.) turns on/off. Feeds into the existing rumble
// debouncing/timing logic in input.cpp (updateRumble()/DoRumble()).
void systemCartridgeRumble(bool RumbleOn);

// Reads the Wii Remote's current orientation (from the built-in
// accelerometer, already enabled via WPAD_FMT_BTNS_ACC_IR in SetupPads())
// as a tilt reading for games with a built-in tilt/motion sensor cartridge
// - Kirby Tilt 'n' Tumble, Koro Koro Puzzle Happy Panechu, WarioWare
// Twisted, Yoshi Topsy-Turvy. tiltX/tiltY are roughly -90..90 degrees,
// where 0 is the Wii Remote held flat/level.
void GetWiimoteTilt(float *tiltX, float *tiltY);

#endif
