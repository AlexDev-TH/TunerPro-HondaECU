#pragma once

#include <windows.h>

#include <string>

enum PanelCommand {
    kPanelOpen,
    kPanelFlashRead,
    kPanelFlashWrite,
    kPanelEeprom,
    kPanelData,
    kPanelTroubleCodes,
    kPanelCommandCount
};

// Opens or raises the Honda ECU window, owned by TunerPro's main window. UI thread only.
void PanelShow(HINSTANCE instance, HWND owner, PanelCommand command);
// True while a flash or EEPROM write is running, when closing TunerPro would damage the ECU.
bool PanelWriteInProgress();
// Stops the worker and closes the adapter. UI thread only.
void PanelShutdown();

// Provided by plugin.cpp.
std::wstring TunerProCurrentBinPath();
bool TunerProOpenBin(const std::wstring& path);
// Thai when TunerPro itself runs in Thai, otherwise English.
bool TunerProIsThai();
