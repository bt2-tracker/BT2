// for finding memory leaks in debug mode with Visual Studio
#if defined _DEBUG && defined _MSC_VER
#include <crtdbg.h>
#endif

#ifdef _WIN32
#define WIN32_MEAN_AND_LEAN
#include <windows.h>
#include <SDL2/SDL_syswm.h>
#else
#include <limits.h>
#include <signal.h>
#include <unistd.h> // chdir()
#endif
#include <stdio.h>
#include <sys/stat.h>
#include "ft2_header.h"
#include "ft2_config.h"
#include "ft2_diskop.h"
#include "ft2_module_loader.h"
#include "ft2_module_saver.h"
#if defined _WIN32 || defined __EMSCRIPTEN__
#include "bt2_rescue.h" // BerntTracker: the rescue copy (also when the program crashes)
#endif
#include "ft2_sample_loader.h"
#include "ft2_mouse.h"
#include "ft2_midi.h"
#include "ft2_video.h"
#include "ft2_trim.h"
#include "ft2_inst_ed.h"
#include "ft2_sampling.h"
#include "ft2_textboxes.h"
#include "ft2_sysreqs.h"
#include "ft2_keyboard.h"
#include "ft2_sample_ed.h"
#include "ft2_sample_ed_features.h"
#include "ft2_structs.h"
#if defined _WIN32 && !defined __EMSCRIPTEN__
#include "bt2_windiskop.h" // BerntTracker (v0.35): files dragged from Windows
#endif
#include "ft2_touchkeys.h" // BerntTracker: on-screen keyboard
#include "ft2_audio.h"
#include "ft2_wasapi.h" // BerntTracker (Windows): exclusive mode
#include "ft2_asio.h" // BerntTracker (Windows): ASIO
#include "bt2_diskop.h" // BerntTracker: Disk Op. like on the iPad (Modland)

#define CRASH_TEXT "Oh no! The Fasttracker II clone has crashed...\nA backup of the song was hopefully " \
                   "saved to the current module directory.\n\nPlease report this bug if you can.\n" \
                   "Try to mention what you did before the crash happened.\n" \
                   "My email is on the bottom of https://16-bits.org"

static bool backupMadeAfterCrash;

#ifdef _WIN32
#define SYSMSG_FILE_ARG (WM_USER+1)
#define ARGV_SHARED_MEM_MAX_LEN ((PATH_MAX+1) * sizeof (WCHAR))
#define SHARED_HWND_NAME TEXT("Local\\FT2CloneHwnd")
#define SHARED_FILENAME TEXT("Local\\FT2CloneFilename")
static HWND hWnd;
static HANDLE oneInstHandle, hMapFile;
static LPCTSTR sharedMemBuf;
#endif

static void handleSDLEvents(void);

void readInput(void)
{
	readMouseXY();
	readKeyModifiers();
	setSyncedReplayerVars();
	handleSDLEvents();
}

void handleThreadEvents(void)
{
	if (okBoxData.active)
	{
		okBoxData.returnData = okBox(okBoxData.type, okBoxData.headline, okBoxData.text, okBoxData.checkBoxCallback);
		okBoxData.active = false;
	}
}

