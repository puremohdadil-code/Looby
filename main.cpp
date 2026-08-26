// 7 time to start
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif   
    
#include <windows.h>
#include <commctrl.h>
#include <mmsystem.h>
#include <shellapi.h>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "winmm.lib")

namespace {

constexpr int kMaxRecordMs = 2 * 60 * 1000;
constexpr int kRecordDelayMs = 3 * 1000;
constexpr int kMaxPlaybackMs = 60 * 60 * 1000;

constexpr int kHotkeyPlayback = 100;
constexpr int kHotkeyRecord = 101;
constexpr int kHotkeyStopRecord = 102;

constexpr int kBtnRecord = 201;
constexpr int kBtnPlay = 202;
constexpr int kBtnClear = 203;
constexpr int kBtnExit = 204;
constexpr int kTimerUi = 301;

enum class EventType : std::uint8_t {
    Keyboard,
    MouseMove,
    MouseButton,
    MouseWheel
};

struct MacroEvent {
    DWORD atMs{};
    EventType type{EventType::Keyboard};

    DWORD vkCode{};
    DWORD scanCode{};
    DWORD keyFlags{};

    LONG mouseDx{};
    LONG mouseDy{};
    DWORD mouseFlags{};
    DWORD mouseData{};
};

HWND g_hwnd{};
HWND g_recordButton{};
HWND g_playButton{};
HWND g_clearButton{};
HWND g_exitButton{};

HHOOK g_keyboardHook{};
HHOOK g_mouseHook{};
std::thread g_recordThread;
std::thread g_playThread;

std::vector<MacroEvent> g_events;
CRITICAL_SECTION g_eventsLock;

std::atomic_bool g_recording{false};
std::atomic_bool g_waitingToRecord{false};
std::atomic_bool g_playing{false};
std::atomic_bool g_stopPlayback{false};
std::atomic_bool g_stopRecording{false};
std::atomic_bool g_inPlayback{false};

std::chrono::steady_clock::time_point g_recordStart{};
std::chrono::steady_clock::time_point g_modeStart{};
std::wstring g_status = L"Ready";

COLORREF kBg = RGB(18, 22, 28);
COLORREF kPanel = RGB(30, 36, 45);
COLORREF kPanel2 = RGB(40, 48, 59);
COLORREF kAccent = RGB(74, 163, 255);
COLORREF kGreen = RGB(88, 204, 132);
COLORREF kRed = RGB(255, 96, 96);
COLORREF kText = RGB(238, 243, 248);
COLORREF kMuted = RGB(162, 174, 188);

DWORD ElapsedMs(std::chrono::steady_clock::time_point start) {
    return static_cast<DWORD>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start)
            .count());
}

