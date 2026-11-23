// WARVRConfig.exe: a small settings window for WARVR, for people who would rather not edit WARVR.ini by hand.
//
// It lives next to WARVR.dll in the Warhammer folder and edits, keeping everything else in those files as it was:
//   WARVR.ini   the main options (the same keys you can type by hand; every other option stays there)
//   dxvk.conf   anisotropic filtering, when the game runs on DXVK
//   HKCU\Environment DXVK_LOG_LEVEL   whether DXVK writes its own WAR_d3d9.log
//   <server>\Configs\World.xml   the time of day on your own ProjectWAR server (needs docs/SERVER_TIME.md once)
// Changes apply the next time the game (or the WorldServer) starts. 32-bit, no extra DLLs.
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601     // Windows 7
#endif
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>
#include "settings.h"

#ifdef _MSC_VER
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif

using settings::IniGet;
using settings::IniSet;

// ---------------------------------------------------------------- files and strings
static std::wstring Widen(const std::string& s)
{
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

static std::string Narrow(const std::wstring& w)
{
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

static bool ReadAll(const std::wstring& path, std::string* out)
{
    out->clear();
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    char buf[65536];
    DWORD got = 0;
    while (ReadFile(h, buf, sizeof(buf), &got, nullptr) && got) out->append(buf, got);
    CloseHandle(h);
    return true;
}

// Writes to a temporary file first and then swaps it in, so a failed write never leaves half a settings file.
static bool WriteAll(const std::wstring& path, const std::string& data, std::wstring* err)
{
    const std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD wrote = 0;
    bool ok = h != INVALID_HANDLE_VALUE && WriteFile(h, data.data(), (DWORD)data.size(), &wrote, nullptr) && wrote == data.size();
    DWORD e = GetLastError();
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    if (ok) { ok = MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0; e = GetLastError(); }
    if (!ok) {
        DeleteFileW(tmp.c_str());
        wchar_t msg[512] = L"";
        FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, e, 0, msg, 512, nullptr);
        *err = L"Could not write " + path + L":\n" + msg;
        if (e == ERROR_ACCESS_DENIED)
            *err += L"\nIf the game is under Program Files, run WARVRConfig.exe as administrator.";
    }
    return ok;
}

static bool FileExists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

static std::wstring ExeDir()
{
    wchar_t p[MAX_PATH * 2] = L"";
    GetModuleFileNameW(nullptr, p, MAX_PATH * 2);
    std::wstring s = p;
    return s.substr(0, s.find_last_of(L"\\/"));
}

static std::wstring JoinPath(std::wstring a, const std::wstring& b)
{
    while (!a.empty() && (a.back() == L'\\' || a.back() == L'/')) a.pop_back();
    return a + L"\\" + b;
}

// ---------------------------------------------------------------- the choices offered
static const wchar_t* kHudLabels[] = { L"Flat (as in the original mod)", L"Curved (bent around you)", L"Low (curved, lower, leaned back)" };
static const char* kHudValues[] = { "flat", "curved", "low" };
static const wchar_t* kOriginLabels[] = { L"Hand (low and to the right)", L"Eyes (only the dot shows)" };
static const char* kOriginValues[] = { "hand", "eyes" };
static const char* kDrawValues[] = { "1.0", "1.3", "1.5", "2", "3", "4" };
static const wchar_t* kDrawLabels[] = { L"x1.0 (the game's own)", L"x1.3 (looks good, costs some speed)", L"x1.5", L"x2", L"x3", L"x4" };
static const char* kAnisoValues[] = { "", "2", "4", "8", "16" };
static const wchar_t* kAnisoLabels[] = { L"DXVK's default", L"2x anisotropic", L"4x anisotropic", L"8x anisotropic", L"16x anisotropic" };

enum {
    IDC_AUTOVR = 101, IDC_HUD, IDC_LASER, IDC_ORIGIN, IDC_DRAW, IDC_DRAWFLAT, IDC_GFX, IDC_LAZY, IDC_ANISO, IDC_DXVKLOG,
    IDC_LOG, IDC_OPENLOG, IDC_SERVERDIR, IDC_BROWSE, IDC_TIME, IDC_SERVERSTATUS, IDC_OPENINI, IDC_HINT, IDC_STATUS,
};

struct State {
    std::wstring dir, iniPath, confPath;
    std::string ini, conf;              // file contents as loaded (or last saved)
    std::vector<std::string> drawValues; // parallel to the draw distance list (a value typed by hand gets its own entry)
    bool hudCustom = false;              // hud_curve / hud_tilt set by hand: left alone unless a style is picked here
    bool dxvkLogOn = true;               // as found in the registry
    std::wstring serverExe, worldXml, worldXmlDev;
    bool serverOk = false;
    int timeLoaded = 0;
    std::wstring serverDirShown;         // the folder RefreshServer last looked at
    bool dirty = false;
};
static State g;
static HWND g_wnd;
static HFONT g_font;
static int g_dpi = 96;
static int S(int v) { return MulDiv(v, g_dpi, 96); }

static HWND Ctl(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id)
{
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h), g_wnd,
                             (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)g_font, FALSE);
    return c;
}
static HWND Item(int id) { return GetDlgItem(g_wnd, id); }
static bool Checked(int id) { return SendMessageW(Item(id), BM_GETCHECK, 0, 0) == BST_CHECKED; }
static void Check(int id, bool on) { SendMessageW(Item(id), BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0); }
static int Sel(int id) { return (int)SendMessageW(Item(id), CB_GETCURSEL, 0, 0); }
static void SetSel(int id, int i) { SendMessageW(Item(id), CB_SETCURSEL, i, 0); }
static void AddItem(int id, const std::wstring& s) { SendMessageW(Item(id), CB_ADDSTRING, 0, (LPARAM)s.c_str()); }
static void SetText(int id, const std::wstring& s) { SetWindowTextW(Item(id), s.c_str()); }
static std::wstring GetText(int id)
{
    int n = GetWindowTextLengthW(Item(id));
    std::wstring s(n + 1, L'\0');
    GetWindowTextW(Item(id), &s[0], n + 1);
    s.resize(n);
    return s;
}

