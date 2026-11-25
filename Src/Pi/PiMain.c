/*****************************************************************************
**
** blueberryMSX
** https://github.com/pokebyte/blueberryMSX
**
** An MSX Emulator for Raspberry Pi based on blueMSX
**
** Copyright (C) 2003-2006 Daniel Vik
** Copyright (C) 2014 Akop Karapetyan
**
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation; either version 2 of the License, or
** (at your option) any later version.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
**
******************************************************************************
*/

#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include <SDL.h>

//#ifdef RASPI_GPIO
#include "PiGpio.h"
//#endif

#include "CommandLine.h"
#include "Properties.h"
#include "ArchFile.h"
#include "VideoRender.h"
#include "AudioMixer.h"
#include "Casette.h"
#include "Led.h"
#include "PrinterIO.h"
#include "UartIO.h"
#include "MidiIO.h"
#include "Machine.h"
#include "Board.h"
#include "Emulator.h"
#include "FileHistory.h"
#include "InputEvent.h"
#include "Actions.h"
#include "Language.h"
#include "LaunchFile.h"
#include "ArchEvent.h"
#include "ArchSound.h"
#include "ArchNotifications.h"
#include "JoystickPort.h"
#include "PiShortcuts.h"
#include "PiVideo.h"
#include "PiUdev.h"
#include "PiMouse.h"
#include "PiInput.h"
#include "InputEvent.h"

void keyboardInit(Properties *properties);
void keyboardUpdate(SDL_KeyboardEvent *event);
int piKeyboardEvdevInit(void);
void piKeyboardEvdevDestroy(void);
void joystickButtonUpdate(SDL_JoyButtonEvent *event);
void joystickAxisUpdate(SDL_JoyAxisEvent *event);
void actionToggleVideoColorMode(void);
#define EVENT_UPDATE_DISPLAY 2

static void setDefaultPaths(const char* rootDir);
static void handleEvent(SDL_Event* event);

Video *video;
Properties *properties;
static Mixer *mixer;

static Shortcuts* shortcuts;
static int doQuit = 0;

static long displayUpdates = 0;
static int pendingDisplayEvents = 0;
static void *dpyUpdateAckEvent = NULL;

#define JOYSTICK_COUNT 2
static SDL_Joystick *joysticks[JOYSTICK_COUNT];

int archUpdateEmuDisplay(int syncMode)
{
	//fprintf(stderr,"PiMain.c is calling SDL_Event \n");
	SDL_Event event;
	//fprintf(stderr,"PiMain.c SDL_Event returned %d\n",event);

	//fprintf(stderr,"PiMain.c pendingDisplayEvents is %d\n", pendingDisplayEvents);
	if (pendingDisplayEvents > 1) {
		return 1;
	}

	pendingDisplayEvents++;

	event.type = SDL_USEREVENT;
	//fprintf(stderr,"PiMain.c event.type is %d\n", event.type);

	event.user.code = EVENT_UPDATE_DISPLAY;
	event.user.data1 = NULL;
	event.user.data2 = NULL;

	SDL_PushEvent(&event);

	if (properties->emulation.syncMethod == P_EMU_SYNCFRAMES) {
//		archEventWait(dpyUpdateAckEvent, 500);
	}

	return 1;
}

void archUpdateWindow()
{
}

void archEmulationStartNotification()
{
}

void archEmulationStopNotification()
{
}

void archEmulationStartFailure()
{
}

void archTrap(UInt8 value)
{
}

void archQuit()
{
	doQuit = 1;
	SDL_Quit();
	//system("sudo aconnect -x");
}

static void updateLeds()
{
	gpioUpdateLeds();
}

extern uint32_t screenWidth;
extern uint32_t screenHeight;

