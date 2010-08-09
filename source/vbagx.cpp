/****************************************************************************
 * Visual Boy Advance GX
 *
 * Tantric September 2008
 *
 * vbagx.cpp
 *
 * This file controls overall program flow. Most things start and end here!
 ***************************************************************************/

#include <gccore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ogcsys.h>
#include <unistd.h>
#include <wiiuse/wpad.h>
#include <sys/iosupport.h>

#ifdef HW_RVL
#include <di/di.h>
#endif

#include "vbagx.h"
#include "vbasupport.h"
#include "preferences.h"
#include "audio.h"
#include "networkop.h"
#include "filebrowser.h"
#include "fileop.h"
#include "menu.h"
#include "input.h"
#include "video.h"
#include "gamesettings.h"
#include "mem2.h"
#include "utils/FreeTypeGX.h"

#include "vba/gba/Globals.h"
#include "vba/gba/Sound.h"

extern "C" {
extern void __exception_setreload(int t);
extern u32 __di_check_ahbprot(void);
}

extern int emulating;
void StopColorizing();
void gbSetPalette(u32 RRGGBB[]);
int ScreenshotRequested = 0;
int ConfigRequested = 0;
int ShutdownRequested = 0;
int ResetRequested = 0;
int ExitRequested = 0;
char appPath[1024] = { 0 };
char loadedFile[1024] = { 0 };

/****************************************************************************
 * Shutdown / Reboot / Exit
 ***************************************************************************/

static void ExitCleanup()
{
	ShutdownAudio();
	StopGX();
	HaltDeviceThread();
	UnmountAllFAT();

#ifdef HW_RVL
	DI_Close();
#endif
}

#ifdef HW_DOL
	#define PSOSDLOADID 0x7c6000a6
	int *psoid = (int *) 0x80001800;
	void (*PSOReload) () = (void (*)()) 0x80001800;
#endif

void ExitApp()
{
#ifdef HW_RVL
	ShutoffRumble();
#endif

	SavePrefs(SILENT);

	if (ROMLoaded && !ConfigRequested && GCSettings.AutoSave == 1)
		SaveBatteryOrStateAuto(FILE_SRAM, SILENT);

	ExitCleanup();

	if(ShutdownRequested)
		SYS_ResetSystem(SYS_POWEROFF, 0, 0);

	#ifdef HW_RVL
	if(GCSettings.ExitAction == 0) // Auto
	{
		char * sig = (char *)0x80001804;
		if(
			sig[0] == 'S' &&
			sig[1] == 'T' &&
			sig[2] == 'U' &&
			sig[3] == 'B' &&
			sig[4] == 'H' &&
			sig[5] == 'A' &&
			sig[6] == 'X' &&
			sig[7] == 'X')
			GCSettings.ExitAction = 3; // Exit to HBC
		else
			GCSettings.ExitAction = 1; // HBC not found
	}
	#endif

	if(GCSettings.ExitAction == 1) // Exit to Menu
	{
		#ifdef HW_RVL
			SYS_ResetSystem(SYS_RETURNTOMENU, 0, 0);
		#else
			#define SOFTRESET_ADR ((volatile u32*)0xCC003024)
			*SOFTRESET_ADR = 0x00000000;
		#endif
	}
	else if(GCSettings.ExitAction == 2) // Shutdown Wii
	{
		SYS_ResetSystem(SYS_POWEROFF, 0, 0);
	}
	else // Exit to Loader
	{
		#ifdef HW_RVL
			exit(0);
		#else
			if (psoid[0] == PSOSDLOADID)
				PSOReload();
		#endif
	}
}

#ifdef HW_RVL
void ShutdownCB()
{
	if(!inNetworkInit)
		ShutdownRequested = 1;
}
void ResetCB()
{
	if(!inNetworkInit)
		ResetRequested = 1;
}
#endif

#ifdef HW_DOL
/****************************************************************************
 * ipl_set_config
 * lowlevel Qoob Modchip disable
 ***************************************************************************/

