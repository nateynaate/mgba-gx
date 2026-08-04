/****************************************************************************
 * mGBA-GX
 *
 * Fork of Visual Boy Advance GX (Tantric, 2008-2023)
 * mGBA-GX modifications 2026
 *
 * preferences.cpp
 *
 * Preferences save/load to XML file
 ***************************************************************************/

#include <gccore.h>
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <ogcsys.h>
#include <mxml.h>

#include "vbagx.h"
#include "menu.h"
#include "fileop.h"
#include "video.h"
#include "filebrowser.h"
#include "input.h"
#include "button_mapping.h"
#include "gamesettings.h"

struct SGCSettings GCSettings;
static gamePalette *palettes = NULL;
static int loadedPalettes = 0;

/****************************************************************************
 * Prepare Preferences Data
 *
 * This sets up the save buffer for saving.
 ***************************************************************************/
static mxml_node_t *xml = NULL;
static mxml_node_t *data = NULL;
static mxml_node_t *section = NULL;
static mxml_node_t *item = NULL;
static mxml_node_t *elem = NULL;

static mxml_node_t *mxmlFindNewElement(mxml_node_t *parent, const char *nodename, const char *attr=NULL, const char *value=NULL)
{
	mxml_node_t *node = mxmlFindElement(parent, xml, nodename, attr, value, MXML_DESCEND);
	if (!node)
	{
		node = mxmlNewElement(parent, nodename);
		if (attr && value) mxmlElementSetAttr(node, attr, value);
	}
	return node;
}

static char temp[20];

static const char * toStr(int i)
{
	sprintf(temp, "%d", i);
	return temp;
}
static const char * toHex(u32 i)
{
	sprintf(temp, "0x%06X", i);
	return temp;
}
static const char * FtoStr(float i)
{
	sprintf(temp, "%.2f", i);
	return temp;
}

static void createXMLSection(const char * name, const char * description)
{
	section = mxmlNewElement(data, "section");
	mxmlElementSetAttr(section, "name", name);
	mxmlElementSetAttr(section, "description", description);
}

static void createXMLSetting(const char * name, const char * description, const char * value)
{
	item = mxmlNewElement(section, "setting");
	mxmlElementSetAttr(item, "name", name);
	mxmlElementSetAttr(item, "value", value);
	mxmlElementSetAttr(item, "description", description);
}

static void createXMLController(u32 controller[], const char * name, const char * description)
{
	item = mxmlNewElement(section, "controller");
	mxmlElementSetAttr(item, "name", name);
	mxmlElementSetAttr(item, "description", description);

	// create buttons
	for(int i=0; i < MAXJP; i++)
	{
		elem = mxmlNewElement(item, "button");
		mxmlElementSetAttr(elem, "number", toStr(i));
		mxmlElementSetAttr(elem, "assignment", toStr(controller[i]));
	}
}

static const char * XMLSaveCallback(mxml_node_t *node, int where)
{
	const char *name;

	name = mxmlGetElement(node);

	if(where == MXML_WS_BEFORE_CLOSE)
	{
		if(!strcmp(name, "file") || !strcmp(name, "section"))
			return ("\n");
		else if(!strcmp(name, "controller"))
			return ("\n\t");
	}
	if (where == MXML_WS_BEFORE_OPEN)
	{
		if(!strcmp(name, "file"))
			return ("\n");
		else if(!strcmp(name, "section"))
			return ("\n\n");
		else if(!strcmp(name, "setting") || !strcmp(name, "controller"))
			return ("\n\t");
		else if(!strcmp(name, "button"))
			return ("\n\t\t");
	}
	return (NULL);
}

static const char * XMLSavePalCallback(mxml_node_t *node, int where)
{
	const char *name;

	name = mxmlGetElement(node);

	if(where == MXML_WS_BEFORE_CLOSE)
	{
		if(!strcmp(name, "palette") || !strcmp(name, "game"))
			return ("\n");
		else if(!strcmp(name, "bkgr") || !strcmp(name, "wind") || !strcmp(name, "obj0") || !strcmp(name, "obj1"))
			return ("\n\t");
	}
	if (where == MXML_WS_BEFORE_OPEN)
	{
		if(!strcmp(name, "palette"))
			return ("\n");
		else if(!strcmp(name, "game"))
			return ("\n\n");
		else if(!strcmp(name, "bkgr") || !strcmp(name, "wind") || !strcmp(name, "obj0") || !strcmp(name, "obj1"))
			return ("\n\t");
	}
	return (NULL);
}