static int joystickSlot(SDL_JoystickID id)
{
    for (int i = 0; i < JOYSTICK_COUNT; i++) {
        if (joysticks[i] &&
            SDL_JoystickInstanceID(joysticks[i]) == id) {
            return i;
        }
    }
    return -1;
}
static void handleEvent(SDL_Event* event)
{
	switch (event->type) {
	case SDL_JOYHATMOTION:
	    {
	        int slot = joystickSlot(event->jhat.which);

	        if (slot >= 0 && event->jhat.hat == 0) {
	            SDL_JoyAxisEvent axis = {0};

	            axis.which = slot;

	            /* 左右 */
	            axis.axis = 0;
	            axis.value =
	                (event->jhat.value & SDL_HAT_LEFT)  ? -32767 :
	                (event->jhat.value & SDL_HAT_RIGHT) ?  32767 : 0;
	            joystickAxisUpdate(&axis);

	            /* 上下 */
	            axis.axis = 1;
	            axis.value =
	                (event->jhat.value & SDL_HAT_UP)   ? -32767 :
	                (event->jhat.value & SDL_HAT_DOWN) ?  32767 : 0;
	            joystickAxisUpdate(&axis);
	        }
	        break;
	    }
	    case SDL_JOYDEVICEADDED:
	    {
	        int index = event->jdevice.which;
		SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(index);
		int alreadyOpen = 0;

		for (int i = 0; i < JOYSTICK_COUNT; i++) {
		    if (joysticks[i] &&
		        SDL_JoystickInstanceID(joysticks[i]) == id) {
		        alreadyOpen = 1;
		        break;
		    }
		}

		if (alreadyOpen) {
		    fprintf(stderr, "[JOY] already opened id=%d\n", (int)id);
		    break;
		}

	        SDL_Joystick *joy = SDL_JoystickOpen(index);

	        if (joy) {
	            int i;
	            for (i = 0; i < JOYSTICK_COUNT; i++) {
	                if (joysticks[i] == NULL) {
	                    joysticks[i] = joy;
	                    fprintf(stderr, "[JOY] added slot=%d index=%d name=%s\n",
	                            i, index, SDL_JoystickName(joy));
	                    break;
	                }
	            }
	            if (i == JOYSTICK_COUNT) {
	                SDL_JoystickClose(joy);
	            }
	        }
	        break;
	    }

	    case SDL_JOYDEVICEREMOVED:
	    {
	        SDL_JoystickID id = event->jdevice.which;
	        for (int i = 0; i < JOYSTICK_COUNT; i++) {
	            if (joysticks[i] &&
	                SDL_JoystickInstanceID(joysticks[i]) == id) {
	                fprintf(stderr, "[JOY] removed slot=%d id=%d\n", i, (int)id);
	                SDL_JoystickClose(joysticks[i]);
	                joysticks[i] = NULL;
	            }
	        }
	        inputEventReset();
	        break;
	    }
	case SDL_USEREVENT:
		switch (event->user.code) {
		case EVENT_UPDATE_DISPLAY:
			if (displayUpdates++ == 0) {
				piMouseInit();
				piMouseSetCaptureRect(0, 0, screenWidth, screenHeight);
				
				// Prevent joystick sticking
				inputEventReset();
			}

			piUpdateEmuDisplay();
			updateLeds();
			//archEventSet(dpyUpdateAckEvent);
			pendingDisplayEvents--;
			break;
		}
		break;
	case SDL_JOYBUTTONDOWN:
	case SDL_JOYBUTTONUP:
        if (event->type == SDL_JOYBUTTONUP)
        {
            if (inputEventGetState(EC_JOY_BUTTONL) * inputEventGetState(EC_JOY_BUTTONR))
                actionQuit();
			else if (inputEventGetState(EC_JOY_BUTTONL) * inputEventGetState(EC_JOY1_BUTTON4))	// plaire
				actionToggleVideoColorMode();
			else if (inputEventGetState(EC_JOY_BUTTONL) * inputEventGetState(EC_JOY1_BUTTON2))
				actionToggleScanlinesEnable();
            else if (inputEventGetState(EC_JOY_BUTTONR))	// plaire avoid key duplicate
                actionDiskQuickChange();
        }
		{
		    int slot = joystickSlot(event->jbutton.which);
		    if (slot >= 0) {
		        SDL_JoyButtonEvent button = event->jbutton;
		        button.which = slot;
		        joystickButtonUpdate(&button);
		    }
		}
		break;
	case SDL_JOYAXISMOTION:
		{
		    int slot = joystickSlot(event->jaxis.which);
		    if (slot >= 0) {
		        SDL_JoyAxisEvent axis = event->jaxis;
		        axis.which = slot;
		        joystickAxisUpdate(&axis);
		    }
		}
		break;
	case SDL_KEYDOWN:
	case SDL_KEYUP:
		keyboardUpdate(&event->key);
		if (event->type == SDL_KEYDOWN)
			shortcutCheckDown(shortcuts, HOTKEY_TYPE_KEYBOARD, event->key.keysym.mod, event->key.keysym.sym);
		else
			shortcutCheckUp(shortcuts, HOTKEY_TYPE_KEYBOARD, event->key.keysym.mod, event->key.keysym.sym);
		break;
	// DEPRECATED on sdl2 -- case SDL_ACTIVEEVENT:
	case SDL_WINDOWEVENT_ENTER:
		inputEventReset();
		break;
	case SDL_MOUSEBUTTONDOWN:
	case SDL_MOUSEBUTTONUP:
		piMouseButton(event->button.button, event->button.state);
		break;
	case SDL_MOUSEMOTION:
		piMouseMove(event->motion.x, event->motion.y);
		break;
	}
}

