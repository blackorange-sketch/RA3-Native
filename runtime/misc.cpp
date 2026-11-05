// Smaller DLLs: ADVAPI32 (registry), SHLWAPI, SHELL32, OLE32, USER32/GDI32/IMM32 (headless bring-up stubs).
// Windowing is a placeholder until the SDL layer exists: windows are fake handles.
#include <unistd.h>

#include "rt.h"

using namespace rt;

#define ADV "ADVAPI32.dll"
#define SHL "SHLWAPI.dll"
#define SHE "SHELL32.dll"
#define OLE "ole32.dll"
#define U "USER32.dll"
#define IMM "IMM32.dll"

// ---- registry: nothing exists, the game falls back to defaults
IMP_ID(ADV, "RegOpenKeyExA", RegOpenKeyExA, 20) { SET(2); }
IMP_ID(ADV, "RegOpenKeyExW", RegOpenKeyExW, 20) { SET(2); }
IMP_ID(ADV, "RegCreateKeyExA", RegCreateKeyExA, 36) { SET(5); }
IMP_ID(ADV, "RegQueryValueExA", RegQueryValueExA, 24) { SET(2); }
IMP_ID(ADV, "RegQueryValueExW", RegQueryValueExW, 24) { SET(2); }
IMP_ID(ADV, "RegSetValueExA", RegSetValueExA, 24) { SET(0); }
IMP_ID(ADV, "RegCloseKey", RegCloseKey, 4) { SET(0); }

// ---- shlwapi
IMP_ID(SHL, "PathFileExistsW", PathFileExistsW, 4)
{
	std::string hp = hostPath(utf16to8(gwstr(ARG(0))));
	SET(access(hp.c_str(), F_OK) == 0);
}
IMP_ID(SHL, "PathFindFileNameA", PathFindFileNameA, 4)
{
	std::string s = gstr(ARG(0));
	size_t k = s.find_last_of("\\/");
	SET(k == std::string::npos ? ARG(0) : ARG(0) + (uint32_t)k + 1);
}
IMP_ID(SHL, "PathRemoveExtensionA", PathRemoveExtensionA, 4)
{
	std::string s = gstr(ARG(0));
	size_t d = s.find_last_of('.'), sl = s.find_last_of("\\/");
	if (d != std::string::npos && (sl == std::string::npos || d > sl)) guest::store<uint8_t>(ARG(0) + (uint32_t)d, 0);
}
IMP_ID(SHL, "PathCombineW", PathCombineW, 12)
{
	std::u16string a = gwstr(ARG(1)), b = gwstr(ARG(2));
	if (!a.empty() && a.back() != u'\\' && a.back() != u'/') a += u'\\';
	putWStr(ARG(0), a + b);
	SET(ARG(0));
}

// ---- shell32
IMP_ID(SHE, "SHGetSpecialFolderPathA", SHGetSpecialFolderPathA, 16) { putStr(ARG(1), "C:\\Users\\player\\Documents"); SET(1); }
IMP_ID(SHE, "SHGetSpecialFolderPathW", SHGetSpecialFolderPathW, 16) { putWStr(ARG(1), u"C:\\Users\\player\\Documents"); SET(1); }
IMP_ID(SHE, "ShellExecuteW", ShellExecuteW, 24) { SET(33); }

// ---- ole32
IMP_ID(OLE, "CoInitialize", CoInitialize, 4) { SET(0); }
IMP_ID(OLE, "CoUninitialize", CoUninitialize, 0) {}
IMP_ID(OLE, "CoTaskMemFree", CoTaskMemFree, 4) { gfree(ARG(0)); }
IMP_ID(OLE, "CoCreateInstance", CoCreateInstance, 20) { if (ARG(4)) guest::store<uint32_t>(ARG(4), 0); SET(0x80040154); }
IMP_ID(OLE, "OleRun", OleRun, 4) { SET(0); }

// ---- imm32: no IME
IMP_ID(IMM, "ImmGetContext", ImmGetContext, 4) { SET(0); }
IMP_ID(IMM, "ImmReleaseContext", ImmReleaseContext, 8) { SET(1); }
IMP_ID(IMM, "ImmAssociateContext", ImmAssociateContext, 8) { SET(0); }
IMP_ID(IMM, "ImmCreateContext", ImmCreateContext, 0) { SET(0); }
IMP_ID(IMM, "ImmDestroyContext", ImmDestroyContext, 4) { SET(1); }
IMP_ID(IMM, "ImmGetProperty", ImmGetProperty, 8) { SET(0); }
IMP_ID(IMM, "ImmGetIMEFileNameA", ImmGetIMEFileNameA, 12) { SET(0); }
IMP_ID(IMM, "ImmGetCompositionStringW", ImmGetCompositionStringW, 16) { SET(0); }
IMP_ID(IMM, "ImmGetCandidateListW", ImmGetCandidateListW, 16) { SET(0); }
IMP_ID(IMM, "ImmGetCandidateListCountW", ImmGetCandidateListCountW, 8) { SET(0); }
IMP_ID(IMM, "ImmNotifyIME", ImmNotifyIME, 16) { SET(0); }
IMP_ID(IMM, "ImmSetOpenStatus", ImmSetOpenStatus, 8) { SET(0); }