static int
preparePrefsData ()
{
	xml = mxmlNewXML("1.0");
	mxmlSetWrapMargin(0); // disable line wrapping

	data = mxmlNewElement(xml, "file");
	mxmlElementSetAttr(data, "app", APPNAME);
	mxmlElementSetAttr(data, "version", APPVERSION);

	createXMLSection("File", "File Settings");

	createXMLSetting("AutoLoad", "Auto Load", toStr(GCSettings.AutoLoad));
	createXMLSetting("AutoSave", "Auto Save", toStr(GCSettings.AutoSave));
	createXMLSetting("LoadMethod", "Load Method", toStr(GCSettings.LoadMethod));
	createXMLSetting("SaveMethod", "Save Method", toStr(GCSettings.SaveMethod));
	createXMLSetting("LoadFolder", "Load Folder", GCSettings.LoadFolder);
	createXMLSetting("LastFileLoaded", "Last File Loaded", GCSettings.LastFileLoaded);
	createXMLSetting("SaveFolder", "Save Folder", GCSettings.SaveFolder);
	createXMLSetting("StateFolder", "Save State Folder", GCSettings.StateFolder);
	createXMLSetting("AppendAuto", "Append 'Auto' to Auto-Save Filenames", toStr(GCSettings.AppendAuto));
	createXMLSetting("GBFolder", "GB Folder", GCSettings.GBFolder);
	createXMLSetting("GBCFolder", "GBC Folder", GCSettings.GBCFolder);
	createXMLSetting("GBAFolder", "GBA Folder", GCSettings.GBAFolder);
	createXMLSetting("RecentROMs", "Recently Played", GCSettings.RecentROMs);
	createXMLSetting("LastActiveTab", "Last Active Tab", toStr(GCSettings.LastActiveTab));
	createXMLSetting("ScreenshotsFolder", "Screenshots Folder", GCSettings.ScreenshotsFolder);
	createXMLSetting("GBABorderFolder", "GBA Borders Folder", GCSettings.GBABorderFolder);
	createXMLSetting("GBCBorderFolder", "GBC Borders Folder", GCSettings.GBCBorderFolder);
	createXMLSetting("CheatFolder", "Cheats Folder", GCSettings.CheatFolder);
	createXMLSetting("CoverFolder", "Covers Folder", GCSettings.CoverFolder);
	createXMLSetting("ArtworkFolder", "Artwork Folder", GCSettings.ArtworkFolder);

	createXMLSection("Video", "Video Settings");

	createXMLSetting("videomode", "Video Mode", toStr(GCSettings.videomode));
	createXMLSetting("gbaZoomHor", "GBA Horizontal Zoom Level", FtoStr(GCSettings.gbaZoomHor));
	createXMLSetting("gbaZoomVert", "GBA Vertical Zoom Level", FtoStr(GCSettings.gbaZoomVert));
	createXMLSetting("gbZoomHor", "GB Horizontal Zoom Level", FtoStr(GCSettings.gbZoomHor));
	createXMLSetting("gbZoomVert", "GB Vertical Zoom Level", FtoStr(GCSettings.gbZoomVert));
	createXMLSetting("gbFixed", "GB Fixed Pixel Ratio", toStr(GCSettings.gbFixed));
	createXMLSetting("gbaFixed", "GBA Fixed Pixel Ratio", toStr(GCSettings.gbaFixed));
	createXMLSetting("render", "Video Filtering", toStr(GCSettings.render));
	createXMLSetting("scaling", "Aspect Ratio Correction", toStr(GCSettings.scaling));
	createXMLSetting("xshift", "Horizontal Video Shift", toStr(GCSettings.xshift));
	createXMLSetting("yshift", "Vertical Video Shift", toStr(GCSettings.yshift));

	createXMLSection("Menu", "Menu Settings");

#ifdef HW_RVL
	createXMLSetting("WiimoteOrientation", "Wiimote Orientation", toStr(GCSettings.WiimoteOrientation));
#endif
	createXMLSetting("ExitAction", "Exit Action", toStr(GCSettings.ExitAction));
	createXMLSetting("MusicVolume", "Music Volume", toStr(GCSettings.MusicVolume));
	createXMLSetting("SFXVolume", "Sound Effects Volume", toStr(GCSettings.SFXVolume));
	createXMLSetting("Rumble", "Rumble", toStr(GCSettings.Rumble));
	createXMLSetting("language", "Language", toStr(GCSettings.language));
	createXMLSetting("PreviewImage", "Preview Image", toStr(GCSettings.PreviewImage));

	createXMLSection("Emulation", "Emulation Settings");

	createXMLSetting("BasicPalette", "GB Color Emulation", toStr(GCSettings.BasicPalette));
	createXMLSetting("MotionTilt", "Wii Remote Tilt Control", toStr(GCSettings.MotionTilt));
	createXMLSetting("ClassicBrowser", "Classic (Non-Tabbed) Game List", toStr(GCSettings.ClassicBrowser));
	createXMLSetting("GBAColorEmulation", "GBA Color Emulation", toStr(GCSettings.GBAColorEmulation));
	createXMLSetting("GBCColorEmulation", "GBC Color Emulation", toStr(GCSettings.GBCColorEmulation));
	createXMLSetting("InterframeBlending", "Interframe Blending", toStr(GCSettings.InterframeBlending));
	createXMLSetting("FastForwardSpeed", "Fast Forward Speed", toStr(GCSettings.FastForwardSpeed));
	createXMLSetting("Frameskip", "Frameskip", toStr(GCSettings.Frameskip));
	createXMLSetting("FilterMethod", "Video Filter", toStr(GCSettings.FilterMethod));
	
	createXMLSection("Controller", "Controller Settings");

	createXMLController(btnmap[CTRLR_GCPAD], "gcpadmap", "GameCube Pad");
	createXMLController(btnmap[CTRLR_WIIMOTE], "wmpadmap", "Wiimote");
	createXMLController(btnmap[CTRLR_CLASSIC], "ccpadmap", "Classic Controller");
	createXMLController(btnmap[CTRLR_NUNCHUK], "ncpadmap", "Nunchuk");
	createXMLController(btnmap[CTRLR_WUPC], "wupcpadmap", "Wii U Pro Controller");
	createXMLController(btnmap[CTRLR_WIIDRC], "drcpadmap", "Wii U Gamepad");

	// Fast-forward-hold button per controller (see ffmap, vbagx.h) - a
	// single scalar per controller, not a 10-slot button list, so this
	// uses plain createXMLSetting rather than createXMLController (which
	// hardcodes writing MAXJP entries and would overrun a lone u32).
	// GCPAD isn't included - it keeps its hardcoded C-Stick-Right binding,
	// not user-remappable (see FastForwardHeld(), input.cpp).
	createXMLSetting("ffmap_wiimote", "Wiimote Fast Forward Button", toStr((int)ffmap[CTRLR_WIIMOTE]));
	createXMLSetting("ffmap_classic", "Classic Controller Fast Forward Button", toStr((int)ffmap[CTRLR_CLASSIC]));
	createXMLSetting("ffmap_nunchuk", "Nunchuk Fast Forward Button", toStr((int)ffmap[CTRLR_NUNCHUK]));
	createXMLSetting("ffmap_wupc", "Wii U Pro Controller Fast Forward Button", toStr((int)ffmap[CTRLR_WUPC]));
	createXMLSetting("ffmap_wiidrc", "Wii U Gamepad Fast Forward Button", toStr((int)ffmap[CTRLR_WIIDRC]));

	createXMLSection("Emulation", "Emulation Settings");

	createXMLSetting("OffsetMinutesUTC", "Offset from UTC (minutes)", toStr(GCSettings.OffsetMinutesUTC));
	createXMLSetting("GBHardware", "Hardware (GB/GBC)", toStr(GCSettings.GBHardware));
	createXMLSetting("SGBBorder", "Border (GB/GBC)", toStr(GCSettings.SGBBorder));
	createXMLSetting("GBABorderFile", "Border (GBA)", GCSettings.GBABorderFile);
	createXMLSetting("GBBorderFile", "Border (GB/GBC)", GCSettings.GBBorderFile);

	int datasize = mxmlSaveString(xml, (char *)savebuffer, SAVEBUFFERSIZE, XMLSaveCallback);

	mxmlDelete(xml);

	return datasize;
}