void SetStatus(const std::wstring& text) {
    g_status = text;
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

std::wstring CountText() {
    EnterCriticalSection(&g_eventsLock);
    size_t count = g_events.size();
    LeaveCriticalSection(&g_eventsLock);
    return std::to_wstring(count) + L" events saved";
}

void ClearEvents() {
    EnterCriticalSection(&g_eventsLock);
    g_events.clear();
    LeaveCriticalSection(&g_eventsLock);
}

void PushEvent(const MacroEvent& event) {
    EnterCriticalSection(&g_eventsLock);
    g_events.push_back(event);
    LeaveCriticalSection(&g_eventsLock);
}

void PushMouseMoveEvent(DWORD atMs, LONG dx, LONG dy) {
    if (dx == 0 && dy == 0) {
        return;
    }

    EnterCriticalSection(&g_eventsLock);

    // Raw mice can report faster than the millisecond timer used by this app.
    // Merge movement packets that land in the same millisecond so playback stays smooth
    // without creating an unnecessarily huge recording.
    if (!g_events.empty()) {
        auto& last = g_events.back();
        if (last.type == EventType::MouseMove && last.atMs == atMs) {
            last.mouseDx += dx;
            last.mouseDy += dy;
            LeaveCriticalSection(&g_eventsLock);
            return;
        }
    }

    MacroEvent event{};
    event.atMs = atMs;
    event.type = EventType::MouseMove;
    event.mouseDx = dx;
    event.mouseDy = dy;
    event.mouseFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_MOVE_NOCOALESCE;
    g_events.push_back(event);

    LeaveCriticalSection(&g_eventsLock);
}

std::vector<MacroEvent> CopyEvents() {
    EnterCriticalSection(&g_eventsLock);
    auto copy = g_events;
    LeaveCriticalSection(&g_eventsLock);

    std::stable_sort(copy.begin(), copy.end(), [](const MacroEvent& a, const MacroEvent& b) {
        return a.atMs < b.atMs;
    });
    return copy;
}

DWORD MacroDurationMs(const std::vector<MacroEvent>& events) {
    if (events.empty()) {
        return 0;
    }

    DWORD duration = events.back().atMs;
    for (const auto& event : events) {
        duration = std::max(duration, event.atMs);
    }
    return duration > 0 ? duration : 1;
}

bool IsKeyboardHotkey(DWORD vk, bool keyDown) {
    if (!keyDown) {
        return false;
    }

    const bool ctrlDown = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool shiftDown = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    return (ctrlDown && vk == '0') || (shiftDown && (vk == '9' || vk == '2'));
}

LRESULT CALLBACK KeyboardProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION && g_recording && !g_inPlayback) {
        const auto* data = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
        const bool injected = (data->flags & LLKHF_INJECTED) != 0;
        const bool keyDown = wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN;

        if (!injected && !IsKeyboardHotkey(data->vkCode, keyDown)) {
            MacroEvent event{};
            event.atMs = ElapsedMs(g_recordStart);
            event.type = EventType::Keyboard;
            event.vkCode = data->vkCode;
            event.scanCode = data->scanCode;
            event.keyFlags = data->flags;
            PushEvent(event);
        }
    }

    return CallNextHookEx(g_keyboardHook, code, wParam, lParam);
}

void PushMouseButtonEvent(DWORD atMs, DWORD flags, DWORD data = 0) {
    MacroEvent event{};
    event.atMs = atMs;
    event.type = EventType::MouseButton;
    event.mouseFlags = flags;
    event.mouseData = data;
    PushEvent(event);
}

void PushMouseWheelEvent(DWORD atMs, DWORD flags, LONG wheelDelta) {
    MacroEvent event{};
    event.atMs = atMs;
    event.type = EventType::MouseWheel;
    event.mouseFlags = flags;
    event.mouseData = static_cast<DWORD>(wheelDelta);
    PushEvent(event);
}

LRESULT CALLBACK MouseProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION && g_recording && !g_inPlayback) {
        const auto* data = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);
        const bool injected = (data->flags & LLMHF_INJECTED) != 0;

        if (!injected) {
            const DWORD atMs = ElapsedMs(g_recordStart);

            switch (wParam) {
                case WM_LBUTTONDOWN:
                    PushMouseButtonEvent(atMs, MOUSEEVENTF_LEFTDOWN);
                    break;
                case WM_LBUTTONUP:
                    PushMouseButtonEvent(atMs, MOUSEEVENTF_LEFTUP);
                    break;
                case WM_RBUTTONDOWN:
                    PushMouseButtonEvent(atMs, MOUSEEVENTF_RIGHTDOWN);
                    break;
                case WM_RBUTTONUP:
                    PushMouseButtonEvent(atMs, MOUSEEVENTF_RIGHTUP);
                    break;
                case WM_MBUTTONDOWN:
                    PushMouseButtonEvent(atMs, MOUSEEVENTF_MIDDLEDOWN);
                    break;
                case WM_MBUTTONUP:
                    PushMouseButtonEvent(atMs, MOUSEEVENTF_MIDDLEUP);
                    break;
                case WM_XBUTTONDOWN:
                    PushMouseButtonEvent(atMs, MOUSEEVENTF_XDOWN, HIWORD(data->mouseData));
                    break;
                case WM_XBUTTONUP:
                    PushMouseButtonEvent(atMs, MOUSEEVENTF_XUP, HIWORD(data->mouseData));
                    break;
                case WM_MOUSEWHEEL:
                    PushMouseWheelEvent(
                        atMs, MOUSEEVENTF_WHEEL,
                        static_cast<LONG>(static_cast<SHORT>(HIWORD(data->mouseData))));
                    break;
                case WM_MOUSEHWHEEL:
                    PushMouseWheelEvent(
                        atMs, MOUSEEVENTF_HWHEEL,
                        static_cast<LONG>(static_cast<SHORT>(HIWORD(data->mouseData))));
                    break;
                default:
                    break;
            }
        }
    }

    return CallNextHookEx(g_mouseHook, code, wParam, lParam);
}