static std::string IniValue(const char* key, const char* def)
{
    std::string v;
    return IniGet(g.ini, key, &v) ? v : def;
}

// ---------------------------------------------------------------- the server part
// Reads the server folder's state into the window: is it a WorldServer folder, does that build know the time setting.
static void RefreshServer()
{
    const std::wstring dir = GetText(IDC_SERVERDIR);
    g.serverDirShown = dir;
    g.serverOk = false;
    g.serverExe = g.worldXml = g.worldXmlDev = L"";
    std::wstring msg;
    int sel = 0;
    std::string exe;
    if (dir.empty()) {
        msg = L"Only for your own ProjectWAR server. Pick its folder to change the time of day.";
    } else if (!ReadAll(JoinPath(dir, L"WorldServer.exe"), &exe)) {
        msg = L"There is no WorldServer.exe in this folder. Pick the server's bin\\Release folder.";
    } else if (!settings::ServerHasTimeOffset(exe)) {
        msg = L"This server build has no time-of-day setting yet. Make the two-line change in docs\\SERVER_TIME.md "
              L"(in the WARVR download) and build the server once.";
    } else {
        g.serverOk = true;
        g.serverExe = JoinPath(dir, L"WorldServer.exe");
        g.worldXml = JoinPath(dir, L"Configs\\World.xml");
        // building the server copies this file over Configs\World.xml, so the setting goes there too
        const std::wstring dev = JoinPath(dir, L"..\\..\\WorldServer\\Configs\\LocalDevelopment\\World.xml");
        if (FileExists(dev)) g.worldXmlDev = dev;
        std::string xml, v;
        if (ReadAll(g.worldXml, &xml) && settings::XmlGet(xml, "TimeOfDayOffsetHours", &v)) {
            int h = (int)std::lround(atof(v.c_str()));
            sel = ((h % 24) + 24) % 24;
        }
        msg = L"After saving, restart the WorldServer. The new time shows at your next login or zone change.";
    }
    g.timeLoaded = sel;
    SetSel(IDC_TIME, sel);
    EnableWindow(Item(IDC_TIME), g.serverOk);
    SetText(IDC_SERVERSTATUS, msg);
}