static void setDefaultPaths(const char* rootDir)
{
	char buffer[512]; // FIXME

	propertiesSetDirectory(rootDir, rootDir);

	sprintf(buffer, "%s/Audio Capture", rootDir);
	archCreateDirectory(buffer);
	actionSetAudioCaptureSetDirectory(buffer, "");

	sprintf(buffer, "%s/Video Capture", rootDir);
	archCreateDirectory(buffer);
	actionSetAudioCaptureSetDirectory(buffer, "");

	sprintf(buffer, "%s/QuickSave", rootDir);
	archCreateDirectory(buffer);
	actionSetQuickSaveSetDirectory(buffer, "");

	sprintf(buffer, "%s/SRAM", rootDir);
	archCreateDirectory(buffer);
	boardSetDirectory(buffer);

	sprintf(buffer, "%s/Casinfo", rootDir);
	archCreateDirectory(buffer);
	tapeSetDirectory(buffer, "");

	sprintf(buffer, "%s/Databases", rootDir);
	archCreateDirectory(buffer);
	mediaDbLoad(buffer);

	sprintf(buffer, "%s/Shortcut Profiles", rootDir);
	archCreateDirectory(buffer);
	shortcutsSetDirectory(buffer);

	sprintf(buffer, "%s/Machines", rootDir);
	archCreateDirectory(buffer);
	machineSetDirectory(buffer);

	sprintf(buffer, "%s/Media", rootDir);
	archCreateDirectory(buffer);
}

#ifndef BLUEMSX_VERSION
#define BLUEMSX_VERSION "dev"
#endif

