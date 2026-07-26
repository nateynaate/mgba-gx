/****************************************************************************
 * Visual Boy Advance GX
 *
 * Carl Kenner Febuary 2009
 *
 * gameinput.h
 *
 * Wii/Gamecube controller helpers
 *
 * mGBA-GX NOTE: the per-game special control schemes (Zelda, Metroid,
 * Mario, Star Wars, Mortal Kombat, TMNT, Harry Potter, Boktai, One Piece,
 * Lord of the Rings, Castlevania, Kid Dracula, etc.) that used to live here
 * were only ever reachable through the "Match Wii Controls" setting, which
 * has been removed (untested, disabled in the UI, unused). Their game-ID
 * defines, function prototypes, and implementation files
 * (gameinput.cpp, inputzelda.cpp, inputmario.cpp, inputmetroid.cpp,
 * inputmortalkombat.cpp, inputstarwars.cpp) have all been removed.
 ***************************************************************************/

#ifndef _GAMEINPUT_H_
#define _GAMEINPUT_H_

u8 gbReadMemory(u16 address);
void gbWriteMemory(u16 address, u8 value);

u32 StandardDPad(unsigned short pad);
u32 StandardMovement(unsigned short pad);
u32 StandardSideways(unsigned short pad);
u32 StandardClassic(unsigned short pad);
u32 StandardGamecube(unsigned short pad);
u32 DecodeWiimote(unsigned short pad);
u32 DecodeClassic(unsigned short pad);
u32 DecodeGamecube(unsigned short pad);
u32 DecodeNunchuk(unsigned short pad);

// For developers who don't have gamecube pads but need to test gamecube input
u32 PAD_ButtonsDownFake(unsigned short pad);
u32 PAD_ButtonsHeldFake(unsigned short pad);
u32 PAD_ButtonsUpFake(unsigned short pad);

#endif
