#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

#include "audio_capture.h"
#include "transfer.h"

#include <chrono>
#include <fstream>
#include <mutex>
#include <string>

namespace {

constexpr UINT kTrayIconId = 1001;
constexpr UINT kTrayCallbackMessage = WM_APP + 1;
constexpr UINT kMenuExitId = 2001;
constexpr UINT kAppIconResourceId = 101;
constexpr wchar_t kLogFileName[] = L"calliope_desktop.log";

void logLine(const std::string& message) {
    static std::mutex logMutex;
    std::lock_guard<std::mutex> lock(logMutex);

    std::ofstream out(kLogFileName, std::ios::app);
    if (!out.is_open()) {
        return;
    }

    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    tm localTime{};
    localtime_s(&localTime, &time);

    char buffer[32]{};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &localTime);
    out << "[" << buffer << "] " << message << '\n';
}

class AppController {
public:
    AppController()
        : transfer_([](const std::string& message) { logLine(message); }),
          capture_(
              [](const std::string& message) { logLine(message); },
              [this](AudioFrame frame) { transfer_.sendAudioFrame(frame); }) {}

    bool start() {
        if (!transfer_.start()) {
            return false;
        }
        return capture_.start();
    }

    void stop() {
        capture_.stop();
        transfer_.stop();
    }

private:
    TransferServer transfer_;
    AudioCaptureService capture_;
};

AppController g_app;
NOTIFYICONDATAW g_trayIcon{};
HICON g_appIcon = nullptr;

void removeTrayIcon() {
    Shell_NotifyIconW(NIM_DELETE, &g_trayIcon);
}

bool addTrayIcon(HWND hwnd) {
    ZeroMemory(&g_trayIcon, sizeof(g_trayIcon));
    g_trayIcon.cbSize = sizeof(g_trayIcon);
    g_trayIcon.hWnd = hwnd;
    g_trayIcon.uID = kTrayIconId;
    g_trayIcon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_trayIcon.uCallbackMessage = kTrayCallbackMessage;
    g_trayIcon.hIcon = g_appIcon ? g_appIcon : LoadIcon(nullptr, IDI_APPLICATION);
    wcscpy_s(g_trayIcon.szTip, L"Calliope Audio Stream");
    return Shell_NotifyIconW(NIM_ADD, &g_trayIcon) == TRUE;
}

void showContextMenu(HWND hwnd) {
    HMENU menu = CreatePopupMenu();
    if (!menu) {
        return;
    }
    InsertMenuW(menu, -1, MF_BYPOSITION | MF_STRING, kMenuExitId, L"Cikis");

    POINT pt{};
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_BOTTOMALIGN | TPM_LEFTALIGN, pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(menu);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE:
            addTrayIcon(hwnd);
            if (!g_app.start()) {
                DestroyWindow(hwnd);
            }
            return 0;
        case WM_COMMAND:
            if (LOWORD(wParam) == kMenuExitId) {
                DestroyWindow(hwnd);
            }
            return 0;
        case kTrayCallbackMessage:
            if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU) {
                showContextMenu(hwnd);
            }
            return 0;
        case WM_DESTROY:
            g_app.stop();
            removeTrayIcon();
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    logLine("Application starting");
    g_appIcon = static_cast<HICON>(
        LoadImageW(instance, MAKEINTRESOURCEW(kAppIconResourceId), IMAGE_ICON, 256, 256, LR_DEFAULTCOLOR));
    if (!g_appIcon) {
        g_appIcon = LoadIcon(nullptr, IDI_APPLICATION);
    }

    const wchar_t* className = L"CalliopeTrayClass";
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.lpszClassName = className;
    wc.hIcon = g_appIcon ? g_appIcon : LoadIcon(nullptr, IDI_APPLICATION);
    wc.hIconSm = g_appIcon ? g_appIcon : LoadIcon(nullptr, IDI_APPLICATION);

    if (!RegisterClassExW(&wc)) {
        logLine("RegisterClassW failed");
        return 1;
    }

    HWND hwnd = CreateWindowExW(
        0,
        className,
        L"Calliope",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        300,
        200,
        nullptr,
        nullptr,
        instance,
        nullptr);

    if (!hwnd) {
        logLine("CreateWindowExW failed");
        return 1;
    }

    if (g_appIcon) {
        SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(g_appIcon));
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(g_appIcon));
    }

    ShowWindow(hwnd, SW_HIDE);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_appIcon) {
        DestroyIcon(g_appIcon);
        g_appIcon = nullptr;
    }
    return static_cast<int>(msg.wParam);
}