void handleEvents(void)
{
#ifdef __EMSCRIPTEN__
	webHandlePageRequests(); // BerntTracker (web): requests from the page (e.g. Ctrl+F: the Find screen)
#endif

#ifdef _WIN32
	// BerntTracker: the sound card went away in exclusive mode (unplugged, changed), or the ASIO driver
	// asks to be started again (new settings in its panel): open the sound again
	if (winExclusiveDeviceLost() || winAsioResetRequested())
		setNewAudioSettings();
#endif

#ifdef HAS_MIDI
	// called after MIDI has been initialized
	if (midi.rescanDevicesFlag)
	{
		midi.rescanDevicesFlag = false;

		rescanMidiInputDevices();
		if (ui.configScreenShown && editor.currConfigScreen == CONFIG_SCREEN_MIDI_INPUT)
			drawMidiInputList();
	}
#endif

	if (editor.trimThreadWasDone)
	{
		editor.trimThreadWasDone = false;
		trimThreadDone();
	}

	if (editor.updateCurSmp)
	{
		editor.updateCurSmp = false;

		updateNewInstrument();
		updateNewSample();

		diskOpSetFilename(DISKOP_ITEM_SAMPLE, editor.tmpFilenameU);

		removeSampleIsLoadingFlag();
		setMouseBusy(false);
	}

	if (editor.updateCurInstr)
	{
		editor.updateCurInstr = false;

		updateNewInstrument();
		updateNewSample();

		diskOpSetFilename(DISKOP_ITEM_INSTR, editor.tmpInstrFilenameU);
		setMouseBusy(false);
	}

	// some Disk Op. stuff

	if (editor.diskOpReadDir)
	{
		editor.diskOpReadDir = false;
#ifdef BT2_DISKOP
		bt2DiskOpConfineDir(); // BerntTracker: "../" from the top of the BT2 folder or Modland: the BT2 folder
#endif
		diskOp_StartDirReadThread();
	}

	if (editor.diskOpReadDone)
	{
		editor.diskOpReadDone = false;
		if (ui.diskOpShown)
			diskOp_DrawDirectory();
	}

	handleLoadMusicEvents();

	if (editor.samplingAudioFlag) handleSamplingUpdates();
	if (ui.setMouseBusy) mouseAnimOn();
	if (ui.setMouseIdle) mouseAnimOff();

	if (editor.updateWindowTitle)
	{
		editor.updateWindowTitle = false;
		updateWindowTitle(false);
	}
}

// Windows specific routines
#ifdef _WIN32
static bool instanceAlreadyOpen(void)
{
	hMapFile = OpenFileMapping(FILE_MAP_ALL_ACCESS, FALSE, SHARED_HWND_NAME);
	if (hMapFile != NULL)
		return true; // another instance is already open

	// no instance is open, let's created a shared memory file with hWnd in it
	oneInstHandle = CreateFileMapping(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof (HWND), SHARED_HWND_NAME);
	if (oneInstHandle != NULL)
	{
		sharedMemBuf = (LPTSTR)MapViewOfFile(oneInstHandle, FILE_MAP_ALL_ACCESS, 0, 0, sizeof (HWND));
		if (sharedMemBuf != NULL)
		{
			CopyMemory((PVOID)sharedMemBuf, &video.hWnd, sizeof (HWND));
			UnmapViewOfFile(sharedMemBuf);
			sharedMemBuf = NULL;
		}
	}

	return false;
}

bool handleSingleInstancing(int32_t argc, char **argv)
{
	SDL_SysWMinfo wmInfo;

	SDL_VERSION(&wmInfo.version);
	if (!SDL_GetWindowWMInfo(video.window, &wmInfo))
		return false;

	video.hWnd = wmInfo.info.win.window;
	if (instanceAlreadyOpen() && argc >= 2 && argv[1][0] != '\0')
	{
		sharedMemBuf = (LPTSTR)MapViewOfFile(hMapFile, FILE_MAP_ALL_ACCESS, 0, 0, sizeof (HWND));
		if (sharedMemBuf != NULL)
		{
			memcpy(&hWnd, sharedMemBuf, sizeof (HWND));

			UnmapViewOfFile(sharedMemBuf);
			sharedMemBuf = NULL;
			CloseHandle(hMapFile);
			hMapFile = NULL;

			hMapFile = CreateFileMapping(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, ARGV_SHARED_MEM_MAX_LEN, SHARED_FILENAME);
			if (hMapFile != NULL)
			{
				sharedMemBuf = (LPTSTR)MapViewOfFile(hMapFile, FILE_MAP_ALL_ACCESS, 0, 0, ARGV_SHARED_MEM_MAX_LEN);
				if (sharedMemBuf != NULL)
				{
					strcpy((char *)sharedMemBuf, argv[1]);

					UnmapViewOfFile(sharedMemBuf);
					sharedMemBuf = NULL;

					SendMessage(hWnd, SYSMSG_FILE_ARG, 0, 0);
					Sleep(80); // wait a bit to make sure first instance received msg

					CloseHandle(hMapFile);
					hMapFile = NULL;

					return true; // quit instance now
				}
			}

			return true;
		}

		CloseHandle(hMapFile);
		hMapFile = NULL;
	}

	SDL_EventState(SDL_SYSWMEVENT, SDL_ENABLE);
	return false;
}

