// ps5cam-tray: notification-area control for the PS5 Camera. Changes apply live to any app that
// is using the camera (the media source re-reads the settings twice a second).
#include <windows.h>
#include <shellapi.h>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "../common/settings.h"

using namespace ps5cam;

namespace {

constexpr UINT WM_TRAY = WM_APP + 1;
constexpr UINT kTimerStatus = 1;
constexpr UINT kTrayId = 1;

enum Cmd : UINT {
    CmdBokehToggle = 99,
    CmdModeBase = 100,   // + ViewMode
    CmdBlurBase = 200,   // + level index
    CmdFocusAuto = 300,
    CmdFocusBase = 301,  // + index
    CmdHighlightsBase = 400,
    CmdAutoBrightness = 500,
    CmdPrefer60 = 501,
    CmdFullHdOnly = 502,
    CmdRecalibrate = 600,
    CmdOpenCamera = 601,
    CmdOpenLogs = 602,
    CmdExit = 700,
};

const wchar_t* kModeNames[] = {L"Портрет (боке по глубине)", L"Обычная камера", L"Второй сенсор",
    L"Карта глубины", L"Оба сенсора (стерео)"};
const struct {
    const wchar_t* name;
    uint32_t value;
} kBlur[] = {{L"Лёгкое", 25}, {L"Среднее", 50}, {L"Сильное", 75}, {L"Максимальное", 100}},
  kFocus[] = {{L"Близко (до 60 см)", 85}, {L"Средне (около 1 м)", 50}, {L"Далеко", 20}},
  kHighlights[] = {{L"Без бликов", 0}, {L"Обычные", 150}, {L"Яркие", 300}};

constexpr uint32_t kModeBokeh = 0;
constexpr uint32_t kModeMain = 1;

HWND g_wnd = nullptr;
bool g_added = false;  // Explorer may not be ready at logon; retried from the timer
HICON g_iconOn = nullptr, g_iconOff = nullptr;  // bokeh on: blue lenses; off: grey
UINT g_taskbarCreated = 0;

// Draws the camera itself: a dark rounded bar with two lenses.
HICON MakeIcon(int size, bool bokeh)
{
    const float glassR = bokeh ? 40.0f : 95.0f, glassG = bokeh ? 90.0f : 98.0f, glassB = bokeh ? 200.0f : 105.0f;
    BITMAPV5HEADER bi = {};
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = size;
    bi.bV5Height = -size;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;
    void* bits = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(dc, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    auto* px = static_cast<uint32_t*>(bits);
    const float s = size / 32.0f;
    auto cover = [](float dist) { return std::fmax(0.0f, std::fmin(1.0f, 0.5f - dist)); };
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            float fx = (x + 0.5f) / s, fy = (y + 0.5f) / s;
            // Rounded bar 2..30 x 9..23, radius 6.
            float qx = std::fmax(std::fabs(fx - 16) - 8, 0.0f), qy = std::fmax(std::fabs(fy - 16) - 1, 0.0f);
            float bar = std::sqrt(qx * qx + qy * qy) - 6.0f;
            float a = cover(bar * s);
            float r = 30, g = 32, b = 38;
            for (float cx : {10.5f, 21.5f}) {
                float d = std::sqrt((fx - cx) * (fx - cx) + (fy - 16) * (fy - 16));
                float ring = cover((d - 4.2f) * s);
                float glass = cover((d - 2.6f) * s);
                r = r + (150 - r) * ring;
                g = g + (160 - g) * ring;
                b = b + (175 - b) * ring;
                r = r + (glassR - r) * glass;
                g = g + (glassG - g) * glass;
                b = b + (glassB - b) * glass;
                float glint = cover((std::sqrt((fx - cx + 1) * (fx - cx + 1) + (fy - 15) * (fy - 15)) - 0.8f) * s);
                r += (255 - r) * glint;
                g += (255 - g) * glint;
                b += (255 - b) * glint;
            }
            uint32_t A = static_cast<uint32_t>(a * 255);
            px[y * size + x] = (A << 24) | (static_cast<uint32_t>(r * a) << 16) | (static_cast<uint32_t>(g * a) << 8) |
                               static_cast<uint32_t>(b * a);
        }
    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    ICONINFO ii = {TRUE, 0, 0, mask, color};
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    return icon;
}

std::wstring StatusText()
{
    Status st = LoadStatus();
    Settings s = LoadSettings();
    std::wstring t = L"PS5 Camera";
    if (st.streaming) {
        wchar_t buf[128];
        swprintf_s(buf, L"\n%ls, %.0f к/с", st.format.c_str(), st.fpsX100 / 100.0);
        t += buf;
        if (!st.error.empty()) t += L"\n" + st.error;
    } else {
        t += L"\nне используется";
    }
    t += L"\n";
    t += s.mode == kModeBokeh ? L"Боке включено" : s.mode == kModeMain ? L"Боке выключено" : kModeNames[s.mode < 5 ? s.mode : 0];
    t += L"\nКлик: вкл/выкл боке";
    if (t.size() > 127) t.resize(127);
    return t;
}

void UpdateTray(DWORD message)
{
    NOTIFYICONDATAW nid = {sizeof(nid)};
    nid.hWnd = g_wnd;
    nid.uID = kTrayId;
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE | NIF_SHOWTIP;
    nid.uCallbackMessage = WM_TRAY;
    nid.hIcon = LoadSettings().mode == kModeBokeh ? g_iconOn : g_iconOff;
    wcsncpy_s(nid.szTip, StatusText().c_str(), _TRUNCATE);
    if (message == NIM_MODIFY && !g_added) message = NIM_ADD;
    BOOL ok = Shell_NotifyIconW(message, &nid);
    if (!ok && message == NIM_ADD) ok = Shell_NotifyIconW(NIM_MODIFY, &nid);  // icon may still exist
    g_added = ok != FALSE;  // a failed modify (Explorer restarted) re-adds on the next tick
    if (message == NIM_ADD && ok) {
        nid.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &nid);
    }
}

