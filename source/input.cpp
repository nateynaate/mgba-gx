/****************************************************************************
 * Visual Boy Advance GX
 *
 * Tantric 2008-2023
 *
 * input.cpp
 *
 * Wii/Gamecube controller management
 ***************************************************************************/

#include <gccore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ogcsys.h>
#include <unistd.h>
#include <wiiuse/wpad.h>
#include <ogc/lwp_watchdog.h>

#include "vbagx.h"
#include "button_mapping.h"
#include "audio.h"
#include "video.h"
#include "input.h"
#include "gameinput.h"
#include "vbasupport.h"
#include "fastmath.h"
#include "gui/gui.h"

// Note: VBA_BUTTON_A/B/SELECT/START/R/L are already defined in input.h -
// no need to redefine them here.

#define ANALOG_SENSITIVITY 30

int rumbleRequest[4] = {0,0,0,0};
int playerMapping[4] = {0,1,2,3};
GuiTrigger userInput[4];

#ifdef HW_RVL
static int rumbleCount[4] = {0,0,0,0};
#endif

static bool cartridgeRumble = false, possibleCartridgeRumble = false;
static int gameRumbleCount = 0, menuRumbleCount = 0, rumbleCountAlready = 0;

static unsigned int vbapadmap[10]; // VBA controller buttons
u32 btnmap[6][10]; // button mapping
u32 ffmap[6]; // per-controller "hold to fast-forward" button - see vbagx.h for why this is separate from btnmap