static void createXMLPalette(gamePalette *p, bool overwrite, const char *newname = NULL)
{
	if (!newname)
		newname = p->gameName;
	section = mxmlFindElement(xml, xml, "game", "name", newname, MXML_DESCEND);
	if (section && !overwrite)
	{
		return;
	}
	else if (!section)
	{
		section = mxmlNewElement(data, "game");
	}
	mxmlElementSetAttr(section, "name", newname);
	mxmlElementSetAttr(section, "use", "1");
	item = mxmlFindNewElement(section, "bkgr");
	mxmlElementSetAttr(item, "c0", toHex(p->palette[0]));
	mxmlElementSetAttr(item, "c1", toHex(p->palette[1]));
	mxmlElementSetAttr(item, "c2", toHex(p->palette[2]));
	mxmlElementSetAttr(item, "c3", toHex(p->palette[3]));
	item = mxmlFindNewElement(section, "wind");
	mxmlElementSetAttr(item, "c0", toHex(p->palette[4]));
	mxmlElementSetAttr(item, "c1", toHex(p->palette[5]));
	mxmlElementSetAttr(item, "c2", toHex(p->palette[6]));
	mxmlElementSetAttr(item, "c3", toHex(p->palette[7]));
	item = mxmlFindNewElement(section, "obj0");
	mxmlElementSetAttr(item, "c0", toHex(p->palette[8]));
	mxmlElementSetAttr(item, "c1", toHex(p->palette[9]));
	mxmlElementSetAttr(item, "c2", toHex(p->palette[10]));
	item = mxmlFindNewElement(section, "obj1");
	mxmlElementSetAttr(item, "c0", toHex(p->palette[11]));
	mxmlElementSetAttr(item, "c1", toHex(p->palette[12]));
	mxmlElementSetAttr(item, "c2", toHex(p->palette[13]));
}

static int
preparePalData (gamePalette pals[], int palCount)
{
	xml = mxmlNewXML("1.0");
	mxmlSetWrapMargin(0); // disable line wrapping

	data = mxmlNewElement(xml, "palette");
	mxmlElementSetAttr(data, "app", APPNAME);
	mxmlElementSetAttr(data, "version", APPVERSION);
	for (int i=0; i<palCount; i++)
		createXMLPalette(&pals[i], false);

	int datasize = mxmlSaveString(xml, (char *)savebuffer, SAVEBUFFERSIZE, XMLSavePalCallback);

	mxmlDelete(xml);

	return datasize;
}

/****************************************************************************
 * loadXMLSetting
 *
 * Load XML elements into variables for an individual variable
 ***************************************************************************/

static void loadXMLSetting(char * var, const char * name, int maxsize)
{
	item = mxmlFindElement(xml, xml, "setting", "name", name, MXML_DESCEND);
	if(item)
	{
		const char * tmp = mxmlElementGetAttr(item, "value");
		if(tmp)
			snprintf(var, maxsize, "%s", tmp);
	}
}
static void loadXMLSetting(int * var, const char * name)
{
	item = mxmlFindElement(xml, xml, "setting", "name", name, MXML_DESCEND);
	if(item)
	{
		const char * tmp = mxmlElementGetAttr(item, "value");
		if(tmp)
			*var = atoi(tmp);
	}
}
static void loadXMLSetting(float * var, const char * name)
{
	item = mxmlFindElement(xml, xml, "setting", "name", name, MXML_DESCEND);
	if(item)
	{
		const char * tmp = mxmlElementGetAttr(item, "value");
		if(tmp)
			*var = atof(tmp);
	}
}

/****************************************************************************
 * loadXMLController
 *
 * Load XML elements into variables for a controller mapping
 ***************************************************************************/

static void loadXMLController(u32 controller[], const char * name)
{
	item = mxmlFindElement(xml, xml, "controller", "name", name, MXML_DESCEND);

	if(item)
	{
		// populate buttons
		for(int i=0; i < MAXJP; i++)
		{
			elem = mxmlFindElement(item, xml, "button", "number", toStr(i), MXML_DESCEND);
			if(elem)
			{
				const char * tmp = mxmlElementGetAttr(elem, "assignment");
				if(tmp)
					controller[i] = atoi(tmp);
			}
		}
	}
}