bool RegisterRawMouseInput(HWND hwnd) {
    RAWINPUTDEVICE device{};
    device.usUsagePage = 0x01;  // Generic desktop controls
    device.usUsage = 0x02;      // Mouse
    device.dwFlags = RIDEV_INPUTSINK;
    device.hwndTarget = hwnd;

    return RegisterRawInputDevices(&device, 1, sizeof(device)) == TRUE;
}

void UnregisterRawMouseInput() {
    RAWINPUTDEVICE device{};
    device.usUsagePage = 0x01;
    device.usUsage = 0x02;
    device.dwFlags = RIDEV_REMOVE;
    device.hwndTarget = nullptr;
    RegisterRawInputDevices(&device, 1, sizeof(device));
}

void HandleRawMouseInput(LPARAM lParam) {
    if (!g_recording || g_inPlayback) {
        return;
    }

    UINT size = 0;
    if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, nullptr, &size,
                        sizeof(RAWINPUTHEADER)) != 0 ||
        size == 0) {
        return;
    }

    std::vector<BYTE> buffer(size);
    const UINT bytesRead = GetRawInputData(
        reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, buffer.data(), &size,
        sizeof(RAWINPUTHEADER));

    if (bytesRead == static_cast<UINT>(-1) || bytesRead != size) {
        return;
    }

    const auto* raw = reinterpret_cast<const RAWINPUT*>(buffer.data());
    if (raw->header.dwType != RIM_TYPEMOUSE) {
        return;
    }

    const RAWMOUSE& mouse = raw->data.mouse;

    // 3D games normally use relative raw mouse movement. Absolute packets are
    // usually produced by tablets/touch devices, so they are intentionally skipped.
    if ((mouse.usFlags & MOUSE_MOVE_ABSOLUTE) == 0) {
        PushMouseMoveEvent(ElapsedMs(g_recordStart), mouse.lLastX, mouse.lLastY);
    }
}

void SendKeyEvent(const MacroEvent& event) {
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = static_cast<WORD>(event.vkCode);
    input.ki.wScan = static_cast<WORD>(event.scanCode);
    input.ki.dwFlags = 0;

    if (event.keyFlags & LLKHF_EXTENDED) {
        input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    }
    if (event.keyFlags & LLKHF_UP) {
        input.ki.dwFlags |= KEYEVENTF_KEYUP;
    }

    SendInput(1, &input, sizeof(INPUT));
}

void SendMouseEvent(const MacroEvent& event) {
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dx = event.mouseDx;
    input.mi.dy = event.mouseDy;
    input.mi.mouseData = event.mouseData;
    input.mi.dwFlags = event.mouseFlags;
    SendInput(1, &input, sizeof(INPUT));
}

void SendMacroEvent(const MacroEvent& event) {
    switch (event.type) {
        case EventType::Keyboard:
            SendKeyEvent(event);
            break;
        case EventType::MouseMove:
        case EventType::MouseButton:
        case EventType::MouseWheel:
            SendMouseEvent(event);
            break;
    }
}

