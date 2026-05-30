// settings_dialog.cpp — Modal settings dialog implementation
// T027: FPS radio buttons, directory browser, OK/Cancel, persists via AppSettings

#include "app/settings_dialog.h"
#include "app/app_icon.h"
#include "app/ui_theme.h"
#include "utils/logging.h"

#include <windows.h>
#include <dwmapi.h>
#include <shlobj.h>     // SHBrowseForFolder
#include <commctrl.h>
#include <string>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "dwmapi.lib")

namespace sr {

// ============================================================================
// Control IDs
// ============================================================================
constexpr int IDC_RADIO_30   = 2001;
constexpr int IDC_RADIO_60   = 2002;
constexpr int IDC_EDIT_DIR   = 2003;
constexpr int IDC_BTN_BROWSE = 2004;
constexpr int IDC_BTN_OK     = 2005;
constexpr int IDC_BTN_CANCEL = 2006;
constexpr int IDC_LBL_FPS    = 2007;
constexpr int IDC_LBL_DIR    = 2008;
constexpr int IDC_CHK_CAMERA = 2009;
constexpr int IDC_CHK_HQ     = 2010;

// ============================================================================
// Dialog internal state (per-instance, allocated on the stack of the caller)
// ============================================================================
struct DlgState {
    AppSettings* settings = nullptr;   // in/out
    bool         ok       = false;     // true when user pressed OK
    HWND         hwnd     = nullptr;
    HBRUSH       bg_brush = nullptr;
    HBRUSH       edit_brush = nullptr;
    HFONT        ui_font = nullptr;
    HFONT        bold_font = nullptr;
};

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

static void EnableDarkTitleBar(HWND hwnd) {
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
}

static BOOL CALLBACK ApplyDialogFont(HWND child, LPARAM param) {
    auto* state = reinterpret_cast<DlgState*>(param);
    if (state && state->ui_font) {
        SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(state->ui_font), TRUE);
    }
    return TRUE;
}

static void DestroyDialogTheme(DlgState* state) {
    if (!state) return;
    if (state->bg_brush) {
        DeleteObject(state->bg_brush);
        state->bg_brush = nullptr;
    }
    if (state->edit_brush) {
        DeleteObject(state->edit_brush);
        state->edit_brush = nullptr;
    }
    if (state->ui_font) {
        DeleteObject(state->ui_font);
        state->ui_font = nullptr;
    }
    if (state->bold_font) {
        DeleteObject(state->bold_font);
        state->bold_font = nullptr;
    }
}

static HMENU ControlId(int id) noexcept {
    return reinterpret_cast<HMENU>(static_cast<INT_PTR>(id));
}

// ============================================================================
// Folder browser (SHBrowseForFolder)
// ============================================================================
static std::wstring BrowseForFolder(HWND parent, const std::wstring& initial) {
    wchar_t display_name[MAX_PATH]{};

    BROWSEINFOW bi{};
    bi.hwndOwner    = parent;
    bi.lpszTitle    = L"Select output folder for recordings:";
    bi.ulFlags      = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_USENEWUI;
    bi.pszDisplayName = display_name;

    // Set initial selection via callback
    std::wstring init = initial;
    bi.lParam = reinterpret_cast<LPARAM>(init.c_str());
    bi.lpfn   = [](HWND hwnd, UINT msg, LPARAM lp, LPARAM data) -> int {
        (void)lp;
        if (msg == BFFM_INITIALIZED && data) {
            SendMessageW(hwnd, BFFM_SETSELECTIONW, TRUE, data);
        }
        return 0;
    };

    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return {};

    wchar_t path[MAX_PATH]{};
    SHGetPathFromIDListW(pidl, path);
    CoTaskMemFree(pidl);
    return path;
}

