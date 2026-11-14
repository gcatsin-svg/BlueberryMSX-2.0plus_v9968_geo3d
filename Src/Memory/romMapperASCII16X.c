/****************************************************************************
 * ASCII16-X mapper with AMD flash programming support.
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
    UInt32 romMask;
    int romMapper[4];
} RomMapperASCII16X;

static void mapBank(RomMapperASCII16X* rm, int page)
{
    UInt32 offset = (UInt32)rm->romMapper[page] << 14;
    UInt8* data = amdFlashGetPage(rm->flash, offset);
    slotMapPage(rm->slot, rm->sslot, rm->startPage + page, data, 1, 0);
    slotMapPage(rm->slot, rm->sslot, rm->startPage + page + 1,
                data + 0x2000, 1, 0);
}

static void destroy(RomMapperASCII16X* rm)
{
    slotUnregister(rm->slot, rm->sslot, rm->startPage);
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

    address += rm->startPage << 13;
    if (address < 0x4000 || address >= 0xc000) return;

    /* Translate the CPU window into the selected physical 16 KiB bank. */
    page = address < 0x8000 ? 0 : 2;
    physical = ((UInt32)rm->romMapper[page] << 14) | (address & 0x3fff);

    /*
     * AmdFlash AMD_TYPE_2 recognizes 555/2AA using an 11-bit mask.
     * ASCII16-X software uses AAA/555 (12-bit command addresses).
     * Remap only the unlock command addresses, never the program target.
     */
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

    /* Original ASCII16-X bank switching. */
    if (address < 0x6000 || address >= 0xc000 ||
        (address >= 0x8000 && address < 0xa000)) return;

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
    while (mappedSize < size) mappedSize *= 2;

    rm = (RomMapperASCII16X*)calloc(1, sizeof(*rm));
    if (rm == NULL) return 0;
    rm->slot = slot;
    rm->sslot = sslot;
    rm->startPage = startPage;
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
    slotRegister(slot, sslot, startPage, 4, NULL, NULL, write, destroy, rm);
    mapBank(rm, 0);
    mapBank(rm, 2);
    return 1;
}