void WaitUntil(std::chrono::steady_clock::time_point deadline) {
    while (!g_stopPlayback) {
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            break;
        }

        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
        if (remaining > 3) {
            Sleep(static_cast<DWORD>(remaining - 1));
        } else {
            SwitchToThread();
        }
    }
}

void StopPlayback() {
    if (g_playing) {
        g_stopPlayback = true;
    }
}

void PlaybackThread() {
    auto events = CopyEvents();
    if (events.empty()) {
        g_playing = false;
        SetStatus(L"No recording saved yet");
        return;
    }

    auto playbackStart = std::chrono::steady_clock::now();
    auto playbackEnd = playbackStart + std::chrono::milliseconds(kMaxPlaybackMs);
    DWORD macroDuration = MacroDurationMs(events);
    g_modeStart = playbackStart;
    SetStatus(L"Playing macro");

    timeBeginPeriod(1);

    while (!g_stopPlayback && std::chrono::steady_clock::now() < playbackEnd) {
        auto loopStart = std::chrono::steady_clock::now();

        for (const auto& event : events) {
            if (g_stopPlayback || std::chrono::steady_clock::now() >= playbackEnd) {
                break;
            }

            auto eventTime = loopStart + std::chrono::milliseconds(event.atMs);
            WaitUntil(eventTime < playbackEnd ? eventTime : playbackEnd);

            if (g_stopPlayback || std::chrono::steady_clock::now() >= playbackEnd) {
                break;
            }

            g_inPlayback = true;
            SendMacroEvent(event);
            g_inPlayback = false;
        }

        auto loopEnd = loopStart + std::chrono::milliseconds(macroDuration);
        WaitUntil(loopEnd < playbackEnd ? loopEnd : playbackEnd);
    }

    timeEndPeriod(1);
    g_inPlayback = false;
    g_playing = false;
    g_stopPlayback = false;
    SetStatus(ElapsedMs(playbackStart) >= kMaxPlaybackMs ? L"Playback stopped after one hour" : L"Playback stopped");
}

void StartPlayback() {
    if (g_waitingToRecord || g_recording) {
        SetStatus(L"Stop recording before playback");
        return;
    }
    if (g_playing) {
        StopPlayback();
        return;
    }

    if (g_playThread.joinable()) {
        g_playThread.join();
    }

    g_stopPlayback = false;
    g_playing = true;
    g_playThread = std::thread(PlaybackThread);
}

void RecordingThread() {
    g_waitingToRecord = true;
    g_stopRecording = false;
    ClearEvents();
    g_modeStart = std::chrono::steady_clock::now();
    SetStatus(L"Recording starts in 3 seconds");

    for (int i = 0; i < kRecordDelayMs / 100; ++i) {
        if (g_stopRecording) {
            g_waitingToRecord = false;
            SetStatus(L"Recording cancelled");
            return;
        }
        Sleep(100);
    }

    g_waitingToRecord = false;
    g_recordStart = std::chrono::steady_clock::now();
    g_modeStart = g_recordStart;
    g_recording = true;
    SetStatus(L"Recording for 2 minutes");

    while (!g_stopRecording && ElapsedMs(g_recordStart) < kMaxRecordMs) {
        Sleep(25);
    }

    g_recording = false;
    g_stopRecording = false;
    SetStatus(L"Recording saved");
}

void StartNewRecording() {
    if (g_playing) {
        StopPlayback();
    }

    if (g_recording || g_waitingToRecord) {
        g_stopRecording = true;
        return;
    }

    if (g_recordThread.joinable()) {
        g_recordThread.join();
    }

    g_recordThread = std::thread(RecordingThread);
}

void StopRecording() {
    if (g_recording || g_waitingToRecord) {
        g_stopRecording = true;
        SetStatus(L"Stopping recording");
    }
}

void JoinFinishedThreads() {
    if (!g_recording && !g_waitingToRecord && g_recordThread.joinable()) {
        g_recordThread.join();
    }
    if (!g_playing && g_playThread.joinable()) {
        g_playThread.join();
    }
}

