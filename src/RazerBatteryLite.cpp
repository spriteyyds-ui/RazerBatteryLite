// Razer Battery Lite - Windows 10/11, C++14, Unicode.
// Lightweight Chinese tray battery monitor for Razer DeathAdder V3 Pro.
// v1.1: mouse silhouette + numeric level, charging bolt, accurate status.
// Protocol knowledge acknowledged in README and NOTICE.
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
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

namespace
{
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
    enum class ChargeState
    {
        Unknown = -1,
        NotCharging = 0,
        Charging = 1
    };
    struct BatteryReading
    {
        int percent = -1;
        ChargeState charge = ChargeState::Unknown;
    };
    int gBattery = -1;
    ChargeState gCharge = ChargeState::Unknown;
    bool gIconAdded = false;

    struct DevicePath
    {
        std::wstring path;
        int score;
    };

    bool IsTargetProduct(USHORT pid)
    {
        // DeathAdder V3 Pro standard wired/dongle, hardware revisions, HyperPolling dongle.
        return pid == 0x00B6 || pid == 0x00B7 || pid == 0x00C2 ||
               pid == 0x00C3 || pid == 0x00B3;
    }

    std::vector<DevicePath> FindRazerInterfaces()
    {
        std::vector<DevicePath> devices;
        GUID guid{};
        HidD_GetHidGuid(&guid);
        HDEVINFO info = SetupDiGetClassDevsW(&guid, nullptr, nullptr,
                                             DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
        if (info == INVALID_HANDLE_VALUE)
            return devices;

        for (DWORD i = 0;; ++i)
        {
            SP_DEVICE_INTERFACE_DATA iface{};
            iface.cbSize = sizeof(iface);
            if (!SetupDiEnumDeviceInterfaces(info, nullptr, &guid, i, &iface))
                break;
            DWORD bytes = 0;
            SetupDiGetDeviceInterfaceDetailW(info, &iface, nullptr, 0, &bytes, nullptr);
            if (bytes < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W))
                continue;
            std::vector<BYTE> storage(bytes);
            auto *detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W *>(storage.data());
            detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
            if (!SetupDiGetDeviceInterfaceDetailW(info, &iface, detail, bytes, nullptr, nullptr))
                continue;
            std::wstring path(detail->DevicePath);

            HANDLE handle = CreateFileW(path.c_str(), 0,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
            if (handle == INVALID_HANDLE_VALUE)
                continue;
            HIDD_ATTRIBUTES attr{};
            attr.Size = sizeof(attr);
            bool target = HidD_GetAttributes(handle, &attr) &&
                          attr.VendorID == 0x1532 && IsTargetProduct(attr.ProductID);
            int score = 0;
            if (target)
            {
                // Prefer a wired mouse when cable and wireless dongle both exist:
                // it supplies the authoritative charging status.
                score = attr.ProductID == 0x00B6 || attr.ProductID == 0x00C2 ? 30 : attr.ProductID == 0x00B7 || attr.ProductID == 0x00C3 ? 20
                                                                                                                                         : 0;
                std::wstring lower = path;
                std::transform(lower.begin(), lower.end(), lower.begin(),
                               [](wchar_t c)
                               { return static_cast<wchar_t>(std::towlower(c)); });
                if (lower.find(L"mi_00") != std::wstring::npos)
                    score += 20;

                PHIDP_PREPARSED_DATA data = nullptr;
                if (HidD_GetPreparsedData(handle, &data))
                {
                    HIDP_CAPS caps{};
                    if (HidP_GetCaps(data, &caps) == HIDP_STATUS_SUCCESS)
                    {
                        if (caps.UsagePage == 0x01 && caps.Usage == 0x02)
                            score += 30;
                        if (caps.UsagePage == 0x01 && caps.Usage == 0x06)
                            score -= 20;
                    }
                    HidD_FreePreparsedData(data);
                }
                devices.push_back({path, score});
            }
            CloseHandle(handle);
        }
        SetupDiDestroyDeviceInfoList(info);
        std::stable_sort(devices.begin(), devices.end(),
                         [](const DevicePath &a, const DevicePath &b)
                         { return a.score > b.score; });
        return devices;
    }

    // Razer HID report layout (without Windows' leading Report ID):
    // [0] status, [1] transaction id, [2..3] remaining packets,
    // [4] protocol, [5] argument count, [6] command class,
    // [7] command id, [8..87] args, [88] XOR checksum, [89] reserved.
    // GET_BATTERY_LEVEL = 0x07/0x80, response argument [1] is 0..255.
    // Read-only 0x07/0x80 and 0x07/0x84 commands; no DPI/polling/macro changes.
    int ExtractPercent(const BYTE *packet, size_t count)
    {
        // Depending on the HID stack the leading Report ID may be returned or absent.
        for (size_t offset = 0; offset <= 1; ++offset)
        {
            if (count < offset + 11)
                continue;
            const BYTE *p = packet + offset;
            if (p[0] != 0x02 || p[1] != 0x1F || p[6] != 0x07 || p[7] != 0x80)
                continue;
            const int raw = p[9];
            return (raw * 100 + 127) / 255;
        }
        return -1;
    }

    int ReadDeviceBattery(HANDLE handle)
    {
        // HID feature report size differs across interfaces/firmware.
        for (DWORD length : {91u, 90u})
        {
            BYTE request[91]{};
            BYTE *p = request + 1; // Windows report ID 0 at request[0].
            p[0] = 0x00;
            p[1] = 0x1F;
            p[5] = 0x02;
            p[6] = 0x07;
            p[7] = 0x80;
            for (int i = 2; i <= 87; ++i)
                p[88] ^= p[i];
            if (!HidD_SetFeature(handle, request, length))
                continue;
            Sleep(45);
            for (int retry = 0; retry != 3; ++retry)
            {
                BYTE response[91]{};
                if (HidD_GetFeature(handle, response, length))
                {
                    const int value = ExtractPercent(response, length);
                    if (value >= 0)
                        return value;
                }
                Sleep(45);
            }
        }
        return -1;
    }

    // Separately query the charging flag. Failure must never discard a valid level.
    // GET_CHARGING_STATUS (class 0x07, id 0x84), response arg[1]: 0=no, 1=yes.
    int ReadDeviceCharging(HANDLE handle)
    {
        for (DWORD length : {91u, 90u})
        {
            BYTE request[91]{};
            BYTE *p = request + 1;
            p[0] = 0x00;
            p[1] = 0x1F;
            p[5] = 0x02;
            p[6] = 0x07;
            p[7] = 0x84;
            for (int i = 2; i <= 87; ++i)
                p[88] ^= p[i];
            if (!HidD_SetFeature(handle, request, length))
                continue;
            Sleep(45);
            for (int retry = 0; retry < 3; ++retry)
            {
                BYTE response[91]{};
                if (HidD_GetFeature(handle, response, length))
                {
                    for (size_t offset = 0; offset <= 1; ++offset)
                    {
                        if (length < offset + 10)
                            continue;
                        const BYTE *r = response + offset;
                        if (r[0] != 0x02 || r[1] != 0x1F ||
                            r[6] != 0x07 || r[7] != 0x84)
                            continue;
                        if (r[9] == 0 || r[9] == 1)
                            return r[9];
                    }
                }
                Sleep(45);
            }
        }
        return -1;
    }

    BatteryReading ReadBattery()
    {
        for (const auto &dev : FindRazerInterfaces())
        {
            // Keep the existing, hardware-tested battery path unchanged.
            for (DWORD access : {static_cast<DWORD>(0),
                                 static_cast<DWORD>(GENERIC_READ | GENERIC_WRITE)})
            {
                HANDLE handle = CreateFileW(dev.path.c_str(), access,
                                            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
                if (handle == INVALID_HANDLE_VALUE)
                    continue;
                BatteryReading reading;
                reading.percent = ReadDeviceBattery(handle);
                if (reading.percent >= 0)
                {
                    const int flag = ReadDeviceCharging(handle);
                    if (flag == 0)
                        reading.charge = ChargeState::NotCharging;
                    if (flag == 1)
                        reading.charge = ChargeState::Charging;
                    CloseHandle(handle);
                    return reading;
                }
                CloseHandle(handle);
            }
        }
        return {};
    }

    std::wstring ExecutablePath()
    {
        std::wstring out(MAX_PATH, L'\0');
        for (;;)
        {
            DWORD n = GetModuleFileNameW(nullptr, &out[0], static_cast<DWORD>(out.size()));
            if (n == 0)
                return L"";
            if (n < out.size() - 1)
            {
                out.resize(n);
                return out;
            }
            out.resize(out.size() * 2);
        }
    }

    bool IsStartupOn()
    {
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
            return false;
        wchar_t value[4096]{};
        DWORD type = 0, size = sizeof(value);
        LONG result = RegQueryValueExW(key, kStartupName, nullptr, &type,
                                       reinterpret_cast<BYTE *>(value), &size);
        RegCloseKey(key);
        return result == ERROR_SUCCESS && type == REG_SZ && value[0] != L'\0';
    }

    bool SetStartup(bool enable)
    {
        HKEY key = nullptr;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0,
                            KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
            return false;
        LONG result = ERROR_SUCCESS;
        if (enable)
        {
            const std::wstring entry = L"\"" + ExecutablePath() + L"\"";
            result = RegSetValueExW(key, kStartupName, 0, REG_SZ,
                                    reinterpret_cast<const BYTE *>(entry.c_str()),
                                    static_cast<DWORD>((entry.size() + 1) * sizeof(wchar_t)));
        }
        else
        {
            result = RegDeleteValueW(key, kStartupName);
            if (result == ERROR_FILE_NOT_FOUND)
                result = ERROR_SUCCESS;
        }
        RegCloseKey(key);
        return result == ERROR_SUCCESS;
    }

    // Windows taskbar can be light or dark; normal icons should remain legible.
    bool SystemUsesLightTaskbar()
    {
        DWORD value = 0, size = sizeof(value);
        return RegGetValueW(HKEY_CURRENT_USER,
                            L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                            L"SystemUsesLightTheme", RRF_RT_REG_DWORD,
                            nullptr, &value, &size) == ERROR_SUCCESS &&
               value != 0;
    }

    std::wstring StatusLabel(int level, ChargeState charge)
    {
        if (level < 0)
            return L"读取失败 / 未连接";
        if (charge == ChargeState::Charging)
            return L"充电中";
        if (charge == ChargeState::NotCharging)
            return level >= 100 ? L"已充满" : L"使用中";
        return L"充电状态未知";
    }

    // Render white geometry and text on a transparent 64x64 canvas, then tint its
    // coverage into premultiplied BGRA. This needs no icon assets or GDI+ runtime.
    HICON MakeMouseIcon(int level, ChargeState charge)
    {
        constexpr int side = 64;
        BITMAPINFO bitmap{};
        bitmap.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bitmap.bmiHeader.biWidth = side;
        bitmap.bmiHeader.biHeight = -side;
        bitmap.bmiHeader.biPlanes = 1;
        bitmap.bmiHeader.biBitCount = 32;
        bitmap.bmiHeader.biCompression = BI_RGB;
        void *pixels = nullptr;
        HDC screen = GetDC(nullptr);
        if (!screen)
            return CopyIcon(LoadIconW(nullptr, IDI_APPLICATION));
        HDC memory = CreateCompatibleDC(screen);
        HBITMAP color = CreateDIBSection(screen, &bitmap, DIB_RGB_COLORS, &pixels, nullptr, 0);
        if (!memory || !color || !pixels)
        {
            if (color)
                DeleteObject(color);
            if (memory)
                DeleteDC(memory);
            ReleaseDC(nullptr, screen);
            return CopyIcon(LoadIconW(nullptr, IDI_APPLICATION));
        }
        std::fill_n(static_cast<BYTE *>(pixels), side * side * 4, BYTE{0});
        HGDIOBJ oldBitmap = SelectObject(memory, color);
        HGDIOBJ oldBrush = SelectObject(memory, GetStockObject(NULL_BRUSH));
        HPEN pen = CreatePen(PS_SOLID, 5, RGB(255, 255, 255));
        HGDIOBJ oldPen = SelectObject(memory, pen);
        SetBkMode(memory, TRANSPARENT);
        SetTextColor(memory, RGB(255, 255, 255));

        // Vertical pill-shaped mouse outline.
        RoundRect(memory, 9, 3, 55, 61, 46, 46);
        // Wheel marker; replace this line with a lightning stroke while charging.
        if (charge == ChargeState::Charging && level >= 0)
        {
            POINT bolt[] = {{36, 10}, {30, 18}, {37, 18}, {33, 27}, {43, 16}, {36, 16}};
            for (int i = 0; i < 5; ++i)
            {
                MoveToEx(memory, bolt[i].x, bolt[i].y, nullptr);
                LineTo(memory, bolt[i + 1].x, bolt[i + 1].y);
            }
        }
        else
        {
            MoveToEx(memory, 32, 11, nullptr);
            LineTo(memory, 32, 24);
        }

        // 100% uses smaller text, so all digits stay inside the silhouette.
        const std::wstring text = level < 0 ? L"--" : std::to_wstring(level);
        HFONT font = CreateFontW(text.size() == 3 ? -23 : -31, 0, 0, 0, FW_BOLD,
                                 FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                 CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS,
                                 L"Arial");
        if (font)
        {
            HGDIOBJ oldFont = SelectObject(memory, font);
            RECT area{11, 29, 53, 56};
            DrawTextW(memory, text.c_str(), -1, &area,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            SelectObject(memory, oldFont);
            DeleteObject(font);
        }
        SelectObject(memory, oldPen);
        SelectObject(memory, oldBrush);
        DeleteObject(pen);
        GdiFlush(); // Ensure all drawing has reached the DIB before reading pixels.

        BYTE red = 214, green = 222, blue = 230; // neutral on dark taskbar
        if (SystemUsesLightTaskbar())
        {
            red = 67;
            green = 75;
            blue = 86;
        }
        if (level < 0)
        {
            red = 150;
            green = 150;
            blue = 150;
        }
        else if (charge == ChargeState::Charging ||
                 (level == 100 && charge == ChargeState::NotCharging))
        {
            red = 34;
            green = 176;
            blue = 112;
        }
        else if (level <= 10)
        {
            red = 239;
            green = 69;
            blue = 69;
        }
        else if (level <= 20)
        {
            red = 235;
            green = 167;
            blue = 51;
        }

        auto *bgra = static_cast<BYTE *>(pixels);
        for (int i = 0; i < side * side; ++i)
        {
            const BYTE opacity = std::max({bgra[i * 4], bgra[i * 4 + 1], bgra[i * 4 + 2]});
            bgra[i * 4] = static_cast<BYTE>(blue * opacity / 255);
            bgra[i * 4 + 1] = static_cast<BYTE>(green * opacity / 255);
            bgra[i * 4 + 2] = static_cast<BYTE>(red * opacity / 255);
            bgra[i * 4 + 3] = opacity;
        }

        // AND-mask must be entirely clear when using an ARGB icon.
        BYTE maskPixels[side * ((side + 31) / 32) * 4]{};
        HBITMAP mask = CreateBitmap(side, side, 1, 1, maskPixels);
        ICONINFO iconInfo{};
        iconInfo.fIcon = TRUE;
        iconInfo.hbmColor = color;
        iconInfo.hbmMask = mask;
        HICON icon = mask ? CreateIconIndirect(&iconInfo) : nullptr;
        SelectObject(memory, oldBitmap);
        DeleteDC(memory);
        ReleaseDC(nullptr, screen);
        if (mask)
            DeleteObject(mask);
        DeleteObject(color);
        return icon ? icon : CopyIcon(LoadIconW(nullptr, IDI_APPLICATION));
    }

    void UpdateTray(bool add)
    {
        if (!gWindow)
            return;
        HICON nextIcon = MakeMouseIcon(gBattery, gCharge);
        NOTIFYICONDATAW icon{};
        icon.cbSize = sizeof(icon);
        icon.hWnd = gWindow;
        icon.uID = kTrayId;
        icon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        icon.uCallbackMessage = kTrayMessage;
        icon.hIcon = nextIcon;
        const std::wstring tip = L"蝰蛇电量 Lite · DeathAdder V3 Pro\n电量：" +
                                 (gBattery < 0 ? L"--" : std::to_wstring(gBattery) + L"%") +
                                 L"\n状态：" + StatusLabel(gBattery, gCharge);
        wcsncpy(icon.szTip, tip.c_str(), sizeof(icon.szTip) / sizeof(wchar_t) - 1);
        if (Shell_NotifyIconW((add || !gIconAdded) ? NIM_ADD : NIM_MODIFY, &icon))
        {
            gIconAdded = true;
            if (gIcon)
                DestroyIcon(gIcon);
            gIcon = nextIcon;
        }
        else if (nextIcon)
        {
            DestroyIcon(nextIcon);
        }
    }

    void RemoveTray()
    {
        if (gIconAdded && gWindow)
        {
            NOTIFYICONDATAW icon{};
            icon.cbSize = sizeof(icon);
            icon.hWnd = gWindow;
            icon.uID = kTrayId;
            Shell_NotifyIconW(NIM_DELETE, &icon);
        }
        gIconAdded = false;
        if (gIcon)
            DestroyIcon(gIcon);
        gIcon = nullptr;
    }

    void ShowMenu(HWND window)
    {
        HMENU menu = CreatePopupMenu();
        if (!menu)
            return;
        const std::wstring levelText = gBattery < 0 ? L"电量：--"
                                                    : L"电量：" + std::to_wstring(gBattery) + L"%";
        const std::wstring statusText = L"状态：" + StatusLabel(gBattery, gCharge);
        AppendMenuW(menu, MF_STRING | MF_DISABLED | MF_GRAYED, 0, levelText.c_str());
        AppendMenuW(menu, MF_STRING | MF_DISABLED | MF_GRAYED, 0, statusText.c_str());
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

    void ReadWorker()
    {
        HANDLE events[] = {gStopEvent, gWakeEvent};
        while (true)
        {
            BatteryReading reading = ReadBattery();
            PostMessageW(gWindow, kBatteryMessage,
                         static_cast<WPARAM>(reading.percent + 1),
                         static_cast<LPARAM>(static_cast<int>(reading.charge)));
            const DWORD wake = WaitForMultipleObjects(2, events, FALSE, 30000);
            if (wake == WAIT_OBJECT_0 || wake == WAIT_FAILED)
                break;
        }
    }

    LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (message == gTaskbarCreated && gTaskbarCreated != 0)
        {
            gIconAdded = false;
            UpdateTray(true);
            return 0;
        }
        switch (message)
        {
        case WM_SETTINGCHANGE:
            UpdateTray(false); // Repaint when Windows taskbar light/dark mode changes.
            return 0;
        case kBatteryMessage:
        {
            const int newValue = static_cast<int>(wParam) - 1;
            const ChargeState newCharge = static_cast<ChargeState>(static_cast<int>(lParam));
            if (newValue != gBattery || newCharge != gCharge)
            {
                gBattery = newValue;
                gCharge = newCharge;
                UpdateTray(false);
            }
            return 0;
        }
        case kTrayMessage:
            if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU)
                ShowMenu(window);
            else if ((lParam == WM_LBUTTONUP || lParam == WM_LBUTTONDBLCLK) && gWakeEvent)
                SetEvent(gWakeEvent);
            return 0;
        case WM_COMMAND:
            switch (LOWORD(wParam))
            {
            case kRefreshCommand:
                if (gWakeEvent)
                    SetEvent(gWakeEvent);
                break;
            case kStartupCommand:
                if (!SetStartup(!IsStartupOn()))
                    MessageBoxW(window, L"无法修改开机启动设置。", L"蝰蛇电量 Lite", MB_OK | MB_ICONWARNING);
                break;
            case kAboutCommand:
                MessageBoxW(window,
                            L"蝰蛇电量 Lite v1.1.0\n专用于雷蛇炼狱蝰蛇 V3 Pro\n\n"
                            L"鼠标轮廓显示电量；顶部闪电代表正在充电。\n"
                            L"电量 <= 20% 显示橙色，<= 10% 显示红色。\n"
                            L"每 30 秒刷新，可左键单击立即刷新。\n"
                            L"充电状态读取失败时显示未知，不影响电量。\n\n"
                            L"无需联网或 Razer Synapse。\n协议参考和开源声明见 README、NOTICE。",
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

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
    gSingleton = CreateMutexW(nullptr, TRUE, L"Local\\RazerBatteryLite.DeathAdderV3Pro");
    if (!gSingleton)
        return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        CloseHandle(gSingleton);
        return 0;
    }

    gStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    gWakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!gStopEvent || !gWakeEvent)
        return 2;
    gTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = kClassName;
    if (!RegisterClassW(&windowClass))
        return 3;
    gWindow = CreateWindowExW(WS_EX_TOOLWINDOW, kClassName, L"RazerBatteryLite", 0,
                              0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
    if (!gWindow)
        return 4;
    // CreateWindowExW sends WM_CREATE before gWindow has been assigned.
    // Add the icon here so that it is visible even when the first query fails.
    UpdateTray(true);

    gWorker = std::thread(ReadWorker);
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    SetEvent(gStopEvent);
    SetEvent(gWakeEvent);
    if (gWorker.joinable())
        gWorker.join();
    CloseHandle(gStopEvent);
    CloseHandle(gWakeEvent);
    ReleaseMutex(gSingleton);
    CloseHandle(gSingleton);
    return 0;
}
