// Smaller DLLs: ADVAPI32 (registry), SHLWAPI, SHELL32, OLE32, USER32/GDI32/IMM32 (headless bring-up stubs).
// Windowing is a placeholder until the SDL layer exists: windows are fake handles.
#include <unistd.h>

#include "rt.h"
#include "input.h"

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
IMP_ID(U, "RegisterWindowMessageA", RegisterWindowMessageA, 4) { SET(0xc100); }
IMP_ID(U, "CreateWindowExW", CreateWindowExW, 48) { SET(0x30001); }
IMP_ID(U, "ShowWindow", ShowWindow, 8) { SET(1); }
IMP_ID(U, "UpdateWindow", UpdateWindow, 4) { SET(1); }
IMP_ID(U, "SetWindowPos", SetWindowPos, 28) { log("SetWindowPos(hwnd %x, x %d y %d cx %d cy %d flags %x)", ARG(0), (int)ARG(2), (int)ARG(3), (int)ARG(4), (int)ARG(5), ARG(6)); SET(1); }
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
IMP_ID(U, "ClientToScreen", ClientToScreen, 8) { SET(1); }
IMP_ID(U, "AdjustWindowRect", AdjustWindowRect, 12) { SET(1); }
IMP_ID(U, "GetSystemMetrics", GetSystemMetrics, 4) { uint32_t i = ARG(0); int w, h; input::backSize(w, h); SET(i == 0 ? w : i == 1 ? h : i == 80 ? 1 : 0); }   // the "desktop" is always as big as the game surface
IMP_ID(U, "SetCursor", SetCursor, 4) { SET(0); }
IMP_ID(U, "LoadIconA", LoadIconA, 8) { SET(0x40001); }
IMP_ID(U, "LoadCursorFromFileA", LoadCursorFromFileA, 4) { SET(0x40002); }
IMP_ID(U, "ClipCursor", ClipCursor, 4) { SET(1); }
IMP_ID(U, "SetTimer", SetTimer, 16) { SET(1); }
IMP_ID(U, "KillTimer", KillTimer, 8) { SET(1); }
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

// ---- window messages: the message queue is empty for now (input is delivered later by the Android layer)
IMP_ID(U, "TranslateMessage", TranslateMessage, 4) { SET(0); }
IMP_ID(U, "PostQuitMessage", PostQuitMessage, 4) { log("PostQuitMessage(%d)", (int)ARG(0)); SET(0); }
IMP_ID(U, "GetWindowRect", GetWindowRect, 8) { uint32_t r = ARG(1); int w, h; input::backSize(w, h); guest::store<uint32_t>(r, 0); guest::store<uint32_t>(r + 4, 0); guest::store<uint32_t>(r + 8, w); guest::store<uint32_t>(r + 12, h); SET(1); }
IMP_ID(U, "ScreenToClient", ScreenToClient, 8) { SET(1); }
IMP_ID(U, "SetWindowTextA", SetWindowTextA, 8) { SET(1); }
IMP_ID(U, "FindWindowA", FindWindowA, 8) { SET(0); }
IMP_ID(U, "GetSystemMenu", GetSystemMenu, 8) { SET(0); }
IMP_ID(U, "DeleteMenu", DeleteMenu, 12) { SET(0); }
IMP_ID(U, "DrawMenuBar", DrawMenuBar, 4) { SET(1); }
IMP_ID(U, "EnumDisplayDevicesA", EnumDisplayDevicesA, 16) { SET(0); }
IMP_ID(U, "GetKeyboardLayoutList", GetKeyboardLayoutList, 8) { if (ARG(0) && ARG(1)) guest::store<uint32_t>(ARG(1), 0x04090409); SET(1); }
IMP_ID(U, "EmptyClipboard", EmptyClipboard, 0) { SET(1); }
IMP_ID(U, "SetClipboardData", SetClipboardData, 8) { SET(0); }
IMP_ID(U, "IsClipboardFormatAvailable", IsClipboardFormatAvailable, 4) { SET(0); }
IMP_ID(U, "GetDlgItem", GetDlgItem, 8) { SET(0); }
IMP_ID(U, "SendDlgItemMessageA", SendDlgItemMessageA, 20) { SET(0); }
IMP_ID(U, "EndDialog", EndDialog, 8) { SET(1); }
IMP_ID(U, "DialogBoxIndirectParamA", DialogBoxIndirectParamA, 20) { log("DialogBoxIndirectParamA (ignored)"); SET(1); }

// ---- other missing imports (fail cleanly / report "nothing available")
IMP_ID(SHE, "ShellExecuteA", ShellExecuteA, 24) { SET(33); }
IMP_ID(ADV, "OpenProcessToken", OpenProcessToken, 12) { SET(0); }
IMP_ID(ADV, "OpenThreadToken", OpenThreadToken, 16) { SET(0); }
IMP_ID(ADV, "LookupPrivilegeValueA", LookupPrivilegeValueA, 12) { SET(0); }
IMP_ID(ADV, "AdjustTokenPrivileges", AdjustTokenPrivileges, 24) { SET(0); }
IMP_ID("USP10.dll", "ScriptItemize", ScriptItemize, 28) { SET(0x80004005); }
IMP_ID("USP10.dll", "ScriptBreak", ScriptBreak, 16) { SET(0x80004005); }
IMP_ID("WS2_32.dll", "WSARecv", WSARecv, 28) { SET(0xffffffff); }
IMP_ID("WS2_32.dll", "WSARecvFrom", WSARecvFrom, 36) { SET(0xffffffff); }
IMP_ID("WS2_32.dll", "WSAGetOverlappedResult", WSAGetOverlappedResult, 20) { SET(0); }

// msvcp80!_FInf: float infinity constant (data export)
namespace
{
struct FInfReg { FInfReg() { imports::registerData("msvcp80.dll", "_FInf", 16, []() -> uint32_t { return 0x7f800000u; }); } } fInfReg;
}