// ============================================================================
// Dialog WndProc
// ============================================================================
static LRESULT CALLBACK SettingsDlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    DlgState* state = reinterpret_cast<DlgState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {

    case WM_CREATE: {
        auto* cs    = reinterpret_cast<CREATESTRUCTW*>(lp);
        state = reinterpret_cast<DlgState*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        state->hwnd = hwnd;
        state->bg_brush = CreateSolidBrush(ui::kWindowBackground);
        state->edit_brush = CreateSolidBrush(ui::kSurface);
        state->ui_font = CreateFontW(
            -17, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        state->bold_font = CreateFontW(
            -18, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        EnableDarkTitleBar(hwnd);

        // ---------- FPS group ----------
        int y = 16;
        CreateWindowW(L"BUTTON", L"Video Quality",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            14, y, 690, 82, hwnd, ControlId(IDC_LBL_FPS), nullptr, nullptr);

        CreateWindowW(L"BUTTON", L"30 fps  (4 Mbps \u2014 recommended for battery)",
            WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | WS_GROUP,
            28, y + 28, 650, 22, hwnd, ControlId(IDC_RADIO_30), nullptr, nullptr);

        CreateWindowW(L"BUTTON", L"60 fps  (6 Mbps \u2014 smoother, more CPU/disk)",
            WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
            28, y + 54, 650, 22, hwnd, ControlId(IDC_RADIO_60), nullptr, nullptr);

        // Set current fps selection
        if (state->settings->fps == 60) {
            CheckRadioButton(hwnd, IDC_RADIO_30, IDC_RADIO_60, IDC_RADIO_60);
        } else {
            CheckRadioButton(hwnd, IDC_RADIO_30, IDC_RADIO_60, IDC_RADIO_30);
        }

        // ---------- High Quality checkbox ----------
        y += 96;
        CreateWindowW(L"BUTTON", L"High Quality (8/10 Mbps \u2014 larger files, sharper video)",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            18, y, 680, 22, hwnd, ControlId(IDC_CHK_HQ), nullptr, nullptr);
        CheckDlgButton(hwnd, IDC_CHK_HQ,
            state->settings->high_quality ? BST_CHECKED : BST_UNCHECKED);

        // ---------- Output directory group ----------
        y += 34;
        CreateWindowW(L"BUTTON", L"Output Directory",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            14, y, 690, 72, hwnd, ControlId(IDC_LBL_DIR), nullptr, nullptr);

        CreateWindowW(L"EDIT", state->settings->output_dir.c_str(),
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
            28, y + 30, 420, 24, hwnd, ControlId(IDC_EDIT_DIR), nullptr, nullptr);

        CreateWindowW(L"BUTTON", L"Browse\u2026",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            464, y + 28, 110, 28, hwnd, ControlId(IDC_BTN_BROWSE), nullptr, nullptr);

        // ---------- hint ----------
        y += 82;
        CreateWindowW(L"STATIC", L"Leave directory blank to use default (Videos\\Recordings)",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            18, y, 680, 20, hwnd, nullptr, nullptr, nullptr);

        y += 32;
        CreateWindowW(L"BUTTON", L"Enable floating camera overlay (always on top)",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            18, y, 680, 22, hwnd, ControlId(IDC_CHK_CAMERA), nullptr, nullptr);
        CheckDlgButton(hwnd, IDC_CHK_CAMERA,
            state->settings->camera_overlay_enabled ? BST_CHECKED : BST_UNCHECKED);

        // ---------- OK / Cancel ----------
        y += 42;
        CreateWindowW(L"BUTTON", L"OK",
            WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
            350, y, 88, 30, hwnd, ControlId(IDC_BTN_OK), nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"Cancel",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            450, y, 88, 30, hwnd, ControlId(IDC_BTN_CANCEL), nullptr, nullptr);

        EnumChildWindows(hwnd, ApplyDialogFont, reinterpret_cast<LPARAM>(state));
        if (state->bold_font) {
            SendDlgItemMessageW(hwnd, IDC_LBL_FPS, WM_SETFONT,
                                reinterpret_cast<WPARAM>(state->bold_font), TRUE);
            SendDlgItemMessageW(hwnd, IDC_LBL_DIR, WM_SETFONT,
                                reinterpret_cast<WPARAM>(state->bold_font), TRUE);
        }

        break;
    }

    case WM_ERASEBKGND: {
        HDC hdc = reinterpret_cast<HDC>(wp);
        RECT rc{};
        GetClientRect(hwnd, &rc);
        HBRUSH brush = state && state->bg_brush ? state->bg_brush : GetSysColorBrush(COLOR_WINDOW);
        FillRect(hdc, &rc, brush);
        return 1;
    }

    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
        HDC hdc = reinterpret_cast<HDC>(wp);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, ui::kText);
        return reinterpret_cast<INT_PTR>(state && state->bg_brush
            ? state->bg_brush
            : GetSysColorBrush(COLOR_WINDOW));
    }

    case WM_CTLCOLOREDIT: {
        HDC hdc = reinterpret_cast<HDC>(wp);
        SetBkColor(hdc, ui::kSurface);
        SetTextColor(hdc, ui::kText);
        return reinterpret_cast<INT_PTR>(state && state->edit_brush
            ? state->edit_brush
            : GetSysColorBrush(COLOR_WINDOW));
    }

    case WM_COMMAND:
        switch (LOWORD(wp)) {

        case IDC_BTN_BROWSE: {
            wchar_t cur_dir[MAX_PATH]{};
            GetDlgItemTextW(hwnd, IDC_EDIT_DIR, cur_dir, MAX_PATH);
            std::wstring chosen = BrowseForFolder(hwnd, cur_dir);
            if (!chosen.empty()) {
                SetDlgItemTextW(hwnd, IDC_EDIT_DIR, chosen.c_str());
            }
            break;
        }

        case IDC_BTN_OK: {
            if (state) {
                // Read fps
                state->settings->fps = IsDlgButtonChecked(hwnd, IDC_RADIO_60)
                    ? 60u : 30u;

                state->settings->set_high_quality(
                    IsDlgButtonChecked(hwnd, IDC_CHK_HQ) == BST_CHECKED);

                // Read output dir
                wchar_t dir[MAX_PATH]{};
                GetDlgItemTextW(hwnd, IDC_EDIT_DIR, dir, MAX_PATH);
                state->settings->output_dir = dir;

                state->settings->camera_overlay_enabled =
                    (IsDlgButtonChecked(hwnd, IDC_CHK_CAMERA) == BST_CHECKED);

                state->ok = true;
            }
            DestroyWindow(hwnd);
            break;
        }

        case IDC_BTN_CANCEL:
            DestroyWindow(hwnd);
            break;
        }
        break;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        break;

    case WM_DESTROY:
        DestroyDialogTheme(state);
        PostQuitMessage(0);   // Ends our local modal loop
        break;

    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    return 0;
}

