#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

#include "audio_capture.h"
#include "transfer.h"

#include <chrono>
#include <atomic>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

namespace {

constexpr UINT kTrayIconId = 1001;
constexpr UINT kTrayCallbackMessage = WM_APP + 1;
constexpr UINT kMenuExitId = 2001;
constexpr UINT kAppIconResourceId = 101;
constexpr wchar_t kLogFileName[] = L"calliope_desktop.log";
constexpr wchar_t kAdbReverseArgs[] = L"reverse tcp:4010 tcp:4010";

bool fileExists(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::wstring joinPath(const std::wstring& left, const std::wstring& right) {
    if (left.empty()) {
        return right;
    }
    if (left.back() == L'\\' || left.back() == L'/') {
        return left + right;
    }
    return left + L'\\' + right;
}

std::wstring getExecutableDirectory() {
    wchar_t buffer[MAX_PATH]{};
    const DWORD len = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return L"";
    }

    std::wstring path(buffer, len);
    const size_t pos = path.find_last_of(L"\\/");
    if (pos == std::wstring::npos) {
        return L"";
    }
    return path.substr(0, pos);
}

std::wstring getParentDirectory(const std::wstring& path) {
    if (path.empty()) {
        return L"";
    }

    const size_t pos = path.find_last_of(L"\\/");
    if (pos == std::wstring::npos) {
        return L"";
    }
    return path.substr(0, pos);
}

std::wstring findAdbExecutable() {
    const std::wstring exeDir = getExecutableDirectory();
    const std::wstring exeParent = getParentDirectory(exeDir);
    const std::wstring exeGrandParent = getParentDirectory(exeParent);
    std::wstring roots[] = {
        exeDir.empty() ? L"" : joinPath(exeDir, L"tools"),
        exeDir.empty() ? L"" : exeDir,
        exeParent.empty() ? L"" : joinPath(exeParent, L"tools"),
        exeParent.empty() ? L"" : exeParent,
        exeGrandParent.empty() ? L"" : joinPath(exeGrandParent, L"tools"),
        exeGrandParent.empty() ? L"" : exeGrandParent,
        [] {
            wchar_t buffer[MAX_PATH]{};
            const DWORD len = GetEnvironmentVariableW(L"ANDROID_SDK_ROOT", buffer, MAX_PATH);
            return len > 0 && len < MAX_PATH ? std::wstring(buffer, len) : std::wstring();
        }(),
        [] {
            wchar_t buffer[MAX_PATH]{};
            const DWORD len = GetEnvironmentVariableW(L"ANDROID_HOME", buffer, MAX_PATH);
            return len > 0 && len < MAX_PATH ? std::wstring(buffer, len) : std::wstring();
        }(),
        [] {
            wchar_t buffer[MAX_PATH]{};
            const DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, MAX_PATH);
            return len > 0 && len < MAX_PATH ? joinPath(std::wstring(buffer, len), L"Android\\Sdk") : std::wstring();
        }(),
        L"C:\\Program Files\\Android\\Android Studio",
        L"C:\\Program Files (x86)\\Android\\Android Studio",
    };

    for (const auto& root : roots) {
        if (root.empty()) {
            continue;
        }

        const std::wstring candidate = joinPath(joinPath(root, L"platform-tools"), L"adb.exe");
        if (fileExists(candidate)) {
            return candidate;
        }
    }

    return L"";
}

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

bool tryRunAdbReverse() {
    const std::wstring adbPath = findAdbExecutable();
    STARTUPINFOW startupInfo{};
    startupInfo.cb = sizeof(startupInfo);
    startupInfo.dwFlags = STARTF_USESHOWWINDOW;
    startupInfo.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION processInfo{};
    std::wstring commandLine = adbPath.empty() ? L"adb reverse tcp:4010 tcp:4010" : kAdbReverseArgs;
    const wchar_t* applicationName = adbPath.empty() ? nullptr : adbPath.c_str();

    const BOOL created = CreateProcessW(
        applicationName,
        commandLine.data(),
        nullptr,
        nullptr,
        FALSE,
        CREATE_NO_WINDOW,
        nullptr,
        nullptr,
        &startupInfo,
        &processInfo);

    if (!created) {
        logLine("adb reverse not started: adb not found or failed to launch");
        return false;
    }

    WaitForSingleObject(processInfo.hProcess, INFINITE);

    DWORD exitCode = 1;
    GetExitCodeProcess(processInfo.hProcess, &exitCode);
    CloseHandle(processInfo.hThread);
    CloseHandle(processInfo.hProcess);

    if (exitCode != 0) {
        logLine("adb reverse exited with code " + std::to_string(exitCode));
        return false;
    }

    logLine("adb reverse tcp:4010 tcp:4010 completed");
    return true;
}

class AppController {
public:
    AppController()
        : transfer_([](const std::string& message) { logLine(message); }),
          capture_(
              [](const std::string& message) { logLine(message); },
              [this](AudioFrame frame) { transfer_.sendAudioFrame(frame); }) {}

    ~AppController() {
        stop();
    }

    bool start() {
        if (!transfer_.start()) {
            return false;
        }
        stopRequested_.store(false);
        adbReverseThread_ = std::thread([this]() {
            for (int attempt = 1; attempt <= 30 && !stopRequested_.load(); ++attempt) {
                if (tryRunAdbReverse()) {
                    return;
                }
                if (stopRequested_.load()) {
                    return;
                }
                std::this_thread::sleep_for(std::chrono::seconds(2));
            }
        });
        return capture_.start();
    }

    void stop() {
        stopRequested_.store(true);
        if (adbReverseThread_.joinable()) {
            adbReverseThread_.join();
        }
        capture_.stop();
        transfer_.stop();
    }

private:
    TransferServer transfer_;
    AudioCaptureService capture_;
    std::atomic<bool> stopRequested_{false};
    std::thread adbReverseThread_;
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