static void loadXMLPaletteFromSection(gamePalette &pal)
{
	if (section)
	{
		strncpy(pal.gameName, mxmlElementGetAttr(section, "name"), 17);
		item = mxmlFindElement(section, xml, "bkgr", NULL, NULL, MXML_DESCEND);
		if (item)
		{
			const char * tmp = mxmlElementGetAttr(item, "c0");
			if (tmp)
				pal.palette[0] = strtoul(tmp, NULL, 16);
			tmp = mxmlElementGetAttr(item, "c1");
			if (tmp)
				pal.palette[1] = strtoul(tmp, NULL, 16);
			tmp = mxmlElementGetAttr(item, "c2");
			if (tmp)
				pal.palette[2] = strtoul(tmp, NULL, 16);
			tmp = mxmlElementGetAttr(item, "c3");
			if (tmp)
				pal.palette[3] = strtoul(tmp, NULL, 16);
		}
		item = mxmlFindElement(section, xml, "wind", NULL, NULL, MXML_DESCEND);
		if (item)
		{
			const char * tmp = mxmlElementGetAttr(item, "c0");
			if (tmp)
				pal.palette[4] = strtoul(tmp, NULL, 16);
			tmp = mxmlElementGetAttr(item, "c1");
			if (tmp)
				pal.palette[5] = strtoul(tmp, NULL, 16);
			tmp = mxmlElementGetAttr(item, "c2");
			if (tmp)
				pal.palette[6] = strtoul(tmp, NULL, 16);
			tmp = mxmlElementGetAttr(item, "c3");
			if (tmp)
				pal.palette[7] = strtoul(tmp, NULL, 16);
		}
		item = mxmlFindElement(section, xml, "obj0", NULL, NULL, MXML_DESCEND);
		if (item)
		{
			const char * tmp = mxmlElementGetAttr(item, "c0");
			if (tmp)
				pal.palette[8] = strtoul(tmp, NULL, 16);
			tmp = mxmlElementGetAttr(item, "c1");
			if (tmp)
				pal.palette[9] = strtoul(tmp, NULL, 16);
			tmp = mxmlElementGetAttr(item, "c2");
			if (tmp)
				pal.palette[10] = strtoul(tmp, NULL, 16);
		}
		item = mxmlFindElement(section, xml, "obj1", NULL, NULL, MXML_DESCEND);
		if (item)
		{
			const char * tmp = mxmlElementGetAttr(item, "c0");
			if (tmp)
				pal.palette[11] = strtoul(tmp, NULL, 16);
			tmp = mxmlElementGetAttr(item, "c1");
			if (tmp)
				pal.palette[12] = strtoul(tmp, NULL, 16);
			tmp = mxmlElementGetAttr(item, "c2");
			if (tmp)
				pal.palette[13] = strtoul(tmp, NULL, 16);
		}
		const char *use = mxmlElementGetAttr(section, "use");
		if (use)
		{
			if (atoi(use) == 0)
				pal.use = 0;
			else
				pal.use = 1;
		}
		else
		{
			pal.use = 1;
		}
	}
}

/****************************************************************************
 * decodePrefsData
 *
 * Decodes preferences - parses XML and loads preferences into the variables
 ***************************************************************************/