// ============================================================================
// Public API
// ============================================================================
bool ShowSettingsDialog(HWND parent, AppSettings& settings) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize        = sizeof(wc);
        wc.style         = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc   = SettingsDlgProc;
        wc.hInstance     = GetModuleHandleW(nullptr);
        wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = L"SRSettingsDialog";
        wc.hIcon         = load_app_icon(wc.hInstance);
        wc.hIconSm       = load_app_icon(wc.hInstance,
                                         GetSystemMetrics(SM_CXSMICON),
                                         GetSystemMetrics(SM_CYSMICON));
        if (!RegisterClassExW(&wc)) {
            SR_LOG_ERROR(L"RegisterClassEx for SettingsDialog failed");
            return false;
        }
        registered = true;
    }

    DlgState state{ &settings, false, nullptr };

    // Compute centered position relative to parent
    RECT parent_rect{};
    if (parent) GetWindowRect(parent, &parent_rect);
    int dlg_w = 728, dlg_h = 620;
    int x = parent_rect.left + (parent_rect.right  - parent_rect.left - dlg_w) / 2;
    int y = parent_rect.top  + (parent_rect.bottom - parent_rect.top  - dlg_h) / 2;

    HWND dlg = CreateWindowExW(
        WS_EX_DLGMODALFRAME | WS_EX_APPWINDOW,
        L"SRSettingsDialog",
        L"Recording Settings",
        WS_CAPTION | WS_SYSMENU | WS_VISIBLE | WS_POPUP,
        x, y, dlg_w, dlg_h,
        parent, nullptr,
        GetModuleHandleW(nullptr),
        &state
    );
    if (!dlg) {
        SR_LOG_ERROR(L"CreateWindowEx for SettingsDialog failed: %u", GetLastError());
        return false;
    }

    // Disable parent to create modal behaviour
    if (parent) EnableWindow(parent, FALSE);

    // Run local message loop until dialog is destroyed (PostQuitMessage(0))
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(dlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    // Re-enable parent and bring it to front
    if (parent) {
        EnableWindow(parent, TRUE);
        SetForegroundWindow(parent);
    }

    return state.ok;
}

} // namespace sr