// ---- user32 / gdi32 (placeholders)
IMP_ID(U, "wsprintfA", wsprintfA, 0) { ArgReader ap{c->r[ESP] + 12}; SET(putStr(ARG(0), formatA(gstr(ARG(1)), ap))); }
IMP_ID(U, "RegisterClassW", RegisterClassW, 4) { SET(0xc001); }
IMP_ID(U, "RegisterWindowMessageA", RegisterWindowMessageA, 4) { SET(0xc100); }
IMP_ID(U, "CreateWindowExW", CreateWindowExW, 48) { SET(0x30001); }
IMP_ID(U, "ShowWindow", ShowWindow, 8) { SET(1); }
IMP_ID(U, "UpdateWindow", UpdateWindow, 4) { SET(1); }
IMP_ID(U, "SetWindowPos", SetWindowPos, 28) { SET(1); }
IMP_ID(U, "SetWindowTextW", SetWindowTextW, 8) { SET(1); }
IMP_ID(U, "SetForegroundWindow", SetForegroundWindow, 4) { SET(1); }
IMP_ID(U, "SetFocus", SetFocus, 4) { SET(0); }
IMP_ID(U, "GetFocus", GetFocus, 0) { SET(0x30001); }
IMP_ID(U, "GetForegroundWindow", GetForegroundWindow, 0) { SET(0x30001); }
IMP_ID(U, "GetDesktopWindow", GetDesktopWindow, 0) { SET(0x10010); }
IMP_ID(U, "FindWindowW", FindWindowW, 8) { SET(0); }
IMP_ID(U, "IsIconic", IsIconic, 4) { SET(0); }
IMP_ID(U, "IsWindowUnicode", IsWindowUnicode, 4) { SET(1); }
IMP_ID(U, "GetWindowLongA", GetWindowLongA, 8) { SET(0); }
IMP_ID(U, "DefWindowProcA", DefWindowProcA, 16) { SET(0); }
IMP_ID(U, "DefWindowProcW", DefWindowProcW, 16) { SET(0); }
IMP_ID(U, "GetClientRect", GetClientRect, 8) { guest::store<int32_t>(ARG(1), 0); guest::store<int32_t>(ARG(1) + 4, 0); guest::store<int32_t>(ARG(1) + 8, 1280); guest::store<int32_t>(ARG(1) + 12, 720); SET(1); }
IMP_ID(U, "ClientToScreen", ClientToScreen, 8) { SET(1); }
IMP_ID(U, "AdjustWindowRect", AdjustWindowRect, 12) { SET(1); }
IMP_ID(U, "GetSystemMetrics", GetSystemMetrics, 4) { uint32_t i = ARG(0); SET(i == 0 ? 1280 : i == 1 ? 720 : i == 80 ? 1 : 0); }
IMP_ID(U, "SetCursor", SetCursor, 4) { SET(0); }
IMP_ID(U, "LoadIconA", LoadIconA, 8) { SET(0x40001); }
IMP_ID(U, "LoadCursorFromFileA", LoadCursorFromFileA, 4) { SET(0x40002); }
IMP_ID(U, "ClipCursor", ClipCursor, 4) { SET(1); }
IMP_ID(U, "SetTimer", SetTimer, 16) { SET(1); }
IMP_ID(U, "KillTimer", KillTimer, 8) { SET(1); }
IMP_ID(U, "GetKeyState", GetKeyState, 4) { SET(0); }
IMP_ID(U, "GetAsyncKeyState", GetAsyncKeyState, 4) { SET(0); }
IMP_ID(U, "GetKeyboardLayout", GetKeyboardLayout, 4) { SET(0x04190419); }
IMP_ID(U, "GetKeyNameTextW", GetKeyNameTextW, 12) { SET(0); }
IMP_ID(U, "GetDoubleClickTime", GetDoubleClickTime, 0) { SET(500); }
IMP_ID(U, "OpenClipboard", OpenClipboard, 4) { SET(0); }
IMP_ID(U, "CloseClipboard", CloseClipboard, 0) { SET(1); }
IMP_ID(U, "GetClipboardData", GetClipboardData, 4) { SET(0); }
IMP_ID(U, "MessageBoxA", MessageBoxA, 16) { log("MessageBoxA: %s | %s", gstr(ARG(2)).c_str(), gstr(ARG(1)).c_str()); SET(1); }
IMP_ID(U, "MessageBoxW", MessageBoxW, 16) { log("MessageBoxW: %s | %s", utf16to8(gwstr(ARG(2))).c_str(), utf16to8(gwstr(ARG(1))).c_str()); SET(1); }
IMP_ID("GDI32.dll", "GetStockObject", GetStockObject, 4) { SET(0x50001); }
IMP_ID("NETAPI32.dll", "Netbios", Netbios, 4) { SET(0x0b); }