static bool
decodePrefsData ()
{
	bool result = false;

	xml = mxmlLoadString(NULL, (char *)savebuffer, MXML_TEXT_CALLBACK);
	printf("[prefs] mxmlLoadString -> %s\n", xml ? "non-null" : "NULL");

	if(xml)
	{
		// check settings version
		// we don't do anything with the version #, but we'll store it anyway
		item = mxmlFindElement(xml, xml, "file", "version", NULL, MXML_DESCEND);
		printf("[prefs] <file version=...> element -> %s\n", item ? "found" : "NOT FOUND");
		if(item) // a version entry exists
		{
			const char * version = mxmlElementGetAttr(item, "version");
			printf("[prefs] version attr = \"%s\"\n", version ? version : "(null)");

			if(version && strlen(version) == 5)
			{
				// this code assumes version in format X.X.X
				// XX.X.X, X.XX.X, or X.X.XX will NOT work
				int verMajor = version[0] - '0';
				int verMinor = version[2] - '0';
				int verPoint = version[4] - '0';

				// check that the versioning is valid
				// NOTE: originally required verMajor >= 2, inherited from
				// upstream VBA-GX (whose XML schema only stabilized at
				// 2.x). mGBA-GX's APPVERSION is "1.0.0", so that floor of 2
				// caused this check to fail for every settings.xml this
				// fork has ever written - decodePrefsData() would then
				// leave `result` false and skip every loadXMLSetting() call
				// entirely, silently discarding all saved settings (Load/
				// Save Device, Preview Image, SGB Border, Recent ROMs,
				// everything) back to defaults on every boot, regardless
				// of how many places call SavePrefs(). This is the actual
				// root cause of settings "not persisting."
				if(!(verMajor >= 1 && verMajor <= 9 &&
					verMinor >= 0 && verMinor <= 9 &&
					verPoint >= 0 && verPoint <= 9))
					result = false;
				else
					result = true;
			}
		}

		printf("[prefs] decodePrefsData: version check result = %s\n", result ? "true" : "false");

		if(result)
		{
			// File Settings

			loadXMLSetting(&GCSettings.AutoLoad, "AutoLoad");
			loadXMLSetting(&GCSettings.AutoSave, "AutoSave");
			loadXMLSetting(&GCSettings.LoadMethod, "LoadMethod");
			loadXMLSetting(&GCSettings.SaveMethod, "SaveMethod");
			loadXMLSetting(GCSettings.LoadFolder, "LoadFolder", sizeof(GCSettings.LoadFolder));
			loadXMLSetting(GCSettings.LastFileLoaded, "LastFileLoaded", sizeof(GCSettings.LastFileLoaded));
			loadXMLSetting(GCSettings.SaveFolder, "SaveFolder", sizeof(GCSettings.SaveFolder));
			loadXMLSetting(GCSettings.StateFolder, "StateFolder", sizeof(GCSettings.StateFolder));
			loadXMLSetting(&GCSettings.AppendAuto, "AppendAuto");
			loadXMLSetting(GCSettings.GBFolder, "GBFolder", sizeof(GCSettings.GBFolder));
			loadXMLSetting(GCSettings.GBCFolder, "GBCFolder", sizeof(GCSettings.GBCFolder));
			loadXMLSetting(GCSettings.GBAFolder, "GBAFolder", sizeof(GCSettings.GBAFolder));
			loadXMLSetting(GCSettings.RecentROMs, "RecentROMs", sizeof(GCSettings.RecentROMs));
			loadXMLSetting(&GCSettings.LastActiveTab, "LastActiveTab");
			//loadXMLSetting(GCSettings.CheatFolder, "CheatFolder", sizeof(GCSettings.CheatFolder));
			loadXMLSetting(GCSettings.ScreenshotsFolder, "ScreenshotsFolder", sizeof(GCSettings.ScreenshotsFolder));
			loadXMLSetting(GCSettings.GBABorderFolder, "GBABorderFolder", sizeof(GCSettings.GBABorderFolder));
			loadXMLSetting(GCSettings.GBCBorderFolder, "GBCBorderFolder", sizeof(GCSettings.GBCBorderFolder));
			loadXMLSetting(GCSettings.CheatFolder, "CheatFolder", sizeof(GCSettings.CheatFolder));
			loadXMLSetting(GCSettings.CoverFolder, "CoverFolder", sizeof(GCSettings.CoverFolder));
			loadXMLSetting(GCSettings.ArtworkFolder, "ArtworkFolder", sizeof(GCSettings.ArtworkFolder));

			// Video Settings

			loadXMLSetting(&GCSettings.videomode, "videomode");
			loadXMLSetting(&GCSettings.gbaZoomHor, "gbaZoomHor");
			loadXMLSetting(&GCSettings.gbaZoomVert, "gbaZoomVert");
			loadXMLSetting(&GCSettings.gbZoomHor, "gbZoomHor");
			loadXMLSetting(&GCSettings.gbZoomVert, "gbZoomVert");
			loadXMLSetting(&GCSettings.gbaFixed, "gbaFixed");
			loadXMLSetting(&GCSettings.gbFixed, "gbFixed");
			loadXMLSetting(&GCSettings.render, "render");
			loadXMLSetting(&GCSettings.scaling, "scaling");
			loadXMLSetting(&GCSettings.xshift, "xshift");
			loadXMLSetting(&GCSettings.yshift, "yshift");

			// Menu Settings

			loadXMLSetting(&GCSettings.WiimoteOrientation, "WiimoteOrientation");
			loadXMLSetting(&GCSettings.ExitAction, "ExitAction");
			loadXMLSetting(&GCSettings.MusicVolume, "MusicVolume");
			loadXMLSetting(&GCSettings.SFXVolume, "SFXVolume");
			loadXMLSetting(&GCSettings.Rumble, "Rumble");
			loadXMLSetting(&GCSettings.language, "language");
			loadXMLSetting(&GCSettings.PreviewImage, "PreviewImage");

			// Controller Settings
			loadXMLController(btnmap[CTRLR_GCPAD], "gcpadmap");
			loadXMLController(btnmap[CTRLR_WIIMOTE], "wmpadmap");
			loadXMLController(btnmap[CTRLR_CLASSIC], "ccpadmap");
			loadXMLController(btnmap[CTRLR_NUNCHUK], "ncpadmap");
			loadXMLController(btnmap[CTRLR_WUPC], "wupcpadmap");
			loadXMLController(btnmap[CTRLR_WIIDRC], "drcpadmap");
			loadXMLSetting((int*)&ffmap[CTRLR_WIIMOTE], "ffmap_wiimote");
			loadXMLSetting((int*)&ffmap[CTRLR_CLASSIC], "ffmap_classic");
			loadXMLSetting((int*)&ffmap[CTRLR_NUNCHUK], "ffmap_nunchuk");
			loadXMLSetting((int*)&ffmap[CTRLR_WUPC], "ffmap_wupc");
			loadXMLSetting((int*)&ffmap[CTRLR_WIIDRC], "ffmap_wiidrc");
			// Emulation Settings
			
			loadXMLSetting(&GCSettings.OffsetMinutesUTC, "OffsetMinutesUTC");
			loadXMLSetting(&GCSettings.GBHardware, "GBHardware");
			loadXMLSetting(&GCSettings.SGBBorder, "SGBBorder");
			// Settings saved before .png/.sgb were unified into one cycle
			// list could have SGBBorder == 3 ("From .sgb file"); that mode
			// no longer exists, so fold it into the merged mode 2.
			if (GCSettings.SGBBorder > 2)
				GCSettings.SGBBorder = 2;
			loadXMLSetting(GCSettings.GBABorderFile, "GBABorderFile", sizeof(GCSettings.GBABorderFile));
			loadXMLSetting(GCSettings.GBBorderFile, "GBBorderFile", sizeof(GCSettings.GBBorderFile));
			loadXMLSetting(&GCSettings.BasicPalette, "BasicPalette");
			loadXMLSetting(&GCSettings.MotionTilt, "MotionTilt");
			loadXMLSetting(&GCSettings.ClassicBrowser, "ClassicBrowser");
			// New key names (renamed from *ColorCorrection to
			// *ColorEmulation). Fall back to the old key names so
			// settings.xml files saved before the rename still apply.
			loadXMLSetting(&GCSettings.GBAColorEmulation, "GBAColorEmulation");
			loadXMLSetting(&GCSettings.GBCColorEmulation, "GBCColorEmulation");
			if (GCSettings.GBAColorEmulation == 0)
				loadXMLSetting(&GCSettings.GBAColorEmulation, "GBAColorCorrection");
			if (GCSettings.GBCColorEmulation == 0)
				loadXMLSetting(&GCSettings.GBCColorEmulation, "GBCColorCorrection");
			loadXMLSetting(&GCSettings.InterframeBlending, "InterframeBlending");
			loadXMLSetting(&GCSettings.FastForwardSpeed, "FastForwardSpeed");
			loadXMLSetting(&GCSettings.Frameskip, "Frameskip");
			loadXMLSetting(&GCSettings.FilterMethod, "FilterMethod");
		}
		mxmlDelete(xml);
	}
	return result;
}

