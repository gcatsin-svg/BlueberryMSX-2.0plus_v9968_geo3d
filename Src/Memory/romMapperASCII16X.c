/****************************************************************************
 * ASCII16-X mapper with AMD flash programming and 64 KiB slot mirrors.
 * Experimental: flash identification reads and persistence are not implemented.
 ****************************************************************************/
#include "romMapperASCII16X.h"
#include "MediaDb.h"
#include "SlotManager.h"
#include "DeviceManager.h"
#include "SaveState.h"
#include "AmdFlash.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef struct {
    int deviceHandle;
    AmdFlash* flash;
    int slot, sslot, startPage;
    int registeredStartPage, registeredPages;
    UInt32 romMask;
    int romMapper[4];
} RomMapperASCII16X;

static void mapBank(RomMapperASCII16X* rm, int page)
{
    UInt32 offset = (UInt32)rm->romMapper[page] << 14;
    UInt8* data = amdFlashGetPage(rm->flash, offset);
    int base = rm->startPage + page;

    slotMapPage(rm->slot, rm->sslot, base, data, 1, 0);
    slotMapPage(rm->slot, rm->sslot, base + 1, data + 0x2000, 1, 0);

    if (rm->registeredPages == 8) {
        int mirror = page == 0 ? 6 : 0;
        slotMapPage(rm->slot, rm->sslot, mirror, data, 1, 0);
        slotMapPage(rm->slot, rm->sslot, mirror + 1, data + 0x2000, 1, 0);
    }
}

static void destroy(RomMapperASCII16X* rm)
{
    slotUnregister(rm->slot, rm->sslot, rm->registeredStartPage);
    deviceManagerUnregister(rm->deviceHandle);
    amdFlashDestroy(rm->flash);
    free(rm);
}

static void saveState(RomMapperASCII16X* rm)
{
    SaveState* state = saveStateOpenForWrite("mapperASCII16X");
    char tag[16];
    int i;
    for (i = 0; i < 4; i++) {
        sprintf(tag, "romMapper%d", i);
        saveStateSet(state, tag, rm->romMapper[i]);
    }
    saveStateClose(state);
    amdFlashSaveState(rm->flash);
}

static void loadState(RomMapperASCII16X* rm)
{
    SaveState* state = saveStateOpenForRead("mapperASCII16X");
    char tag[16];
    int i;
    for (i = 0; i < 4; i++) {
        sprintf(tag, "romMapper%d", i);
        rm->romMapper[i] = saveStateGet(state, tag, 0) & rm->romMask;
    }
    saveStateClose(state);
    amdFlashLoadState(rm->flash);
    mapBank(rm, 0);
    mapBank(rm, 2);
}

static void write(RomMapperASCII16X* rm, UInt16 address, UInt8 value)
{
    UInt32 physical;
    UInt32 commandAddress;
    int page, bankNumber;
    int wasInProgress;

    /* The slot callback receives an offset from registeredStartPage. */
    address += rm->registeredStartPage << 13;

    if (rm->registeredPages == 8) {
        /* 0000-3FFF mirrors bank 2; C000-FFFF mirrors bank 1. */
        page = (address < 0x4000 || (address >= 0x8000 && address < 0xc000)) ? 2 : 0;
    }
    else {
        if (address < 0x4000 || address >= 0xc000) return;
        page = address < 0x8000 ? 0 : 2;
    }

    physical = ((UInt32)rm->romMapper[page] << 14) | (address & 0x3fff);

    /* Translate ASCII16-X unlock addresses for the existing AMD_TYPE_2 engine.
     * Preserve the physical address for the actual programmed byte. */
    wasInProgress = amdFlashCmdInProgress(rm->flash);
    commandAddress = physical;
    if (!wasInProgress && (address & 0x0fff) == 0x0aaa && value == 0xaa) {
        commandAddress = (physical & ~0x7ffU) | 0x555;
    }
    else if (wasInProgress && (address & 0x0fff) == 0x0555 && value == 0x55) {
        commandAddress = (physical & ~0x7ffU) | 0x2aa;
    }
    else if (wasInProgress && (address & 0x0fff) == 0x0aaa &&
             (value == 0xa0 || value == 0x80 || value == 0x90 || value == 0xaa)) {
        commandAddress = (physical & ~0x7ffU) | 0x555;
    }
    amdFlashWrite(rm->flash, commandAddress, value);

    /* Bank registers repeat every 16 KiB at offsets 2000-3FFF. */
    if (!(address & 0x2000)) return;
    page = (address & 0x1000) ? 2 : 0;
    bankNumber = ((address & 0x0f00) | value) & rm->romMask;
    if (rm->romMapper[page] == bankNumber) return;
    rm->romMapper[page] = bankNumber;
    mapBank(rm, page);
}

int romMapperASCII16XCreate(const char* filename, UInt8* romData,
                           int size, int slot, int sslot, int startPage)
{
    DeviceCallbacks callbacks = { destroy, NULL, saveState, loadState };
    RomMapperASCII16X* rm;
    int mappedSize = 0x8000;
    (void)filename;

    if (size <= 0 || startPage < 0 || startPage > 4) return 0;
    while (mappedSize < size) mappedSize *= 2;

    rm = (RomMapperASCII16X*)calloc(1, sizeof(*rm));
    if (rm == NULL) return 0;
    rm->slot = slot;
    rm->sslot = sslot;
    rm->startPage = startPage;
    rm->registeredStartPage = startPage == 2 ? 0 : startPage;
    rm->registeredPages = startPage == 2 ? 8 : 4;
    rm->romMask = mappedSize / 0x4000 - 1;
    rm->flash = amdFlashCreate(AMD_TYPE_2, mappedSize, 0x10000,
                               0, romData, size, NULL, 0);
    if (rm->flash == NULL) {
        free(rm);
        return 0;
    }
    rm->romMapper[0] = 0;
    rm->romMapper[2] = 0;
    rm->deviceHandle = deviceManagerRegister(ROM_ASCII16X, &callbacks, rm);
    slotRegister(slot, sslot, rm->registeredStartPage, rm->registeredPages,
                 NULL, NULL, write, destroy, rm);
    mapBank(rm, 0);
    mapBank(rm, 2);
    return 1;
}

