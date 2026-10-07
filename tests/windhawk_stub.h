// Minimal stand-in for Windhawk's API so the mod can be compiled and tested
// outside Windhawk (e.g. with MinGW). Not used by Windhawk itself.
#pragma once
#include <windows.h>
#include <cstdio>
#include <cwchar>

#define Wh_Log(fmt, ...) fwprintf(stderr, fmt L"\n", ##__VA_ARGS__)

inline int Wh_GetIntSetting(PCWSTR name) {
    if (wcscmp(name, L"taskbarIconSize") == 0) return 24;
    if (wcscmp(name, L"debugLogging") == 0) return 1;
    return 1;
}
inline PCWSTR Wh_GetStringSetting(PCWSTR name) {
    if (wcscmp(name, L"filter") == 0) return L"lanczos3";
    return L"auto";
}
inline void Wh_FreeStringSetting(PCWSTR) {}
inline BOOL Wh_SetFunctionHook(void*, void*, void**) { return TRUE; }
inline int g_stubIntValue = 0;
inline int Wh_GetIntValue(PCWSTR, int def) { return g_stubIntValue ? g_stubIntValue : def; }
inline BOOL Wh_SetIntValue(PCWSTR, int v) { g_stubIntValue = v; return TRUE; }