void ResetControls(int wiiCtrl)
{
	int i;

	// VBA controller buttons
	// All other pads are mapped to this
	i=0;
	vbapadmap[i++] = VBA_BUTTON_B;
	vbapadmap[i++] = VBA_BUTTON_A;
	vbapadmap[i++] = VBA_BUTTON_SELECT;
	vbapadmap[i++] = VBA_BUTTON_START;
	vbapadmap[i++] = VBA_UP;
	vbapadmap[i++] = VBA_DOWN;
	vbapadmap[i++] = VBA_LEFT;
	vbapadmap[i++] = VBA_RIGHT;
	vbapadmap[i++] = VBA_BUTTON_L;
	vbapadmap[i++] = VBA_BUTTON_R;

	/*** Gamecube controller Padmap ***/
	if(wiiCtrl == CTRLR_GCPAD || wiiCtrl == -1)
	{
		i=0;
		btnmap[CTRLR_GCPAD][i++] = PAD_BUTTON_B;
		btnmap[CTRLR_GCPAD][i++] = PAD_BUTTON_A;
		btnmap[CTRLR_GCPAD][i++] = PAD_TRIGGER_Z;
		btnmap[CTRLR_GCPAD][i++] = PAD_BUTTON_START;
		btnmap[CTRLR_GCPAD][i++] = PAD_BUTTON_UP;
		btnmap[CTRLR_GCPAD][i++] = PAD_BUTTON_DOWN;
		btnmap[CTRLR_GCPAD][i++] = PAD_BUTTON_LEFT;
		btnmap[CTRLR_GCPAD][i++] = PAD_BUTTON_RIGHT;
		btnmap[CTRLR_GCPAD][i++] = PAD_TRIGGER_L;
		btnmap[CTRLR_GCPAD][i++] = PAD_TRIGGER_R;
	}

	/*** Wiimote Padmap ***/
	if(wiiCtrl == CTRLR_WIIMOTE || wiiCtrl == -1)
	{
		i=0;
		btnmap[CTRLR_WIIMOTE][i++] = WPAD_BUTTON_1;
		btnmap[CTRLR_WIIMOTE][i++] = WPAD_BUTTON_2;
		btnmap[CTRLR_WIIMOTE][i++] = WPAD_BUTTON_MINUS;
		btnmap[CTRLR_WIIMOTE][i++] = WPAD_BUTTON_PLUS;
		btnmap[CTRLR_WIIMOTE][i++] = WPAD_BUTTON_RIGHT;
		btnmap[CTRLR_WIIMOTE][i++] = WPAD_BUTTON_LEFT;
		btnmap[CTRLR_WIIMOTE][i++] = WPAD_BUTTON_UP;
		btnmap[CTRLR_WIIMOTE][i++] = WPAD_BUTTON_DOWN;
		btnmap[CTRLR_WIIMOTE][i++] = WPAD_BUTTON_B;
		btnmap[CTRLR_WIIMOTE][i++] = WPAD_BUTTON_A;
		ffmap[CTRLR_WIIMOTE] = 0; // unassigned by default - see vbagx.h comment on ffmap
	}

	/*** Classic Controller Padmap ***/
	if(wiiCtrl == CTRLR_CLASSIC || wiiCtrl == -1)
	{
		i=0;
		btnmap[CTRLR_CLASSIC][i++] = WPAD_CLASSIC_BUTTON_Y;
		btnmap[CTRLR_CLASSIC][i++] = WPAD_CLASSIC_BUTTON_B;
		btnmap[CTRLR_CLASSIC][i++] = WPAD_CLASSIC_BUTTON_MINUS;
		btnmap[CTRLR_CLASSIC][i++] = WPAD_CLASSIC_BUTTON_PLUS;
		btnmap[CTRLR_CLASSIC][i++] = WPAD_CLASSIC_BUTTON_UP;
		btnmap[CTRLR_CLASSIC][i++] = WPAD_CLASSIC_BUTTON_DOWN;
		btnmap[CTRLR_CLASSIC][i++] = WPAD_CLASSIC_BUTTON_LEFT;
		btnmap[CTRLR_CLASSIC][i++] = WPAD_CLASSIC_BUTTON_RIGHT;
		btnmap[CTRLR_CLASSIC][i++] = WPAD_CLASSIC_BUTTON_FULL_L;
		btnmap[CTRLR_CLASSIC][i++] = WPAD_CLASSIC_BUTTON_FULL_R;
		ffmap[CTRLR_CLASSIC] = 0; // unassigned by default - see vbagx.h comment on ffmap
	}

	/*** Nunchuk + wiimote Padmap ***/
	if(wiiCtrl == CTRLR_NUNCHUK || wiiCtrl == -1)
	{
		i=0;
		btnmap[CTRLR_NUNCHUK][i++] = WPAD_NUNCHUK_BUTTON_C;
		btnmap[CTRLR_NUNCHUK][i++] = WPAD_NUNCHUK_BUTTON_Z;
		btnmap[CTRLR_NUNCHUK][i++] = WPAD_BUTTON_MINUS;
		btnmap[CTRLR_NUNCHUK][i++] = WPAD_BUTTON_PLUS;
		btnmap[CTRLR_NUNCHUK][i++] = WPAD_BUTTON_UP;
		btnmap[CTRLR_NUNCHUK][i++] = WPAD_BUTTON_DOWN;
		btnmap[CTRLR_NUNCHUK][i++] = WPAD_BUTTON_LEFT;
		btnmap[CTRLR_NUNCHUK][i++] = WPAD_BUTTON_RIGHT;
		btnmap[CTRLR_NUNCHUK][i++] = WPAD_BUTTON_2;
		btnmap[CTRLR_NUNCHUK][i++] = WPAD_BUTTON_1;
		ffmap[CTRLR_NUNCHUK] = 0; // unassigned by default - see vbagx.h comment on ffmap
	}
	
	/*** Wii U Pro Padmap ***/
	if(wiiCtrl == CTRLR_WUPC || wiiCtrl == -1)
	{
		i=0;
		btnmap[CTRLR_WUPC][i++] = WPAD_CLASSIC_BUTTON_Y;
		btnmap[CTRLR_WUPC][i++] = WPAD_CLASSIC_BUTTON_B;
		btnmap[CTRLR_WUPC][i++] = WPAD_CLASSIC_BUTTON_MINUS;
		btnmap[CTRLR_WUPC][i++] = WPAD_CLASSIC_BUTTON_PLUS;
		btnmap[CTRLR_WUPC][i++] = WPAD_CLASSIC_BUTTON_UP;
		btnmap[CTRLR_WUPC][i++] = WPAD_CLASSIC_BUTTON_DOWN;
		btnmap[CTRLR_WUPC][i++] = WPAD_CLASSIC_BUTTON_LEFT;
		btnmap[CTRLR_WUPC][i++] = WPAD_CLASSIC_BUTTON_RIGHT;
		btnmap[CTRLR_WUPC][i++] = WPAD_CLASSIC_BUTTON_FULL_L;
		btnmap[CTRLR_WUPC][i++] = WPAD_CLASSIC_BUTTON_FULL_R;
		ffmap[CTRLR_WUPC] = 0; // unassigned by default - see vbagx.h comment on ffmap
	}
	
	/*** Wii U Gamepad Padmap ***/
	if(wiiCtrl == CTRLR_WIIDRC || wiiCtrl == -1)
	{
		i=0;
		btnmap[CTRLR_WIIDRC][i++] = WIIDRC_BUTTON_Y;
		btnmap[CTRLR_WIIDRC][i++] = WIIDRC_BUTTON_B;
		btnmap[CTRLR_WIIDRC][i++] = WIIDRC_BUTTON_MINUS;
		btnmap[CTRLR_WIIDRC][i++] = WIIDRC_BUTTON_PLUS;
		btnmap[CTRLR_WIIDRC][i++] = WIIDRC_BUTTON_UP;
		btnmap[CTRLR_WIIDRC][i++] = WIIDRC_BUTTON_DOWN;
		btnmap[CTRLR_WIIDRC][i++] = WIIDRC_BUTTON_LEFT;
		btnmap[CTRLR_WIIDRC][i++] = WIIDRC_BUTTON_RIGHT;
		btnmap[CTRLR_WIIDRC][i++] = WIIDRC_BUTTON_L;
		btnmap[CTRLR_WIIDRC][i++] = WIIDRC_BUTTON_R;
		ffmap[CTRLR_WIIDRC] = 0; // unassigned by default - see vbagx.h comment on ffmap
	}
}