static void ipl_set_config(unsigned char c)
{
	volatile unsigned long* exi = (volatile unsigned long*)0xCC006800;
	unsigned long val,addr;
	addr=0xc0000000;
	val = c << 24;
	exi[0] = ((((exi[0]) & 0x405) | 256) | 48);	//select IPL
	//write addr of IPL
	exi[0 * 5 + 4] = addr;
	exi[0 * 5 + 3] = ((4 - 1) << 4) | (1 << 2) | 1;
	while (exi[0 * 5 + 3] & 1);
	//write the ipl we want to send
	exi[0 * 5 + 4] = val;
	exi[0 * 5 + 3] = ((4 - 1) << 4) | (1 << 2) | 1;
	while (exi[0 * 5 + 3] & 1);

	exi[0] &= 0x405;	//deselect IPL
}
#endif

/****************************************************************************
 * IOS Check
 ***************************************************************************/
#ifdef HW_RVL
static bool FindIOS(u32 ios)
{
	s32 ret;
	u32 n;

	u64 *titles = NULL;
	u32 num_titles=0;

	ret = ES_GetNumTitles(&num_titles);
	if (ret < 0)
		return false;

	if(num_titles < 1) 
		return false;

	titles = (u64 *)memalign(32, num_titles * sizeof(u64) + 32);
	if (!titles)
		return false;

	ret = ES_GetTitles(titles, num_titles);
	if (ret < 0)
	{
		free(titles);
		return false;
	}
		
	for(n=0; n < num_titles; n++)
	{
		if((titles[n] & 0xFFFFFFFF)==ios) 
		{
			free(titles); 
			return true;
		}
	}
    free(titles); 
	return false;
}

bool SaneIOS()
{
	bool res = false;
	u32 num_titles=0;
	u32 tmd_size;
	u32 ios = IOS_GetVersion();
	u32 tmdbuffer[MAX_SIGNED_TMD_SIZE] ATTRIBUTE_ALIGN(32); 

	if(ios > 200)
		return false;

	if (ES_GetNumTitles(&num_titles) < 0)
		return false;

	if(num_titles < 1) 
		return false;

	u64 *titles = (u64 *)memalign(32, num_titles * sizeof(u64) + 32);

	if (ES_GetTitles(titles, num_titles) < 0)
	{
		free(titles);
		return false;
	}

	for(u32 n=0; n < num_titles; n++)
	{
		if((titles[n] & 0xFFFFFFFF) != ios) 
			continue;

		if (ES_GetStoredTMDSize(titles[n], &tmd_size) < 0)
			break;

		if (tmd_size > 4096)
			break;

		if (ES_GetStoredTMD(titles[n], (signed_blob *)tmdbuffer, tmd_size) < 0)
			break;

		if (tmdbuffer[1] || tmdbuffer[2])
		{
			res = true;
			break;
		}
	}
    free(titles);
	return res;
}
#endif
/****************************************************************************
 * USB Gecko Debugging
 ***************************************************************************/

static bool gecko = false;
static mutex_t gecko_mutex = 0;

static ssize_t __out_write(struct _reent *r, int fd, const char *ptr, size_t len)
{
	u32 level;

	if (!ptr || len <= 0 || !gecko)
		return -1;

	LWP_MutexLock(gecko_mutex);
	level = IRQ_Disable();
	usb_sendbuffer(1, ptr, len);
	IRQ_Restore(level);
	LWP_MutexUnlock(gecko_mutex);
	return len;
}

const devoptab_t gecko_out = {
	"stdout",	// device name
	0,			// size of file structure
	NULL,		// device open
	NULL,		// device close
	__out_write,// device write
	NULL,		// device read
	NULL,		// device seek
	NULL,		// device fstat
	NULL,		// device stat
	NULL,		// device link
	NULL,		// device unlink
	NULL,		// device chdir
	NULL,		// device rename
	NULL,		// device mkdir
	0,			// dirStateSize
	NULL,		// device diropen_r
	NULL,		// device dirreset_r
	NULL,		// device dirnext_r
	NULL,		// device dirclose_r
	NULL		// device statvfs_r
};

