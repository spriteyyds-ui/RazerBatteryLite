// Razer Battery Lite - Windows 10/11, C++14, Unicode.
// A small battery-only tray companion for Razer DeathAdder V3 Pro.
// Protocol knowledge acknowledged in README and NOTICE.
#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <algorithm>
#include <atomic>
#include <cwchar>
#include <cwctype>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "hid.lib")
#pragma comment(lib, "advapi32.lib")

namespace {
constexpr UINT kTrayId = 1;
constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kBatteryMessage = WM_APP + 2;
constexpr UINT kRefreshCommand = 1001;
constexpr UINT kStartupCommand = 1002;
constexpr UINT kAboutCommand = 1003;
constexpr UINT kExitCommand = 1004;
constexpr wchar_t kClassName[] = L"RazerBatteryLite.HiddenWindow";
constexpr wchar_t kStartupName[] = L"RazerBatteryLite";
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

HWND gWindow = nullptr;
HICON gIcon = nullptr;
HANDLE gStopEvent = nullptr;
HANDLE gWakeEvent = nullptr;
HANDLE gSingleton = nullptr;
std::thread gWorker;
UINT gTaskbarCreated = 0;
int gBattery = -1;
bool gIconAdded = false;

struct DevicePath {
    std::wstring path;
    int score;
};

bool IsTargetProduct(USHORT pid) {
    // DeathAdder V3 Pro standard wired/dongle, hardware revisions, HyperPolling dongle.
    return pid == 0x00B6 || pid == 0x00B7 || pid == 0x00C2 ||
           pid == 0x00C3 || pid == 0x00B3;
}

std::vector<DevicePath> FindRazerInterfaces() {
    std::vector<DevicePath> devices;
    GUID guid{};
    HidD_GetHidGuid(&guid);
    HDEVINFO info = SetupDiGetClassDevsW(&guid, nullptr, nullptr,
                                         DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (info == INVALID_HANDLE_VALUE) return devices;

    for (DWORD i = 0;; ++i) {
        SP_DEVICE_INTERFACE_DATA iface{};
        iface.cbSize = sizeof(iface);
        if (!SetupDiEnumDeviceInterfaces(info, nullptr, &guid, i, &iface)) break;
        DWORD bytes = 0;
        SetupDiGetDeviceInterfaceDetailW(info, &iface, nullptr, 0, &bytes, nullptr);
        if (bytes < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) continue;
        std::vector<BYTE> storage(bytes);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(storage.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(info, &iface, detail, bytes, nullptr, nullptr)) continue;
        std::wstring path(detail->DevicePath);

        HANDLE handle = CreateFileW(path.c_str(), 0,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (handle == INVALID_HANDLE_VALUE) continue;
        HIDD_ATTRIBUTES attr{};
        attr.Size = sizeof(attr);
        bool target = HidD_GetAttributes(handle, &attr) &&
                      attr.VendorID == 0x1532 && IsTargetProduct(attr.ProductID);
        int score = 0;
        if (target) {
            // 00B7/00C3 are the normal receiver; 00B3 is a generic HyperPolling dongle.
            score = attr.ProductID == 0x00B7 || attr.ProductID == 0x00C3 ? 20 :
                    attr.ProductID == 0x00B6 || attr.ProductID == 0x00C2 ? 10 : 0;
            std::wstring lower = path;
            std::transform(lower.begin(), lower.end(), lower.begin(),
                           [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
            if (lower.find(L"mi_00") != std::wstring::npos) score += 20;

            PHIDP_PREPARSED_DATA data = nullptr;
            if (HidD_GetPreparsedData(handle, &data)) {
                HIDP_CAPS caps{};
                if (HidP_GetCaps(data, &caps) == HIDP_STATUS_SUCCESS) {
                    if (caps.UsagePage == 0x01 && caps.Usage == 0x02) score += 30;
                    if (caps.UsagePage == 0x01 && caps.Usage == 0x06) score -= 20;
                }
                HidD_FreePreparsedData(data);
            }
            devices.push_back({path, score});
        }
        CloseHandle(handle);
    }
    SetupDiDestroyDeviceInfoList(info);
    std::stable_sort(devices.begin(), devices.end(),
        [](const DevicePath& a, const DevicePath& b) { return a.score > b.score; });
    return devices;
}

// Razer HID report layout (without Windows' leading Report ID):
// [0] status, [1] transaction id, [2..3] remaining packets,
// [4] protocol, [5] argument count, [6] command class,
// [7] command id, [8..87] args, [88] XOR checksum, [89] reserved.
// GET_BATTERY_LEVEL = 0x07/0x80, response argument [1] is 0..255.
// Only a read-only status command is sent: no DPI/polling/macro changes.
int ExtractPercent(const BYTE* packet, size_t count) {
    // Depending on the HID stack the leading Report ID may be returned or absent.
    for (size_t offset = 0; offset <= 1; ++offset) {
        if (count < offset + 11) continue;
        const BYTE* p = packet + offset;
        if (p[0] != 0x02 || p[1] != 0x1F || p[6] != 0x07 || p[7] != 0x80) continue;
        const int raw = p[9];
        return (raw * 100 + 127) / 255;
    }
    return -1;
}

int ReadDeviceBattery(HANDLE handle) {
    // HID feature report size differs across interfaces/firmware.
    for (DWORD length : {91u, 90u}) {
        BYTE request[91]{};
        BYTE* p = request + 1;  // Windows report ID 0 at request[0].
        p[0] = 0x00;
        p[1] = 0x1F;
        p[5] = 0x02;
        p[6] = 0x07;
        p[7] = 0x80;
        for (int i = 2; i <= 87; ++i) p[88] ^= p[i];
        if (!HidD_SetFeature(handle, request, length)) continue;
        Sleep(45);
        for (int retry = 0; retry != 3; ++retry) {
            BYTE response[91]{};
            if (HidD_GetFeature(handle, response, length)) {
                const int value = ExtractPercent(response, length);
                if (value >= 0) return value;
            }
            Sleep(45);
        }
    }
    return -1;
}

int ReadBattery() {
    for (const auto& dev : FindRazerInterfaces()) {
        // Zero-access handle can issue feature IOCTLs even on mouse-class HID interfaces.
        for (DWORD access : {static_cast<DWORD>(0),
                             static_cast<DWORD>(GENERIC_READ | GENERIC_WRITE)}) {
            HANDLE handle = CreateFileW(dev.path.c_str(), access,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
            if (handle == INVALID_HANDLE_VALUE) continue;
            const int result = ReadDeviceBattery(handle);
            CloseHandle(handle);
            if (result >= 0) return result;
        }
    }
    return -1;
}

std::wstring ExecutablePath() {
    std::wstring out(MAX_PATH, L'\0');
    for (;;) {
        DWORD n = GetModuleFileNameW(nullptr, &out[0], static_cast<DWORD>(out.size()));
        if (n == 0) return L"";
        if (n < out.size() - 1) { out.resize(n); return out; }
        out.resize(out.size() * 2);
    }
}

bool IsStartupOn() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return false;
    wchar_t value[4096]{};
    DWORD type = 0, size = sizeof(value);
    LONG result = RegQueryValueExW(key, kStartupName, nullptr, &type,
                                   reinterpret_cast<BYTE*>(value), &size);
    RegCloseKey(key);
    return result == ERROR_SUCCESS && type == REG_SZ && value[0] != L'\0';
}

bool SetStartup(bool enable) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return false;
    LONG result = ERROR_SUCCESS;
    if (enable) {
        const std::wstring entry = L"\"" + ExecutablePath() + L"\"";
        result = RegSetValueExW(key, kStartupName, 0, REG_SZ,
                reinterpret_cast<const BYTE*>(entry.c_str()),
                static_cast<DWORD>((entry.size() + 1) * sizeof(wchar_t)));
    } else {
        result = RegDeleteValueW(key, kStartupName);
        if (result == ERROR_FILE_NOT_FOUND) result = ERROR_SUCCESS;
    }
    RegCloseKey(key);
    return result == ERROR_SUCCESS;
}

HICON MakeNumberIcon(int percent) {
    constexpr int side = 32;
    BITMAPINFO bitmap{};
    bitmap.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmap.bmiHeader.biWidth = side;
    bitmap.bmiHeader.biHeight = -side;  // top down
    bitmap.bmiHeader.biPlanes = 1;
    bitmap.bmiHeader.biBitCount = 32;
    bitmap.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    HDC screen = GetDC(nullptr);
    HDC memory = CreateCompatibleDC(screen);
    HBITMAP color = CreateDIBSection(screen, &bitmap, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!memory || !color || !pixels) {
        if (color) DeleteObject(color);
        if (memory) DeleteDC(memory);
        if (screen) ReleaseDC(nullptr, screen);
        return LoadIconW(nullptr, IDI_APPLICATION);
    }
    HGDIOBJ previous = SelectObject(memory, color);
    SetBkMode(memory, TRANSPARENT);
    SetTextColor(memory, RGB(255, 255, 255));
    const std::wstring txt = percent < 0 ? L"--" : std::to_wstring(percent);
    HFONT font = CreateFontW(txt.size() == 3 ? -22 : -27, 0, 0, 0, FW_BOLD,
        FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    HGDIOBJ oldFont = SelectObject(memory, font);
    RECT rect{0, 0, side, side};
    DrawTextW(memory, txt.c_str(), -1, &rect,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(memory, oldFont);
    DeleteObject(font);

    // Convert grayscale glyph coverage to premultiplied BGRA alpha; transparent background.
    // A green icon is visible on both dark and light taskbars, amber/red indicates low battery.
    BYTE red = 90, green = 215, blue = 115;
    if (percent < 0) { red = 170; green = 170; blue = 170; }
    else if (percent < 20) { red = 255; green = 85; blue = 85; }
    else if (percent < 50) { red = 250; green = 175; blue = 75; }
    auto* bgra = static_cast<BYTE*>(pixels);
    for (int i = 0; i < side * side; ++i) {
        BYTE coverage = std::max({bgra[i * 4], bgra[i * 4 + 1], bgra[i * 4 + 2]});
        bgra[i * 4] = static_cast<BYTE>(blue * coverage / 255);
        bgra[i * 4 + 1] = static_cast<BYTE>(green * coverage / 255);
        bgra[i * 4 + 2] = static_cast<BYTE>(red * coverage / 255);
        bgra[i * 4 + 3] = coverage;
    }

    BYTE emptyMask[side * 4]{};  // 32 pixels, 1 bpp, 4 bytes per scanline
    HBITMAP mask = CreateBitmap(side, side, 1, 1, emptyMask);
    ICONINFO ii{};
    ii.fIcon = TRUE;
    ii.hbmColor = color;
    ii.hbmMask = mask;
    HICON result = CreateIconIndirect(&ii);
    SelectObject(memory, previous);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
    DeleteObject(mask);
    DeleteObject(color);
    return result ? result : LoadIconW(nullptr, IDI_APPLICATION);
}

void UpdateTray(bool add) {
    if (!gWindow) return;
    HICON nextIcon = MakeNumberIcon(gBattery);
    NOTIFYICONDATAW icon{};
    icon.cbSize = sizeof(icon);
    icon.hWnd = gWindow;
    icon.uID = kTrayId;
    icon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    icon.uCallbackMessage = kTrayMessage;
    icon.hIcon = nextIcon;
    std::wstring tip = gBattery >= 0
        ? L"炼狱蝰蛇 V3 Pro · 电量 " + std::to_wstring(gBattery) + L"%"
        : L"炼狱蝰蛇 V3 Pro · 未读取到电量";
    wcsncpy(icon.szTip, tip.c_str(), sizeof(icon.szTip) / sizeof(wchar_t) - 1);
    if (Shell_NotifyIconW((add || !gIconAdded) ? NIM_ADD : NIM_MODIFY, &icon)) {
        gIconAdded = true;
        if (gIcon) DestroyIcon(gIcon);
        gIcon = nextIcon;
    } else if (nextIcon) {
        DestroyIcon(nextIcon);
    }
}

void RemoveTray() {
    if (gIconAdded && gWindow) {
        NOTIFYICONDATAW icon{};
        icon.cbSize = sizeof(icon);
        icon.hWnd = gWindow;
        icon.uID = kTrayId;
        Shell_NotifyIconW(NIM_DELETE, &icon);
    }
    gIconAdded = false;
    if (gIcon) DestroyIcon(gIcon);
    gIcon = nullptr;
}

void ShowMenu(HWND window) {
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    const std::wstring status = gBattery < 0 ? L"电量：读取失败 / 未连接"
        : L"电量：" + std::to_wstring(gBattery) + L"%";
    AppendMenuW(menu, MF_STRING | MF_DISABLED | MF_GRAYED, 0, status.c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kRefreshCommand, L"立即刷新");
    AppendMenuW(menu, MF_STRING | (IsStartupOn() ? MF_CHECKED : 0),
                kStartupCommand, L"开机自动启动");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kAboutCommand, L"关于");
    AppendMenuW(menu, MF_STRING, kExitCommand, L"退出");
    POINT pos{};
    GetCursorPos(&pos);
    SetForegroundWindow(window);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_LEFTALIGN,
                   pos.x, pos.y, 0, window, nullptr);
    PostMessageW(window, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

void ReadWorker() {
    HANDLE events[] = {gStopEvent, gWakeEvent};
    while (true) {
        const int level = ReadBattery();
        PostMessageW(gWindow, kBatteryMessage, static_cast<WPARAM>(level + 1), 0);
        const DWORD wake = WaitForMultipleObjects(2, events, FALSE, 30000);
        if (wake == WAIT_OBJECT_0 || wake == WAIT_FAILED) break;
    }
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == gTaskbarCreated && gTaskbarCreated != 0) {
        gIconAdded = false;
        UpdateTray(true);
        return 0;
    }
    switch (message) {
    case kBatteryMessage: {
        const int newValue = static_cast<int>(wParam) - 1;
        if (newValue != gBattery) {
            gBattery = newValue;
            UpdateTray(false);
        }
        return 0;
    }
    case kTrayMessage:
        if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU) ShowMenu(window);
        else if (lParam == WM_LBUTTONDBLCLK && gWakeEvent) SetEvent(gWakeEvent);
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case kRefreshCommand:
            if (gWakeEvent) SetEvent(gWakeEvent);
            break;
        case kStartupCommand:
            if (!SetStartup(!IsStartupOn()))
                MessageBoxW(window, L"无法修改开机启动设置。", L"蝰蛇电量 Lite", MB_OK | MB_ICONWARNING);
            break;
        case kAboutCommand:
            MessageBoxW(window,
                L"蝰蛇电量 Lite v1.0\n专用于雷蛇炼狱蝰蛇 V3 Pro\n\n"
                L"每 30 秒刷新一次电量，绿色正常，橙色低于 50%，红色低于 20%。\n"
                L"无需联网或 Razer Synapse。\n\n协议参考和开源声明见 README、NOTICE。",
                L"关于 · 蝰蛇电量 Lite", MB_OK | MB_ICONINFORMATION);
            break;
        case kExitCommand:
            DestroyWindow(window);
            break;
        }
        return 0;
    case WM_DESTROY:
        RemoveTray();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    gSingleton = CreateMutexW(nullptr, TRUE, L"Local\\RazerBatteryLite.DeathAdderV3Pro");
    if (!gSingleton) return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS) { CloseHandle(gSingleton); return 0; }

    gStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    gWakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!gStopEvent || !gWakeEvent) return 2;
    gTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = kClassName;
    if (!RegisterClassW(&windowClass)) return 3;
    gWindow = CreateWindowExW(WS_EX_TOOLWINDOW, kClassName, L"RazerBatteryLite", 0,
        0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
    if (!gWindow) return 4;
    // CreateWindowExW sends WM_CREATE before gWindow has been assigned.
    // Add the icon here so that it is visible even when the first query fails.
    UpdateTray(true);

    gWorker = std::thread(ReadWorker);
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    SetEvent(gStopEvent);
    SetEvent(gWakeEvent);
    if (gWorker.joinable()) gWorker.join();
    CloseHandle(gStopEvent);
    CloseHandle(gWakeEvent);
    ReleaseMutex(gSingleton);
    CloseHandle(gSingleton);
    return 0;
}