/****************************************************************************
 * UpdatePads
 *
 * Scans pad and wpad
 ***************************************************************************/

void
UpdatePads()
{
	#ifdef HW_RVL
	WiiDRC_ScanPads();
	WPAD_ScanPads();
	for(int i=0;i<4;i++)
{
    userInput[i].wpad = WPAD_Data(i);
}
	#endif

	PAD_ScanPads();

	for(int i=3; i >= 0; i--)
	{
		userInput[i].pad.btns_d = PAD_ButtonsDown(i);
		userInput[i].pad.btns_u = PAD_ButtonsUp(i);
		userInput[i].pad.btns_h = PAD_ButtonsHeld(i);
		userInput[i].pad.stickX = PAD_StickX(i);
		userInput[i].pad.stickY = PAD_StickY(i);
		userInput[i].pad.substickX = PAD_SubStickX(i);
		userInput[i].pad.substickY = PAD_SubStickY(i);
		userInput[i].pad.triggerL = PAD_TriggerL(i);
		userInput[i].pad.triggerR = PAD_TriggerR(i);
	}
#ifdef HW_RVL
	if(WiiDRC_Inited() && WiiDRC_Connected())
	{
		userInput[0].wiidrcdata.btns_d = WiiDRC_ButtonsDown();
		userInput[0].wiidrcdata.btns_u = WiiDRC_ButtonsUp();
		userInput[0].wiidrcdata.btns_h = WiiDRC_ButtonsHeld();
		userInput[0].wiidrcdata.stickX = WiiDRC_lStickX();
		userInput[0].wiidrcdata.stickY = WiiDRC_lStickY();
		userInput[0].wiidrcdata.substickX = WiiDRC_rStickX();
		userInput[0].wiidrcdata.substickY = WiiDRC_rStickY();
	}
#endif
}

/****************************************************************************
 * GetWiimoteTilt
 *
 * Returns the Wii Remote's current pitch/roll (in degrees, ~-90..90, 0 =
 * held flat/level) from its built-in accelerometer. wiiuse computes
 * wpad->orient every time WPAD data is read, as long as WPAD_FMT_BTNS_ACC_IR
 * (set once in SetupPads()) is active - no extra polling call is needed
 * here.
 ***************************************************************************/
void
GetWiimoteTilt(float *tiltX, float *tiltY)
{
	*tiltX = 0.0f;
	*tiltY = 0.0f;
#ifdef HW_RVL
	// libogc's own wpad.c only updates wpad->orient when err == WPAD_ERR_NONE
	// (it early-returns before the orient assignment otherwise) - that's the
	// correct freshness check, not data_present, which is a bitmask of which
	// optional data types decoded in a given packet and isn't a reliable
	// signal that orientation data is valid.
	if(userInput[0].wpad && userInput[0].wpad->err == WPAD_ERR_NONE)
	{
		*tiltX = userInput[0].wpad->orient.roll;
		*tiltY = userInput[0].wpad->orient.pitch;
	}
#endif
}

/****************************************************************************
 * SetupPads
 *
 * Sets up userInput triggers for use
 ***************************************************************************/
void
SetupPads()
{
	PAD_Init();

	#ifdef HW_RVL
	// read wiimote accelerometer and IR data
	WPAD_SetDataFormat(WPAD_CHAN_ALL,WPAD_FMT_BTNS_ACC_IR);
	WPAD_SetVRes(WPAD_CHAN_ALL, screenwidth, screenheight);
	#endif

	for(int i=0; i < 4; i++)
	{
		userInput[i].chan = i;
		#ifdef HW_RVL
		userInput[i].wpad = WPAD_Data(i);
		#endif
	}
}

/****************************************************************************
 * ShutoffRumble
 ***************************************************************************/
void ShutoffRumble()
{
#ifdef HW_RVL
	if(CONF_GetPadMotorMode() == 0)
		return;
	for(int i=0;i<4;i++)
	{
		WPAD_Rumble(i, 0);
		rumbleCount[i] = 0;
		rumbleRequest[i] = 0;
	}
#endif
	PAD_ControlMotor(PAD_CHAN0, PAD_MOTOR_STOP);
}

/****************************************************************************
 * DoRumble
 ***************************************************************************/
void DoRumble(int i)
{
#ifdef HW_RVL
	if(CONF_GetPadMotorMode() == 0 || !GCSettings.Rumble) return;

	if(rumbleRequest[i] && rumbleCount[i] < 3)
	{
		WPAD_Rumble(i, 1); // rumble on
		rumbleCount[i]++;
	}
	else if(rumbleRequest[i])
	{
		rumbleCount[i] = 12;
		rumbleRequest[i] = 0;
	}
	else
	{
		if(rumbleCount[i])
			rumbleCount[i]--;
		WPAD_Rumble(i, 0); // rumble off
	}
#endif
}

