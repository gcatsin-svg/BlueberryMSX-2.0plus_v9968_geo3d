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

#include "PiMouse.h"
#include <SDL.h>
#include "MsxTypes.h"
#include "ArchInput.h"
#include <linux/input.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <pthread.h>


typedef struct SdlMouse {
    SDL_Cursor* cursorArrow;
    SDL_Cursor* cursorCross;
    AmEnableMode mode;
    int hasFocus;
    struct {
        int top;
        int left;
        int bottom;
        int right;
    } captRgn;
    int buttonState;
    int defX;
    int defY;
    int lockDx;
    int lockDy;
    int dx;
    int dy;
} SdlMouse;

SdlMouse mouse;
static pthread_mutex_t mouseMutex = PTHREAD_MUTEX_INITIALIZER;

static char* arrow[] = {
    "X                               ",
    "XX                              ",
    "X.X                             ",
    "X..X                            ",
    "X...X                           ",
    "X....X                          ",
    "X.....X                         ",
    "X......X                        ",
    "X.......X                       ",
    "X........X                      ",
    "X.........X                     ",
    "X..... XXXXX                    ",
    "X..X...X                        ",
    "X.X X...X                       ",
    "XX  X...X                       ",
    "X    X...X                      ",
    "     X...X                      ",
    "      X...X                     ",
    "      X...X                     ",
    "       XXX                      ",
    "                                ",
    "                                ",
    "                                ",
    "                                ",
    "                                ",
    "                                ",
    "                                ",
    "                                ",
    "                                ",
    "                                ",
    "                                ",
    "                                "
};

static char* cross[] = {
    "             XXXXX              ",
    "            X.....X             ",
    "            X.....X             ",
    "            X.....X             ",
    "            X.....X             ",
    "             X...X              ",
    "             X...X              ",
    "             X...X              ",
    "              X.X               ",
    "              X.X               ",
    "              X.X               ",
    "               X                ",
    " XXXX          X          XXXX  ",
    "X....XXX               XXX....X ",
    "X.......XXX    X    XXX.......X ",
    "X..........XX X X XX..........X ",
    "X.......XXX    X    XXX.......X ",
    "X....XXX               XXX....X ",
    " XXXX          X          XXXX  ",
    "               X                ",
    "              X.X               ",
    "              X.X               ",
    "              X.X               ",
    "             X...X              ",
    "             X...X              ",
    "             X...X              ",
    "            X.....X             ",
    "            X.....X             ",
    "            X.....X             ",
    "            X.....X             ",
    "             XXXXX              ",
    "                                "
};

static SDL_Cursor* sdlCreateCursor(const char* image[], int hx, int hy)
{
    Uint8 data[4 * 32];
    Uint8 mask[4 * 32];
    int x;
    int y;
    int i = 0;

    for (y = 0; y < 32; y++) {
        for (x = 0; x < 32; x++) {
            mask[i] = (mask[i] << 1) | (image[y][x] != ' ' ? 1 : 0);
            data[i] = (data[i] << 1) | (image[y][x] == 'X' ? 1 : 0);
            if (x % 8 == 7) {
                i++;
            }
        }
    }
    
    return SDL_CreateCursor(data, mask, 32, 32, hx, hy);
}

static int piMouseInRect(int x, int y)
{
    return x > mouse.captRgn.left && x < mouse.captRgn.right &&
           y > mouse.captRgn.top  && y < mouse.captRgn.bottom;
}

void piMouseSetCaptureRect(int x, int y, int width, int height)
{
    if (mouse.cursorArrow == NULL) {
        mouse.cursorArrow = sdlCreateCursor((const char**)arrow, 0, 0);
    }

    if (mouse.cursorCross == NULL) {
        mouse.cursorCross = sdlCreateCursor((const char**)cross, 15, 15);
    }

    mouse.captRgn.top    = y;
    mouse.captRgn.left   = x;
    mouse.captRgn.bottom = y + height - 1;
    mouse.captRgn.right  = x + width - 1;
    mouse.defX           = x + 100;
    mouse.defY           = y + 100;

    SDL_SetCursor(mouse.cursorArrow);
}

void piMouseInit()
{
    SDL_ShowCursor(SDL_DISABLE);

    pthread_mutex_lock(&mouseMutex);
    mouse.hasFocus = 1;
    pthread_mutex_unlock(&mouseMutex);
}

void piMouseButton(int button, int pressed)
{
    pthread_mutex_lock(&mouseMutex);

    /* Normal mouse input is handled by evdev. */
    if (mouse.mode == AM_ENABLE_LASER) {
        int mask = 0;

        if (button == SDL_BUTTON_LEFT) mask = 1;
        if (button == SDL_BUTTON_RIGHT ||
            button == SDL_BUTTON_MIDDLE) mask = 2;

        if (pressed)
            mouse.buttonState |= mask;
        else
            mouse.buttonState &= ~mask;
    }

    pthread_mutex_unlock(&mouseMutex);
}