static bool
decodePalsData ()
{
	bool result = false;

	xml = mxmlLoadString(NULL, (char *) savebuffer, MXML_TEXT_CALLBACK);

	if (xml)
	{
		// count number of palettes in file
		loadedPalettes = 0;
		item = mxmlFindElement(xml, xml, "palette", NULL, NULL, MXML_DESCEND);
		for (section = mxmlFindElement(item, xml, "game", NULL, NULL,
				MXML_DESCEND); section; section = mxmlFindElement(section, xml,
				"game", NULL, NULL, MXML_NO_DESCEND))
		{
			loadedPalettes++;
		}
		// Allocate enough memory for all palettes in file, plus all hardcoded palettes,
		// plus one new palette
		if (palettes)
			free(palettes);

		palettes = (gamePalette *)malloc(sizeof(gamePalette)*loadedPalettes);
		// Load all palettes in file, hardcoded palettes are added later
		int i = 0;
		for (section = mxmlFindElement(item, xml, "game", NULL, NULL,
				MXML_DESCEND); section; section = mxmlFindElement(section, xml,
				"game", NULL, NULL, MXML_NO_DESCEND))
		{
			loadXMLPaletteFromSection(palettes[i]);
			i++;
		}
		mxmlDelete(xml);
	}
	return result;
}

/****************************************************************************
 * FixInvalidSettings
 *
 * Attempts to correct at least some invalid settings - the ones that
 * might cause crashes
 ***************************************************************************/
void FixInvalidSettings()
{
	if(GCSettings.LoadMethod >= DEVICE_LENGTH)
		GCSettings.LoadMethod = DEVICE_AUTO;
	if(GCSettings.SaveMethod >= DEVICE_LENGTH)
		GCSettings.SaveMethod = DEVICE_AUTO;
	if(!(GCSettings.gbaZoomHor >= 0.5 && GCSettings.gbaZoomHor <= 1.6))
		GCSettings.gbaZoomHor = 1.0;
	if(!(GCSettings.gbaZoomVert >= 0.5 && GCSettings.gbaZoomVert <= 1.6))
		GCSettings.gbaZoomVert = 1.0;
	if(!(GCSettings.gbZoomHor >= 0.5 && GCSettings.gbZoomHor <= 1.6))
		GCSettings.gbZoomHor = 1.0;
	if(!(GCSettings.gbZoomVert >= 0.5 && GCSettings.gbZoomVert <= 1.6))
		GCSettings.gbZoomVert = 1.0;
	if(!(GCSettings.xshift > -50 && GCSettings.xshift < 50))
		GCSettings.xshift = 0;
	if(!(GCSettings.yshift > -50 && GCSettings.yshift < 50))
		GCSettings.yshift = 0;
	if(!(GCSettings.MusicVolume >= 0 && GCSettings.MusicVolume <= 100))
		GCSettings.MusicVolume = 20;
	if(!(GCSettings.SFXVolume >= 0 && GCSettings.SFXVolume <= 100))
		GCSettings.SFXVolume = 40;
	if(GCSettings.language < LANG_JAPANESE || GCSettings.language >= LANG_LENGTH)
		GCSettings.language = LANG_ENGLISH;
	if(!(GCSettings.render >= RENDER_FILTERED && GCSettings.render < RENDER_LENGTH))
		GCSettings.render = RENDER_FILTERED_SHARP;
	if(!(GCSettings.videomode >= VIDEOMODE_AUTO && GCSettings.videomode < VIDEOMODE_LENGTH))
		GCSettings.videomode = VIDEOMODE_AUTO;
	// Wasn't previously bounds-checked here at all (harmless today since
	// both GX_Render's filter check and the menu label default safely to
	// "off"/"None" for anything unrecognized, but a corrupted settings
	// file or a downgrade from a future version with more filters could
	// otherwise carry forward a stale/out-of-range value indefinitely).
	if(!(GCSettings.FilterMethod >= FILTER_NONE && GCSettings.FilterMethod < FILTER_LENGTH))
		GCSettings.FilterMethod = FILTER_NONE;
}

/****************************************************************************
 * DefaultSettings
 *
 * Sets all the defaults!
 ***************************************************************************/