static int SilenceNeeded = 0;

static void updateRumble()
{
	if(!GCSettings.Rumble) return;

	bool r = false;
	if (ConfigRequested) r = (menuRumbleCount > 0);
	else r = cartridgeRumble || possibleCartridgeRumble || (gameRumbleCount > 0) || (menuRumbleCount > 0);

	if (SilenceNeeded > 0)
	{
		if (r)
		{
			SilenceNeeded = 5;
			// It will always be greater than 0 after that!
			r = false;
		}
		else
		{
			if (--SilenceNeeded > 0) r = false;
		}
	}

#ifdef HW_RVL
	// Rumble wii remote 0
	WPAD_Rumble(0, r);
#endif
	PAD_ControlMotor(PAD_CHAN0, r?PAD_MOTOR_RUMBLE:PAD_MOTOR_STOP);
}

void updateRumbleFrame()
{
	if(!GCSettings.Rumble) return;

	// If we already rumbled continuously for more than 50 frames,
	// then disable rumbling for a while.
	if (rumbleCountAlready > 70) {
		SilenceNeeded = 5;
		rumbleCountAlready = 0;
	} else if (ConfigRequested) {
		if (menuRumbleCount>0) ++rumbleCountAlready;
		else rumbleCountAlready=0;
	} else {
		if (gameRumbleCount>0 || menuRumbleCount>0 || possibleCartridgeRumble)
			++rumbleCountAlready;
		else rumbleCountAlready=0;
	}
	updateRumble();
	if (gameRumbleCount>0 && !ConfigRequested) --gameRumbleCount;
	if (menuRumbleCount>0) --menuRumbleCount;
}

void systemPossibleCartridgeRumble(bool RumbleOn) {
	possibleCartridgeRumble = RumbleOn;
	updateRumble();
}

void systemCartridgeRumble(bool RumbleOn) {
	cartridgeRumble = RumbleOn;
	possibleCartridgeRumble = false;
	updateRumble();
}

void systemGameRumble(int RumbleForFrames) {
	if (RumbleForFrames > gameRumbleCount) gameRumbleCount = RumbleForFrames;
}

void systemGameRumbleOnlyFor(int OnlyRumbleForFrames) {
	gameRumbleCount = OnlyRumbleForFrames;
}

u32 StandardMovement(unsigned short chan)
{
	u32 J = 0;

	s8 pad_x = userInput[chan].pad.stickX;
	s8 pad_y = userInput[chan].pad.stickY;
	#ifdef HW_RVL
	s8 wm_ax = userInput[0].WPAD_StickX(0);
	s8 wm_ay = userInput[0].WPAD_StickY(0);
	s16 wiidrc_ax = userInput[chan].wiidrcdata.stickX;
	s16 wiidrc_ay = userInput[chan].wiidrcdata.stickY;
	#endif
	
	/***
	Gamecube Joystick input, same as normal
	***/
	if (pad_y > ANALOG_SENSITIVITY)
		J |= VBA_UP;
	else if (pad_y < -ANALOG_SENSITIVITY)
		J |= VBA_DOWN;
	if (pad_x < -ANALOG_SENSITIVITY)
		J |= VBA_LEFT;
	else if (pad_x > ANALOG_SENSITIVITY)
		J |= VBA_RIGHT;
#ifdef HW_RVL
	/***
	Wii Joystick (classic, nunchuk) input
	***/
	if (wm_ay > ANALOG_SENSITIVITY)
		J |= VBA_UP;
	else if (wm_ay < -ANALOG_SENSITIVITY)
		J |= VBA_DOWN;
	if (wm_ax < -ANALOG_SENSITIVITY)
		J |= VBA_LEFT;
	else if (wm_ax > ANALOG_SENSITIVITY)
		J |= VBA_RIGHT;

	/* Wii U Gamepad */
	if (wiidrc_ay > ANALOG_SENSITIVITY)
		J |= VBA_UP;
	else if (wiidrc_ay < -ANALOG_SENSITIVITY)
		J |= VBA_DOWN;
	if (wiidrc_ax < -ANALOG_SENSITIVITY)
		J |= VBA_LEFT;
	else if (wiidrc_ax > ANALOG_SENSITIVITY)
		J |= VBA_RIGHT;
#endif
	return J;
}