void piMouseMove(int x, int y)
{
    pthread_mutex_lock(&mouseMutex);

    if (mouse.mode == AM_ENABLE_LASER &&
        mouse.captRgn.right > mouse.captRgn.left &&
        mouse.captRgn.bottom > mouse.captRgn.top) {

        mouse.hasFocus = piMouseInRect(x, y);

        mouse.dx = 0x10000 * (x - mouse.captRgn.left) /
                   (mouse.captRgn.right - mouse.captRgn.left);

        mouse.dy = 0x10000 * (y - mouse.captRgn.top) /
                   (mouse.captRgn.bottom - mouse.captRgn.top);

        if (mouse.hasFocus)
            SDL_SetCursor(mouse.cursorCross);
        else
            SDL_SetCursor(mouse.cursorArrow);
    }

    pthread_mutex_unlock(&mouseMutex);
}

void archMouseGetState(int* dx, int* dy)
{
    pthread_mutex_lock(&mouseMutex);

    *dx = 0;
    *dy = 0;

    if (mouse.hasFocus) {
        if (mouse.mode == AM_ENABLE_LASER) {
            *dx = mouse.dx;
            *dy = mouse.dy;
        }

        if (mouse.mode == AM_ENABLE_MOUSE) {
            *dx = -mouse.dx;
            *dy = -mouse.dy;
            mouse.dx = 0;
            mouse.dy = 0;
        }
    }

    pthread_mutex_unlock(&mouseMutex);
}

int archMouseGetButtonState(int checkAlways)
{
    int state;

    pthread_mutex_lock(&mouseMutex);
    state = mouse.hasFocus ? mouse.buttonState : 0;
    pthread_mutex_unlock(&mouseMutex);

    return state;
}

void archMouseEmuEnable(AmEnableMode mode)
{
    pthread_mutex_lock(&mouseMutex);

    if (mouse.mode != mode) {
        mouse.dx = 0;
        mouse.dy = 0;
        mouse.buttonState = 0;
    }

    mouse.mode = mode;

    if (mode == AM_ENABLE_MOUSE)
        mouse.hasFocus = 1;

    pthread_mutex_unlock(&mouseMutex);
}


/* Linux evdev mouse input */
static int mouseFd = -1;
static SDL_atomic_t mouseScanRequested = {0};

void piMouseEvdevRequestScan(void)
{
    SDL_AtomicSet(&mouseScanRequested, 1);
}

static void piMouseEvdevOpen(void)
{
    DIR *dir;
    struct dirent *ent;

    dir = opendir("/dev/input");
    if (!dir) return;

    while ((ent = readdir(dir)) != NULL) {
        char path[256];
        unsigned char relbits[REL_MAX / 8 + 1] = {0};
        unsigned char keybits[KEY_MAX / 8 + 1] = {0};
        int fd;

        if (strncmp(ent->d_name, "event", 5) != 0)
            continue;

        snprintf(path, sizeof(path),
                 "/dev/input/%s", ent->d_name);

        fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;

        if (ioctl(fd, EVIOCGBIT(EV_REL, sizeof(relbits)), relbits) < 0 ||
            ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits) < 0 ||
            !(relbits[REL_X / 8] & (1 << (REL_X % 8))) ||
            !(relbits[REL_Y / 8] & (1 << (REL_Y % 8))) ||
            !(keybits[BTN_LEFT / 8] & (1 << (BTN_LEFT % 8)))) {
            close(fd);
            continue;
        }

        mouseFd = fd;
        fprintf(stderr, "[MOUSE] opened %s\n", path);
        break;
    }

    closedir(dir);
}

void piMouseEvdevDestroy(void)
{
    if (mouseFd >= 0) {
        close(mouseFd);
        mouseFd = -1;
    }

    pthread_mutex_lock(&mouseMutex);
    mouse.dx = 0;
    mouse.dy = 0;
    mouse.buttonState = 0;
    pthread_mutex_unlock(&mouseMutex);
}

void piMouseEvdevPoll(void)
{
    struct input_event ev;
    ssize_t len;

    if (SDL_AtomicCAS(&mouseScanRequested, 1, 0)) {
        if (mouseFd < 0)
            piMouseEvdevOpen();
    }

    if (mouseFd < 0)
        return;

    while ((len = read(mouseFd, &ev, sizeof(ev))) == sizeof(ev)) {
        pthread_mutex_lock(&mouseMutex);

        if (mouse.mode == AM_ENABLE_MOUSE) {
            mouse.hasFocus = 1;
            if (ev.type == EV_REL) {
                if (ev.code == REL_X) mouse.dx += ev.value;
                if (ev.code == REL_Y) mouse.dy += ev.value;
            }

            if (ev.type == EV_KEY) {
                if (ev.code == BTN_LEFT) {
                    if (ev.value) mouse.buttonState |= 1;
                    else mouse.buttonState &= ~1;
                }

                if (ev.code == BTN_RIGHT) {
                    if (ev.value) mouse.buttonState |= 2;
                    else mouse.buttonState &= ~2;
                }
            }
        }

        pthread_mutex_unlock(&mouseMutex);
    }

    if (len == 0 ||
        (len < 0 && errno != EAGAIN &&
         errno != EWOULDBLOCK && errno != EINTR)) {
        fprintf(stderr, "[MOUSE] disconnected\n");
        piMouseEvdevDestroy();
    }
}