void
DefaultSettings ()
{
	memset (&GCSettings, 0, sizeof (GCSettings));
	ResetControls(); // controller button mappings

	GCSettings.LoadMethod = DEVICE_AUTO; // Auto, SD, DVD, USB, Network (SMB)
	GCSettings.SaveMethod = DEVICE_AUTO; // Auto, SD, USB, Network (SMB)
	sprintf (GCSettings.LoadFolder, "%s/%s", APPFOLDER, loadFolder[LOADFOLDER_ROMS].name); // Path to game files
	sprintf (GCSettings.SaveFolder, "%s/%s", APPFOLDER, saveFolder[SAVEFOLDER_SAVES].name); // Path to save files
	sprintf (GCSettings.StateFolder, "%s/%s", APPFOLDER, saveFolder[SAVEFOLDER_STATES].name); // Path to save state files
	sprintf (GCSettings.GBFolder, "%s/%s", APPFOLDER, loadFolder[LOADFOLDER_GB].name); // Path to GB rom files
	sprintf (GCSettings.GBCFolder, "%s/%s", APPFOLDER, loadFolder[LOADFOLDER_GBC].name); // Path to GBC rom files
	sprintf (GCSettings.GBAFolder, "%s/%s", APPFOLDER, loadFolder[LOADFOLDER_GBA].name); // Path to GBA rom files
	sprintf (GCSettings.ScreenshotsFolder, "%s/%s", APPFOLDER, loadFolder[LOADFOLDER_SCREENSHOTS].name); // Path to screenshots files
	sprintf (GCSettings.GBABorderFolder, "%s/%s", APPFOLDER, loadFolder[LOADFOLDER_BORDERS_GBA].name); // Path to GBA border files
	sprintf (GCSettings.GBCBorderFolder, "%s/%s", APPFOLDER, loadFolder[LOADFOLDER_BORDERS_GBC].name); // Path to GB/GBC border files
	sprintf (GCSettings.CheatFolder, "%s/%s", APPFOLDER, loadFolder[LOADFOLDER_CHEATS].name); // Path to per-game .cheats files
	sprintf (GCSettings.CoverFolder, "%s/%s", APPFOLDER, loadFolder[LOADFOLDER_COVERS].name); // Path to cover files
	sprintf (GCSettings.ArtworkFolder, "%s/%s", APPFOLDER, loadFolder[LOADFOLDER_ARTWORK].name); // Path to artwork files

	GCSettings.AutoLoad = 1;
	GCSettings.AutoSave = 1;
	GCSettings.AppendAuto = 1;

	GCSettings.gbaZoomHor = 1.0; // GBA horizontal zoom level
	GCSettings.gbaZoomVert = 1.0; // GBA vertical zoom level
	GCSettings.gbZoomHor = 1.0; // GBA horizontal zoom level
	GCSettings.gbZoomVert = 1.0; // GBA vertical zoom level
	GCSettings.gbFixed = 0; // not fixed - use zoom level
	GCSettings.gbaFixed = 0; // not fixed - use zoom level
	GCSettings.videomode = VIDEOMODE_AUTO;
	GCSettings.render = RENDER_FILTERED_SHARP;
	GCSettings.scaling = SCALING_PARTIAL_STRETCH;

	GCSettings.xshift = 0; // horizontal video shift
	GCSettings.yshift = 0; // vertical video shift

	GCSettings.WiimoteOrientation = WIIMOTEORIENTATION_VERTICAL;
#ifdef HW_RVL
	GCSettings.ExitAction = EXITACTION_WII_AUTO;
#else
	GCSettings.ExitAction = EXITACTION_GC_RETURN_TO_LOADER;
#endif
	GCSettings.AutoloadGame = 0;
	GCSettings.MusicVolume = 20;
	GCSettings.SFXVolume = 40;
	GCSettings.Rumble = 1;
	GCSettings.PreviewImage = PREVIEWIMAGE_COVER;
	
	GCSettings.BasicPalette = 0;
	GCSettings.MotionTilt = 1; // On by default - Wii Remote tilt for tilt-sensor GB/GBC games
	GCSettings.ClassicBrowser = 0; // Off by default - tabbed browser is the new default interface
	GCSettings.GBAColorEmulation = 0; // Off by default - stylistic choice, let people opt in
	GCSettings.GBCColorEmulation = 0;
	GCSettings.InterframeBlending = 0; // Off by default - only a handful of games rely on it
	GCSettings.FastForwardSpeed = 0; // Off (1x) by default
	GCSettings.Frameskip = 0; // Off by default
	GCSettings.FilterMethod = FILTER_NONE; // Off by default - stylistic choice, let people opt in
	
#ifdef HW_RVL
	GCSettings.language = CONF_GetLanguage();

	if(GCSettings.language == LANG_TRAD_CHINESE)
		GCSettings.language = LANG_SIMP_CHINESE;
#else
	GCSettings.language = SYS_GetLanguage() + LANG_ENGLISH;
#endif
	GCSettings.OffsetMinutesUTC = 0;
	GCSettings.GBHardware = 0;
	GCSettings.SGBBorder = 0;
	GCSettings.GBABorderFile[0] = 0; // "" = None by default
	GCSettings.GBBorderFile[0] = 0; // "" = None by default
}


/****************************************************************************
 * Save Preferences
 ***************************************************************************/
static char prefpath[MAXPATHLEN] = { 0 };

bool
SavePrefs (bool silent)
{
	char filepath[MAXPATHLEN];
	int datasize;
	int offset = 0;
	int device = DEVICE_AUTO;
	
	if(prefpath[0] != 0) {
		sprintf(filepath, "%s/%s", prefpath, PREF_FILE_NAME);
		FindDevice(filepath, &device);
	}
	else if(appPath[0] != 0)
	{
		sprintf(filepath, "%s/%s", appPath, PREF_FILE_NAME);
		strcpy(prefpath, appPath);
		FindDevice(filepath, &device);
	}
	else
	{
		autoSaveMethod(true);
		device = GCSettings.SaveMethod;

		if(!ChangeInterface(device, silent)) {
			return false;
		}
		
		sprintf(filepath, "%s%s", pathPrefix[device], APPFOLDER);
		if(!CreateDirectory(filepath)) {
			return false;
		}

		sprintf(filepath, "%s%s/%s", pathPrefix[device], APPFOLDER, PREF_FILE_NAME);
		sprintf(prefpath, "%s%s", pathPrefix[device], APPFOLDER);
	}
	
	if(device == DEVICE_AUTO)
		return false;

	printf("[prefs] SavePrefs: device=%d filepath=%s\n", device, filepath);

	if (!silent)
		ShowAction ("Saving preferences...");

	FixInvalidSettings();

	AllocSaveBuffer ();
	datasize = preparePrefsData ();

	offset = SaveFile(filepath, datasize, silent);
	printf("[prefs] SaveFile(%s, %d bytes) -> offset=%d\n", filepath, datasize, offset);

	FreeSaveBuffer ();

	CancelAction();

	if (offset > 0)
	{
		if (!silent)
			InfoPrompt("Preferences saved");

		if(appPath[0] == 0)
			strcpy(appPath, prefpath);
		return true;
	}
	return false;
}

/****************************************************************************
 * Load Preferences from specified filepath
 ***************************************************************************/