u32 StandardDPad(unsigned short pad)
{
	u32 J = 0;
	u32 jp = userInput[pad].pad.btns_h;
#ifdef HW_RVL
	u32 exp_type;
	if ( WPAD_Probe(pad, &exp_type) != 0 )
		exp_type = WPAD_EXP_NONE;
	u32 wp = userInput[pad].wpad->btns_h;
	if (wp & WPAD_BUTTON_RIGHT)
		J |= VBA_RIGHT;
	else if (wp & WPAD_BUTTON_LEFT)
		J |= VBA_LEFT;
	if (wp & WPAD_BUTTON_UP)
		J |= VBA_UP;
	else if (wp & WPAD_BUTTON_DOWN)
		J |= VBA_DOWN;
	if (exp_type == WPAD_EXP_CLASSIC)
	{
		if (wp & WPAD_CLASSIC_BUTTON_UP)
			J |= VBA_UP;
		else if (wp & WPAD_CLASSIC_BUTTON_DOWN)
			J |= VBA_DOWN;
		if (wp & WPAD_CLASSIC_BUTTON_LEFT)
			J |= VBA_LEFT;
		else if (wp & WPAD_CLASSIC_BUTTON_RIGHT)
			J |= VBA_RIGHT;
	}
	/* Wii U Gamepad */
	u32 wiidrcp = userInput[pad].wiidrcdata.btns_h;
	if (wiidrcp & WIIDRC_BUTTON_UP)
		J |= VBA_UP;
	else if (wiidrcp & WIIDRC_BUTTON_DOWN)
		J |= VBA_DOWN;
	if (wiidrcp & WIIDRC_BUTTON_LEFT)
		J |= VBA_LEFT;
	else if (wiidrcp & WIIDRC_BUTTON_RIGHT)
		J |= VBA_RIGHT;
#endif
	if (jp & PAD_BUTTON_UP)
		J |= VBA_UP;
	else if (jp & PAD_BUTTON_DOWN)
		J |= VBA_DOWN;
	if (jp & PAD_BUTTON_LEFT)
		J |= VBA_LEFT;
	else if (jp & PAD_BUTTON_RIGHT)
		J |= VBA_RIGHT;
	return J;
}

u32 StandardSideways(unsigned short pad)
{
	u32 J = 0;
#ifdef HW_RVL
	u32 wp = userInput[pad].wpad->btns_h;

	if (wp & WPAD_BUTTON_RIGHT)
		J |= VBA_UP;
	else if (wp & WPAD_BUTTON_LEFT)
		J |= VBA_DOWN;
	if (wp & WPAD_BUTTON_UP)
		J |= VBA_LEFT;
	else if (wp & WPAD_BUTTON_DOWN)
		J |= VBA_RIGHT;

	if (wp & WPAD_BUTTON_PLUS)
		J |= VBA_BUTTON_START;
	if (wp & WPAD_BUTTON_MINUS)
		J |= VBA_BUTTON_SELECT;

	if (wp & WPAD_BUTTON_1)
		J |= VBA_BUTTON_B;
	if (wp & WPAD_BUTTON_2)
		J |= VBA_BUTTON_A;
	if (cartridgeType == CARTRIDGE_GBA)
	{
		if (wp & WPAD_BUTTON_A)
			J |= VBA_BUTTON_R;
		if (wp & WPAD_BUTTON_B)
			J |= VBA_BUTTON_L;
	}
	else
	{
		if (wp & WPAD_BUTTON_B || wp & WPAD_BUTTON_A)
			J |= VBA_SPEED;
	}
#endif
	return J;
}

u32 StandardClassic(unsigned short pad)
{
	u32 J = 0;
#ifdef HW_RVL
	u32 wp = userInput[pad].wpad->btns_h;

	if (wp & WPAD_CLASSIC_BUTTON_RIGHT)
		J |= VBA_RIGHT;
	else if (wp & WPAD_CLASSIC_BUTTON_LEFT)
		J |= VBA_LEFT;
	if (wp & WPAD_CLASSIC_BUTTON_UP)
		J |= VBA_UP;
	else if (wp & WPAD_CLASSIC_BUTTON_DOWN)
		J |= VBA_DOWN;

	if (wp & WPAD_CLASSIC_BUTTON_PLUS)
		J |= VBA_BUTTON_START;
	if (wp & WPAD_CLASSIC_BUTTON_MINUS)
		J |= VBA_BUTTON_SELECT;

	if (wp & WPAD_CLASSIC_BUTTON_FULL_L || wp & WPAD_CLASSIC_BUTTON_ZL)
		J |= VBA_BUTTON_L;
	if (wp & WPAD_CLASSIC_BUTTON_FULL_R || wp & WPAD_CLASSIC_BUTTON_ZR)
		J |= VBA_BUTTON_R;

	if (wp & WPAD_CLASSIC_BUTTON_A)
		J |= VBA_BUTTON_A;
	if (wp & WPAD_CLASSIC_BUTTON_B)
		J |= VBA_BUTTON_B;
	if (wp & WPAD_CLASSIC_BUTTON_Y || wp & WPAD_CLASSIC_BUTTON_X)
		J |= VBA_SPEED;
#endif
	return J;
}