void InstallHooks() {
    HINSTANCE module = GetModuleHandleW(nullptr);
    g_keyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardProc, module, 0);
    g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, MouseProc, module, 0);
}

void RemoveHooks() {
    if (g_keyboardHook) {
        UnhookWindowsHookEx(g_keyboardHook);
        g_keyboardHook = nullptr;
    }
    if (g_mouseHook) {
        UnhookWindowsHookEx(g_mouseHook);
        g_mouseHook = nullptr;
    }
}

void PaintRoundedRect(HDC dc, RECT rc, COLORREF color, int radius) {
    HBRUSH brush = CreateSolidBrush(color);
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    auto oldBrush = SelectObject(dc, brush);
    auto oldPen = SelectObject(dc, pen);
    RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, radius, radius);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(brush);
    DeleteObject(pen);
}

void DrawTextLine(HDC dc, const std::wstring& text, RECT rc, int size, COLORREF color, DWORD flags = DT_LEFT) {
    HFONT font = CreateFontW(size, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    auto oldFont = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    DrawTextW(dc, text.c_str(), -1, &rc, flags | DT_SINGLELINE | DT_VCENTER);
    SelectObject(dc, oldFont);
    DeleteObject(font);
}

int ProgressPercent() {
    if (g_recording) {
        return std::min(100, static_cast<int>((ElapsedMs(g_recordStart) * 100) / kMaxRecordMs));
    }
    if (g_waitingToRecord) {
        return std::min(100, static_cast<int>((ElapsedMs(g_modeStart) * 100) / kRecordDelayMs));
    }
    if (g_playing) {
        return std::min(100, static_cast<int>((ElapsedMs(g_modeStart) * 100) / kMaxPlaybackMs));
    }
    return 0;
}

void PaintUi(HWND hwnd) {
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(hwnd, &ps);
    RECT client{};
    GetClientRect(hwnd, &client);

    HBRUSH bg = CreateSolidBrush(kBg);
    FillRect(dc, &client, bg);
    DeleteObject(bg);

    RECT title{24, 22, client.right - 24, 62};
    DrawTextLine(dc, L"Loopy Macro Recorder", title, 28, kText);

    RECT subtitle{24, 62, client.right - 24, 92};
    DrawTextLine(dc, L"Ctrl + 0 plays/stops. Shift + 9 records. Shift + 2 stops recording.", subtitle, 17, kMuted);

    RECT panel{24, 112, client.right - 24, 250};
    PaintRoundedRect(dc, panel, kPanel, 16);

    RECT statusRect{48, 132, client.right - 48, 166};
    DrawTextLine(dc, g_status, statusRect, 22, (g_recording || g_waitingToRecord) ? kGreen : (g_playing ? kAccent : kText));

    RECT countRect{48, 166, client.right - 48, 198};
    DrawTextLine(dc, CountText(), countRect, 17, kMuted);

    RECT bar{48, 210, client.right - 48, 224};
    PaintRoundedRect(dc, bar, kPanel2, 8);
    int pct = ProgressPercent();
    if (pct > 0) {
        RECT fill = bar;
        fill.right = fill.left + ((bar.right - bar.left) * pct / 100);
        PaintRoundedRect(dc, fill, g_playing ? kAccent : kGreen, 8);
    }

    RECT help{24, 270, client.right - 24, 310};
    DrawTextLine(dc, L"Tip: recording captures keyboard, mouse buttons, wheel, and raw relative movement for 3D camera input.", help, 16, kMuted);

    EndPaint(hwnd, &ps);
}

void CreateButton(HWND parent, HWND& out, int id, const wchar_t* text, int x, int y, int w, int h) {
    out = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                          x, y, w, h, parent, reinterpret_cast<HMENU>(id),
                          GetModuleHandleW(nullptr), nullptr);
    HFONT font = CreateFontW(18, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    SendMessageW(out, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

void LayoutButtons(HWND hwnd) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    int gap = 12;
    int y = rc.bottom - 76;
    int buttonW = (rc.right - 48 - gap * 3) / 4;
    CreateButton(hwnd, g_recordButton, kBtnRecord, L"Record New", 24, y, buttonW, 44);
    CreateButton(hwnd, g_playButton, kBtnPlay, L"Play / Stop", 24 + (buttonW + gap), y, buttonW, 44);
    CreateButton(hwnd, g_clearButton, kBtnClear, L"Clear", 24 + (buttonW + gap) * 2, y, buttonW, 44);
    CreateButton(hwnd, g_exitButton, kBtnExit, L"Exit", 24 + (buttonW + gap) * 3, y, buttonW, 44);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE:
            LayoutButtons(hwnd);
            RegisterHotKey(hwnd, kHotkeyPlayback, MOD_CONTROL | MOD_NOREPEAT, '0');
            RegisterHotKey(hwnd, kHotkeyRecord, MOD_SHIFT | MOD_NOREPEAT, '9');
            RegisterHotKey(hwnd, kHotkeyStopRecord, MOD_SHIFT | MOD_NOREPEAT, '2');
            SetTimer(hwnd, kTimerUi, 100, nullptr);
            InstallHooks();
            if (!RegisterRawMouseInput(hwnd)) {
                SetStatus(L"Raw mouse input could not be registered");
            }
            return 0;

        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case kBtnRecord:
                    StartNewRecording();
                    break;
                case kBtnPlay:
                    StartPlayback();
                    break;
                case kBtnClear:
                    if (!g_recording && !g_waitingToRecord && !g_playing) {
                        ClearEvents();
                        SetStatus(L"Recording cleared");
                    }
                    break;
                case kBtnExit:
                    DestroyWindow(hwnd);
                    break;
            }
            return 0;

        case WM_HOTKEY:
            if (wParam == kHotkeyPlayback) {
                StartPlayback();
            } else if (wParam == kHotkeyRecord) {
                StartNewRecording();
            } else if (wParam == kHotkeyStopRecord) {
                StopRecording();
            }
            return 0;

        case WM_INPUT:
            HandleRawMouseInput(lParam);
            return DefWindowProcW(hwnd, msg, wParam, lParam);

        case WM_TIMER:
            JoinFinishedThreads();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;

        case WM_PAINT:
            PaintUi(hwnd);
            return 0;

        case WM_CTLCOLORBTN:
            SetBkMode(reinterpret_cast<HDC>(wParam), TRANSPARENT);
            return reinterpret_cast<LRESULT>(CreateSolidBrush(kPanel2));

        case WM_DESTROY:
            g_stopRecording = true;
            g_stopPlayback = true;
            if (g_recordThread.joinable()) {
                g_recordThread.join();
            }
            if (g_playThread.joinable()) {
                g_playThread.join();
            }
            RemoveHooks();
            UnregisterRawMouseInput();
            UnregisterHotKey(hwnd, kHotkeyPlayback);
            UnregisterHotKey(hwnd, kHotkeyRecord);
            UnregisterHotKey(hwnd, kHotkeyStopRecord);
            KillTimer(hwnd, kTimerUi);
            PostQuitMessage(0);
            return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCmd) {
    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    InitializeCriticalSection(&g_eventsLock);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wc.hbrBackground = CreateSolidBrush(kBg);
    wc.lpszClassName = L"LoopyMacroRecorderWindow";

    RegisterClassExW(&wc);

    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"Loopy Macro Recorder",
                             WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                             CW_USEDEFAULT, CW_USEDEFAULT, 620, 430,
                             nullptr, nullptr, instance, nullptr);

    if (!g_hwnd) {
        DeleteCriticalSection(&g_eventsLock);
        return 1;
    }

    ShowWindow(g_hwnd, showCmd);
    UpdateWindow(g_hwnd);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    DeleteCriticalSection(&g_eventsLock);
    return static_cast<int>(msg.wParam);
}