void AddItem(HMENU m, UINT id, const wchar_t* text, bool checked, bool radio = true, bool enabled = true)
{
    MENUITEMINFOW mi = {sizeof(mi)};
    mi.fMask = MIIM_ID | MIIM_STRING | MIIM_STATE | MIIM_FTYPE;
    mi.fType = radio ? MFT_RADIOCHECK : MFT_STRING;
    mi.fState = (checked ? MFS_CHECKED : 0) | (enabled ? 0 : MFS_DISABLED);
    mi.wID = id;
    mi.dwTypeData = const_cast<wchar_t*>(text);
    InsertMenuItemW(m, GetMenuItemCount(m), TRUE, &mi);
}

void Save(const Settings& s)
{
    if (!SaveSettings(s))
        MessageBoxW(g_wnd, L"Не удалось сохранить настройки. Переустановите драйвер PS5 Camera.", L"PS5 Camera", MB_ICONWARNING);
    UpdateTray(NIM_MODIFY);
}

// Bokeh on <-> plain camera; from a diagnostic view it switches bokeh on. Apps using the camera
// pick it up within half a second.
void ToggleBokeh()
{
    Settings s = LoadSettings();
    s.mode = s.mode == kModeBokeh ? kModeMain : kModeBokeh;
    Save(s);
}

