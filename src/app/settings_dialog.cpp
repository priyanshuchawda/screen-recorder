// settings_dialog.cpp — Modal settings dialog implementation
// T027: FPS radio buttons, directory browser, OK/Cancel, persists via AppSettings

#include "app/settings_dialog.h"
#include "utils/logging.h"

#include <windows.h>
#include <shlobj.h>     // SHBrowseForFolder
#include <commctrl.h>
#include <string>

#pragma comment(lib, "comctl32.lib")

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

// ============================================================================
// Dialog internal state (per-instance, allocated on the stack of the caller)
// ============================================================================
struct DlgState {
    AppSettings* settings = nullptr;   // in/out
    bool         ok       = false;     // true when user pressed OK
    HWND         hwnd     = nullptr;
};

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

        // ---------- FPS group ----------
        int y = 14;
        CreateWindowW(L"BUTTON", L"Video Quality",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            10, y, 330, 70, hwnd, (HMENU)IDC_LBL_FPS, nullptr, nullptr);

        CreateWindowW(L"BUTTON", L"30 fps  (8 Mbps \u2014 recommended for battery)",
            WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | WS_GROUP,
            20, y + 22, 300, 20, hwnd, (HMENU)IDC_RADIO_30, nullptr, nullptr);

        CreateWindowW(L"BUTTON", L"60 fps  (14 Mbps \u2014 smoother, more CPU/disk)",
            WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
            20, y + 44, 300, 20, hwnd, (HMENU)IDC_RADIO_60, nullptr, nullptr);

        // Set current fps selection
        if (state->settings->fps == 60) {
            CheckRadioButton(hwnd, IDC_RADIO_30, IDC_RADIO_60, IDC_RADIO_60);
        } else {
            CheckRadioButton(hwnd, IDC_RADIO_30, IDC_RADIO_60, IDC_RADIO_30);
        }

        // ---------- Output directory group ----------
        y += 82;
        CreateWindowW(L"BUTTON", L"Output Directory",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            10, y, 330, 62, hwnd, (HMENU)IDC_LBL_DIR, nullptr, nullptr);

        CreateWindowW(L"EDIT", state->settings->output_dir.c_str(),
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
            20, y + 22, 235, 22, hwnd, (HMENU)IDC_EDIT_DIR, nullptr, nullptr);

        CreateWindowW(L"BUTTON", L"Browse\u2026",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            264, y + 21, 70, 24, hwnd, (HMENU)IDC_BTN_BROWSE, nullptr, nullptr);

        // ---------- hint ----------
        y += 66;
        CreateWindowW(L"STATIC", L"Leave directory blank to use default (Videos\\Recordings)",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            12, y, 340, 18, hwnd, nullptr, nullptr, nullptr);

        y += 24;
        CreateWindowW(L"BUTTON", L"Enable floating camera overlay (always on top)",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            14, y, 330, 20, hwnd, (HMENU)IDC_CHK_CAMERA, nullptr, nullptr);
        CheckDlgButton(hwnd, IDC_CHK_CAMERA,
            state->settings->camera_overlay_enabled ? BST_CHECKED : BST_UNCHECKED);

        // ---------- OK / Cancel ----------
        y += 30;
        CreateWindowW(L"BUTTON", L"OK",
            WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
            160, y, 80, 28, hwnd, (HMENU)IDC_BTN_OK, nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"Cancel",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            252, y, 80, 28, hwnd, (HMENU)IDC_BTN_CANCEL, nullptr, nullptr);

        break;
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
                state->settings->bitrate_bps = (state->settings->fps == 60)
                    ? 14'000'000u : 8'000'000u;

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
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"SRSettingsDialog";
        wc.hIcon         = LoadIconW(nullptr, IDI_APPLICATION);
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
    int dlg_w = 358, dlg_h = 312;
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
