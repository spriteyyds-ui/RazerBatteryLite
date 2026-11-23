// Razer Battery Lite - Windows 10/11, C++14, Unicode.
// Lightweight Chinese tray battery monitor for Razer DeathAdder V3 Pro.
// v1.2: color-coded realistic mouse icon; charging bolt; Windows 11-style popup.
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
#include <windowsx.h>
#include <dwmapi.h>
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
#pragma comment(lib, "dwmapi.lib")

namespace
{
    constexpr UINT kTrayId = 1;
    constexpr UINT kTrayMessage = WM_APP + 1;
    constexpr UINT kBatteryMessage = WM_APP + 2;
    constexpr wchar_t kClassName[] = L"RazerBatteryLite.HiddenWindow";
    constexpr wchar_t kMenuClassName[] = L"RazerBatteryLite.FluentMenu";
    constexpr wchar_t kStartupName[] = L"RazerBatteryLite";
    constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

    HWND gWindow = nullptr;
    HWND gMenuWindow = nullptr;
    int gMenuHover = -1;
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
        const std::wstring current = L"\"" + ExecutablePath() + L"\"";
        return result == ERROR_SUCCESS && type == REG_SZ &&
               _wcsicmp(current.c_str(), value) == 0;
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

    // Colors show battery LEVEL, never charging state. Red always wins below 20%.
    // The large bolt alone distinguishes charging from ordinary use.
    COLORREF BatteryColor(int level)
    {
        if (level < 0)
            return RGB(148, 153, 164);
        if (level < 20)
            return RGB(244, 68, 84); // red: 0..19
        if (level < 40)
            return RGB(247, 152, 49); // orange: 20..39
        if (level < 60)
            return RGB(235, 194, 43); // yellow: 40..59
        if (level < 80)
            return RGB(155, 214, 67); // lime: 60..79
        return RGB(42, 202, 114);     // green: 80..100
    }

    bool IsCharging(int level, ChargeState charge)
    {
        return level >= 0 && charge == ChargeState::Charging;
    }

    // A mouse outline with a narrower bottom, curved shoulders and a real wheel.
    // Charging replaces the wheel with a large, solid lightning bolt, never a logo.
    // All drawing is white; the caller tints the anti-aliased pixels by battery color.
    void DrawMouseShape(HDC dc, bool charging)
    {
        HPEN contour = CreatePen(PS_SOLID, 5, RGB(255, 255, 255));
        HPEN thin = CreatePen(PS_SOLID, 3, RGB(255, 255, 255));
        HGDIOBJ oldPen = SelectObject(dc, contour);
        HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
        POINT outline[] = {
            {32, 3}, {17, 3}, {9, 12}, {9, 27}, {9, 42}, {11, 57}, {27, 61}, {32, 62}, {37, 62}, {37, 61}, {53, 57}, {55, 42}, {55, 27}, {55, 12}, {47, 3}, {32, 3}};
        // 5 cubic Bezier segments (3*n+1 points) making a closed tapered outline.
        PolyBezier(dc, outline, 16);

        SelectObject(dc, thin);
        if (charging)
        {
            // High-contrast, large solid bolt: nearly half the mouse's height.
            POINT bolt[] = {
                {37, 10}, {22, 35}, {31, 34}, {28, 54}, {44, 29}, {35, 30}, {43, 10}};
            HBRUSH solid = CreateSolidBrush(RGB(255, 255, 255));
            SelectObject(dc, solid);
            Polygon(dc, bolt, static_cast<int>(sizeof(bolt) / sizeof(bolt[0])));
            SelectObject(dc, GetStockObject(NULL_BRUSH));
            DeleteObject(solid);
        }
        else
        {
            // Curved seam between left/right mouse buttons, and short wheel.
            MoveToEx(dc, 14, 26, nullptr);
            LineTo(dc, 50, 26);
            MoveToEx(dc, 32, 4, nullptr);
            LineTo(dc, 32, 12);
            HBRUSH wheel = CreateSolidBrush(RGB(255, 255, 255));
            SelectObject(dc, wheel);
            RoundRect(dc, 29, 13, 35, 24, 6, 6);
            SelectObject(dc, GetStockObject(NULL_BRUSH));
            DeleteObject(wheel);
        }
        SelectObject(dc, oldBrush);
        SelectObject(dc, oldPen);
        DeleteObject(thin);
        DeleteObject(contour);
    }