// ---------------------------------------------------------------- load / save
static bool DxvkLogFromRegistry()
{
    wchar_t buf[64] = L"";
    DWORD size = sizeof(buf), type = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Environment", L"DXVK_LOG_LEVEL", RRF_RT_REG_SZ, &type, buf, &size) != ERROR_SUCCESS)
        return true;                                               // not set: DXVK logs (its default)
    return _wcsicmp(buf, L"none") != 0;
}

static void Load()
{
    g.dir = ExeDir();
    g.iniPath = JoinPath(g.dir, L"WARVR.ini");
    g.confPath = JoinPath(g.dir, L"dxvk.conf");
    ReadAll(g.iniPath, &g.ini);
    ReadAll(g.confPath, &g.conf);

    Check(IDC_AUTOVR, settings::IsOn(IniValue("auto_vr", "off")));

    std::string v;
    g.hudCustom = IniGet(g.ini, "hud_curve", &v) || IniGet(g.ini, "hud_tilt", &v);
    const std::string hud = IniValue("hud_style", "flat");
    int hs = settings::IEq(hud, "curved") ? 1 : (settings::IEq(hud, "low") ? 2 : 0);
    if (g.hudCustom) { AddItem(IDC_HUD, L"Custom (hud_curve / hud_tilt in WARVR.ini)"); hs = 3; }
    SetSel(IDC_HUD, hs);

    Check(IDC_LASER, settings::IsOn(IniValue("laser", "off")));
    const std::string origin = IniValue("laser_origin", "hand");
    SetSel(IDC_ORIGIN, settings::IEq(origin, "eyes") || settings::IEq(origin, "head") ? 1 : 0);

    const double dd = atof(IniValue("draw_distance", "1.0").c_str());
    int di = -1;
    for (int i = 0; i < 6; ++i) if (std::fabs(atof(kDrawValues[i]) - dd) < 0.001) di = i;
    if (di < 0 && dd >= 1.0 && dd <= 16.0) {                       // a value typed by hand: offer it as it is
        const std::string mine = IniValue("draw_distance", "1.0");
        g.drawValues.push_back(mine);
        AddItem(IDC_DRAW, L"x" + Widen(mine) + L" (from WARVR.ini)");
        di = 6;
    }
    SetSel(IDC_DRAW, di < 0 ? 0 : di);
    const std::string flat = IniValue("draw_distance_flat", "on");
    Check(IDC_DRAWFLAT, !(settings::IEq(flat, "off") || flat == "0"));

    Check(IDC_LAZY, settings::IsOn(IniValue("lazy_state", "off")));
    Check(IDC_LOG, settings::IsOn(IniValue("log", "off")));

    std::string aniso;
    int ai = 0;
    if (settings::ConfGet(g.conf, "d3d9.samplerAnisotropy", &aniso))
        for (int i = 1; i < 5; ++i) if (aniso == kAnisoValues[i]) ai = i;
    SetSel(IDC_ANISO, ai);
    g.dxvkLogOn = DxvkLogFromRegistry();
    Check(IDC_DXVKLOG, g.dxvkLogOn);

    std::string d3d9;
    ReadAll(JoinPath(g.dir, L"d3d9.dll"), &d3d9);
    const wchar_t* gfx = L"";
    switch (settings::DetectD3D9(d3d9)) {
        case settings::D3D9Kind::Dxvk:     gfx = L"This folder runs the game on DXVK (see docs\\DXVK.md)."; break;
        case settings::D3D9Kind::DgVoodoo: gfx = L"This folder runs the game on dgVoodoo."; break;
        case settings::D3D9Kind::Other:    gfx = L"There is a d3d9.dll here that is neither DXVK nor dgVoodoo."; break;
        case settings::D3D9Kind::None:     gfx = L"No d3d9.dll here: the mod needs dgVoodoo or DXVK next to WAR.exe (see the README)."; break;
    }
    SetText(IDC_GFX, gfx);

    std::string server;
    if (IniGet(g.ini, "server_dir", &server, true)) SetText(IDC_SERVERDIR, Widen(server));
    RefreshServer();

    SetText(IDC_STATUS, FileExists(g.iniPath) ? L"Settings from " + g.iniPath : L"No WARVR.ini yet: saving creates one.");
    g.dirty = false;
}