u32 StandardGamecube(unsigned short pad)
{
	u32 J = 0;
	u32 jp = userInput[pad].pad.btns_h;
	if (jp & PAD_BUTTON_UP)
		J |= VBA_UP;
	else if (jp & PAD_BUTTON_DOWN)
		J |= VBA_DOWN;
	if (jp & PAD_BUTTON_LEFT)
		J |= VBA_LEFT;
	else if (jp & PAD_BUTTON_RIGHT)
		J |= VBA_RIGHT;
	if (jp & PAD_BUTTON_A)
		J |= VBA_BUTTON_A;
	if (jp & PAD_BUTTON_B)
		J |= VBA_BUTTON_B;
	if (jp & PAD_BUTTON_START)
		J |= VBA_BUTTON_START;
	if (jp & PAD_BUTTON_X)
		J |= VBA_BUTTON_SELECT;
	if (jp & PAD_TRIGGER_L)
		J |= VBA_BUTTON_L;
	if (jp & PAD_TRIGGER_R)
		J |= VBA_BUTTON_R;
	if (jp & PAD_TRIGGER_Z || jp & PAD_BUTTON_Y)
		J |= VBA_SPEED;
	return J;
}

u32 DecodeGamecube(unsigned short pad)
{
	u32 J = 0;
	u32 jp = userInput[pad].pad.btns_h;
	for (u32 i = 0; i < MAXJP; ++i)
	{
		if (jp & btnmap[CTRLR_GCPAD][i])
			J |= vbapadmap[i];
	}
	return J;
}

u32 DecodeWiimote(unsigned short pad)
{
	u32 J = 0;
	#ifdef HW_RVL
	WPADData * wp = WPAD_Data(pad);

	u32 wpad_btns_h = wp->btns_h;

	if(wp->exp.type == WPAD_EXP_NONE)
	{
		for (u32 i = 0; i < MAXJP; ++i)
		{
			if (wpad_btns_h & btnmap[CTRLR_WIIMOTE][i] )
				J |= vbapadmap[i];
		}
	}

	#endif
	return J;
}

u32 DecodeClassic(unsigned short pad)
{
	u32 J = 0;
	#ifdef HW_RVL
	WPADData * wp = WPAD_Data(pad);
	u32 wpad_btns_h = wp->btns_h;
	
	if(wp->exp.type == WPAD_EXP_CLASSIC){
		if (wp->exp.classic.type == 2) {
			for (u32 i = 0; i < MAXJP; ++i){
				if (wpad_btns_h & btnmap[CTRLR_WUPC][i] )
					J |= vbapadmap[i];
			}
		}
		else {
			for (u32 i = 0; i < MAXJP; ++i){
				if (wpad_btns_h & btnmap[CTRLR_CLASSIC][i] )
					J |= vbapadmap[i];
			}
		}
	}
	#endif
	return J;
}

u32 DecodeNunchuk(unsigned short pad)
{
	u32 J = 0;
	#ifdef HW_RVL
	WPADData * wp = WPAD_Data(pad);
	u32 wpad_btns_h = wp->btns_h;
	
	if(wp->exp.type == WPAD_EXP_NUNCHUK){
		for (u32 i = 0; i < MAXJP; ++i){
			if (wpad_btns_h & btnmap[CTRLR_NUNCHUK][i] )
				J |= vbapadmap[i];
		}
	}
	#endif
	return J;
}

#ifdef HW_RVL
static u32 CCToGC(u32 w) {
	u32 gc = 0;
	if (w & WPAD_CLASSIC_BUTTON_A) gc |= PAD_BUTTON_A;
	if (w & WPAD_CLASSIC_BUTTON_B) gc |= PAD_BUTTON_B;
	if (w & WPAD_CLASSIC_BUTTON_X) gc |= PAD_BUTTON_X;
	if (w & WPAD_CLASSIC_BUTTON_Y) gc |= PAD_BUTTON_Y;
	if (w & WPAD_CLASSIC_BUTTON_PLUS) gc |= PAD_BUTTON_START;
	if (w & WPAD_CLASSIC_BUTTON_MINUS) gc |= 0;
	if (w & (WPAD_CLASSIC_BUTTON_ZL | WPAD_CLASSIC_BUTTON_ZR)) gc |= PAD_TRIGGER_Z;
	if (w & WPAD_CLASSIC_BUTTON_FULL_L) gc |= PAD_TRIGGER_L;
	if (w & WPAD_CLASSIC_BUTTON_FULL_R) gc |= PAD_TRIGGER_R;
	if (w & WPAD_CLASSIC_BUTTON_UP) gc |= PAD_BUTTON_UP;
	else if (w & WPAD_CLASSIC_BUTTON_DOWN) gc |= PAD_BUTTON_DOWN;
	if (w & WPAD_CLASSIC_BUTTON_LEFT) gc |= PAD_BUTTON_LEFT;
	else if (w & WPAD_CLASSIC_BUTTON_RIGHT) gc |= PAD_BUTTON_RIGHT;
	return gc;
}
#endif