void ShowMenu()
{
    Settings s = LoadSettings();
    Status st = LoadStatus();
    HMENU menu = CreatePopupMenu();
    std::wstring header = st.streaming ? L"PS5 Camera: " + st.format : L"PS5 Camera: не используется";
    if (st.streaming && !st.error.empty()) header += L" (" + st.error + L")";
    AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, header.c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    AddItem(menu, CmdBokehToggle, L"Боке (размытие фона)\tклик по значку", s.mode == kModeBokeh, false);
    HMENU modes = CreatePopupMenu();
    for (UINT i = 0; i < 5; ++i) AddItem(modes, CmdModeBase + i, kModeNames[i], s.mode == i);
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(modes), L"Режим (в том числе для проверки)");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    HMENU blur = CreatePopupMenu();
    for (UINT i = 0; i < 4; ++i) AddItem(blur, CmdBlurBase + i, kBlur[i].name, s.blur == kBlur[i].value);
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(blur), L"Размытие фона");

    HMENU focus = CreatePopupMenu();
    AddItem(focus, CmdFocusAuto, L"Автофокус на человеке", s.autoFocus);
    for (UINT i = 0; i < 3; ++i) AddItem(focus, CmdFocusBase + i, kFocus[i].name, !s.autoFocus && s.focus == kFocus[i].value);
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(focus), L"Фокус");

    HMENU hl = CreatePopupMenu();
    for (UINT i = 0; i < 3; ++i) AddItem(hl, CmdHighlightsBase + i, kHighlights[i].name, s.highlights == kHighlights[i].value);
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(hl), L"Блики боке");

    AddItem(menu, CmdAutoBrightness, L"Автояркость (для тёмной комнаты)", s.autoBrightness, false);
    AddItem(menu, CmdFullHdOnly, L"Только Full HD 60 к/с (при следующем запуске камеры)", s.fullHdOnly, false);
    if (!s.fullHdOnly)
        AddItem(menu, CmdPrefer60, L"Предпочитать 60 к/с (при следующем запуске камеры)", s.prefer60, false);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, CmdRecalibrate, L"Перекалибровать стерео");
    AppendMenuW(menu, MF_STRING, CmdOpenCamera, L"Открыть приложение «Камера»");
    AppendMenuW(menu, MF_STRING, CmdOpenLogs, L"Папка журналов");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, CmdExit, L"Закрыть значок");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_wnd);
    UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, g_wnd, nullptr);
    DestroyMenu(menu);
    PostMessageW(g_wnd, WM_NULL, 0, 0);
    if (!cmd) return;

    if (cmd == CmdBokehToggle) {
        ToggleBokeh();
        return;
    }
    if (cmd >= CmdModeBase && cmd < CmdModeBase + 5) s.mode = cmd - CmdModeBase;
    else if (cmd >= CmdBlurBase && cmd < CmdBlurBase + 4) {
        s.blur = kBlur[cmd - CmdBlurBase].value;
        s.mode = 0;
    } else if (cmd == CmdFocusAuto) s.autoFocus = true;
    else if (cmd >= CmdFocusBase && cmd < CmdFocusBase + 3) {
        s.autoFocus = false;
        s.focus = kFocus[cmd - CmdFocusBase].value;
    } else if (cmd >= CmdHighlightsBase && cmd < CmdHighlightsBase + 3) s.highlights = kHighlights[cmd - CmdHighlightsBase].value;
    else if (cmd == CmdAutoBrightness) s.autoBrightness = !s.autoBrightness;
    else if (cmd == CmdPrefer60) s.prefer60 = !s.prefer60;
    else if (cmd == CmdFullHdOnly) s.fullHdOnly = !s.fullHdOnly;
    else if (cmd == CmdRecalibrate) {
        BumpCalibrationRequest();
        return;
    } else if (cmd == CmdOpenCamera) {
        ShellExecuteW(nullptr, L"open", L"microsoft.windows.camera:", nullptr, nullptr, SW_SHOWNORMAL);
        return;
    } else if (cmd == CmdOpenLogs) {
        wchar_t dir[MAX_PATH];
        ExpandEnvironmentStringsW(L"%ProgramData%\\PS5Camera", dir, MAX_PATH);
        ShellExecuteW(nullptr, L"open", dir, nullptr, nullptr, SW_SHOWNORMAL);
        return;
    } else if (cmd == CmdExit) {
        DestroyWindow(g_wnd);
        return;
    }
    Save(s);
}

LRESULT CALLBACK WndProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == g_taskbarCreated && g_taskbarCreated) {
        g_added = false;
        UpdateTray(NIM_ADD);  // Explorer restarted
        return 0;
    }
    switch (msg) {
    case WM_TRAY:
        switch (LOWORD(lp)) {
        case WM_CONTEXTMENU:  // right click or Shift+F10: the menu
            ShowMenu();
            break;
        case NIN_SELECT:      // left click or Enter/Space: bokeh on/off
        case NIN_KEYSELECT:
            ToggleBokeh();
            break;
        }
        return 0;
    case WM_TIMER:
        if (wp == kTimerStatus) UpdateTray(NIM_MODIFY);
        return 0;
    case WM_DESTROY: {
        NOTIFYICONDATAW nid = {sizeof(nid)};
        nid.hWnd = wnd;
        nid.uID = kTrayId;
        Shell_NotifyIconW(NIM_DELETE, &nid);
        PostQuitMessage(0);
        return 0;
    }
    }
    return DefWindowProcW(wnd, msg, wp, lp);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int)
{
    HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\PS5CameraTray");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"PS5CameraTray";
    RegisterClassExW(&wc);
    // A hidden top-level window (not message-only): it receives the TaskbarCreated broadcast and can
    // take the foreground so the menu closes when clicking elsewhere.
    g_wnd = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"PS5 Camera", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
    if (!g_wnd) return 1;
    const int iconSize = GetSystemMetrics(SM_CXSMICON) * 2 > 40 ? 32 : 16;
    g_iconOn = MakeIcon(iconSize, true);
    g_iconOff = MakeIcon(iconSize, false);
    UpdateTray(NIM_ADD);
    SetTimer(g_wnd, kTimerStatus, 2000, nullptr);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    DestroyIcon(g_iconOn);
    DestroyIcon(g_iconOff);
    if (single) CloseHandle(single);
    return 0;
}