int main(int argc, char **argv)
{
	printf("BlueberryMSX 2.0 Plus v%s\n", BLUEMSX_VERSION);

	/* The physical keyboard is shared with the tty: a physical Ctrl+C
	 * generates SIGINT for the foreground process. Ignore it so the
	 * emulator keeps running and the key reaches the MSX (BASIC BREAK);
	 * quitting is done with the F12 hotkey. */
	signal(SIGINT, SIG_IGN);

//#ifdef RASPI_GPIO
	//fprintf(stderr,"PiMain is calling gpioInit()\n");
	gpioInit();
//#endif

	if (!piInitUdev()) {
		fprintf(stderr, "piInitUdev() failed");
		piDestroyVideo();
		return 1;
	}
	
	if (!piInitVideo()) {
		fprintf(stderr, "piInitVideo() failed");
		return 1;
	}

	if (SDL_InitSubSystem(SDL_INIT_TIMER|SDL_INIT_EVENTS|SDL_INIT_GAMECONTROLLER|SDL_INIT_AUDIO) != 0){
		fprintf(stderr,"PiMain SDL_Init failed: %s\n", SDL_GetError());
		/*SDL_Quit();
		exit(1);*/
	}

/*	if (SDL_InitSubSystem(SDL_INIT_TIMER|SDL_INIT_EVENTS|SDL_INIT_GAMECONTROLLER|SDL_INIT_AUDIO) != 0){
		fprintf(stderr,"PiMain SDL_INIT_EVERYTHING failed: %s\n", SDL_GetError());
		//SDL_Quit();
		//exit(1);
	}
*/

	SDL_ShowCursor(SDL_DISABLE);
	SDL_JoystickEventState(SDL_ENABLE);

	//fprintf(stderr,"PiMain.c is calling SDL_Event event\n");
	SDL_Event event;
	char szLine[8192] = "";
	int resetProperties;
	char path[512] = "";
	int i;

	for (i = 1; i < argc; i++) {
		if (strchr(argv[i], ' ') != NULL && argv[i][0] != '\"') {
			strcat(szLine, "\"");
			strcat(szLine, argv[i]);
			strcat(szLine, "\"");
		} else {
			strcat(szLine, argv[i]);
		}
		strcat(szLine, " ");
	}

	fprintf(stderr, "[JOY] SDL_NumJoysticks=%d\n", SDL_NumJoysticks());

	for (i = 0; i < JOYSTICK_COUNT; i++) {
	    joysticks[i] = SDL_JoystickOpen(i);

	    if (joysticks[i]) {
	        fprintf(stderr,
	                "[JOY] index=%d attached=%d axes=%d buttons=%d name=%s\n",
	                i,
	                SDL_JoystickGetAttached(joysticks[i]),
	                SDL_JoystickNumAxes(joysticks[i]),
	                SDL_JoystickNumButtons(joysticks[i]),
	                SDL_JoystickName(joysticks[i]));
	    }
	}

	setDefaultPaths(archGetCurrentDirectory());

	resetProperties = emuCheckResetArgument(szLine);
	strcat(path, archGetCurrentDirectory());
	strcat(path, DIR_SEPARATOR "bluemsx.ini");
	
    //system("aconnect 20:0 128:0");
	
	properties = propCreate(resetProperties, 0, P_KBD_EUROPEAN, 0, "");

	if (resetProperties == 2) {
		piDestroyVideo();
		piDestroyUdev();
		propDestroy(properties);
		return 0;
	}

	video = videoCreate();
	videoSetPalMode(video, VIDEO_PAL_FAST);
	videoSetColors(video, properties->video.saturation,
		properties->video.brightness,
		properties->video.contrast, properties->video.gamma);
	videoSetScanLines(video, properties->video.scanlinesEnable,
		properties->video.scanlinesPct);
	videoSetColorSaturation(video, properties->video.colorSaturationEnable,
		properties->video.colorSaturationWidth);

	dpyUpdateAckEvent = archEventCreate(0);

	fprintf(stderr,"PiMain is calling keyboardInit with %d\n",properties);
	keyboardInit(properties);
	piKeyboardEvdevInit();

	// Larger buffers cause sound delay
	// properties->sound.bufSize = 40;

	mixer = mixerCreate();
	for (i = 0; i < MIXER_CHANNEL_TYPE_COUNT; i++) {
		mixerSetChannelTypeVolume(mixer, i,
			properties->sound.mixerChannel[i].volume);
		mixerSetChannelTypePan(mixer, i,
			properties->sound.mixerChannel[i].pan);
		mixerEnableChannelType(mixer, i,
			properties->sound.mixerChannel[i].enable);
	}
	mixerSetMasterVolume(mixer, properties->sound.masterVolume);
	mixerEnableMaster(mixer, properties->sound.masterEnable);

	emulatorInit(properties, mixer);
	actionInit(video, properties, mixer);
	tapeSetReadOnly(properties->cassette.readOnly);

	langInit();
	langSetLanguage(properties->language);

	joystickPortSetType(0, properties->joy1.typeId);
	joystickPortSetType(1, properties->joy2.typeId);

	printerIoSetType(properties->ports.Lpt.type,
		properties->ports.Lpt.fileName);
	printerIoSetType(properties->ports.Lpt.type,
		properties->ports.Lpt.fileName);
	uartIoSetType(properties->ports.Com.type,
		properties->ports.Com.fileName);
	midiIoSetMidiOutType(properties->sound.MidiOut.type,
		properties->sound.MidiOut.fileName);
	midiIoSetMidiInType(properties->sound.MidiIn.type,
		properties->sound.MidiIn.fileName);
	ykIoSetMidiInType(properties->sound.YkIn.type,
		properties->sound.YkIn.fileName);

	emulatorRestartSound();

	videoUpdateAll(video, properties);

	shortcuts = shortcutsCreate();

	mediaDbSetDefaultRomType(properties->cartridge.defaultType);

	for (i = 0; i < PROP_MAX_CARTS; i++) {
		if (properties->media.carts[i].fileName[0]) {
			insertCartridge(properties, i,
				properties->media.carts[i].fileName,
				properties->media.carts[i].fileNameInZip,
				properties->media.carts[i].type, -1);
		}
		updateExtendedRomName(i,
			properties->media.carts[i].fileName,
			properties->media.carts[i].fileNameInZip);
	}

	for (i = 0; i < PROP_MAX_DISKS; i++) {
		if (properties->media.disks[i].fileName[0]) {
			insertDiskette(properties, i,
				properties->media.disks[i].fileName,
				properties->media.disks[i].fileNameInZip, -1);
		}
		updateExtendedDiskName(i,
			properties->media.disks[i].fileName,
			properties->media.disks[i].fileNameInZip);
	}

	for (i = 0; i < PROP_MAX_TAPES; i++) {
		if (properties->media.tapes[i].fileName[0]) {
			insertCassette(properties, i,
				properties->media.tapes[i].fileName,
				properties->media.tapes[i].fileNameInZip, 0);
		}
		updateExtendedCasName(i,
			properties->media.tapes[i].fileName,
			properties->media.tapes[i].fileNameInZip);
	}

	{
		Machine* machine = machineCreate(properties->emulation.machineName);
		if (machine != NULL) {
			boardSetMachine(machine);
			machineDestroy(machine);
		} else {
			fprintf(stderr, "Error creating machine\n");
			piDestroyVideo();
			piDestroyUdev();
			return 1;
		}
	}

	boardSetFdcTimingEnable(properties->emulation.enableFdcTiming);
	boardSetY8950Enable(properties->sound.chip.enableY8950);
	boardSetYm2413Enable(properties->sound.chip.enableYM2413);
	boardSetMoonsoundEnable(properties->sound.chip.enableMoonsound);
fprintf(stderr, "[MOONSOUND] enable=%d board=%d\n",
        properties->sound.chip.enableMoonsound,
        boardGetMoonsoundEnable());
	boardSetVideoAutodetect(properties->video.detectActiveMonitor);

fprintf(stderr, "[PI CMDLINE] argc=%d szLine=<%s>\n", argc, szLine);
	i = emuTryStartWithArguments(properties, szLine, NULL);
	if (i < 0) {
		fprintf(stderr, "Failed to parse command line\n");
		videoDestroy(video);
		propDestroy(properties);
		archSoundDestroy();
		mixerDestroy(mixer);

		piDestroyVideo();
		piDestroyUdev();
		SDL_Quit();

		return 1;
	}

	if (i == 0) {
		emulatorStart(NULL);
	}

	piScanDevices();
	
	fprintf(stderr, "Powering on\n");

	while (!doQuit) {
	    if (SDL_WaitEventTimeout(&event, 100)) {
	        do {
	            handleEvent(&event);
	        } while (SDL_PollEvent(&event));
	    }

	    piMouseEvdevPoll();

            if (joysticks[0] && SDL_JoystickGetAttached(joysticks[0])) {
	        SDL_JoystickUpdate();

	        static int prevX = 99999;
	        static int prevY = 99999;

	        int x = SDL_JoystickGetAxis(joysticks[0], 0);
	        int y = SDL_JoystickGetAxis(joysticks[0], 1);

	        if (x != prevX || y != prevY) {
	            prevX = x;
	            prevY = y;
	        }
	    }
	}

	videoDestroy(video);
	propDestroy(properties);
	archSoundDestroy();
	mixerDestroy(mixer);
	gpioShutdown();

#ifdef RASPI_GPIO
	//gpioTogglePowerLed(0);
#endif

	piDestroyVideo();
	piDestroyUdev();
	piMouseEvdevDestroy();
        piKeyboardEvdevDestroy();
	fprintf(stderr,"PiMain is calling SDL_Quit()");
	SDL_Quit();
#ifdef RPMC_FRONTLED
    frontled(0);
#endif
	fprintf(stderr, "Powered off\n");
//	SDL_Init(0);
//	exit(0);
	return 0;
}