static bool Save()
{
    std::wstring err;
    std::string ini = g.ini;
    // writes a key when it is already in the file or differs from the mod's default, so a fresh file stays short
    auto put = [&](const char* key, const std::string& val, const char* def) {
        std::string cur;
        if (IniGet(ini, key, &cur) || !settings::IEq(val, def)) ini = IniSet(ini, key, val);
    };
    put("auto_vr", Checked(IDC_AUTOVR) ? "on" : "off", "off");
    const int hs = Sel(IDC_HUD);
    if (hs >= 0 && hs < 3) {
        put("hud_style", kHudValues[hs], "flat");
        if (g.hudCustom) {          // a style picked here replaces the hand-made HUD shape (kept as a comment)
            ini = settings::IniDisable(ini, "hud_curve");
            ini = settings::IniDisable(ini, "hud_tilt");
            g.hudCustom = false;
        }
    }
    put("laser", Checked(IDC_LASER) ? "on" : "off", "off");
    put("laser_origin", kOriginValues[Sel(IDC_ORIGIN) == 1 ? 1 : 0], "hand");
    const int di = Sel(IDC_DRAW);
    put("draw_distance", di >= 0 && di < 6 ? kDrawValues[di] : g.drawValues.empty() ? "1.0" : g.drawValues[0], "1.0");
    put("draw_distance_flat", Checked(IDC_DRAWFLAT) ? "on" : "off", "on");
    put("lazy_state", Checked(IDC_LAZY) ? "on" : "off", "off");
    put("log", Checked(IDC_LOG) ? "on" : "off", "off");
    std::string sd = Narrow(GetText(IDC_SERVERDIR)), cur;
    if (!sd.empty() || IniGet(ini, "server_dir", &cur, true)) ini = IniSet(ini, "server_dir", sd);
    if (ini != g.ini) {
        if (!WriteAll(g.iniPath, ini, &err)) { MessageBoxW(g_wnd, err.c_str(), L"WARVR settings", MB_ICONERROR); return false; }
        g.ini = ini;
    }

    const int ai = Sel(IDC_ANISO);
    const std::string conf = settings::ConfSet(g.conf, "d3d9.samplerAnisotropy", kAnisoValues[ai < 0 ? 0 : ai]);
    if (conf != g.conf) {
        if (!WriteAll(g.confPath, conf, &err)) { MessageBoxW(g_wnd, err.c_str(), L"WARVR settings", MB_ICONERROR); return false; }
        g.conf = conf;
    }

    std::wstring extra;
    if (Checked(IDC_DXVKLOG) != g.dxvkLogOn) {
        HKEY k;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Environment", 0, KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
            if (Checked(IDC_DXVKLOG)) RegDeleteValueW(k, L"DXVK_LOG_LEVEL");
            else RegSetValueExW(k, L"DXVK_LOG_LEVEL", 0, REG_SZ, (const BYTE*)L"none", 5 * sizeof(wchar_t));
            RegCloseKey(k);
            DWORD_PTR r;
            SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)L"Environment", SMTO_ABORTIFHUNG, 2000, &r);
            g.dxvkLogOn = Checked(IDC_DXVKLOG);
            extra += L" The DXVK log change applies to programs started from now on.";
        }
    }

    if (g.serverOk && Sel(IDC_TIME) != g.timeLoaded) {
        const std::string hours = std::to_string(Sel(IDC_TIME));
        for (const std::wstring& path : { g.worldXml, g.worldXmlDev }) {
            if (path.empty()) continue;
            std::string xml;
            if (!ReadAll(path, &xml) || !settings::XmlSet(&xml, "TimeOfDayOffsetHours", hours, "RealmId")) {
                MessageBoxW(g_wnd, (L"Could not read the server's " + path).c_str(), L"WARVR settings", MB_ICONERROR);
                return false;
            }
            if (!WriteAll(path, xml, &err)) { MessageBoxW(g_wnd, err.c_str(), L"WARVR settings", MB_ICONERROR); return false; }
        }
        g.timeLoaded = Sel(IDC_TIME);
        extra += L" Restart the WorldServer for the new time.";
    }

    SetText(IDC_STATUS, L"Saved. Start the game to use the new settings." + extra);
    g.dirty = false;
    return true;
}