// For developers who don't have gamecube pads but need to test gamecube input
u32 PAD_ButtonsHeldFake(unsigned short pad)
{
	u32 gc = 0;
	#ifdef HW_RVL
	if (userInput[pad].wpad->exp.type == WPAD_EXP_CLASSIC) {
		gc = CCToGC(userInput[pad].wpad->btns_h);
	}
	#endif
	return gc;
}
u32 PAD_ButtonsDownFake(unsigned short pad)
{
	u32 gc = 0;
	#ifdef HW_RVL
	if (userInput[pad].wpad->exp.type == WPAD_EXP_CLASSIC) {
		gc = CCToGC(userInput[pad].wpad->btns_d);
	}
	#endif
	return gc;
}
u32 PAD_ButtonsUpFake(unsigned short pad)
{
	u32 gc = 0;
	#ifdef HW_RVL
	if (userInput[pad].wpad->exp.type == WPAD_EXP_CLASSIC) {
		gc = CCToGC(userInput[pad].wpad->btns_u);
	}
	#endif
	return gc;
}

/****************************************************************************
 * DecodeJoy
 *
 * Reads the STATE (not changes) from a controller and reports
 * this STATE (not changes) to VBA
 ****************************************************************************/

static u32 DecodeJoy(unsigned short pad)
{
#ifdef HW_RVL
	CursorX = userInput[pad].wpad->ir.x;
	CursorY = userInput[pad].wpad->ir.y;
	CursorValid = userInput[pad].wpad->ir.valid;
#else
	CursorX = CursorY = CursorValid = 0;
#endif

	// The "Match Wii Controls" feature (per-game special control schemes)
	// has been removed - it was untested, disabled in the UI, and unused.

	// the function result, J, is a combination of flags for all the VBA buttons that are down
	u32 J = StandardMovement(pad);
	// Report pressed buttons (gamepads)
	u32 pad_btns_h   = userInput[pad].pad.btns_h; // GCN
	u32 wiidrcp_btns_h  = userInput[pad].wiidrcdata.btns_h;
#ifdef HW_RVL
	u32 wpad_btns_h = userInput[pad].wpad->btns_h;
	int wpad_exp_type = userInput[pad].wpad->exp.type;
	bool isWUPC = userInput[pad].wpad->exp.classic.type == 2;

	if(wpad_exp_type == WPAD_EXP_NONE)
	{ // wiimote

		for (u32 i =0; i < MAXJP; ++i)
		{
			if ((pad_btns_h & btnmap[CTRLR_GCPAD][i]) // gamecube controller
					|| ( wiidrcp_btns_h & btnmap[CTRLR_WIIDRC][i] ) //wii u gamepad
					|| ( (wpad_btns_h & btnmap[CTRLR_WIIMOTE][i]) ))
			J |= vbapadmap[i];
		}

	}
	else if(wpad_exp_type == WPAD_EXP_CLASSIC)
	{ // classic controller
		if (isWUPC) {
			for (u32 i = 0; i < MAXJP; ++i)
			{
				if (wpad_btns_h & btnmap[CTRLR_WUPC][i] )
					J |= vbapadmap[i];
			}
		}
		else {
			for (u32 i = 0; i < MAXJP; ++i)
			{
				if ((pad_btns_h & btnmap[CTRLR_GCPAD][i]) // gamecube controller
						|| ( wiidrcp_btns_h & btnmap[CTRLR_WIIDRC][i] ) //wiiu gamepad
						|| ( (wpad_btns_h & btnmap[CTRLR_CLASSIC][i]) ))
				J |= vbapadmap[i];
			}
		}
	}
	else if(wpad_exp_type == WPAD_EXP_NUNCHUK)
	{ // nunchuk + wiimote

		for (u32 i =0; i < MAXJP; ++i)
		{
			if ((pad_btns_h & btnmap[CTRLR_GCPAD][i]) // gamecube controller
					|| ( (wpad_btns_h & btnmap[CTRLR_NUNCHUK][i]) ))
			J |= vbapadmap[i];
		}
	}
	else
	// Check out this trickery!
	// If all else fails OR if HW_RVL is undefined, the result is the same
#endif
	{
		for (u32 i = 0; i < MAXJP; ++i)
		{
			if ((pad_btns_h & btnmap[CTRLR_GCPAD][i]) || (wiidrcp_btns_h & btnmap[CTRLR_WIIDRC][i]))
				J |= vbapadmap[i];
		}
	}
	return J;
}