bool
LoadPrefsFromMethod (char * path)
{
	bool retval = false;
	int offset = 0;
	char filepath[MAXPATHLEN];
	sprintf(filepath, "%s/%s", path, PREF_FILE_NAME);

	AllocSaveBuffer ();

	offset = LoadFile(filepath, SILENT);
	printf("[prefs] LoadFile(%s) -> offset=%d\n", filepath, offset);

	if (offset > 0) {
		retval = decodePrefsData ();
		printf("[prefs] decodePrefsData() -> %s\n", retval ? "true" : "false");
	}

	FreeSaveBuffer ();

	if(retval)
	{
		strcpy(prefpath, path);

		if(appPath[0] == 0)
			strcpy(appPath, prefpath);
	}

	return retval;
}

/****************************************************************************
 * Load Preferences
 * Checks sources consecutively until we find a preference file
 ***************************************************************************/
static bool prefLoadAttempted = false;
static bool prefFound = false;

bool LoadPrefs()
{
	if(prefLoadAttempted) // already attempted loading
		return true;

	prefLoadAttempted = true;

	bool prefFound = false;
	char filepath[5][MAXPATHLEN];
	int numDevices;

	// NOTE: a USB-mount retry loop was tried here to fix settings.xml
	// sometimes not being found on USB-based installs (see git history/
	// chat log), but it's pulled out for now while we isolate a black-
	// screen-on-real-hardware regression that appeared after adding it.
	// The retry loop itself only calls ChangeInterface(), which is used
	// safely elsewhere in this exact form - but it's the newest, least-
	// tested code path running this early in boot, so it's the first
	// thing to rule in/out.

#ifdef HW_RVL
	numDevices = 5;
	sprintf(filepath[0], "%s", appPath);
	sprintf(filepath[1], "sd:/apps/%s", APPFOLDER);
	sprintf(filepath[2], "usb:/apps/%s", APPFOLDER);
	sprintf(filepath[3], "sd:/%s", APPFOLDER);
	sprintf(filepath[4], "usb:/%s", APPFOLDER);
#else
	numDevices = 4;
	sprintf(filepath[0], "carda:/%s", APPFOLDER);
	sprintf(filepath[1], "cardb:/%s", APPFOLDER);
	sprintf(filepath[2], "port2:/%s", APPFOLDER);
	sprintf(filepath[3], "gcloader:/%s", APPFOLDER);
#endif

	for(int i=0; i<numDevices; i++) {
		printf("[prefs] LoadPrefs: trying %s\n", filepath[i]);
		prefFound = LoadPrefsFromMethod(filepath[i]);

		if(prefFound)
			break;
	}
	printf("[prefs] LoadPrefs: prefFound=%s\n", prefFound ? "true" : "false");

	// Background music is a bundled default asset, not something read out of
	// settings.xml - it must load regardless of whether a prefs file was
	// found this boot. Previously this was set up further down, after the
	// "!prefFound" early return, so a missing/deleted settings.xml (e.g.
	// right after a fresh install, or after manually deleting it) silently
	// left bg_music/bg_music_size uninitialized and the title screen music
	// never played, even though sound effects (separate, hardcoded assets)
	// were unaffected.
#ifdef HW_RVL
	bg_music = (u8 * )bg_music_ogg;
	bg_music_size = bg_music_ogg_size;
	LoadBgMusic();
#endif

	if(!prefFound) {
		return false;
	}

	FixInvalidSettings();

	if(GCSettings.videomode > VIDEOMODE_AUTO) {
		ResetVideo_Menu();
	}

	ChangeLanguage();
	return true;
}

void CreatePathWithPrefix(int device, const char* folder) {
    char fullPath[MAXPATHLEN];
    MakeFilePathForFolderPath(fullPath, device, folder);
    CreateDirectory(fullPath);
}

void CreateMissingDirectories() {
    char defaultFolder[MAXPATHLEN];

    if (GCSettings.SaveMethod > DEVICE_AUTO && ChangeInterface(GCSettings.SaveMethod, NOTSILENT)) {
        const char* savePointers[] = { GCSettings.SaveFolder, GCSettings.StateFolder };

        for (int i = 0; i < SAVEFOLDER_LENGTH; i++) {
            const char* currentPath = savePointers[i];

            if (strncmp(currentPath, APPFOLDER, strlen(APPFOLDER)) == 0) {
                CreatePathWithPrefix(GCSettings.SaveMethod, APPFOLDER);
            }

            GetDefaultFolderPath(defaultFolder, saveFolder[i].name);
            if (strcmp(currentPath, defaultFolder) == 0) {
                CreatePathWithPrefix(GCSettings.SaveMethod, currentPath);
            }
        }
    }

    if (GCSettings.LoadMethod > DEVICE_AUTO && GCSettings.LoadMethod != DEVICE_DVD && ChangeInterface(GCSettings.LoadMethod, NOTSILENT)) {
        const char* loadPointers[] = {
            GCSettings.LoadFolder,
            GCSettings.ScreenshotsFolder,
            GCSettings.CoverFolder,
            GCSettings.ArtworkFolder,
			GCSettings.GBABorderFolder,
			GCSettings.GBCBorderFolder,
			GCSettings.GBFolder,
			GCSettings.GBCFolder,
			GCSettings.GBAFolder,
			GCSettings.CheatFolder
        };

        for (int i = 0; i < LOADFOLDER_LENGTH; i++) {
            const char* currentPath = loadPointers[i];

            if (strncmp(currentPath, APPFOLDER, strlen(APPFOLDER)) == 0) {
                CreatePathWithPrefix(GCSettings.LoadMethod, APPFOLDER);
            }

            GetDefaultFolderPath(defaultFolder, loadFolder[i].name);
            if (strcmp(currentPath, defaultFolder) == 0) {
                CreatePathWithPrefix(GCSettings.LoadMethod, currentPath);
            }
        }
    }
}