static void handleSysMsg(SDL_Event inputEvent)
{
	SDL_SysWMmsg *wmMsg = inputEvent.syswm.msg;
	if (wmMsg->subsystem == SDL_SYSWM_WINDOWS && wmMsg->msg.win.msg == SYSMSG_FILE_ARG)
	{
		hMapFile = OpenFileMapping(FILE_MAP_READ, FALSE, SHARED_FILENAME);
		if (hMapFile != NULL)
		{
			sharedMemBuf = (LPTSTR)MapViewOfFile(hMapFile, FILE_MAP_READ, 0, 0, ARGV_SHARED_MEM_MAX_LEN);
			if (sharedMemBuf != NULL)
			{
				editor.autoPlayOnDrop = true;

				if (video.window != NULL && !video.fullscreen)
				{
					if (SDL_GetWindowFlags(video.window) & SDL_WINDOW_MINIMIZED)
						SDL_RestoreWindow(video.window);

					SDL_RaiseWindow(video.window);
				}

				loadDroppedFile((char *)sharedMemBuf);

				UnmapViewOfFile(sharedMemBuf);
				sharedMemBuf = NULL;
			}

			CloseHandle(hMapFile);
			hMapFile = NULL;
		}
	}
}

void closeSingleInstancing(void)
{
	if (oneInstHandle != NULL)
	{
		CloseHandle(oneInstHandle);
		oneInstHandle = NULL;
	}
}