bool MenuRequested()
{
	for(int i=0; i<4; i++) {
		if (
			(userInput[i].pad.substickX < -70) ||
			(userInput[i].pad.btns_h & PAD_TRIGGER_L &&
			userInput[i].pad.btns_h & PAD_TRIGGER_R &&
			userInput[i].pad.btns_h & PAD_BUTTON_START)
			#ifdef HW_RVL
			|| (userInput[i].wpad->btns_h & WPAD_BUTTON_HOME) ||
			(userInput[i].wpad->btns_h & WPAD_CLASSIC_BUTTON_HOME) ||
			(userInput[i].wiidrcdata.btns_h & WIIDRC_BUTTON_HOME) ||
			(userInput[i].wpad->btns_h & WPAD_CLASSIC_BUTTON_FULL_L &&
			userInput[i].wpad->btns_h & WPAD_CLASSIC_BUTTON_FULL_R &&
			userInput[i].wpad->btns_h & WPAD_CLASSIC_BUTTON_PLUS)
			#endif
		)
		{
			return true; 
		}
	}
	return false;
}

/****************************************************************************
 * FastForwardHeld
 *
 * Returns true if any connected controller's currently-assigned
 * fast-forward-hold button is held. Checked across all 4 controller
 * slots, same as MenuRequested() above - not just the first one.
 *
 *   - GameCube pad: hardcoded to C-Stick RIGHT (substickX > 70), not user
 *     remappable - mirrors MenuRequested()'s own C-Stick LEFT check
 *     (substickX < -70) just above, opposite direction so the two never
 *     collide. GC pad's default btnmap never uses the C-Stick for
 *     anything, so there's no equivalent conflict to the one below.
 *   - Wiimote / Nunchuk / Classic Controller / Wii U Pro Controller /
 *     Wii U GamePad: whatever the user assigned via ffmap[CTRLR_*] in the
 *     "Fast Forward (Hold)" row of each controller's Button Mappings
 *     screen (menu.cpp) - 0 (the default) means unassigned, so it's just
 *     never checked/never triggers. This used to be hardcoded to Minus
 *     for all of these, but Minus is also the default (and commonly
 *     kept) mapping for the in-game Select button - holding Select would
 *     therefore also silently hold fast-forward. Leaving this unassigned
 *     by default and fully user-choosable avoids that collision, and
 *     lets someone opt out of a held-button fast-forward entirely (the
 *     persistent Fast Forward Speed menu setting still works either way -
 *     see GCSettings.FastForwardSpeed, vbagx.h/vbasupport.cpp).
 *   - WPAD_BUTTON_MINUS/WPAD_CLASSIC_BUTTON_MINUS both read from the same
 *     merged userInput[i].wpad->btns_h field DecodeJoy() above already
 *     relies on - libogc's WPAD driver folds Classic Controller bits into
 *     the same btns_h as the Wiimote's own buttons, and per DecodeJoy()'s
 *     isWUPC handling, a Wii U Pro Controller is reported as a Classic
 *     Controller (wpad->exp.classic.type == 2) using the same
 *     WPAD_CLASSIC_BUTTON_* bits - so checking ffmap[CTRLR_WIIMOTE] and
 *     ffmap[CTRLR_CLASSIC]/ffmap[CTRLR_WUPC] against the one merged
 *     btns_h field (rather than needing separate WUPC-specific API)
 *     still holds true here, same as it did for the old hardcoded check.
 ***************************************************************************/
bool FastForwardHeld()
{
	for(int i=0; i<4; i++) {
		if (
			(userInput[i].pad.substickX > 70)
			#ifdef HW_RVL
			|| (ffmap[CTRLR_WIIMOTE]  && (userInput[i].wpad->btns_h & ffmap[CTRLR_WIIMOTE]))
			|| (ffmap[CTRLR_NUNCHUK]  && (userInput[i].wpad->btns_h & ffmap[CTRLR_NUNCHUK]))
			|| (ffmap[CTRLR_CLASSIC]  && (userInput[i].wpad->btns_h & ffmap[CTRLR_CLASSIC]))
			|| (ffmap[CTRLR_WUPC]     && (userInput[i].wpad->btns_h & ffmap[CTRLR_WUPC]))
			|| (ffmap[CTRLR_WIIDRC]   && (userInput[i].wiidrcdata.btns_h & ffmap[CTRLR_WIIDRC]))
			#endif
		)
		{
			return true;
		}
	}
	return false;
}

static int GetPlayerChan(int pad)
{
	for(int i=3; i >= 0; i--) {
		if(playerMapping[i] == pad) {
			return i;
		}
	}
	return pad;
}

u32 GetJoy(int pad)
{
	// request to go back to menu
	if (MenuRequested())
	{
		ScreenshotRequested = 1;
		updateRumbleFrame();
		return 0;
	}

	int chan = GetPlayerChan(pad);

	u32 J = DecodeJoy(chan);
	// don't allow up+down or left+right
	if ((J & 48) == 48)
		J &= ~16;
	if ((J & 192) == 192)
		J &= ~128;
	updateRumbleFrame();

	return J;
}