static void BrowseServer()
{
    BROWSEINFOW bi{};
    bi.hwndOwner = g_wnd;
    bi.lpszTitle = L"Pick your ProjectWAR server folder (the one with WorldServer.exe, usually bin\\Release)";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return;
    wchar_t path[MAX_PATH] = L"";
    if (SHGetPathFromIDListW(pidl, path)) { SetText(IDC_SERVERDIR, path); RefreshServer(); g.dirty = true; }
    CoTaskMemFree(pidl);
}

static void OpenIni()
{
    if (!FileExists(g.iniPath)) {
        std::wstring err;
        if (!WriteAll(g.iniPath, "; WARVR settings. WARVR.ini.example lists every option.\r\n", &err)) {
            MessageBoxW(g_wnd, err.c_str(), L"WARVR settings", MB_ICONERROR);
            return;
        }
        ReadAll(g.iniPath, &g.ini);
    }
    ShellExecuteW(g_wnd, L"open", L"notepad.exe", (L"\"" + g.iniPath + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
    SetText(IDC_STATUS, L"Opened WARVR.ini. Close and reopen this window to see changes made there.");
}

// ---------------------------------------------------------------- window
static void Build()
{
    const DWORD CHK = BS_AUTOCHECKBOX | WS_TABSTOP, CMB = CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL;
    const DWORD GRP = BS_GROUPBOX, BTN = BS_PUSHBUTTON | WS_TABSTOP;

    Ctl(L"BUTTON", L"In VR", GRP, 12, 8, 540, 172, 0);
    Ctl(L"BUTTON", L"Turn VR on by itself when you enter the world (Ctrl+Shift+Numpad * still toggles it)", CHK, 24, 30, 520, 20, IDC_AUTOVR);
    Ctl(L"STATIC", L"HUD style:", 0, 24, 61, 110, 20, 0);
    Ctl(L"COMBOBOX", L"", CMB, 140, 57, 260, 200, IDC_HUD);
    for (auto s : kHudLabels) AddItem(IDC_HUD, s);
    Ctl(L"STATIC", L"Ctrl+Shift+H in the game", 0, 410, 61, 140, 20, 0);
    Ctl(L"BUTTON", L"Laser pointer on from the start (Ctrl+Shift+L toggles it)", CHK, 24, 88, 520, 20, IDC_LASER);
    Ctl(L"STATIC", L"Laser starts at:", 0, 24, 119, 110, 20, 0);
    Ctl(L"COMBOBOX", L"", CMB, 140, 115, 260, 200, IDC_ORIGIN);
    for (auto s : kOriginLabels) AddItem(IDC_ORIGIN, s);
    Ctl(L"STATIC", L"Draw distance:", 0, 24, 149, 110, 20, 0);
    Ctl(L"COMBOBOX", L"", CMB, 140, 145, 260, 200, IDC_DRAW);
    for (auto s : kDrawLabels) AddItem(IDC_DRAW, s);
    Ctl(L"BUTTON", L"Also without VR", CHK, 410, 148, 140, 20, IDC_DRAWFLAT);

    Ctl(L"BUTTON", L"Graphics", GRP, 12, 188, 540, 140, 0);
    Ctl(L"STATIC", L"", 0, 24, 210, 520, 20, IDC_GFX);
    Ctl(L"BUTTON", L"Lazy state: skip some repeated work for every object (experimental)", CHK, 24, 234, 520, 20, IDC_LAZY);
    Ctl(L"STATIC", L"Texture filtering (DXVK):", 0, 24, 265, 150, 20, 0);
    Ctl(L"COMBOBOX", L"", CMB, 180, 261, 220, 200, IDC_ANISO);
    for (auto s : kAnisoLabels) AddItem(IDC_ANISO, s);
    Ctl(L"BUTTON", L"Let DXVK write its own log file, WAR_d3d9.log (DXVK only)", CHK, 24, 294, 520, 20, IDC_DXVKLOG);

    Ctl(L"BUTTON", L"Problems", GRP, 12, 336, 540, 58, 0);
    Ctl(L"BUTTON", L"Write WARVR.log (send it along with bug reports)", CHK, 24, 360, 350, 20, IDC_LOG);
    Ctl(L"BUTTON", L"Open log folder", BTN, 392, 356, 150, 26, IDC_OPENLOG);

    Ctl(L"BUTTON", L"Your own server (optional)", GRP, 12, 402, 540, 152, 0);
    Ctl(L"STATIC", L"Server folder (the one with WorldServer.exe, usually bin\\Release):", 0, 24, 424, 520, 20, 0);
    Ctl(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 24, 446, 410, 24, IDC_SERVERDIR);
    Ctl(L"BUTTON", L"Browse...", BTN, 442, 445, 100, 26, IDC_BROWSE);
    Ctl(L"STATIC", L"Time of day:", 0, 24, 485, 110, 20, 0);
    Ctl(L"COMBOBOX", L"", CMB, 140, 481, 260, 300, IDC_TIME);
    AddItem(IDC_TIME, L"Server clock (UTC)");
    for (int h = 1; h < 24; ++h)
        AddItem(IDC_TIME, L"+" + std::to_wstring(h) + (h == 1 ? L" hour" : L" hours") + (h == 12 ? L" (day and night swapped)" : L""));
    Ctl(L"STATIC", L"", 0, 24, 512, 520, 36, IDC_SERVERSTATUS);

    Ctl(L"STATIC", L"In the game, hold Ctrl+Shift and tap: Numpad * VR on/off, H HUD style, L laser, Numpad / recenter, "
                   L"Numpad - level view, M mouse cursor, PageUp/PageDown world scale.", 0, 12, 562, 540, 34, IDC_HINT);
    Ctl(L"BUTTON", L"Open WARVR.ini (all options)", BTN, 12, 604, 190, 30, IDC_OPENINI);
    Ctl(L"BUTTON", L"Save", BS_DEFPUSHBUTTON | WS_TABSTOP, 352, 604, 95, 30, IDOK);
    Ctl(L"BUTTON", L"Close", BTN, 457, 604, 95, 30, IDCANCEL);
    Ctl(L"STATIC", L"", 0, 12, 644, 540, 36, IDC_STATUS);
}

static bool ConfirmClose()
{
    if (!g.dirty) return true;
    const int r = MessageBoxW(g_wnd, L"Save your changes?", L"WARVR settings", MB_YESNOCANCEL | MB_ICONQUESTION);
    if (r == IDCANCEL) return false;
    return r == IDNO || Save();
}

static LRESULT CALLBACK WndProc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_COMMAND: {
        const int id = LOWORD(wp), code = HIWORD(wp);
        if (id == IDOK) { Save(); return 0; }
        if (id == IDCANCEL) { if (ConfirmClose()) DestroyWindow(w); return 0; }
        if (id == IDC_BROWSE) { BrowseServer(); return 0; }
        if (id == IDC_OPENINI) { OpenIni(); return 0; }
        if (id == IDC_OPENLOG) {
            wchar_t tmp[MAX_PATH] = L"";
            GetTempPathW(MAX_PATH, tmp);
            ShellExecuteW(w, L"open", tmp, nullptr, nullptr, SW_SHOWNORMAL);
            return 0;
        }
        if (id == IDC_SERVERDIR && code == EN_KILLFOCUS) { if (GetText(IDC_SERVERDIR) != g.serverDirShown) RefreshServer(); return 0; }
        if ((code == BN_CLICKED && id != IDC_OPENLOG) || code == CBN_SELCHANGE || (id == IDC_SERVERDIR && code == EN_CHANGE)) g.dirty = true;
        return 0;
    }
    case WM_CLOSE:
        if (ConfirmClose()) DestroyWindow(w);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(w, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR, int show)
{
    SetProcessDPIAware();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    HDC dc = GetDC(nullptr);
    g_dpi = GetDeviceCaps(dc, LOGPIXELSY);
    ReleaseDC(nullptr, dc);
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof(ncm);
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    g_font = CreateFontIndirectW(&ncm.lfMessageFont);

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wc.lpszClassName = L"WARVRConfig";
    RegisterClassW(&wc);

    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT r{ 0, 0, S(564), S(688) };
    AdjustWindowRect(&r, style, FALSE);
    g_wnd = CreateWindowExW(WS_EX_CONTROLPARENT, wc.lpszClassName, L"WARVR settings", style, CW_USEDEFAULT, CW_USEDEFAULT,
                            r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, nullptr);
    Build();
    Load();
    ShowWindow(g_wnd, show);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (IsDialogMessageW(g_wnd, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    CoUninitialize();
    return 0;
}