static LONG WINAPI exceptionHandler(EXCEPTION_POINTERS *ptr)
{
#define BACKUP_FILES_TO_TRY 1000
	char fileName[32];
	struct stat statBuffer;

	if (oneInstHandle != NULL)
		CloseHandle(oneInstHandle);

#ifndef __EMSCRIPTEN__
	/* BerntTracker (v0.35): like the web version - a message in Norwegian, the song goes to the rescue copy
	** (the next start asks to get it back, see bt2_rescue.c), and the program can be started again.
	** Only if the rescue copy can't be written: FT2's own backup (backupNNN.xm in the songs folder).
	*/
	if (!backupMadeAfterCrash)
	{
		backupMadeAfterCrash = true; // (only once, even if the crash happens again in here)

		bool rescued = bt2RescueOnCrash();
		if (!rescued && getDiskOpModPath() != NULL && UNICHAR_CHDIR(getDiskOpModPath()) == 0)
		{
			int32_t i;
			for (i = 1; i < BACKUP_FILES_TO_TRY; i++)
			{
				sprintf(fileName, "backup%03d.xm", (int32_t)i);
				if (stat(fileName, &statBuffer) != 0)
					break;
			}

			if (i != BACKUP_FILES_TO_TRY)
			{
				UNICHAR *fileNameU = cp850ToUnichar(fileName);
				if (fileNameU != NULL)
				{
					saveXM(fileNameU);
					free(fileNameU);
				}
			}
		}

		const wchar_t *text = rescued ?
			L"Bernttracker II fikk en feil og m\u00E5 lukkes.\n\n"
			L"Det du har lagret (Save) er trygt. Hadde l\u00E5ta endringer som ikke var lagret, sp\u00F8r programmet "
			L"neste gang om du vil hente den tilbake (redningskopi).\n\n"
			L"Vil du starte Bernttracker II p\u00E5 nytt n\u00E5?" :
			L"Bernttracker II fikk en feil og m\u00E5 lukkes.\n\n"
			L"Det du har lagret (Save) er trygt. En kopi av l\u00E5ta er fors\u00F8kt lagret som \u00ABbackup001.xm\u00BB "
			L"(eller neste ledige nummer) i mappa med l\u00E5tene dine.\n\n"
			L"Vil du starte Bernttracker II p\u00E5 nytt n\u00E5?";

		if (MessageBoxW(NULL, text, L"Noe gikk galt", MB_YESNO | MB_ICONERROR | MB_TOPMOST) == IDYES)
		{
			wchar_t exe[MAX_PATH];
			if (GetModuleFileNameW(NULL, exe, MAX_PATH) > 0)
			{
				STARTUPINFOW si;
				PROCESS_INFORMATION pi;
				memset(&si, 0, sizeof (si));
				si.cb = sizeof (si);
				memset(&pi, 0, sizeof (pi));
				if (CreateProcessW(exe, NULL, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
				{
					CloseHandle(pi.hThread);
					CloseHandle(pi.hProcess);
				}
			}
		}
	}

	return EXCEPTION_EXECUTE_HANDLER; // (the program ends here, without Windows' own "stopped working" box on top)
#endif

	if (!backupMadeAfterCrash)
	{
		if (getDiskOpModPath() != NULL && UNICHAR_CHDIR(getDiskOpModPath()) == 0)
		{
			// find a free filename
			int32_t i;
			for (i = 1; i < BACKUP_FILES_TO_TRY; i++)
			{
				sprintf(fileName, "backup%03d.xm", (int32_t)i);
				if (stat(fileName, &statBuffer) != 0)
					break; // filename OK
			}

			if (i != BACKUP_FILES_TO_TRY)
			{
				UNICHAR *fileNameU = cp850ToUnichar(fileName);
				if (fileNameU != NULL)
				{
					saveXM(fileNameU);
					free(fileNameU);
				}
			}
		}

		backupMadeAfterCrash = true; // set this flag to prevent multiple backups from being saved at once
		showErrorMsgBox(CRASH_TEXT);
	}

	return EXCEPTION_CONTINUE_SEARCH;

	(void)ptr;
}
#else
static void exceptionHandler(int32_t signal)
{
#define BACKUP_FILES_TO_TRY 1000
	char fileName[32];
	struct stat statBuffer;

	if (signal == 15)
		return;

	if (!backupMadeAfterCrash)
	{
		if (getDiskOpModPath() != NULL && UNICHAR_CHDIR(getDiskOpModPath()) == 0)
		{
			// find a free filename
			int32_t i;
			for (i = 1; i < BACKUP_FILES_TO_TRY; i++)
			{
				sprintf(fileName, "backup%03d.xm", i);
				if (stat(fileName, &statBuffer) != 0)
					break; // filename OK
			}

			if (i != BACKUP_FILES_TO_TRY)
			{
				UNICHAR *fileNameU = cp850ToUnichar(fileName);
				if (fileNameU != NULL)
				{
					saveXM(fileNameU);
					free(fileNameU);
				}
			}
		}

		backupMadeAfterCrash = true; // set this flag to prevent multiple backups from being saved at once
		showErrorMsgBox(CRASH_TEXT);
	}
}
#endif

void setupCrashHandler(void)
{
#ifndef _DEBUG
#ifdef _WIN32
	SetUnhandledExceptionFilter(exceptionHandler);
#else
	struct sigaction act;
	struct sigaction oldAct;

	memset(&act, 0, sizeof (act));
	act.sa_handler = exceptionHandler;
	act.sa_flags = SA_RESETHAND;

	sigaction(SIGILL | SIGABRT | SIGFPE | SIGSEGV, &act, &oldAct);
	sigaction(SIGILL, &act, &oldAct);
	sigaction(SIGABRT, &act, &oldAct);
	sigaction(SIGFPE, &act, &oldAct);
	sigaction(SIGSEGV, &act, &oldAct);
#endif
#endif
}

void handleWaitVblQuirk(SDL_Event *event)
{
	if (event->type == SDL_WINDOWEVENT)
	{
		if (event->window.event == SDL_WINDOWEVENT_HIDDEN)
			video.windowHidden = true;
		else if (event->window.event == SDL_WINDOWEVENT_SHOWN)
			video.windowHidden = false;

		// reset vblank end time if we minimize window
		if (event->window.event == SDL_WINDOWEVENT_MINIMIZED || event->window.event == SDL_WINDOWEVENT_FOCUS_LOST)
			hpc_ResetCounters(&video.vblankHpc);
	}
}

static void handleSDLEvents(void)
{
	SDL_Event event;

	if (!editor.busy)
		handleLastGUIObjectDown(); // this should be handled before main input poll (on next frame)

	while (SDL_PollEvent(&event))
	{
		handleWaitVblQuirk(&event);

		if (editor.busy)
		{
			const uint32_t eventType = event.type;
			const SDL_Scancode key = event.key.keysym.scancode;

			/* The Echo tool in Smp. Ed. can take forever if abused, let
			** mouse buttons/ESC/SIGTERM force-stop it.
			*/
			if (eventType == SDL_MOUSEBUTTONDOWN || eventType == SDL_QUIT ||
				(eventType == SDL_KEYUP && key == SDL_SCANCODE_ESCAPE))
			{
				handleEchoToolPanic();
			}

			// let certain mouse buttons or keyboard keys stop certain events
			if (eventType == SDL_MOUSEBUTTONDOWN ||
				(eventType == SDL_KEYDOWN && key != SDL_SCANCODE_MUTE &&
				 key != SDL_SCANCODE_AUDIOMUTE && key != SDL_SCANCODE_VOLUMEDOWN &&
				 key != SDL_SCANCODE_VOLUMEUP))
			{
				if (editor.samplingAudioFlag)
					stopSampling();

				editor.stopWavRender = true;
			}

			// BerntTracker: releases still go to the on-screen keyboard (a note could keep sounding, and
			// the two-finger tap would stop working), and dropped files are freed
			if (eventType == SDL_FINGERUP || eventType == SDL_WINDOWEVENT)
				tkHandleEvent(&event);
			else if (eventType == SDL_DROPFILE && event.drop.file != NULL)
				SDL_free(event.drop.file);

			continue; // another thread is busy with something, drop input
		}

#ifdef _WIN32
		if (event.type == SDL_SYSWMEVENT)
			handleSysMsg(event);
#endif
		// BerntTracker: clicks/touches on the on-screen keyboard
		if (tkHandleEvent(&event))
			continue;

		// text input when editing texts
		if (event.type == SDL_TEXTINPUT)
		{
			if (editor.editTextFlag)
			{
				if (keyb.ignoreTextEditKey)
				{
					keyb.ignoreTextEditKey = false;
					continue;
				}

				char *inputText = utf8ToCp850(event.text.text, false);
				if (inputText != NULL)
				{
					if (inputText[0] != '\0')
						handleTextEditInputChar(inputText[0]);

					free(inputText);
				}
			}
		}
		else if (event.type == SDL_MOUSEWHEEL)
		{
			if (event.wheel.y > 0)
				mouseWheelHandler(MOUSE_WHEEL_UP);
			else if (event.wheel.y < 0)
				mouseWheelHandler(MOUSE_WHEEL_DOWN);
		}
#if defined _WIN32 && !defined __EMSCRIPTEN__
		else if (event.type == SDL_DROPBEGIN) // BerntTracker (v0.35): files dragged from Windows (bt2_windiskop.c)
		{
			bt2DropBegin();
		}
		else if (event.type == SDL_DROPCOMPLETE)
		{
			if (!video.fullscreen)
			{
				if (SDL_GetWindowFlags(video.window) & SDL_WINDOW_MINIMIZED)
					SDL_RestoreWindow(video.window);

				SDL_RaiseWindow(video.window);
			}

			editor.autoPlayOnDrop = false;
			bt2DropComplete();
		}
		else if (event.type == SDL_DROPFILE && bt2DropCollect(event.drop.file))
		{
			SDL_free(event.drop.file);
		}
#endif
		else if (event.type == SDL_DROPFILE)
		{
			editor.autoPlayOnDrop = false;

			if (!video.fullscreen)
			{
				if (SDL_GetWindowFlags(video.window) & SDL_WINDOW_MINIMIZED)
					SDL_RestoreWindow(video.window);

				SDL_RaiseWindow(video.window);
			}

#ifdef BT2_DISKOP
			if (bt2PrepareDroppedFile(event.drop.file)) // BerntTracker: Modland songs (Find, Disk Op.) are downloaded first
#endif
			loadDroppedFile(event.drop.file);
			SDL_free(event.drop.file);
		}
		else if (event.type == SDL_QUIT)
		{
			if (ui.sysReqShown)
				continue;

			if (editor.editTextFlag)
				exitTextEditing();

			if (!song.isModified)
			{
				editor.throwExit = true;
			}
			else
			{
				if (!video.fullscreen)
				{
					// de-minimize window and set focus so that the user sees the message box
					if (SDL_GetWindowFlags(video.window) & SDL_WINDOW_MINIMIZED)
						SDL_RestoreWindow(video.window);

					SDL_RaiseWindow(video.window);
				}

				if (quitBox(true) == 1)
					editor.throwExit = true;
			}
		}
		else if (event.type == SDL_KEYUP)
		{
#ifdef __EMSCRIPTEN__
			event.key.keysym.sym = webFixKeycode(event.key.keysym.scancode, event.key.keysym.sym);
			setKeyModifiers((SDL_Keymod)event.key.keysym.mod); // modifiers as they were when this key was pressed
#endif
			keyUpHandler(event.key.keysym.scancode, event.key.keysym.sym);
		}
		else if (event.type == SDL_KEYDOWN)
		{
#ifdef __EMSCRIPTEN__
			event.key.keysym.sym = webFixKeycode(event.key.keysym.scancode, event.key.keysym.sym);
			setKeyModifiers((SDL_Keymod)event.key.keysym.mod); // modifiers as they were when this key was pressed
#endif
			keyDownHandler(event.key.keysym.scancode, event.key.keysym.sym, event.key.repeat);
		}
		else if (event.type == SDL_MOUSEBUTTONUP)
		{
			if (event.button.which == SDL_TOUCH_MOUSEID)
				readMouseXY(); // BerntTracker: touch "clicks" jump to a new position, update it first

			mouseButtonUpHandler(event.button.button);
#if defined __APPLE__ && defined __aarch64__
			armMacGhostMouseCursorFix();
#endif
		}
		else if (event.type == SDL_MOUSEBUTTONDOWN)
		{
			if (event.button.which == SDL_TOUCH_MOUSEID)
				readMouseXY(); // BerntTracker: touch "clicks" jump to a new position, update it first

			mouseButtonDownHandler(event.button.button);
		}
#ifdef __EMSCRIPTEN__
		else if (event.type == SDL_WINDOWEVENT && (event.window.event == SDL_WINDOWEVENT_RESIZED ||
		         event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED))
		{
			videoHandleResize();
		}
#endif
#if defined __APPLE__ && defined __aarch64__
		else if (event.type == SDL_MOUSEMOTION)
		{
			armMacGhostMouseCursorFix();
		}
#endif

		if (editor.throwExit)
			editor.programRunning = false;
	}

#ifdef HAS_MIDI
	// MIDI vibrato
	const uint8_t vibDepth = (midi.currMIDIVibDepth >> 9) & 0x0F;
	if (vibDepth > 0)
		recordMIDIEffect(0x04, 0xA0 | vibDepth);
#endif
}