    // Render at 64x64 for high-DPI taskbars; Windows scales it to tray size.
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
        SetBkMode(memory, TRANSPARENT);
        DrawMouseShape(memory, IsCharging(level, charge));
        GdiFlush();

        const COLORREF tint = BatteryColor(level);
        const BYTE red = GetRValue(tint), green = GetGValue(tint), blue = GetBValue(tint);
        auto *bgra = static_cast<BYTE *>(pixels);
        for (int i = 0; i < side * side; ++i)
        {
            const BYTE opacity = std::max({bgra[i * 4], bgra[i * 4 + 1], bgra[i * 4 + 2]});
            bgra[i * 4] = static_cast<BYTE>(blue * opacity / 255);
            bgra[i * 4 + 1] = static_cast<BYTE>(green * opacity / 255);
            bgra[i * 4 + 2] = static_cast<BYTE>(red * opacity / 255);
            bgra[i * 4 + 3] = opacity;
        }

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

    // A minimal owner-painted popup instead of the legacy Win32 context menu.
    // Runs only when right-clicked; no WebView, Qt or background UI thread.
    struct MenuPalette
    {
        COLORREF background, text, muted, separator, hover, border;
    };

    bool LightAppTheme()
    {
        DWORD value = 0, size = sizeof(value);
        return RegGetValueW(HKEY_CURRENT_USER,
                            L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                            L"AppsUseLightTheme", RRF_RT_REG_DWORD,
                            nullptr, &value, &size) == ERROR_SUCCESS &&
               value != 0;
    }

    MenuPalette MenuColors()
    {
        if (LightAppTheme())
        {
            return {RGB(250, 250, 253), RGB(35, 38, 45), RGB(107, 111, 122),
                    RGB(223, 225, 231), RGB(232, 235, 242), RGB(214, 217, 225)};
        }
        return {RGB(33, 35, 43), RGB(242, 244, 248), RGB(173, 181, 194),
                RGB(68, 73, 84), RGB(54, 59, 71), RGB(80, 85, 98)};
    }

    UINT MenuDpi()
    {
        return gMenuWindow ? GetDpiForWindow(gMenuWindow) : 96;
    }

    int Scale(int value)
    {
        return MulDiv(value, static_cast<int>(MenuDpi()), 96);
    }

    RECT MenuRect(int left, int top, int right, int bottom)
    {
        return RECT{Scale(left), Scale(top), Scale(right), Scale(bottom)};
    }

    void FillSolid(HDC dc, RECT rect, COLORREF color)
    {
        HBRUSH brush = CreateSolidBrush(color);
        FillRect(dc, &rect, brush);
        DeleteObject(brush);
    }