void USBGeckoOutput()
{
	LWP_MutexInit(&gecko_mutex, false);
	gecko = usb_isgeckoalive(1);
	
	devoptab_list[STD_OUT] = &gecko_out;
	devoptab_list[STD_ERR] = &gecko_out;
}

/****************************************************************************
* main
*
* Program entry
****************************************************************************/
int main(int argc, char *argv[])
{
	//USBGeckoOutput(); // uncomment to enable USB gecko output
	__exception_setreload(8);

	#ifdef HW_DOL
	ipl_set_config(6); // disable Qoob modchip
	#endif

#ifdef HW_RVL
	// only reload IOS if AHBPROT is not enabled
	u32 version = IOS_GetVersion();

	if(version != 58 && __di_check_ahbprot() != 1)
	{
		if(FindIOS(58))
			IOS_ReloadIOS(58);
		else if((version < 61 || version >= 200) && FindIOS(61))
			IOS_ReloadIOS(61);
	}
	DI_Init();
#endif

	InitDeviceThread();
	InitializeVideo();
	ResetVideo_Menu (); // change to menu video mode
	SetupPads();
	
	// Initialize DVD subsystem (GameCube only)
	#ifdef HW_DOL
	DVD_Init ();
	#endif

	#ifdef HW_RVL
	// Wii Power/Reset buttons
	WPAD_SetPowerButtonCallback((WPADShutdownCallback)ShutdownCB);
	SYS_SetPowerCallback(ShutdownCB);
	SYS_SetResetCallback(ResetCB);
	#endif

	MountAllFAT(); // Initialize libFAT for SD and USB

	InitialiseSound();
	InitialisePalette();
	DefaultSettings (); // Set defaults
	InitFreeType((u8*)font_ttf, font_ttf_size); // Initialize font system
#ifdef HW_RVL
	if(argc > 0 && argv[0] != NULL)
		CreateAppPath(argv[0]); // store path app was loaded from

	InitMem2Manager();
	savebuffer = (unsigned char *)mem2_malloc(SAVEBUFFERSIZE);
	browserList = (BROWSERENTRY *)mem2_malloc(sizeof(BROWSERENTRY)*MAX_BROWSER_SIZE);
	rom = (u8 *)mem2_malloc(1024*1024*32); // allocate 32 MB to GBA ROM
#else
	savebuffer = (unsigned char *)malloc(SAVEBUFFERSIZE);
	browserList = (BROWSERENTRY *)malloc(sizeof(BROWSERENTRY)*MAX_BROWSER_SIZE);
#endif
	gameScreenPng = (u8 *)malloc(512*1024);
	InitGUIThreads();

	while(1) // main loop
	{
		// go back to checking if devices were inserted/removed
		// since we're entering the menu
		ResumeDeviceThread();

		SwitchAudioMode(1);

		if(!ROMLoaded)
			MainMenu(MENU_GAMESELECTION);
		else
			MainMenu(MENU_GAME);

		ConfigRequested = 0;
		ScreenshotRequested = 0;
		SwitchAudioMode(0);

		// stop checking if devices were removed/inserted
		// since we're starting emulation again
		HaltDeviceThread();

		ResetVideo_Emu();

		// GB colorizing - set palette
		if(IsGameboyGame())
		{
			if(GCSettings.colorize && strcmp(RomTitle, "MEGAMAN") != 0)
				gbSetPalette(CurrentPalette.palette);
			else
				StopColorizing();
		}

		while (emulating) // emulation loop
		{
			emulator.emuMain(emulator.emuCount);

			if(ResetRequested)
			{
				emulator.emuReset(); // reset game
				ResetRequested = 0;
			}
			if(ConfigRequested)
			{
				ResetVideo_Menu();
				break; // leave emulation loop
			}
			#ifdef HW_RVL
			if(ShutdownRequested)
				ExitApp();
			#endif
		} // emulation loop
	} // main loop
	return 0;
}