    void RoundedFill(HDC dc, RECT rect, COLORREF color, int radius)
    {
        HBRUSH brush = CreateSolidBrush(color);
        HGDIOBJ oldBrush = SelectObject(dc, brush);
        HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
        RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom,
                  Scale(radius), Scale(radius));
        SelectObject(dc, oldPen);
        SelectObject(dc, oldBrush);
        DeleteObject(brush);
    }

    HFONT MenuFont(int height, int weight)
    {
        return CreateFontW(-Scale(height), 0, 0, 0, weight, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                           CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    }

    void MenuText(HDC dc, const std::wstring &value, RECT area,
                  HFONT font, COLORREF color, UINT flags = DT_LEFT)
    {
        HGDIOBJ oldFont = SelectObject(dc, font);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, color);
        DrawTextW(dc, value.c_str(), -1, &area,
                  flags | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, oldFont);
    }

    // Menu rows: refresh, startup switch, about, exit.
    int MenuRowAt(POINT point)
    {
        const int x = MulDiv(point.x, 96, static_cast<int>(MenuDpi()));
        const int y = MulDiv(point.y, 96, static_cast<int>(MenuDpi()));
        if (x < 12 || x >= 288)
            return -1;
        if (y >= 129 && y < 170)
            return 0;
        if (y >= 171 && y < 212)
            return 1;
        if (y >= 226 && y < 267)
            return 2;
        if (y >= 268 && y < 309)
            return 3;
        return -1;
    }

    void PaintMenu(HWND window)
    {
        PAINTSTRUCT paint{};
        HDC target = BeginPaint(window, &paint);
        RECT client{};
        GetClientRect(window, &client);
        HDC dc = CreateCompatibleDC(target);
        HBITMAP bmp = CreateCompatibleBitmap(target,
                                             client.right - client.left, client.bottom - client.top);
        if (!dc || !bmp)
        {
            if (bmp)
                DeleteObject(bmp);
            if (dc)
                DeleteDC(dc);
            EndPaint(window, &paint);
            return;
        }
        HGDIOBJ oldBmp = SelectObject(dc, bmp);
        const auto theme = MenuColors();
        FillSolid(dc, client, theme.background);
        // Thin inset border and gently rounded rows resemble Win11 menu surfaces.
        HPEN frame = CreatePen(PS_SOLID, 1, theme.border);
        HGDIOBJ oldPen = SelectObject(dc, frame);
        HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
        RoundRect(dc, Scale(1), Scale(1), client.right - Scale(1),
                  client.bottom - Scale(1), Scale(18), Scale(18));
        SelectObject(dc, oldBrush);
        SelectObject(dc, oldPen);
        DeleteObject(frame);

        HFONT titleFont = MenuFont(17, FW_SEMIBOLD);
        HFONT labelFont = MenuFont(13, FW_NORMAL);
        HFONT actionFont = MenuFont(14, FW_NORMAL);
        HFONT levelFont = MenuFont(25, FW_SEMIBOLD);
        if (!titleFont || !labelFont || !actionFont || !levelFont)
        {
            if (titleFont)
                DeleteObject(titleFont);
            if (labelFont)
                DeleteObject(labelFont);
            if (actionFont)
                DeleteObject(actionFont);
            if (levelFont)
                DeleteObject(levelFont);
            SelectObject(dc, oldBmp);
            DeleteObject(bmp);
            DeleteDC(dc);
            EndPaint(window, &paint);
            return;
        }

        // Tiny mouse emblem in the header, with no vendor logo.
        const COLORREF accent = BatteryColor(gBattery);
        HPEN mousePen = CreatePen(PS_SOLID, Scale(2), accent);
        HGDIOBJ mouseOldPen = SelectObject(dc, mousePen);
        HGDIOBJ mouseOldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
        RoundRect(dc, Scale(21), Scale(18), Scale(51), Scale(65), Scale(22), Scale(22));
        if (IsCharging(gBattery, gCharge))
        {
            POINT bolt[] = {{36, 25}, {30, 39}, {36, 38}, {33, 52}, {43, 35}, {37, 36}, {41, 25}};
            for (auto &p : bolt)
            {
                p.x = Scale(p.x);
                p.y = Scale(p.y);
            }
            HBRUSH boltBrush = CreateSolidBrush(accent);
            SelectObject(dc, boltBrush);
            Polygon(dc, bolt, 7);
            SelectObject(dc, GetStockObject(NULL_BRUSH));
            DeleteObject(boltBrush);
        }
        else
        {
            MoveToEx(dc, Scale(36), Scale(23), nullptr);
            LineTo(dc, Scale(36), Scale(35));
        }
        SelectObject(dc, mouseOldBrush);
        SelectObject(dc, mouseOldPen);
        DeleteObject(mousePen);

        MenuText(dc, L"RazerBatteryLite", MenuRect(67, 15, 280, 40),
                 titleFont, theme.text);
        MenuText(dc, L"DeathAdder V3 Pro", MenuRect(68, 41, 280, 63),
                 labelFont, theme.muted);
        const std::wstring percent = gBattery < 0 ? L"--" : std::to_wstring(gBattery) + L"%";
        MenuText(dc, percent, MenuRect(19, 76, 129, 111), levelFont, accent);
        MenuText(dc, StatusLabel(gBattery, gCharge), MenuRect(131, 80, 287, 107),
                 labelFont, theme.muted, DT_RIGHT | DT_END_ELLIPSIS);

        FillSolid(dc, MenuRect(17, 117, 283, 118), theme.separator);
        FillSolid(dc, MenuRect(17, 219, 283, 220), theme.separator);

        const int tops[] = {129, 171, 226, 268};
        const wchar_t *captions[] = {L"立即刷新", L"开机自启动", L"关于", L"退出程序"};
        const wchar_t *glyphs[] = {L"↻", L"⏻", L"ⓘ", L"×"};
        for (int i = 0; i < 4; ++i)
        {
            if (gMenuHover == i)
                RoundedFill(dc, MenuRect(12, tops[i], 288, tops[i] + 40), theme.hover, 10);
            MenuText(dc, glyphs[i], MenuRect(27, tops[i] + 2, 54, tops[i] + 39),
                     titleFont, theme.muted);
            MenuText(dc, captions[i], MenuRect(63, tops[i] + 2, 225, tops[i] + 39),
                     actionFont, theme.text);
        }
        // Compact Windows 11-inspired switch; checking the registry reflects the
        // exact installed EXE path, not a stale path from an older release.
        const bool enabled = IsStartupOn();
        const RECT switchRect = MenuRect(246, 183, 280, 202);
        RoundedFill(dc, switchRect, enabled ? RGB(29, 114, 224) : theme.separator, 18);
        const int knobX = enabled ? 269 : 256;
        RECT knob = MenuRect(knobX - 7, 185, knobX + 7, 200);
        HBRUSH knobBrush = CreateSolidBrush(RGB(255, 255, 255));
        HGDIOBJ previousBrush = SelectObject(dc, knobBrush);
        HGDIOBJ previousPen = SelectObject(dc, GetStockObject(NULL_PEN));
        Ellipse(dc, knob.left, knob.top, knob.right, knob.bottom);
        SelectObject(dc, previousPen);
        SelectObject(dc, previousBrush);
        DeleteObject(knobBrush);

        DeleteObject(titleFont);
        DeleteObject(labelFont);
        DeleteObject(actionFont);
        DeleteObject(levelFont);
        BitBlt(target, 0, 0, client.right, client.bottom, dc, 0, 0, SRCCOPY);
        SelectObject(dc, oldBmp);
        DeleteObject(bmp);
        DeleteDC(dc);
        EndPaint(window, &paint);
    }

    void HideMenu()
    {
        if (gMenuWindow)
            ShowWindow(gMenuWindow, SW_HIDE);
        gMenuHover = -1;
    }

    void PerformMenuCommand(int row)
    {
        HideMenu();
        switch (row)
        {
        case 0:
            if (gWakeEvent)
                SetEvent(gWakeEvent);
            break;
        case 1:
            if (!SetStartup(!IsStartupOn()))
            {
                MessageBoxW(gWindow, L"无法修改开机自启动设置。请检查账户权限。",
                            L"蝰蛇电量 Lite", MB_OK | MB_ICONWARNING);
            }
            break;
        case 2:
            MessageBoxW(gWindow,
                        L"RazerBatteryLite v1.2.0\n雷蛇炼狱蝰蛇 V3 Pro 电量托盘工具\n\n"
                        L"图标颜色表示电量，低于 20% 为红色。\n"
                        L"滚轮表示使用状态，闪电表示正在充电。\n"
                        L"悬停可查看准确百分比；每 30 秒自动刷新。\n\n"
                        L"开机自启使用当前 EXE 路径，不会关闭 Windows 安全保护。\n"
                        L"协议及开源许可说明见 README 和 NOTICE。",
                        L"关于 RazerBatteryLite", MB_OK | MB_ICONINFORMATION);
            break;
        case 3:
            if (gWindow)
                DestroyWindow(gWindow);
            break;
        }
    }

    LRESULT CALLBACK MenuWindowProc(HWND window, UINT msg, WPARAM w, LPARAM l)
    {
        switch (msg)
        {
        case WM_PAINT:
            PaintMenu(window);
            return 0;
        case WM_MOUSEMOVE:
        {
            POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            const int row = MenuRowAt(p);
            if (row != gMenuHover)
            {
                gMenuHover = row;
                InvalidateRect(window, nullptr, FALSE);
            }
            TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window, 0};
            TrackMouseEvent(&tracking);
            return 0;
        }
        case WM_MOUSELEAVE:
            gMenuHover = -1;
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        case WM_LBUTTONUP:
        {
            POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            const int row = MenuRowAt(p);
            if (row >= 0)
                PerformMenuCommand(row);
            return 0;
        }
        case WM_KEYDOWN:
            if (w == VK_ESCAPE)
                HideMenu();
            else if (w == VK_DOWN || w == VK_UP)
            {
                gMenuHover = (gMenuHover < 0) ? 0 : (gMenuHover + (w == VK_DOWN ? 1 : 3)) % 4;
                InvalidateRect(window, nullptr, FALSE);
            }
            else if ((w == VK_RETURN || w == VK_SPACE) && gMenuHover >= 0)
            {
                PerformMenuCommand(gMenuHover);
            }
            return 0;
        case WM_ACTIVATE:
            if (LOWORD(w) == WA_INACTIVE)
                HideMenu();
            return 0;
        case WM_DPICHANGED:
        {
            const RECT *area = reinterpret_cast<const RECT *>(l);
            SetWindowPos(window, nullptr, area->left, area->top,
                         area->right - area->left, area->bottom - area->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            InvalidateRect(window, nullptr, TRUE);
            return 0;
        }
        }
        return DefWindowProcW(window, msg, w, l);
    }

    void ShowMenu(HWND owner)
    {
        (void)owner;
        if (!gMenuWindow)
            return;
        const int width = Scale(300), height = Scale(319);
        POINT cursor{};
        GetCursorPos(&cursor);
        MONITORINFO monitor{sizeof(monitor)};
        const HMONITOR screen = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
        if (!GetMonitorInfoW(screen, &monitor))
            return;
        int left = cursor.x;
        int top = cursor.y - height;
        const RECT work = monitor.rcWork;
        if (left + width > work.right)
            left = work.right - width;
        if (left < work.left)
            left = work.left;
        if (top < work.top)
            top = cursor.y + Scale(4);
        if (top + height > work.bottom)
            top = work.bottom - height;
        gMenuHover = -1;
        InvalidateRect(gMenuWindow, nullptr, TRUE);
        SetWindowPos(gMenuWindow, HWND_TOPMOST, left, top, width, height,
                     SWP_SHOWWINDOW);
        SetForegroundWindow(gMenuWindow); // Allows Escape & click-outside dismissal.
        SetFocus(gMenuWindow);
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
            UpdateTray(false);
            if (gMenuWindow)
                InvalidateRect(gMenuWindow, nullptr, TRUE);
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
        case WM_DESTROY:
            HideMenu();
            if (gMenuWindow)
            {
                DestroyWindow(gMenuWindow);
                gMenuWindow = nullptr;
            }
            RemoveTray();
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
    // Follow the DPI of the monitor hosting the tray popup (Windows 10/11).
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
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
    WNDCLASSW menuClass{};
    menuClass.lpfnWndProc = MenuWindowProc;
    menuClass.hInstance = instance;
    menuClass.lpszClassName = kMenuClassName;
    menuClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    if (!RegisterClassW(&menuClass))
        return 3;
    gMenuWindow = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
                                  kMenuClassName, L"RazerBatteryLite", WS_POPUP,
                                  0, 0, 1, 1, nullptr, nullptr, instance, nullptr);
    if (!gMenuWindow)
        return 4;
    // Window 11: rounded corners & matching dark/light chrome. No added UI library.
    const int roundCorner = 2; // DWMWCP_ROUND
    DwmSetWindowAttribute(gMenuWindow, static_cast<DWMWINDOWATTRIBUTE>(33),
                          &roundCorner, sizeof(roundCorner));
    const BOOL darkMode = !LightAppTheme();
    DwmSetWindowAttribute(gMenuWindow, static_cast<DWMWINDOWATTRIBUTE>(20),
                          &darkMode, sizeof(darkMode));
    gWindow = CreateWindowExW(WS_EX_TOOLWINDOW, kClassName, L"RazerBatteryLite", 0,
                              0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
    if (!gWindow)
    {
        DestroyWindow(gMenuWindow);
        return 4;
    }
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
