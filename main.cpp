#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <initguid.h>         // forces GUIDs below to be *defined* here, not just declared
#include <commoncontrols.h>   // IImageList / SHGetImageList (jumbo 256x256 icons)
#include <commctrl.h>         // trackbar/combobox constants (comctl32 itself is loaded at runtime)
#include <inspectable.h>      // IInspectable/HSTRING for the now-playing media session (WinRT)
#include <wincodec.h>
#include <winhttp.h>          // the download manager         // WIC, to decode album art

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#pragma comment(lib, "comctl32.lib")

// =====================================================================
// AlphaIcon: a true-alpha ARGB pixel buffer extracted from an HICON.
//
// FIX #2 (purple outlines): SHGetFileInfo/DrawIconEx + a color-key
// layered window is what caused the magenta fringe - anti-aliased icon
// edges blend slightly toward the magenta key color and never exactly
// match it, so they don't get keyed out. Extracting the icon's real
// alpha channel (or falling back to its AND mask) and compositing it
// ourselves onto a true per-pixel-alpha buffer removes the fringe
// entirely, because there's no key color to imperfectly match anymore.
// =====================================================================

struct AlphaIcon {
    std::vector<UINT32> pixels; // premultiplied 0xAARRGGBB
    int w = 0, h = 0;
    bool valid = false;
};

// Extracts a premultiplied ARGB pixel buffer from an HICON.
AlphaIcon ExtractIconAlpha(HICON hIcon) {
    AlphaIcon result;
    if (!hIcon) return result;

    ICONINFO ii{};
    if (!GetIconInfo(hIcon, &ii)) return result;

    BITMAP bmColor{};
    GetObject(ii.hbmColor, sizeof(bmColor), &bmColor);

    int w = bmColor.bmWidth;
    int h = bmColor.bmHeight;

    if (w <= 0 || h <= 0) {
        if (ii.hbmColor) DeleteObject(ii.hbmColor);
        if (ii.hbmMask) DeleteObject(ii.hbmMask);
        return result;
    }

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h; // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    std::vector<UINT32> colorBits(static_cast<size_t>(w) * h, 0);

    HDC dc = GetDC(nullptr);
    GetDIBits(dc, ii.hbmColor, 0, h, colorBits.data(), &bmi, DIB_RGB_COLORS);

    bool hasRealAlpha = false;
    for (UINT32 px : colorBits) {
        if ((px >> 24) != 0) { hasRealAlpha = true; break; }
    }

    if (!hasRealAlpha) {
        // Older-style icon with no embedded alpha channel: fall back to
        // the AND mask (1 = transparent, 0 = opaque).
        std::vector<UINT32> maskBits(static_cast<size_t>(w) * h, 0);
        GetDIBits(dc, ii.hbmMask, 0, h, maskBits.data(), &bmi, DIB_RGB_COLORS);

        for (size_t i = 0; i < colorBits.size(); ++i) {
            BYTE a = (maskBits[i] & 0x00FFFFFF) ? 0 : 255;
            BYTE r = (colorBits[i] >> 16) & 0xFF;
            BYTE g = (colorBits[i] >> 8) & 0xFF;
            BYTE b = colorBits[i] & 0xFF;
            colorBits[i] = (static_cast<UINT32>(a) << 24) | (r << 16) | (g << 8) | b;
        }
    }

    ReleaseDC(nullptr, dc);
    DeleteObject(ii.hbmColor);
    DeleteObject(ii.hbmMask);

    // Premultiply RGB by alpha so straight "src over dst" compositing
    // is correct (needed for our own blit routine below).
    for (UINT32& px : colorBits) {
        BYTE a = (px >> 24) & 0xFF;
        BYTE r = (px >> 16) & 0xFF;
        BYTE g = (px >> 8) & 0xFF;
        BYTE b = px & 0xFF;
        r = static_cast<BYTE>((r * a) / 255);
        g = static_cast<BYTE>((g * a) / 255);
        b = static_cast<BYTE>((b * a) / 255);
        px = (static_cast<UINT32>(a) << 24) | (r << 16) | (g << 8) | b;
    }

    result.pixels = std::move(colorBits);
    result.w = w;
    result.h = h;
    result.valid = true;
    return result;
}

struct Item {
    std::wstring name;
    std::wstring path;
    int x;
    int y;
    int w;
    int h;
    HICON hIcon = nullptr;   // runtime only, not saved to layout.txt
    AlphaIcon alpha;         // pre-extracted true-alpha pixels for blitting
    float hoverT = 0.f;      // 0..1 hover animation progress
    DWORD pulseStart = 0;    // GetTickCount() when a double-click pulse began, 0 = none
    DWORD shineStart = 0;    // GetTickCount() when a hover "shine" sweep began, 0 = none
};

std::vector<Item> items;

bool editMode = false;

// Hover effect, set from glass.txt (hoverEffect=lift|shine|bounce|none).
enum { HOVER_LIFT = 0, HOVER_SHINE, HOVER_BOUNCE, HOVER_NONE };
int g_hoverEffect = HOVER_LIFT;
bool dragging = false;
bool resizing = false;
int selected = -1;
int hoverIndex = -1;
POINT dragOffset{};
int resizeStartW = 0;
int resizeStartH = 0;
POINT resizeStartMouse{};

// Autosave layout while editing so even an abrupt Task Manager termination
// usually loses at most a fraction of a second of movement. A final Save()
// still happens on mouse release.
ULONGLONG lastLayoutSaveTick = 0;
const ULONGLONG LAYOUT_AUTOSAVE_MS = 250;

const int HANDLE_SIZE = 12;
const int MIN_SIZE = 32;
const int LABEL_HEIGHT = 20;

HWND g_hwnd = nullptr;

void Render(HWND h); // forward decl: used by UpdateFullscreenVisibility() below,
                      // which is defined earlier in the file than Render() itself.

// --- on-screen debug log -------------------------------------------
std::vector<std::wstring> logLines;

void Log(const std::wstring& s) {
    logLines.push_back(s);
    if (logLines.size() > 28)
        logLines.erase(logLines.begin());
}

void LogF(const wchar_t* fmt, ...) {
    wchar_t buf[256];
    va_list args;
    va_start(args, fmt);
    _vsnwprintf(buf, 255, fmt, args);
    va_end(args);
    buf[255] = L'\0';
    Log(buf);
}
// ---------------------------------------------------------------------

// =====================================================================
// FIX #2 continued: true per-pixel-alpha back buffer.
//
// Everything is drawn into a 32bpp top-down DIB section that we own,
// cleared to fully transparent (0,0,0,0) every frame, then pushed to
// the screen in one shot via UpdateLayeredWindow(..., ULW_ALPHA).
// GDI itself doesn't know about our alpha channel, so any GDI drawing
// (FillRect, FrameRect, DrawText) needs its alpha filled in by hand
// afterwards - the small helpers below do that.
// =====================================================================

HDC g_memDC = nullptr;
HBITMAP g_dib = nullptr;
HBITMAP g_oldBmp = nullptr;
UINT32* g_bits = nullptr;
int g_bufW = 0, g_bufH = 0;

void CreateBackBuffer(int w, int h) {
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h; // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    HDC screenDC = GetDC(nullptr);
    g_memDC = CreateCompatibleDC(screenDC);
    g_dib = CreateDIBSection(screenDC, &bmi, DIB_RGB_COLORS,
                              reinterpret_cast<void**>(&g_bits), nullptr, 0);
    ReleaseDC(nullptr, screenDC);

    g_oldBmp = static_cast<HBITMAP>(SelectObject(g_memDC, g_dib));
    g_bufW = w;
    g_bufH = h;
}

void DestroyBackBuffer() {
    if (g_memDC) {
        SelectObject(g_memDC, g_oldBmp);
        DeleteDC(g_memDC);
        g_memDC = nullptr;
    }
    if (g_dib) {
        DeleteObject(g_dib);
        g_dib = nullptr;
    }
    g_bits = nullptr;
}

inline UINT32* PixelAt(int x, int y) {
    if (x < 0 || y < 0 || x >= g_bufW || y >= g_bufH) return nullptr;
    return &g_bits[static_cast<size_t>(y) * g_bufW + x];
}

void ClearBuffer() {
    if (g_bits) ZeroMemory(g_bits, sizeof(UINT32) * static_cast<size_t>(g_bufW) * g_bufH);
}

// RECT fields are LONG, but the buffer bounds below are int, so
// std::max/std::min (single-type templates) can't be called on them
// directly without a mismatch. Clamp by hand instead.
void ClampRectToBuffer(RECT& r) {
    if (r.left < 0) r.left = 0;
    if (r.top < 0) r.top = 0;
    if (r.right > g_bufW) r.right = g_bufW;
    if (r.bottom > g_bufH) r.bottom = g_bufH;
}

// Stamps a hard-edged alpha value over a rect. Correct for anything GDI
// drew as a solid, hard-edged fill (no anti-aliasing involved), such as
// our panel backgrounds, selection frame and resize grip.
void FillAlphaRect(RECT r, BYTE alpha) {
    ClampRectToBuffer(r);

    for (int y = r.top; y < r.bottom; ++y) {
        for (int x = r.left; x < r.right; ++x) {
            UINT32* p = PixelAt(x, y);
            if (p) *p = (*p & 0x00FFFFFF) | (static_cast<UINT32>(alpha) << 24);
        }
    }
}

// For text drawn on a fully transparent (cleared-to-black) background:
// GDI's anti-aliased blend of "foreground over black" is already
// mathematically equal to a premultiplied-alpha pixel, as long as the
// foreground color has a channel at/near 255. So alpha = the brightest
// channel recovers a correct, fringe-free alpha for that glyph pixel.
void ApplyLuminanceAlpha(RECT r) {
    ClampRectToBuffer(r);

    for (int y = r.top; y < r.bottom; ++y) {
        for (int x = r.left; x < r.right; ++x) {
            UINT32* p = PixelAt(x, y);
            if (!p) continue;
            BYTE b = (*p) & 0xFF;
            BYTE g = (*p >> 8) & 0xFF;
            BYTE rC = (*p >> 16) & 0xFF;
            BYTE a = std::max({ rC, g, b });
            *p = (static_cast<UINT32>(a) << 24) | (rC << 16) | (g << 8) | b;
        }
    }
}

void FillOpaqueRect(HDC dc, RECT r, COLORREF color) {
    HBRUSH br = CreateSolidBrush(color);
    FillRect(dc, &r, br);
    DeleteObject(br);
    FillAlphaRect(r, 255);
}

// Soft radial glow used for the hover / double-click "alive" effect.
void BlendGlow(RECT r, COLORREF color, float strength) {
    ClampRectToBuffer(r);
    if (r.right <= r.left || r.bottom <= r.top || strength <= 0.f) return;

    float cx = (r.left + r.right) / 2.f;
    float cy = (r.top + r.bottom) / 2.f;
    float rx = (r.right - r.left) / 2.f;
    float ry = (r.bottom - r.top) / 2.f;
    if (rx <= 0.f || ry <= 0.f) return;

    BYTE cr = GetRValue(color), cg = GetGValue(color), cb = GetBValue(color);

    for (int y = r.top; y < r.bottom; ++y) {
        float ny = (y + 0.5f - cy) / ry;
        for (int x = r.left; x < r.right; ++x) {
            float nx = (x + 0.5f - cx) / rx;
            float d = nx * nx + ny * ny;
            if (d >= 1.f) continue;

            BYTE alpha = static_cast<BYTE>(std::min(255.f, strength * (1.f - d) * 150.f));
            if (alpha == 0) continue;

            UINT32* dst = PixelAt(x, y);
            if (!dst) continue;

            BYTE sr = static_cast<BYTE>((cr * alpha) / 255);
            BYTE sg = static_cast<BYTE>((cg * alpha) / 255);
            BYTE sb = static_cast<BYTE>((cb * alpha) / 255);

            BYTE da = (*dst >> 24) & 0xFF, dr = (*dst >> 16) & 0xFF;
            BYTE dg = (*dst >> 8) & 0xFF, db = *dst & 0xFF;

            BYTE outA = alpha + static_cast<BYTE>((da * (255 - alpha)) / 255);
            BYTE outR = sr + static_cast<BYTE>((dr * (255 - alpha)) / 255);
            BYTE outG = sg + static_cast<BYTE>((dg * (255 - alpha)) / 255);
            BYTE outB = sb + static_cast<BYTE>((db * (255 - alpha)) / 255);

            *dst = (static_cast<UINT32>(outA) << 24) | (outR << 16) | (outG << 8) | outB;
        }
    }
}

// Nearest-neighbor scaled blit of a premultiplied AlphaIcon with normal
// "src over dst" alpha compositing.
void BlitAlphaIconScaled(const AlphaIcon& icon, int dstX, int dstY, int dstW, int dstH) {
    if (!icon.valid || dstW <= 0 || dstH <= 0) return;

    for (int y = 0; y < dstH; ++y) {
        int dy = dstY + y;
        if (dy < 0 || dy >= g_bufH) continue;

        int sy = std::min(icon.h - 1, static_cast<int>((static_cast<double>(y) / dstH) * icon.h));

        for (int x = 0; x < dstW; ++x) {
            int dx = dstX + x;
            if (dx < 0 || dx >= g_bufW) continue;

            int sx = std::min(icon.w - 1, static_cast<int>((static_cast<double>(x) / dstW) * icon.w));

            UINT32 src = icon.pixels[static_cast<size_t>(sy) * icon.w + sx];
            BYTE sa = (src >> 24) & 0xFF;
            if (sa == 0) continue;

            UINT32* dst = PixelAt(dx, dy);
            if (!dst) continue;

            BYTE sr = (src >> 16) & 0xFF, sg = (src >> 8) & 0xFF, sb = src & 0xFF;
            BYTE da = (*dst >> 24) & 0xFF, dr = (*dst >> 16) & 0xFF;
            BYTE dg = (*dst >> 8) & 0xFF, db = *dst & 0xFF;

            BYTE outA = sa + static_cast<BYTE>((da * (255 - sa)) / 255);
            BYTE outR = sr + static_cast<BYTE>((dr * (255 - sa)) / 255);
            BYTE outG = sg + static_cast<BYTE>((dg * (255 - sa)) / 255);
            BYTE outB = sb + static_cast<BYTE>((db * (255 - sa)) / 255);

            *dst = (static_cast<UINT32>(outA) << 24) | (outR << 16) | (outG << 8) | outB;
        }
    }
}

// Hover "shine": a diagonal band of light sweeping top-left -> bottom-right
// across the icon (t = 0..1), only where the icon itself has pixels.
void BlitShine(const AlphaIcon& icon, int dstX, int dstY, int dstW, int dstH, float t) {
    if (!icon.valid || dstW <= 0 || dstH <= 0) return;
    const float pos = -0.25f + 1.5f * t; // band centre along the diagonal
    const float half = 0.12f;            // band half-width

    for (int y = 0; y < dstH; ++y) {
        int dy = dstY + y;
        if (dy < 0 || dy >= g_bufH) continue;
        int sy = std::min(icon.h - 1, static_cast<int>((static_cast<double>(y) / dstH) * icon.h));

        for (int x = 0; x < dstW; ++x) {
            float u = (static_cast<float>(x) / dstW + static_cast<float>(y) / dstH) * 0.5f;
            float d = std::fabs(u - pos);
            if (d >= half) continue;

            int dx = dstX + x;
            UINT32* dst = PixelAt(dx, dy);
            if (!dst) continue;

            int sx = std::min(icon.w - 1, static_cast<int>((static_cast<double>(x) / dstW) * icon.w));
            BYTE sa = (icon.pixels[static_cast<size_t>(sy) * icon.w + sx] >> 24) & 0xFF;
            float k = (1.f - d / half) * (sa / 255.f) * 0.7f;
            if (k <= 0.f) continue;

            UINT32 out = 0;
            for (int sh = 0; sh < 32; sh += 8) // premultiplied white over dst
                out |= static_cast<UINT32>(k * 255.f + ((*dst >> sh) & 0xFF) * (1.f - k)) << sh;
            *dst = out;
        }
    }
}

// ---------------------------------------------------------------------

// Layout is stored in C:\REICO\layout.txt so icon positions and sizes
// persist between application restarts.
std::wstring Config() {
    const std::wstring dir = L"C:\\REICO";

    // Create the folder if it does not already exist.
    if (!CreateDirectoryW(dir.c_str(), nullptr)) {
        DWORD err = GetLastError();
        if (err != ERROR_ALREADY_EXISTS) {
            LogF(L"Could not create C:\\REICO (error %lu)", err);
        }
    }

    return dir + L"\\layout.txt";
}

void Save() {
    std::wstring filePath = Config();

    HANDLE hFile = CreateFileW(
        filePath.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );

    if (hFile == INVALID_HANDLE_VALUE) {
        LogF(L"SAVE FAILED: CreateFileW error %lu", GetLastError());
        return;
    }

    // UTF-16 LE BOM
    const BYTE bom[] = { 0xFF, 0xFE };
    DWORD written = 0;

    if (!WriteFile(hFile, bom, sizeof(bom), &written, nullptr) ||
        written != sizeof(bom)) {

        LogF(L"SAVE FAILED: could not write BOM, error %lu", GetLastError());
        CloseHandle(hFile);
        return;
    }

    bool success = true;

    for (const auto& i : items) {

        std::wstring line =
            i.name + L"\t" +
            i.path + L"\t" +
            std::to_wstring(i.x) + L"\t" +
            std::to_wstring(i.y) + L"\t" +
            std::to_wstring(i.w) + L"\t" +
            std::to_wstring(i.h) + L"\r\n";

        DWORD bytesToWrite =
            static_cast<DWORD>(line.size() * sizeof(wchar_t));

        DWORD bytesWritten = 0;

        if (!WriteFile(
                hFile,
                line.data(),
                bytesToWrite,
                &bytesWritten,
                nullptr
            ) ||
            bytesWritten != bytesToWrite) {

            LogF(
                L"SAVE FAILED while writing item '%s', error %lu",
                i.name.c_str(),
                GetLastError()
            );

            success = false;
            break;
        }
    }

    // Force the data to disk.
    if (success && !FlushFileBuffers(hFile)) {
        LogF(L"SAVE FAILED: FlushFileBuffers error %lu", GetLastError());
        success = false;
    }

    CloseHandle(hFile);

    if (success) {
        LogF(
            L"SAVE OK: %d items written to C:\\REICO\\layout.txt",
            static_cast<int>(items.size())
        );
    }
}

void ApplySavedLayout() {
    std::wstring filePath = Config();

    HANDLE hFile = CreateFileW(
        filePath.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );

    if (hFile == INVALID_HANDLE_VALUE) {
        Log(L"No existing C:\\REICO\\layout.txt found (using defaults)");
        return;
    }

    DWORD fileSize = GetFileSize(hFile, nullptr);

    if (fileSize == INVALID_FILE_SIZE || fileSize == 0) {
        Log(L"layout.txt exists but is empty");
        CloseHandle(hFile);
        return;
    }

    std::vector<BYTE> buffer(fileSize);

    DWORD bytesRead = 0;

    if (!ReadFile(
            hFile,
            buffer.data(),
            fileSize,
            &bytesRead,
            nullptr
        )) {

        LogF(L"LOAD FAILED: ReadFile error %lu", GetLastError());
        CloseHandle(hFile);
        return;
    }

    CloseHandle(hFile);

    if (bytesRead < 2) {
        Log(L"layout.txt is too small");
        return;
    }

    // Verify UTF-16 LE BOM.
    if (buffer[0] != 0xFF || buffer[1] != 0xFE) {
        Log(L"layout.txt has invalid/missing UTF-16 BOM");
        return;
    }

    size_t wcharCount = (bytesRead - 2) / sizeof(wchar_t);

    std::wstring contents(
        reinterpret_cast<const wchar_t*>(buffer.data() + 2),
        wcharCount
    );

    std::wistringstream f(contents);

    std::wstring line;
    int matched = 0;
    int lines = 0;

    while (std::getline(f, line)) {

        if (line.empty())
            continue;

        lines++;

        std::wistringstream s(line);

        std::wstring name;
        std::wstring path;
        int x, y, ww, hh;

        if (!std::getline(s, name, L'\t'))
            continue;

        if (!std::getline(s, path, L'\t'))
            continue;

        if (!(s >> x >> y >> ww >> hh))
            continue;

        for (auto& it : items) {

            if (it.path == path) {

                it.x = x;
                it.y = y;
                it.w = ww;
                it.h = hh;

                matched++;
                break;
            }
        }
    }

    LogF(
        L"Loaded layout: %d lines, %d items matched",
        lines,
        matched
    );
}

void FreeIcons() {
    for (auto& it : items) {
        if (it.hIcon) {
            DestroyIcon(it.hIcon);
            it.hIcon = nullptr;
        }
        it.alpha = AlphaIcon{};
    }
}

// =====================================================================
// FIX #1 (low resolution icons): pull from the system's jumbo (256x256)
// image list instead of the 32x32 "large icon" list, so icons are
// downscaled from a big source instead of upscaled from a tiny one.
// =====================================================================
HICON GetJumboIcon(const std::wstring& path) {
    SHFILEINFOW sfi{};

    if (!SHGetFileInfoW(path.c_str(), 0, &sfi, sizeof(sfi), SHGFI_SYSICONINDEX)) {
        // Retry using file-attribute-based lookup, which works even if
        // the shortcut's target is missing/broken.
        if (!SHGetFileInfoW(path.c_str(), FILE_ATTRIBUTE_NORMAL, &sfi, sizeof(sfi),
                             SHGFI_SYSICONINDEX | SHGFI_USEFILEATTRIBUTES)) {
            return nullptr;
        }
    }

    IImageList* imageList = nullptr;
    if (FAILED(SHGetImageList(SHIL_JUMBO, IID_IImageList,
                               reinterpret_cast<void**>(&imageList))) || !imageList) {
        return nullptr;
    }

    HICON hIcon = nullptr;
    imageList->GetIcon(sfi.iIcon, ILD_TRANSPARENT, &hIcon);
    imageList->Release();

    return hIcon;
}

void LoadIcons() {
    int ok = 0, fail = 0;

    for (auto& it : items) {
        it.hIcon = GetJumboIcon(it.path);

        if (!it.hIcon) {
            // Last-resort fallback: classic 32x32 large-icon lookup for
            // whatever the jumbo list couldn't resolve.
            SHFILEINFOW sfi2{};
            SHGetFileInfoW(
                it.path.c_str(),
                FILE_ATTRIBUTE_NORMAL,
                &sfi2,
                sizeof(sfi2),
                SHGFI_ICON | SHGFI_LARGEICON | SHGFI_USEFILEATTRIBUTES
            );
            it.hIcon = sfi2.hIcon;
        }

        if (it.hIcon) {
            it.alpha = ExtractIconAlpha(it.hIcon);
            ok++;
        } else {
            fail++;
            LogF(L"  ICON FAILED: %s | %s", it.name.c_str(), it.path.c_str());
        }
    }

    LogF(L"LoadIcons: %d ok, %d failed (jumbo 256x256 where available)", ok, fail);
}

// =====================================================================
// FIX #4 (only finds ~5 items): a real Windows desktop is a MERGE of
// two folders - the per-user desktop and the shared "public/all users"
// desktop. Anything an installer puts on the desktop for every user
// lives in the public one, which was never being scanned before.
// =====================================================================
void ScanFolder(const std::wstring& folder, std::vector<Item>& out, int& scanned, int& skipped) {
    if (folder.empty()) return;

    std::wstring pattern = folder + L"\\*";

    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW(pattern.c_str(), &fd);

    if (hFind == INVALID_HANDLE_VALUE) {
        LogF(L"FindFirstFileW FAILED for %s, GetLastError=%lu", folder.c_str(), GetLastError());
        return;
    }

    do {
        std::wstring name = fd.cFileName;

        if (name == L"." || name == L"..")
            continue;

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) {
            skipped++;
            continue;
        }

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_SYSTEM) {
            skipped++;
            continue;
        }

        if (_wcsicmp(name.c_str(), L"desktop.ini") == 0) {
            skipped++;
            continue;
        }

        Item it;
        it.path = folder + L"\\" + name;

        size_t dot = name.find_last_of(L'.');
        bool isLnk = (dot != std::wstring::npos &&
                      _wcsicmp(name.substr(dot).c_str(), L".lnk") == 0);
        it.name = isLnk ? name.substr(0, dot) : name;

        out.push_back(it);
        scanned++;

    } while (FindNextFileW(hFind, &fd));

    FindClose(hFind);
}

void ApplyIconTint(); // defined in the GLASS section below

void ScanDesktop() {
    Log(L"--- ScanDesktop() starting ---");

    FreeIcons();
    items.clear();

    wchar_t personal[MAX_PATH]{};
    wchar_t common[MAX_PATH]{};

    HRESULT hrPersonal = SHGetFolderPathW(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr, 0, personal);
    HRESULT hrCommon = SHGetFolderPathW(nullptr, CSIDL_COMMON_DESKTOPDIRECTORY, nullptr, 0, common);

    int scanned = 0, skipped = 0;

    if (SUCCEEDED(hrPersonal)) {
        LogF(L"Personal desktop: %s", personal);
        ScanFolder(personal, items, scanned, skipped);
    } else {
        LogF(L"SHGetFolderPathW (personal) FAILED, hr=0x%08X", (unsigned)hrPersonal);
    }

    if (SUCCEEDED(hrCommon)) {
        LogF(L"Public desktop: %s", common);
        ScanFolder(common, items, scanned, skipped);
    } else {
        LogF(L"SHGetFolderPathW (public) FAILED, hr=0x%08X", (unsigned)hrCommon);
    }

    // Installers sometimes drop the same shortcut into both the
    // personal and public desktop folders - de-dup by display name so
    // it doesn't show up twice.
    std::vector<Item> deduped;
    deduped.reserve(items.size());
    for (auto& it : items) {
        bool dup = false;
        for (auto& d : deduped) {
            if (_wcsicmp(d.name.c_str(), it.name.c_str()) == 0) { dup = true; break; }
        }
        if (!dup) deduped.push_back(std::move(it));
    }
    items = std::move(deduped);

    const int cellW = 90, cellH = 90, gap = 16, cols = 10;
    for (size_t i = 0; i < items.size(); ++i) {
        int col = static_cast<int>(i) % cols;
        int row = static_cast<int>(i) / cols;
        items[i].x = 40 + col * (cellW + gap);
        items[i].y = 40 + row * (cellH + gap);
        items[i].w = cellW;
        items[i].h = cellH;
    }

    LogF(L"Scan done: %d items total, %d skipped (hidden/system)", (int)items.size(), skipped);

    ApplySavedLayout();
    LoadIcons();
    ApplyIconTint();

    Log(L"--- ScanDesktop() finished ---");
}

int Hit(POINT p) {
    for (int i = static_cast<int>(items.size()) - 1; i >= 0; --i) {
        const Item& item = items[i];

        if (p.x >= item.x &&
            p.x <= item.x + item.w &&
            p.y >= item.y &&
            p.y <= item.y + item.h) {
            return i;
        }
    }

    return -1;
}

bool HitHandle(const Item& item, POINT p) {
    int hx = item.x + item.w - HANDLE_SIZE;
    int hy = item.y + item.h - HANDLE_SIZE;

    return p.x >= hx && p.x <= item.x + item.w &&
           p.y >= hy && p.y <= item.y + item.h;
}

void Launch(const Item& item) {
    if (item.path.empty())
        return;

    LogF(L"Launching: %s", item.path.c_str());

    ShellExecuteW(
        nullptr,
        L"open",
        item.path.c_str(),
        nullptr,
        nullptr,
        SW_SHOWNORMAL
    );
}

// =====================================================================
// FIX #3: lightweight animation timer. Only runs while something is
// actually mid-animation (hover fade in/out or a double-click pulse),
// so the overlay is idle (no timer, no redraws) the rest of the time.
// =====================================================================
bool g_animTimerOn = false;
const UINT_PTR ANIM_TIMER_ID = 1;

const UINT_PTR ICON_REFRESH_TIMER_ID = 3;

// FIX #7 (F5 only "worked" right after clicking the overlay): WM_KEYDOWN
// only reaches the window that currently has keyboard focus, and this
// overlay is deliberately kept at the bottom of the z-order (FIX #6) so
// it almost never has focus. A global hotkey (WM_HOTKEY) is delivered
// regardless of focus, which is what a background overlay actually
// needs. Using plain F5 as a *global* hotkey would break F5-refresh in
// every other app system-wide, so this uses Ctrl+Shift+F5 instead; the
// old WM_KEYDOWN VK_F5 case is left in place too, as a bonus for whenever
// the overlay does happen to have focus.
const int HOTKEY_RESCAN_ID = 1;

// How many automatic "F5" retries to do after boot before giving up.
// Shell32's icon cache can take several seconds to warm up, so a single
// early retry isn't always enough.
const int ICON_REFRESH_MAX_ATTEMPTS = 6;
int g_iconRefreshAttempts = 0;

// FIX #5 (icons missing on first boot), take 2: checking alpha.valid/
// hIcon to decide whether a rescan is still needed doesn't work,
// because the classic 32x32 icon fallback in LoadIcons() almost always
// succeeds with SOME icon (just the wrong/generic one) even when the
// real jumbo icon lookup is still failing - so that check reports
// "nothing missing" and kills this timer after the very first tick,
// before the shell has actually finished warming up. There is no
// reliable signal for "the icon is visually wrong" to check against, so
// don't try to detect it - just unconditionally do a real ScanDesktop()
// (the same thing manual F5 does) a fixed number of times, a couple of
// seconds apart, then stop. Cheap and harmless to do a few extra times.
void RefreshMissingIcons(HWND h) {
    g_iconRefreshAttempts++;

    LogF(L"Auto-F5 (attempt %d/%d): rescanning desktop...",
         g_iconRefreshAttempts, ICON_REFRESH_MAX_ATTEMPTS);

    ScanDesktop();
    Render(h);

    if (g_iconRefreshAttempts >= ICON_REFRESH_MAX_ATTEMPTS) {
        Log(L"Auto-F5 icon refresh: done with scheduled attempts (manual Ctrl+Shift+F5 still works)");
        KillTimer(h, ICON_REFRESH_TIMER_ID);
    }
}

// Hover moved to another icon (or off all icons): a newly hovered icon starts its shine sweep.
void SetHover(int k) {
    if (k == hoverIndex) return;
    hoverIndex = k;
    if (k >= 0 && k < static_cast<int>(items.size())) items[k].shineStart = GetTickCount();
}

void EnsureAnimTimer(HWND h) {
    if (!g_animTimerOn) {
        SetTimer(h, ANIM_TIMER_ID, 16, nullptr); // ~60fps
        g_animTimerOn = true;
    }
}

void StopAnimTimerIfIdle(HWND h) {
    bool anyActive = (hoverIndex >= 0);

    if (!anyActive) {
        for (auto& it : items) {
            if (it.pulseStart != 0 || it.shineStart != 0 || std::fabs(it.hoverT) > 0.001f) {
                anyActive = true;
                break;
            }
        }
    }

    if (!anyActive && g_animTimerOn) {
        KillTimer(h, ANIM_TIMER_ID);
        g_animTimerOn = false;
    }
}

// =====================================================================
// Auto-hide when something is genuinely fullscreen.
//
// A maximized window ("shrinked" - still leaves room for the taskbar)
// covers the monitor's *work area*, not the whole monitor. A true
// fullscreen/exclusive app (a game, a video player in fullscreen, etc.)
// covers the *entire* monitor, taskbar space included. Comparing the
// foreground window's rect against the full monitor rect (not the work
// area) is the standard way to tell the two apart.
// =====================================================================
const UINT_PTR FULLSCREEN_TIMER_ID = 2;
bool g_overlayHidden = false;

bool IsForegroundWindowFullscreen() {
    HWND fg = GetForegroundWindow();
    if (!fg || fg == g_hwnd) return false;

    DWORD pid = 0; // REICO's own windows (dock, panel, pop-ups) are never "a fullscreen app"
    GetWindowThreadProcessId(fg, &pid);
    if (pid == GetCurrentProcessId()) return false;

    wchar_t cls[256]{};
    GetClassNameW(fg, cls, 256);

    // Ignore the desktop/shell itself - it always "covers the screen"
    // but obviously isn't a fullscreen app stealing focus.
    if (_wcsicmp(cls, L"Progman") == 0 ||
        _wcsicmp(cls, L"WorkerW") == 0 ||
        _wcsicmp(cls, L"Shell_TrayWnd") == 0 ||
        _wcsicmp(cls, L"Button") == 0) {
        return false;
    }

    if (IsIconic(fg)) return false; // minimized

    RECT wr{};
    if (!GetWindowRect(fg, &wr)) return false;

    HMONITOR mon = MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) return false;

    return wr.left <= mi.rcMonitor.left &&
           wr.top <= mi.rcMonitor.top &&
           wr.right >= mi.rcMonitor.right &&
           wr.bottom >= mi.rcMonitor.bottom;
}

void UpdateFullscreenVisibility(HWND h) {
    bool shouldHide = IsForegroundWindowFullscreen();

    if (shouldHide && !g_overlayHidden) {
        g_overlayHidden = true;
        ShowWindow(h, SW_HIDE);
        Log(L"Fullscreen app detected - overlay hidden, waiting in background");
    } else if (!shouldHide && g_overlayHidden) {
        g_overlayHidden = false;
        ShowWindow(h, SW_SHOWNA); // show without stealing focus

        // FIX #6: showing a hidden window re-inserts it at the TOP of the
        // z-order, which is exactly what made it flash on top of whatever
        // window the user had just clicked/restored. Sink it straight back
        // to the bottom before rendering.
        SetWindowPos(h, HWND_BOTTOM, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

        Render(h);
        Log(L"Fullscreen app gone - overlay restored");
    } else if (!g_overlayHidden) {
        // FIX #6 continued: re-assert bottom-of-z-order on every tick too.
        // Various normal window activity (clicking/restoring other
        // windows, apps that call SetWindowPos themselves, etc.) can
        // otherwise nudge this window's z-order back up over time.
        SetWindowPos(h, HWND_BOTTOM, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
}

// =====================================================================
// Start on startup: register ourselves in the per-user Run key so
// Windows launches this automatically at login. Safe/cheap to redo on
// every launch (idempotent - just overwrites the same value).
// =====================================================================
void EnsureRunOnStartup() {
    wchar_t exePath[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, exePath, MAX_PATH)) return;

    std::wstring quoted = L"\"" + std::wstring(exePath) + L"\"";

    HKEY key = nullptr;
    LONG res = RegCreateKeyExW(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
        0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr
    );

    if (res != ERROR_SUCCESS) {
        LogF(L"EnsureRunOnStartup: RegCreateKeyExW failed, err=%ld", res);
        return;
    }

    res = RegSetValueExW(
        key, L"REICO", 0, REG_SZ,
        reinterpret_cast<const BYTE*>(quoted.c_str()),
        static_cast<DWORD>((quoted.size() + 1) * sizeof(wchar_t))
    );

    // the app used to be called RE4InventoryDesktop: drop that entry so the old exe
    // doesn't start at login as well
    RegDeleteValueW(key, L"RE4InventoryDesktop");

    RegCloseKey(key);

    if (res == ERROR_SUCCESS)
        Log(L"Registered for startup (HKCU Run key)");
    else
        LogF(L"EnsureRunOnStartup: RegSetValueExW failed, err=%ld", res);
}

// Renders the whole scene into the ARGB back buffer and pushes it to
// the screen via UpdateLayeredWindow. This is the *only* place pixels
// reach the screen now - there's no normal WM_PAINT drawing anymore.
void Render(HWND h) {
    if (!g_bits) return;

    ClearBuffer();

    HDC dc = g_memDC;
    SetBkMode(dc, TRANSPARENT);

    for (int i = 0; i < static_cast<int>(items.size()); ++i) {
        Item& item = items[i];

        // ---- hover / pulse animation state ----------------------------
        bool hovered = (!editMode && i == hoverIndex);
        float target = hovered ? 1.f : 0.f;
        item.hoverT += (target - item.hoverT) * 0.25f;
        if (std::fabs(item.hoverT - target) < 0.004f) item.hoverT = target;

        float pulseScale = 1.f;
        float pulseGlow = 0.f;
        if (item.pulseStart != 0) {
            DWORD elapsed = GetTickCount() - item.pulseStart;
            const DWORD PULSE_MS = 260;
            if (elapsed < PULSE_MS) {
                float t = elapsed / static_cast<float>(PULSE_MS);
                pulseScale = 1.f + 0.22f * sinf(t * 3.14159f) * (1.f - t * 0.3f);
                pulseGlow = sinf(t * 3.14159f);
            } else {
                item.pulseStart = 0;
            }
        }

        // ---- hover effect (hoverEffect= in glass.txt) --------------------
        float t = item.hoverT, hoverScale = 1.f, raise = 0.f, hoverGlow = 0.f;
        if (g_hoverEffect == HOVER_LIFT) {
            hoverScale = 1.f + 0.12f * t; raise = 6.f * t; hoverGlow = t;
        } else if (g_hoverEffect == HOVER_BOUNCE) {
            // hoverT eases in, so the sin term overshoots on the way up = a pop
            float b = t + 0.6f * sinf(t * 3.14159f);
            hoverScale = 1.f + 0.12f * b; raise = 10.f * b; hoverGlow = 0.6f * t;
        } else if (g_hoverEffect == HOVER_SHINE) {
            hoverGlow = 0.35f * t; // the sweep itself is drawn after the icon
        }
        float scale = hoverScale * pulseScale;

        // No label anymore - the icon fills the whole tile.
        int baseIconW = item.w;
        int baseIconH = item.h;
        int drawW = static_cast<int>(baseIconW * scale);
        int drawH = static_cast<int>(baseIconH * scale);
        int drawX = item.x - (drawW - baseIconW) / 2;
        int drawY = item.y - (drawH - baseIconH) / 2 - static_cast<int>(raise);

        float glowStrength = std::max(hoverGlow, pulseGlow);
        if (glowStrength > 0.02f) {
            RECT glowRect{ drawX - 12, drawY - 12, drawX + drawW + 12, drawY + drawH + 12 };
            BlendGlow(glowRect, RGB(255, 230, 150), glowStrength);
        }

        if (item.alpha.valid) {
            BlitAlphaIconScaled(item.alpha, drawX, drawY, drawW, drawH);

            if (item.shineStart != 0) {
                const DWORD SHINE_MS = 650;
                DWORD el = GetTickCount() - item.shineStart;
                if (g_hoverEffect == HOVER_SHINE && el < SHINE_MS)
                    BlitShine(item.alpha, drawX, drawY, drawW, drawH, el / static_cast<float>(SHINE_MS));
                else
                    item.shineStart = 0;
            }
        } else {
            RECT iconRect{ item.x, item.y, item.x + item.w, item.y + item.h };
            FillOpaqueRect(dc, iconRect, RGB(80, 30, 30));
        }

        if (i == selected && editMode) {
            RECT full{ item.x, item.y, item.x + item.w, item.y + item.h };

            FrameRect(dc, &full, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
            RECT top{ full.left, full.top, full.right, full.top + 1 };
            RECT bottom{ full.left, full.bottom - 1, full.right, full.bottom };
            RECT left{ full.left, full.top, full.left + 1, full.bottom };
            RECT right{ full.right - 1, full.top, full.right, full.bottom };
            FillAlphaRect(top, 255);
            FillAlphaRect(bottom, 255);
            FillAlphaRect(left, 255);
            FillAlphaRect(right, 255);

            RECT grip{
                item.x + item.w - HANDLE_SIZE,
                item.y + item.h - HANDLE_SIZE,
                item.x + item.w,
                item.y + item.h
            };
            FillOpaqueRect(dc, grip, RGB(255, 255, 255));
        }
    }

    POINT ptSrc{ 0, 0 };
    POINT ptDst{ 0, 0 };
    SIZE size{ g_bufW, g_bufH };
    BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };

    UpdateLayeredWindow(h, nullptr, &ptDst, &size, g_memDC, &ptSrc, 0, &bf, ULW_ALPHA);
}

// =====================================================================
// GLASS: translucent teal "frosted glass" for chosen windows
// (File Explorer + cmd by default, Windows 10).
//
// Designed to be nearly free when idle:
//   * NO polling - it listens for window events (SetWinEventHook) and
//     only does work when a matching window is shown or focused.
//   * Default mode uses plain blur-behind, which is much cheaper on the
//     GPU than acrylic (acrylic is the thing that makes dragging
//     windows laggy on Windows 10). "mode=acrylic" in glass.txt opts in.
//
// How it works per window:
//   1. SetLayeredWindowAttributes -> whole window slightly see-through.
//   2. SetWindowCompositionAttribute (undocumented but widely used by
//      TranslucentTB etc.) -> DWM blurs + tints whatever is behind it.
// Everything is undone on exit or with Ctrl+Shift+F6 (toggle).
//
// Settings live in C:\REICO\glass.txt (created on first run).
// =====================================================================

struct GlassAccentPolicy {
    int   AccentState;
    int   AccentFlags;
    DWORD GradientColor; // 0xAABBGGRR
    int   AnimationId;
};

struct GlassCompAttrData {
    int    Attrib;
    PVOID  pvData;
    SIZE_T cbData;
};

typedef BOOL (WINAPI *PFN_SetWindowCompositionAttribute)(HWND, GlassCompAttrData*);

struct GlassConfig {
    BYTE windowAlpha = 215;             // 255 = opaque, lower = more see-through
    BYTE tintR = 0, tintG = 150, tintB = 150;
    BYTE tintAlpha = 110;               // strength of the teal tint over the blur
    bool acrylic = false;               // false = light blur, true = acrylic (heavier)
    bool layered = true;                // true = see-through window (alpha); false = accent only
    int  radius = 0;                    // rounded corner radius in px, 0 = square (Win10 can't round Explorer cleanly)
    int  accentOverride = -1;           // >=0 forces a raw accent state (experiments)
    bool iconTint = true;               // also tint the desktop icons with the glass tint
    int  iconTintAlpha = -1;            // -1 = full effect (255); 0-255 blends with original colours
    int  iconBrightness = 120;          // % brightness of the tinted icons (100 = tint colour at white)
    int  iconEdgeBlur = 3;              // soft-edge radius in source px (0 = hard edges)
    int  iconOpacity = 90;              // % opacity of the icons (100 = solid)
    int  clip = 1;                      // 1 = clip to the visible frame (no tint spill), 0 = corners only
    bool liquid = true;                 // iPhone-style "liquid glass": clear glass, rim light + gloss
    bool iconGlass = true;              // same rim light + gloss on the desktop icons
    int  iconGlassAmount = 65;          // % glass; the rest is the solid tinted icon (recognizability)
    bool nowPlaying = true;             // liquid-glass pop-up when a song starts / stops
    bool dock = false;                  // REICO's glass taskbar at the top (Windows' taskbar auto-hides)
    bool btnGames = true, btnTaskMgr = true, btnNotes = true; // the round buttons at the top-left
    bool downloads = true;              // take browser downloads (REICO Downloads extension)
    std::vector<std::wstring> classes{ L"CabinetWClass" }; // cmd is NOT default: too heavy
};

struct GlassRec {
    HWND hwnd;
    bool hadLayered; // window was already layered before we touched it
    int  lastW = 0;  // size/zoom state the rounded region was built for
    int  lastH = 0;
    RECT lastInset{}; // visible frame inside the window rect, as the region was built for
    bool zoomed = false;
    bool dragging = false; // true between MOVESIZESTART and MOVESIZEEND (lite mode)
    HWND sheen = nullptr;  // click-through "liquid glass" highlight window sitting just above hwnd
    HWND under = nullptr;  // tint, painted in a window just BELOW hwnd, cut to the exact rounded shape
    int  sheenW = 0, sheenH = 0, sheenR = -1; // size the sheen was last painted for
};

const int HOTKEY_GLASS_ID = 2;
const UINT_PTR GLASS_TIMER_ID = 5;
const UINT_PTR GLASS_FAST_TIMER_ID = 6; // only runs while a glass window is being moved/resized

// Tiny diagnostic log (first 20 lines only) so geometry problems can be
// inspected: C:\REICO\glass_debug.txt
int g_glassDebugCount = 0;
void GlassDebug(const wchar_t* fmt, ...) {
    if (g_glassDebugCount++ >= 40) return;
    FILE* f = _wfopen(L"C:\\REICO\\glass_debug.txt", L"a, ccs=UTF-8");
    if (!f) return;
    va_list args;
    va_start(args, fmt);
    vfwprintf(f, fmt, args);
    va_end(args);
    fwprintf(f, L"\n");
    fclose(f);
}

GlassConfig g_glassCfg;
std::vector<GlassRec> g_glassRecs;
bool g_glassOn = true;
PFN_SetWindowCompositionAttribute g_setWCA = nullptr;
HWINEVENTHOOK g_hookShow = nullptr;
HWINEVENTHOOK g_hookFocus = nullptr;

BYTE GlassClampByte(int v, int lo = 0) {
    if (v < lo) v = lo;
    if (v > 255) v = 255;
    return static_cast<BYTE>(v);
}

void LoadGlassConfig() {
    Config(); // side effect: makes sure C:\REICO exists
    const char* path = "C:\\REICO\\glass.txt";
    g_hoverEffect = HOVER_LIFT; // default when the key is missing (also on reload)

    std::ifstream in(path);
    if (!in) {
        std::ofstream out(path);
        out << "# Glass settings - restart the app after editing.\n"
               "# windowAlpha: 80-255 (255 = solid, lower = more see-through)\n"
               "windowAlpha=215\n"
               "# tint: R,G,B of the glass colour (teal)\n"
               "tint=0,150,150\n"
               "# tintAlpha: 0-255, how strong the teal is\n"
               "tintAlpha=110\n"
               "# mode: blur (light on resources) or acrylic (fancier, heavier)\n"
               "mode=blur\n"
               "# layered: 1 = whole window see-through (blur may not show), 0 = accent only\n"
               "layered=1\n"
               "# radius: rounded corners in px (0 = square)\n"
               "radius=0\n"
               "# iconTint: 1 = desktop icons become black & white, then take the tint colour\n"
               "# (like iPhone tinted icons)\n"
               "iconTint=1\n"
               "# iconTintAlpha: -1 = full effect, or 0-255 to mix some original colour back in\n"
               "iconTintAlpha=-1\n"
               "# iconBrightness: percent. 100 = white parts of icons show the exact tint colour,\n"
               "# higher = brighter icons\n"
               "iconBrightness=120\n"
               "# iconEdgeBlur: soft feathered icon edges, 0 = off (try 2-8)\n"
               "iconEdgeBlur=3\n"
               "# iconOpacity: percent, 100 = solid, lower = more see-through icons\n"
               "iconOpacity=90\n"
               "# clip: 1 = glass stops exactly at the visible window edge (but the invisible\n"
               "# resize border is lost - resize with Win+arrows / Alt+Space, or toggle glass\n"
               "# with Ctrl+Shift+F6). 0 = only the corners are cut (edges still resizable).\n"
               "clip=1\n"
               "# liquid: 1 = iPhone-style liquid glass (clear, light rim + gloss, no tint spill past\n"
               "# the edges; mode= is ignored because Windows 10 blur can't be cut to rounded corners)\n"
               "# 0 = old blur/acrylic glass\n"
               "liquid=1\n"
               "# iconGlass: 1 = desktop icons get the same glass rim + gloss\n"
               "iconGlass=1\n"
               "# iconGlassAmount: percent glass, the rest is the solid tinted icon (lower = easier to recognise)\n"
               "iconGlassAmount=65\n"
               "# hoverEffect: lift, shine, bounce or none\n"
               "hoverEffect=lift\n"
               "# nowPlaying: 1 = liquid-glass pop-up at the top for 5 s when a song starts or stops\n"
               "nowPlaying=1\n"
               "# dock: 1 = rounded glass taskbar centred at the top; Windows' own taskbar is set to\n"
               "# auto-hide while it's on and restored when it's off\n"
               "dock=0\n"
               "# the round buttons at the top-left (with the dock): game launcher, Task Manager, board\n"
               "btnGames=1\n"
               "btnTaskMgr=1\n"
               "btnNotes=1\n"
               "# downloads: 1 = REICO's multi-connection download manager takes browser downloads\n"
               "# (needs the REICO Downloads extension from C:\\REICO\\extension)\n"
               "downloads=1\n"
               "# one class= line per window type to affect\n"
               "class=CabinetWClass\n"
               "# cmd (ConsoleWindowClass) is heavy with blur - not enabled by default.\n"
               "# class=ConsoleWindowClass\n";
        Log(L"Glass: wrote default C:\\REICO\\glass.txt");
        return;
    }

    bool classesReset = false;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();

        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);

        if (key == "windowAlpha") {
            g_glassCfg.windowAlpha = GlassClampByte(std::atoi(val.c_str()), 80);
        } else if (key == "tintAlpha") {
            g_glassCfg.tintAlpha = GlassClampByte(std::atoi(val.c_str()));
        } else if (key == "tint") {
            int r = 0, g = 0, b = 0;
            if (sscanf(val.c_str(), "%d,%d,%d", &r, &g, &b) == 3) {
                g_glassCfg.tintR = GlassClampByte(r);
                g_glassCfg.tintG = GlassClampByte(g);
                g_glassCfg.tintB = GlassClampByte(b);
            }
        } else if (key == "mode") {
            g_glassCfg.acrylic = (val == "acrylic");
        } else if (key == "layered") {
            g_glassCfg.layered = (std::atoi(val.c_str()) != 0);
        } else if (key == "radius") {
            int rr = std::atoi(val.c_str());
            g_glassCfg.radius = rr < 0 ? 0 : (rr > 40 ? 40 : rr);
        } else if (key == "iconTint") {
            g_glassCfg.iconTint = (std::atoi(val.c_str()) != 0);
        } else if (key == "iconTintAlpha") {
            g_glassCfg.iconTintAlpha = std::atoi(val.c_str());
        } else if (key == "iconEdgeBlur") {
            int eb = std::atoi(val.c_str());
            g_glassCfg.iconEdgeBlur = eb < 0 ? 0 : (eb > 24 ? 24 : eb);
        } else if (key == "iconOpacity") {
            int op = std::atoi(val.c_str());
            g_glassCfg.iconOpacity = op < 10 ? 10 : (op > 100 ? 100 : op);
        } else if (key == "iconBrightness") {
            int bb = std::atoi(val.c_str());
            g_glassCfg.iconBrightness = bb < 40 ? 40 : (bb > 300 ? 300 : bb);
        } else if (key == "clip") {
            g_glassCfg.clip = std::atoi(val.c_str()) != 0 ? 1 : 0;
        } else if (key == "liquid") {
            g_glassCfg.liquid = std::atoi(val.c_str()) != 0;
        } else if (key == "iconGlass") {
            g_glassCfg.iconGlass = std::atoi(val.c_str()) != 0;
        } else if (key == "iconGlassAmount") {
            int ga = std::atoi(val.c_str());
            g_glassCfg.iconGlassAmount = ga < 0 ? 0 : (ga > 100 ? 100 : ga);
        } else if (key == "dock") {
            g_glassCfg.dock = std::atoi(val.c_str()) != 0;
        } else if (key == "btnGames") {
            g_glassCfg.btnGames = std::atoi(val.c_str()) != 0;
        } else if (key == "btnTaskMgr") {
            g_glassCfg.btnTaskMgr = std::atoi(val.c_str()) != 0;
        } else if (key == "btnNotes") {
            g_glassCfg.btnNotes = std::atoi(val.c_str()) != 0;
        } else if (key == "downloads") {
            g_glassCfg.downloads = std::atoi(val.c_str()) != 0;
        } else if (key == "nowPlaying") {
            g_glassCfg.nowPlaying = std::atoi(val.c_str()) != 0;
        } else if (key == "hoverEffect") {
            g_hoverEffect = val == "shine" ? HOVER_SHINE : val == "bounce" ? HOVER_BOUNCE
                          : val == "none" ? HOVER_NONE : HOVER_LIFT;
        } else if (key == "accent") {
            g_glassCfg.accentOverride = std::atoi(val.c_str());
        } else if (key == "class") {
            if (!classesReset) { g_glassCfg.classes.clear(); classesReset = true; }
            g_glassCfg.classes.push_back(std::wstring(val.begin(), val.end()));
        }
    }
}

// Soft edges: separable box blur of one float plane (zero outside the
// image, so the icon's silhouette feathers outward as well as inward).
void BoxBlurPlane(std::vector<float>& plane, int w, int h, int R) {
    if (R <= 0 || w <= 0 || h <= 0) return;

    std::vector<float> tmp(plane.size());
    const float inv = 1.f / static_cast<float>(2 * R + 1);

    // horizontal
    for (int y = 0; y < h; ++y) {
        const float* row = &plane[static_cast<size_t>(y) * w];
        float* out = &tmp[static_cast<size_t>(y) * w];
        float sum = 0.f;
        for (int x = 0; x <= R && x < w; ++x) sum += row[x];
        for (int x = 0; x < w; ++x) {
            out[x] = sum * inv;
            if (x + R + 1 < w) sum += row[x + R + 1];
            if (x - R >= 0) sum -= row[x - R];
        }
    }

    // vertical
    for (int x = 0; x < w; ++x) {
        float sum = 0.f;
        for (int y = 0; y <= R && y < h; ++y) sum += tmp[static_cast<size_t>(y) * w + x];
        for (int y = 0; y < h; ++y) {
            plane[static_cast<size_t>(y) * w + x] = sum * inv;
            if (y + R + 1 < h) sum += tmp[static_cast<size_t>(y + R + 1) * w + x];
            if (y - R >= 0) sum -= tmp[static_cast<size_t>(y - R) * w + x];
        }
    }
}

// Feathers only the icon's OUTLINE: the alpha channel is blurred, but the
// colours inside the icon stay exactly as they were (so detail is not
// smeared). Where the blur creates new semi-transparent pixels outside the
// old outline, they borrow the blurred neighbouring colour.
void FeatherIconEdges(AlphaIcon& icon, int R) {
    if (!icon.valid || R <= 0) return;

    const int w = icon.w, h = icon.h;
    const size_t n = static_cast<size_t>(w) * h;

    std::vector<float> pa(n), pr(n), pg(n), pb(n);
    for (size_t i = 0; i < n; ++i) {
        UINT32 px = icon.pixels[i];
        pa[i] = static_cast<float>((px >> 24) & 0xFF);
        pr[i] = static_cast<float>((px >> 16) & 0xFF);
        pg[i] = static_cast<float>((px >> 8) & 0xFF);
        pb[i] = static_cast<float>(px & 0xFF);
    }

    std::vector<float> ba = pa, br = pr, bg = pg, bb = pb;
    for (int pass = 0; pass < 2; ++pass) { // two passes ~ a smooth gaussian
        BoxBlurPlane(ba, w, h, R);
        BoxBlurPlane(br, w, h, R);
        BoxBlurPlane(bg, w, h, R);
        BoxBlurPlane(bb, w, h, R);
    }

    for (size_t i = 0; i < n; ++i) {
        float newA = std::min(255.f, ba[i]);
        if (newA < 0.5f) { icon.pixels[i] = 0; continue; }

        float sr, sg, sb; // straight (un-premultiplied) colour
        if (pa[i] > 0.5f) {
            sr = std::min(255.f, pr[i] * 255.f / pa[i]);
            sg = std::min(255.f, pg[i] * 255.f / pa[i]);
            sb = std::min(255.f, pb[i] * 255.f / pa[i]);
        } else {
            sr = std::min(255.f, br[i] * 255.f / std::max(1.f, ba[i]));
            sg = std::min(255.f, bg[i] * 255.f / std::max(1.f, ba[i]));
            sb = std::min(255.f, bb[i] * 255.f / std::max(1.f, ba[i]));
        }

        BYTE a = static_cast<BYTE>(newA);
        BYTE r = static_cast<BYTE>(sr * newA / 255.f);
        BYTE g = static_cast<BYTE>(sg * newA / 255.f);
        BYTE b = static_cast<BYTE>(sb * newA / 255.f);
        icon.pixels[i] = (static_cast<UINT32>(a) << 24) | (r << 16) | (g << 8) | b;
    }
}

// Turns an icon into iPhone "clear" style liquid glass. The wallpaper
// (often a live one) can't be read, so instead of blurring it the icon is
// made of translucent frosted glass the wallpaper shows through:
//   * body: faint frost in the tint colour, with the logo's light parts as brighter
//     glass (so the logo stays readable, as an etched shape);
//   * thickness: a soft darker band inside the edges facing away from the
//     light, like light bending through the rim of a lens;
//   * specular rim: a thin bright line, strongest on the top-left (facing
//     the light) and softer on the opposite edge;
//   * gloss: a curved highlight across the top.
// Edges come from the icon's alpha blurred at two widths (wide = bevel,
// narrow = rim), so they survive scaling down to tile size.
void GlassifyIcon(AlphaIcon& icon) {
    if (!icon.valid) return;

    const int w = icon.w, h = icon.h;
    const size_t n = static_cast<size_t>(w) * h;
    const int Rw = std::max(3, w / 20);  // bevel width (~4 px on a 90 px tile)
    const int Rn = std::max(1, w / 96);  // rim width (~1 px on a 90 px tile)

    std::vector<float> A(n), L(n), Sr(n), Sg(n), Sb(n); // alpha, luma, straight colour 0..1
    for (size_t i = 0; i < n; ++i) {
        UINT32 px = icon.pixels[i];
        int a = (px >> 24) & 0xFF;
        A[i] = static_cast<float>(a);
        if (a) {
            float r = std::min(255.f, ((px >> 16) & 0xFF) * 255.f / a);
            float g = std::min(255.f, ((px >> 8) & 0xFF) * 255.f / a);
            float b = std::min(255.f, (px & 0xFF) * 255.f / a);
            L[i] = (0.299f * r + 0.587f * g + 0.114f * b) / 255.f;
            Sr[i] = r / 255.f; Sg[i] = g / 255.f; Sb[i] = b / 255.f;
        }
    }
    std::vector<float> Hw = A, Hn = A;
    BoxBlurPlane(Hw, w, h, Rw);
    BoxBlurPlane(Hw, w, h, Rw); // two passes ~ smooth bevel
    BoxBlurPlane(Hn, w, h, Rn);

    // frost colour: white, with a hint of the glass tint
    // Glass body colour: the tint (scaled by iconBrightness), mixed with the
    // icon's own colours by iconTintAlpha - same settings as the old tint.
    // iconTint=0 gives clear white glass. Highlights stay white, like real glass.
    const float bright = g_glassCfg.iconBrightness / 100.f;
    const float tr = std::min(1.f, g_glassCfg.tintR / 255.f * bright);
    const float tg = std::min(1.f, g_glassCfg.tintG / 255.f * bright);
    const float tb = std::min(1.f, g_glassCfg.tintB / 255.f * bright);
    const float tMix = !g_glassCfg.iconTint ? 0.f
                     : (g_glassCfg.iconTintAlpha < 0 ? 1.f : std::min(255, g_glassCfg.iconTintAlpha) / 255.f);

    // The glass light follows the tint too: highlights are a pale version of
    // the tint (like light passing through coloured glass), the thickness
    // shade a deep version of it. iconTint=0 keeps plain white / black.
    const float hl = g_glassCfg.iconTint ? 0.5f : 0.f; // how much tint is in the highlights
    const float hr = (1.f - hl) + hl * tr, hg = (1.f - hl) + hl * tg, hb = (1.f - hl) + hl * tb;
    const float dr = g_glassCfg.iconTint ? tr * 0.25f : 0.f;
    const float dg = g_glassCfg.iconTint ? tg * 0.25f : 0.f;
    const float db = g_glassCfg.iconTint ? tb * 0.25f : 0.f;

    const float lx = -0.6f, ly = -0.8f; // light from the top-left

    auto grad = [&](const std::vector<float>& P, int x, int y, float& gx, float& gy) {
        gx = (P[y * w + std::min(x + 1, w - 1)] - P[y * w + std::max(x - 1, 0)]) * 0.5f;
        gy = (P[std::min(y + 1, h - 1) * w + x] - P[std::max(y - 1, 0) * w + x]) * 0.5f;
    };

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            size_t i = static_cast<size_t>(y) * w + x;
            float a0 = A[i] / 255.f;
            if (a0 <= 0.f) { icon.pixels[i] = 0; continue; }

            float gwx, gwy, gnx, gny;
            grad(Hw, x, y, gwx, gwy);
            grad(Hn, x, y, gnx, gny);
            float lw = std::sqrt(gwx * gwx + gwy * gwy), ln = std::sqrt(gnx * gnx + gny * gny);
            float bevel = std::min(1.f, lw * (2 * Rw + 1) / 255.f * 1.6f);
            float rim = std::min(1.f, ln * (2 * Rn + 1) / 255.f);

            // outward normal = -gradient; lit > 0 faces the light
            float lit = 0.f;
            if (ln > 0.01f)      lit = (-gnx * lx - gny * ly) / ln;
            else if (lw > 0.01f) lit = (-gwx * lx - gwy * ly) / lw;
            float litF = std::max(lit, 0.f) + 0.45f * std::max(-lit, 0.f);

            // premultiplied colour (c*) and alpha (al), built up layer by layer, 0..1
            float al = 0.14f + 0.30f * L[i];                   // frosted body + etched logo
            float fr = 1.f, fg = 1.f, fb = 1.f;
            if (g_glassCfg.iconTint) {
                fr = Sr[i] + (tr - Sr[i]) * tMix;
                fg = Sg[i] + (tg - Sg[i]) * tMix;
                fb = Sb[i] + (tb - Sb[i]) * tMix;
            }
            float cr = fr * al, cg = fg * al, cb = fb * al;

            float sh = 0.30f * bevel * (1.f - rim) * std::max(-lit, 0.f); // thickness shade
            cr = sh * dr + cr * (1.f - sh); cg = sh * dg + cg * (1.f - sh); cb = sh * db + cb * (1.f - sh);
            al = sh + al * (1.f - sh);

            auto white = [&](float k) { // tinted highlight, premultiplied "over"
                cr = k * hr + cr * (1.f - k); cg = k * hg + cg * (1.f - k); cb = k * hb + cb * (1.f - k);
                al = k + al * (1.f - k);
            };
            white(0.18f * bevel * std::max(lit, 0.f)); // soft lit bevel
            white(0.85f * rim * (0.15f + 0.85f * litF)); // thin specular rim

            float u = (x + 0.5f) / w - 0.5f, v = (y + 0.5f) / h;
            float gl = 1.f - (u / 0.62f) * (u / 0.62f) - ((v - 0.02f) / 0.42f) * ((v - 0.02f) / 0.42f);
            if (gl > 0.f) white(0.32f * gl * gl);        // curved top gloss

            // silhouette coverage
            cr *= a0; cg *= a0; cb *= a0; al *= a0;
            auto to8 = [](float f) { return static_cast<UINT32>(std::min(255.f, std::max(0.f, f * 255.f + 0.5f))); };
            UINT32 A8 = to8(al);
            UINT32 R8 = std::min(A8, to8(cr)), G8 = std::min(A8, to8(cg)), B8 = std::min(A8, to8(cb));
            icon.pixels[i] = (A8 << 24) | (R8 << 16) | (G8 << 8) | B8;
        }
    }
}

// iPhone-style "tinted icons": every desktop icon is first turned black &
// white (luminance), then coloured with the glass tint - so dark parts stay
// dark and light parts take the tint colour. After that the outline is
// feathered and the icon's overall opacity is applied. All of this runs once
// per icon load (not per frame) on the pre-multiplied pixels, so it adds no
// ongoing cost.
// Styles one icon with the current settings (tint / glass / edges / opacity).
// Shared by the desktop icons and the panel's live slider previews.
void StyleIcon(AlphaIcon& icon) {
    const bool doTint = g_glassCfg.iconTint;
    const int t = (g_glassCfg.iconTintAlpha < 0) ? 255 : std::min(255, g_glassCfg.iconTintAlpha);
    const int bright = g_glassCfg.iconBrightness;
    const int opacity = g_glassCfg.iconOpacity; // percent

    {
        if (!icon.valid) return;

        AlphaIcon glass;
        if (g_glassCfg.iconGlass) {
            glass = icon;
            GlassifyIcon(glass);
        }

        // 1) black & white -> tint
        if (doTint && t > 0) {
            for (UINT32& px : icon.pixels) {
                int a = (px >> 24) & 0xFF;
                if (a == 0) continue;

                // pre-multiplied -> straight colour
                int r = std::min(255, static_cast<int>(((px >> 16) & 0xFF) * 255 / a));
                int g = std::min(255, static_cast<int>(((px >> 8) & 0xFF) * 255 / a));
                int b = std::min(255, static_cast<int>((px & 0xFF) * 255 / a));

                // step 1: black & white
                int luma = (299 * r + 587 * g + 114 * b) / 1000;

                // step 2: colour that grey with the tint (white -> tint colour)
                int mr = std::min(255, g_glassCfg.tintR * luma / 255 * bright / 100);
                int mg = std::min(255, g_glassCfg.tintG * luma / 255 * bright / 100);
                int mb = std::min(255, g_glassCfg.tintB * luma / 255 * bright / 100);

                // optional mix of the original colours back in
                r = (r * (255 - t) + mr * t) / 255;
                g = (g * (255 - t) + mg * t) / 255;
                b = (b * (255 - t) + mb * t) / 255;

                // back to pre-multiplied
                r = r * a / 255;
                g = g * a / 255;
                b = b * a / 255;

                px = (static_cast<UINT32>(a) << 24) | (r << 16) | (g << 8) | b;
            }
        }

        // Liquid glass: blend the glass version with the solid tinted icon by
        // iconGlassAmount (pure glass alone made logos hard to recognise).
        // Skips iconOpacity: the glass sets its own translucency.
        if (g_glassCfg.iconGlass) {
            const UINT32 k = static_cast<UINT32>(g_glassCfg.iconGlassAmount * 255 / 100);
            for (size_t i = 0; i < icon.pixels.size(); ++i) {
                UINT32 g = glass.pixels[i], s = icon.pixels[i], out = 0;
                for (int sh = 0; sh < 32; sh += 8) // premultiplied: blend every channel the same way
                    out |= ((((g >> sh) & 0xFF) * k + ((s >> sh) & 0xFF) * (255 - k)) / 255) << sh;
                icon.pixels[i] = out;
            }
            FeatherIconEdges(icon, g_glassCfg.iconEdgeBlur); // smooth outline
            return;
        }

        // 2) soft, feathered edges
        FeatherIconEdges(icon, g_glassCfg.iconEdgeBlur);

        // 3) overall translucency (scaling every premultiplied channel keeps it correct)
        if (opacity < 100) {
            for (UINT32& px : icon.pixels) {
                int a = ((px >> 24) & 0xFF) * opacity / 100;
                int r = ((px >> 16) & 0xFF) * opacity / 100;
                int g = ((px >> 8) & 0xFF) * opacity / 100;
                int b = (px & 0xFF) * opacity / 100;
                px = (static_cast<UINT32>(a) << 24) | (r << 16) | (g << 8) | b;
            }
        }
    }
}

void ApplyIconTint() {
    for (auto& it : items)
        if (it.alpha.valid) StyleIcon(it.alpha);
}

bool GlassMatches(HWND hwnd) {
    if (!hwnd || hwnd == g_hwnd) return false;
    wchar_t cls[256]{};
    if (!GetClassNameW(hwnd, cls, 256)) return false;
    for (const auto& c : g_glassCfg.classes) {
        if (_wcsicmp(cls, c.c_str()) == 0) return true;
    }
    return false;
}

void SetGlassAccent(HWND hwnd, int state, DWORD color) {
    if (!g_setWCA) return;
    GlassAccentPolicy ap{ state, 2, color, 0 };
    GlassCompAttrData d{ 19 /* WCA_ACCENT_POLICY */, &ap, sizeof(ap) };
    g_setWCA(hwnd, &d);
}


typedef HRESULT (WINAPI *PFN_DwmGetWindowAttribute)(HWND, DWORD, PVOID, DWORD);
typedef HANDLE (WINAPI *PFN_SetThreadDpiAwarenessContext)(HANDLE);
PFN_SetThreadDpiAwarenessContext g_setThreadDpi = nullptr;

// This overlay is DPI-unaware, but Explorer is per-monitor-DPI aware. With
// display scaling above 100% the window rect we read and the rect DWM
// reports would be in different units, so the rounded region ended up
// wrong/ignored. For the duration of the region maths, the calling thread
// is switched to per-monitor-v2 so everything is in the same real pixels.
struct GlassDpiScope {
    HANDLE prev = nullptr;
    GlassDpiScope() {
        if (g_setThreadDpi)
            prev = g_setThreadDpi(reinterpret_cast<HANDLE>(static_cast<intptr_t>(-4)));
    }
    ~GlassDpiScope() {
        if (g_setThreadDpi && prev) g_setThreadDpi(prev);
    }
};
PFN_DwmGetWindowAttribute g_dwmGetAttr = nullptr;
HWINEVENTHOOK g_hookLoc = nullptr;

// Windows 10 has no native rounded corners, so we clip the window with
// a region that only cuts the four VISIBLE corners. (Windows 10 frames
// have ~7px invisible resize borders around the visible edge - cutting
// the whole rectangle would make those un-grabbable, so only the corner
// nibbles are removed.) Region edges are not anti-aliased - that's a
// limit of SetWindowRgn.
void ApplyGlassRegion(HWND hwnd, GlassRec& rec, bool force) {
    int r = g_glassCfg.radius;
    if (r <= 0) {
        // square: once per window, drop any rounded region an earlier (killed) run left on it
        if (rec.lastW != -1) { SetWindowRgn(hwnd, nullptr, TRUE); rec.lastW = -1; }
        return;
    }

    GlassDpiScope dpiScope;

    RECT wr{};
    if (!GetWindowRect(hwnd, &wr)) return;

    bool zoomed = IsZoomed(hwnd) != 0;
    int w = wr.right - wr.left;
    int h = wr.bottom - wr.top;

    RECT vis = wr;
    if (g_dwmGetAttr) {
        RECT fb{};
        if (SUCCEEDED(g_dwmGetAttr(hwnd, 9 /* DWMWA_EXTENDED_FRAME_BOUNDS */, &fb, sizeof(fb))))
            vis = fb;
    }

    int vl = vis.left - wr.left,  vt = vis.top - wr.top;
    int vr = vis.right - wr.left, vb = vis.bottom - wr.top;

    // The inset is part of the key: a freshly opened window reports its frame
    // bounds = the whole window rect for a moment, and a region built from
    // that leaves square corners + the border line until it is rebuilt.
    RECT inset{ vl, vt, vr, vb };
    bool insetSame = EqualRect(&inset, &rec.lastInset) != 0;
    if (!force && w == rec.lastW && h == rec.lastH && zoomed == rec.zoomed && insetSame) return;
    rec.lastW = w; rec.lastH = h; rec.zoomed = zoomed; rec.lastInset = inset;

    if (zoomed || IsIconic(hwnd) || w <= 0 || h <= 0) {
        SetWindowRgn(hwnd, nullptr, TRUE); // maximized/minimized: square, no region
        return;
    }

    int maxR = std::min(vr - vl, vb - vt) / 2;
    if (r > maxR) r = maxR;
    if (r <= 0) return;

    HRGN rgn = nullptr;

    if (g_glassCfg.clip) {
        // Exactly the visible frame, rounded: nothing (tint/blur included)
        // can spill into the invisible resize border.
        rgn = CreateRoundRectRgn(vl, vt, vr + 1, vb + 1, r * 2, r * 2);
    } else {
        rgn = CreateRectRgn(0, 0, w, h);

        auto cut = [&](int sl, int st, int sr, int sb, int el, int et, int er, int eb) {
            HRGN sq = CreateRectRgn(sl, st, sr, sb);
            HRGN el_ = CreateEllipticRgn(el, et, er, eb);
            CombineRgn(sq, sq, el_, RGN_DIFF);
            CombineRgn(rgn, rgn, sq, RGN_DIFF);
            DeleteObject(sq);
            DeleteObject(el_);
        };

        cut(vl,     vt,     vl + r, vt + r,  vl,         vt,         vl + 2 * r, vt + 2 * r);
        cut(vr - r, vt,     vr,     vt + r,  vr - 2 * r, vt,         vr,         vt + 2 * r);
        cut(vl,     vb - r, vl + r, vb,      vl,         vb - 2 * r, vl + 2 * r, vb);
        cut(vr - r, vb - r, vr,     vb,      vr - 2 * r, vb - 2 * r, vr,         vb);
    }

    GlassDebug(L"hwnd=%p win=(%ld,%ld,%ld,%ld) vis=(%ld,%ld,%ld,%ld) dwmCall=%d r=%d clip=%d",
               (void*)hwnd, wr.left, wr.top, wr.right, wr.bottom,
               vis.left, vis.top, vis.right, vis.bottom, g_dwmGetAttr ? 1 : 0, r, g_glassCfg.clip);

    int ok = SetWindowRgn(hwnd, rgn, TRUE); // system owns rgn now - do not delete it
    if (g_glassDebugCount < 40) {
        HRGN chk = CreateRectRgn(0, 0, 0, 0);
        int type = GetWindowRgn(hwnd, chk);
        RECT cb{};
        if (type != ERROR) GetRgnBox(chk, &cb);
        GlassDebug(L"  SetWindowRgn -> %d, readback type=%d box=(%ld,%ld,%ld,%ld)",
                   ok, type, cb.left, cb.top, cb.right, cb.bottom);
        DeleteObject(chk);
    }
}

// =====================================================================
// LIQUID GLASS: Windows 10 can only blur what is behind a window, it
// can't bend light like iOS. What sells the iPhone look is the light on
// the glass: a crisp specular rim that is brightest on the edges facing
// the light (top-left) and the opposite edge (bottom-right), a soft inner
// "thickness" glow, and a faint gloss across the top. We can't draw into
// someone else's window, so this is painted into a separate click-through
// layered window kept exactly over the glass window, one z-step above it.
// Repainted only when the size changes; moving just repositions it.
//
// The tint is NOT done with the DWM accent colour in this mode: tested on
// Win10, every accent state paints the whole window rect and ignores the
// window region, so the colour spilled into the invisible 7px resize
// border and the cut corners. The accent is set to a fully transparent
// gradient instead (keeps the window background see-through, zero spill)
// and the tint is painted in a second layered window placed directly
// below the glass window, cut to the exact anti-aliased rounded shape.
// =====================================================================
void PaintSheen(HWND sheen, int x, int y, int w, int h, int r, bool tintFill) {
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h; // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    UINT32* bits = nullptr;
    HDC sdc = GetDC(nullptr);
    HDC mdc = CreateCompatibleDC(sdc);
    HBITMAP dib = CreateDIBSection(sdc, &bmi, DIB_RGB_COLORS, reinterpret_cast<void**>(&bits), nullptr, 0);
    if (!dib) {
        DeleteDC(mdc);
        ReleaseDC(nullptr, sdc);
        return;
    }
    HGDIOBJ old = SelectObject(mdc, dib);

    const float hx = w / 2.f, hy = h / 2.f, rr = static_cast<float>(r);
    const float band = 10.f;          // px of inner "thickness" glow
    const float glossH = h * 0.35f;   // gloss fades out over the top 35%
    const float lx = -0.6f, ly = -0.8f; // light comes from the top-left

    for (int py = 0; py < h; ++py) {
        for (int px = 0; px < w; ++px) {
            // signed distance to the rounded rect (< 0 inside)
            float ox = px + 0.5f - hx, oy = py + 0.5f - hy;
            float qx = std::fabs(ox) - (hx - rr), qy = std::fabs(oy) - (hy - rr);
            float mx = std::max(qx, 0.f), my = std::max(qy, 0.f);
            float outLen = std::sqrt(mx * mx + my * my);
            float d = outLen + std::min(std::max(qx, qy), 0.f) - rr;

            float cover = std::min(1.f, std::max(0.f, 0.5f - d)); // anti-aliased edge

            if (tintFill) {
                UINT32 A = static_cast<UINT32>(g_glassCfg.tintAlpha * cover);
                UINT32 R = g_glassCfg.tintR * A / 255, G = g_glassCfg.tintG * A / 255, B = g_glassCfg.tintB * A / 255;
                bits[static_cast<size_t>(py) * w + px] = (A << 24) | (R << 16) | (G << 8) | B;
                continue;
            }

            float a = 0.f;

            if (cover > 0.f) {
                float in = -d; // px in from the edge
                if (in < band) {
                    // which way this bit of rim faces
                    float nx, ny;
                    if (mx > 0.f && my > 0.f) { nx = mx / outLen; ny = my / outLen; }
                    else if (qx > qy)         { nx = 1.f; ny = 0.f; }
                    else                      { nx = 0.f; ny = 1.f; }
                    if (ox < 0) nx = -nx;
                    if (oy < 0) ny = -ny;

                    float lit = std::fabs(nx * lx + ny * ly); // lit edge AND opposite edge catch light
                    float rim = std::max(0.f, 1.f - in / 1.8f);
                    float glow = 1.f - in / band;
                    glow *= glow;
                    a += rim * (90.f + 150.f * lit) + glow * 38.f * (0.4f + 0.6f * lit);
                }
                if (py < glossH) a += 22.f * (1.f - py / glossH);
                a = std::min(255.f, a) * cover;
            }

            UINT32 A = static_cast<UINT32>(a);
            bits[static_cast<size_t>(py) * w + px] = (A << 24) | (A << 16) | (A << 8) | A; // premultiplied white
        }
    }

    POINT dst{ x, y }, src{ 0, 0 };
    SIZE sz{ w, h };
    BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(sheen, nullptr, &dst, &sz, mdc, &src, 0, &bf, ULW_ALPHA);

    SelectObject(mdc, old);
    DeleteObject(dib);
    DeleteDC(mdc);
    ReleaseDC(nullptr, sdc);
}

void UpdateSheen(GlassRec& rec) {
    int cloaked = 0; // on another virtual desktop
    if (g_dwmGetAttr && IsWindow(rec.hwnd))
        g_dwmGetAttr(rec.hwnd, 14 /* DWMWA_CLOAKED */, &cloaked, sizeof(cloaked));

    bool want = g_glassOn && g_glassCfg.liquid && IsWindow(rec.hwnd) &&
                IsWindowVisible(rec.hwnd) && !IsIconic(rec.hwnd) && !cloaked;
    if (!want) {
        if (rec.sheen) ShowWindow(rec.sheen, SW_HIDE);
        if (rec.under) ShowWindow(rec.under, SW_HIDE);
        return;
    }

    GlassDpiScope dpiScope; // sheen is created per-monitor aware, so everything is real pixels

    if (!rec.sheen || !rec.under) {
        for (HWND* p : { &rec.sheen, &rec.under }) {
            if (!*p)
                *p = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                                     L"REICOSheen", L"", WS_POPUP, 0, 0, 0, 0,
                                     nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        }
        rec.sheenW = rec.sheenH = 0;
        rec.sheenR = -1;
        if (!rec.sheen || !rec.under) return;
    }

    RECT vis{};
    if (!GetWindowRect(rec.hwnd, &vis)) return;
    if (g_dwmGetAttr) {
        RECT fb{};
        if (SUCCEEDED(g_dwmGetAttr(rec.hwnd, 9 /* DWMWA_EXTENDED_FRAME_BOUNDS */, &fb, sizeof(fb))))
            vis = fb;
    }

    int w = vis.right - vis.left, h = vis.bottom - vis.top;
    if (w <= 0 || h <= 0) return;
    int r = IsZoomed(rec.hwnd) ? 0 : std::min(g_glassCfg.radius, std::min(w, h) / 2);

    if (w != rec.sheenW || h != rec.sheenH || r != rec.sheenR) {
        PaintSheen(rec.sheen, vis.left, vis.top, w, h, r, false);
        PaintSheen(rec.under, vis.left, vis.top, w, h, r, true);
        rec.sheenW = w; rec.sheenH = h; rec.sheenR = r;
    }

    // tint goes directly below the glass window
    SetWindowPos(rec.under, rec.hwnd, vis.left, vis.top, 0, 0, SWP_NOACTIVATE | SWP_NOSIZE | SWP_SHOWWINDOW);

    // Insert right below whatever is directly above the glass window = one
    // step above it. If that is a topmost window, the glass window is the
    // top normal window, so HWND_TOP is the same spot.
    HWND above = GetWindow(rec.hwnd, GW_HWNDPREV);
    bool aboveTop = above && (GetWindowLongPtrW(above, GWL_EXSTYLE) & WS_EX_TOPMOST);
    bool selfTop = (GetWindowLongPtrW(rec.hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
    if (!above || (aboveTop && !selfTop)) above = HWND_TOP;

    UINT flags = SWP_NOACTIVATE | SWP_NOSIZE | SWP_SHOWWINDOW;
    if (above == rec.sheen) flags |= SWP_NOZORDER;
    SetWindowPos(rec.sheen, above, vis.left, vis.top, 0, 0, flags);
}

// Drop records for windows that no longer exist so the list can't grow
// forever (their sheen windows go with them).
void PruneGlassRecs() {
    for (auto& r : g_glassRecs) {
        if (IsWindow(r.hwnd)) continue;
        if (r.sheen) { DestroyWindow(r.sheen); r.sheen = nullptr; }
        if (r.under) { DestroyWindow(r.under); r.under = nullptr; }
    }
    g_glassRecs.erase(
        std::remove_if(g_glassRecs.begin(), g_glassRecs.end(),
                       [](const GlassRec& r) { return !IsWindow(r.hwnd); }),
        g_glassRecs.end());
}

void ApplyGlass(HWND hwnd) {
    if (!g_setWCA || !IsWindow(hwnd)) return;

    GlassRec* rec = nullptr;
    for (auto& r : g_glassRecs) {
        if (r.hwnd == hwnd) { rec = &r; break; }
    }

    if (!rec) {
        PruneGlassRecs();

        LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        g_glassRecs.push_back({ hwnd, (ex & WS_EX_LAYERED) != 0 });
        rec = &g_glassRecs.back();
    }

    // If the window was already layered (e.g. cmd with its own opacity
    // set in Properties), respect that alpha and only add the blur.
    if (!rec->hadLayered && g_glassCfg.layered && g_glassCfg.windowAlpha < 255) {
        LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        if (!(ex & WS_EX_LAYERED))
            SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex | WS_EX_LAYERED);
        SetLayeredWindowAttributes(hwnd, 0, g_glassCfg.windowAlpha, LWA_ALPHA);
    }

    DWORD color = (static_cast<DWORD>(g_glassCfg.tintAlpha) << 24) |
                  (static_cast<DWORD>(g_glassCfg.tintB) << 16) |
                  (static_cast<DWORD>(g_glassCfg.tintG) << 8) |
                  static_cast<DWORD>(g_glassCfg.tintR);

    int state = (g_glassCfg.accentOverride >= 0) ? g_glassCfg.accentOverride
                                                 : (g_glassCfg.acrylic ? 4 : 3);
    if (g_glassCfg.liquid && g_glassCfg.accentOverride < 0) {
        state = 2;  // ACCENT_ENABLE_TRANSPARENTGRADIENT, fully clear: see-through, no spill
        color = 0;  // tint is painted by the "under" window instead (see LIQUID GLASS)
    }
    SetGlassAccent(hwnd, state, color);

    ApplyGlassRegion(hwnd, *rec, false);
    UpdateSheen(*rec);
}

void RestoreGlass(const GlassRec& rec) {
    if (rec.sheen) DestroyWindow(rec.sheen);
    if (rec.under) DestroyWindow(rec.under);
    if (!IsWindow(rec.hwnd)) return;

    SetGlassAccent(rec.hwnd, 0, 0); // ACCENT_DISABLED
    SetWindowRgn(rec.hwnd, nullptr, TRUE); // back to square corners

    if (!rec.hadLayered) {
        LONG_PTR ex = GetWindowLongPtrW(rec.hwnd, GWL_EXSTYLE);
        SetWindowLongPtrW(rec.hwnd, GWL_EXSTYLE, ex & ~static_cast<LONG_PTR>(WS_EX_LAYERED));
    }

    RedrawWindow(rec.hwnd, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN | RDW_UPDATENOW);
}

BOOL CALLBACK GlassEnumProc(HWND hwnd, LPARAM) {
    if (IsWindowVisible(hwnd) && GlassMatches(hwnd))
        ApplyGlass(hwnd);
    return TRUE;
}

// Only fires when some window appears or takes focus. Anything that
// isn't a top-level window of a configured class is rejected in a few
// instructions, so this costs effectively nothing.
void CALLBACK GlassEventProc(HWINEVENTHOOK, DWORD, HWND hwnd, LONG idObject,
                             LONG idChild, DWORD, DWORD) {
    if (!g_glassOn || !hwnd) return;
    if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF) return;
    if (GetAncestor(hwnd, GA_ROOT) != hwnd) return;
    if (GlassMatches(hwnd)) ApplyGlass(hwnd);
}

// The glass effect stays ON while a window is moved or resized. Moving
// needs no extra work (the rounded region travels with the window); only
// RESIZING changes the shape, so while a glass window is being dragged a
// fast timer keeps its rounded region matching its size, and stops the
// moment the user lets go.
void CALLBACK GlassMoveSizeProc(HWINEVENTHOOK, DWORD ev, HWND hwnd, LONG idObject,
                                LONG idChild, DWORD, DWORD) {
    if (!g_glassOn || !hwnd || idObject != OBJID_WINDOW || idChild != CHILDID_SELF) return;

    for (auto& r : g_glassRecs) {
        if (r.hwnd != hwnd) continue;

        if (ev == EVENT_SYSTEM_MOVESIZESTART) {
            r.dragging = true;
            if (g_glassCfg.radius > 0 && g_hwnd)
                SetTimer(g_hwnd, GLASS_FAST_TIMER_ID, 30, nullptr);
        } else if (ev == EVENT_SYSTEM_MOVESIZEEND) {
            r.dragging = false;
            if (g_hwnd) KillTimer(g_hwnd, GLASS_FAST_TIMER_ID);
            ApplyGlassRegion(hwnd, r, true);
        }
        return;
    }
}

// ~30 fps, but ONLY while a glass window is being dragged. Pure moves
// exit immediately (size unchanged); resizes rebuild the small region.
void GlassFastTick() {
    for (auto& r : g_glassRecs) {
        if (r.dragging) ApplyGlassRegion(r.hwnd, r, false);
    }
}

// Twice a second: catches maximize/restore (which fire no move/size
// event) so corners are square when maximized. Only touches the few
// windows in g_glassRecs, so it is effectively free.
void GlassTick() {
    if (!g_glassOn) return;

    PruneGlassRecs();

    for (auto& r : g_glassRecs) {
        UpdateSheen(r); // catches virtual-desktop switches and anything the events missed
        if (r.dragging) continue;

        // Some windows clear their own region (theme change, DPI change,
        // state change). If ours is gone, put it back.
        bool lost = false;
        if (g_glassCfg.radius > 0 && !IsZoomed(r.hwnd) && !IsIconic(r.hwnd)) {
            HRGN chk = CreateRectRgn(0, 0, 0, 0);
            lost = (GetWindowRgn(r.hwnd, chk) == ERROR);
            DeleteObject(chk);
        }

        if (lost) {
            GlassDebug(L"region lost on %p - reapplying", (void*)r.hwnd);
            ApplyGlassRegion(r.hwnd, r, true);
        } else {
            ApplyGlassRegion(r.hwnd, r, false);
        }
    }
}

// Window moved / resized / shown / hidden / destroyed (other processes only):
// keep each glass window's liquid-glass layers stuck to it. Everything that
// isn't a top-level window is rejected immediately.
HWINEVENTHOOK g_hookObj = nullptr;

void CALLBACK GlassObjectProc(HWINEVENTHOOK, DWORD, HWND hwnd, LONG idObject, LONG idChild, DWORD, DWORD) {
    if (!hwnd || idObject != OBJID_WINDOW || idChild != CHILDID_SELF) return;

    for (auto& r : g_glassRecs) {
        if (r.hwnd == hwnd) { UpdateSheen(r); break; }
    }
}

void GlassSetEnabled(bool on) {
    g_glassOn = on;

    if (on) {
        EnumWindows(GlassEnumProc, 0);
        Log(L"Glass: ON");
    } else {
        for (const auto& r : g_glassRecs) RestoreGlass(r);
        g_glassRecs.clear();
        Log(L"Glass: OFF");
    }
}

void GlassInit(HWND owner) {

    HMODULE user32 = GetModuleHandleW(L"user32.dll");

    g_setThreadDpi = user32
        ? reinterpret_cast<PFN_SetThreadDpiAwarenessContext>(
              reinterpret_cast<void*>(GetProcAddress(user32, "SetThreadDpiAwarenessContext")))
        : nullptr;

    HMODULE dwm = LoadLibraryW(L"dwmapi.dll"); // loaded at runtime: no extra link flag needed
    if (dwm)
        g_dwmGetAttr = reinterpret_cast<PFN_DwmGetWindowAttribute>(
            reinterpret_cast<void*>(GetProcAddress(dwm, "DwmGetWindowAttribute")));

    {
        WNDCLASSW wc{};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"REICOSheen";
        RegisterClassW(&wc);
    }

    g_setWCA = user32
        ? reinterpret_cast<PFN_SetWindowCompositionAttribute>(
              reinterpret_cast<void*>(GetProcAddress(user32, "SetWindowCompositionAttribute")))
        : nullptr;

    if (!g_setWCA) {
        Log(L"Glass: SetWindowCompositionAttribute not available - disabled");
        g_glassOn = false;
        return;
    }

    g_hookShow = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW, nullptr,
                                 GlassEventProc, 0, 0,
                                 WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    g_hookFocus = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr,
                                  GlassEventProc, 0, 0,
                                  WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

    {
        FILE* f = _wfopen(L"C:\\REICO\\glass_debug.txt", L"w, ccs=UTF-8"); // fresh log each run
        if (f) fclose(f);
        g_glassDebugCount = 0;
    }

    g_hookLoc = SetWinEventHook(EVENT_SYSTEM_MOVESIZESTART, EVENT_SYSTEM_MOVESIZEEND, nullptr,
                                GlassMoveSizeProc, 0, 0,
                                WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

    g_hookObj = SetWinEventHook(EVENT_OBJECT_DESTROY, EVENT_OBJECT_LOCATIONCHANGE, nullptr,
                                GlassObjectProc, 0, 0,
                                WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

    if (g_glassCfg.radius > 0 || g_glassCfg.liquid)
        SetTimer(owner, GLASS_TIMER_ID, 500, nullptr);

    if (!RegisterHotKey(owner, HOTKEY_GLASS_ID, MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, VK_F6))
        LogF(L"RegisterHotKey (Ctrl+Shift+F6) failed, error %lu", GetLastError());

    {
        HDC sdc = GetDC(nullptr);
        int sysDpi = sdc ? GetDeviceCaps(sdc, LOGPIXELSX) : 0;
        if (sdc) ReleaseDC(nullptr, sdc);
        GlassDebug(L"init: build=dpi-fix radius=%d clip=%d layered=%d setThreadDpi=%d wca=%d systemDpi=%d",
                   g_glassCfg.radius, g_glassCfg.clip, g_glassCfg.layered ? 1 : 0,
                   g_setThreadDpi ? 1 : 0, g_setWCA ? 1 : 0, sysDpi);
    }

    LogF(L"Glass: ready (mode=%s, windowAlpha=%d, %d class(es))",
         g_glassCfg.acrylic ? L"acrylic" : L"blur",
         static_cast<int>(g_glassCfg.windowAlpha),
         static_cast<int>(g_glassCfg.classes.size()));

    GlassSetEnabled(true);
}

void GlassShutdown(HWND owner) {
    UnregisterHotKey(owner, HOTKEY_GLASS_ID);
    KillTimer(owner, GLASS_TIMER_ID);
    KillTimer(owner, GLASS_FAST_TIMER_ID);

    if (g_hookShow)  { UnhookWinEvent(g_hookShow);  g_hookShow = nullptr; }
    if (g_hookFocus) { UnhookWinEvent(g_hookFocus); g_hookFocus = nullptr; }
    if (g_hookLoc)   { UnhookWinEvent(g_hookLoc);   g_hookLoc = nullptr; }
    if (g_hookObj)   { UnhookWinEvent(g_hookObj);   g_hookObj = nullptr; }

    for (const auto& r : g_glassRecs) RestoreGlass(r);
    g_glassRecs.clear();
}

// =====================================================================
// SETTINGS: tray icon, settings window, live reload of glass.txt, presets.
//
// glass.txt stays the single source of truth. The settings window and the
// presets only rewrite it (keeping its comments); every change - from the
// window, a preset, or a hand edit in Notepad - goes through ReloadConfig(),
// which re-applies the window glass and the icon styling live.
// Presets are plain copies of glass.txt in C:\REICO\presets\<name>.txt.
// =====================================================================

namespace fs = std::filesystem;

const UINT WM_TRAY = WM_APP + 10;
const UINT_PTR CFG_WATCH_TIMER_ID = 1; // on g_trayWnd: has glass.txt changed on disk?
const UINT_PTR CFG_APPLY_TIMER_ID = 2; // on g_trayWnd: debounce for settings-window edits
const wchar_t* GLASS_TXT = L"C:\\REICO\\glass.txt";
const wchar_t* PRESET_DIR = L"C:\\REICO\\presets";

HWND g_trayWnd = nullptr;     // hidden window that owns the tray icon and its menu
HWND g_settingsWnd = nullptr;
UINT g_taskbarCreatedMsg = 0; // Explorer restarted -> tray icon must be re-added
FILETIME g_cfgStamp{};
HFONT g_uiFont = nullptr;
COLORREF g_uiTint = 0;        // tint colour being edited in the settings window
HBRUSH g_swatchBrush = nullptr;
HICON g_logoIcon = nullptr;   // the REICO logo, drawn in code (tray + settings window)

void RefreshTrayLogo();       // tray section below
void PanelOnReload();         // glass panel section below
void DockOnReload();          // dock section below

// ---- drawing kit: premultiplied ARGB, anti-aliased shapes + text ------

inline float Clamp01(float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }

struct Canvas { UINT32* px; int w, h; };

// A 32bpp top-down DIB with a DC, so GDI text can be drawn into it.
struct Surface {
    HDC dc = nullptr; HBITMAP bmp = nullptr; HGDIOBJ old = nullptr;
    UINT32* px = nullptr; int w = 0, h = 0;
    Canvas cv() const { return Canvas{ px, w, h }; }
};

void SurfFree(Surface& s) {
    if (s.dc) { if (s.old) SelectObject(s.dc, s.old); DeleteDC(s.dc); }
    if (s.bmp) DeleteObject(s.bmp);
    s = Surface{};
}

bool SurfCreate(Surface& s, int w, int h) {
    SurfFree(s);
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    HDC sdc = GetDC(nullptr);
    s.dc = CreateCompatibleDC(sdc);
    s.bmp = CreateDIBSection(sdc, &bmi, DIB_RGB_COLORS, reinterpret_cast<void**>(&s.px), nullptr, 0);
    ReleaseDC(nullptr, sdc);
    if (!s.dc || !s.bmp) { SurfFree(s); return false; }
    s.old = SelectObject(s.dc, s.bmp);
    s.w = w; s.h = h;
    ZeroMemory(s.px, sizeof(UINT32) * static_cast<size_t>(w) * h);
    return true;
}

// "src over dst" with a straight colour (0..255) at opacity a (0..1).
inline void Over(UINT32& d, float r, float g, float b, float a) {
    if (a <= 0.f) return;
    if (a > 1.f) a = 1.f;
    float k = 1.f - a;
    auto ch = [](float v) { return static_cast<UINT32>(std::min(255.f, v + 0.5f)); };
    UINT32 oa = ch(255.f * a + (d >> 24) * k);
    UINT32 orr = ch(r * a + ((d >> 16) & 0xFF) * k);
    UINT32 og = ch(g * a + ((d >> 8) & 0xFF) * k);
    UINT32 ob = ch(b * a + (d & 0xFF) * k);
    d = (oa << 24) | (orr << 16) | (og << 8) | ob;
}

// signed distance to a rounded rect (< 0 inside)
inline float RRectDist(float px, float py, float x0, float y0, float x1, float y1, float r) {
    float hx = (x1 - x0) * 0.5f, hy = (y1 - y0) * 0.5f;
    r = std::min(r, std::min(hx, hy));
    float qx = std::fabs(px - (x0 + hx)) - (hx - r), qy = std::fabs(py - (y0 + hy)) - (hy - r);
    float mx = std::max(qx, 0.f), my = std::max(qy, 0.f);
    return std::sqrt(mx * mx + my * my) + std::min(std::max(qx, qy), 0.f) - r;
}

template <class F> void ForRRect(Canvas c, float x0, float y0, float x1, float y1, F&& f) {
    int ix0 = std::max(0, static_cast<int>(std::floor(x0)) - 1), iy0 = std::max(0, static_cast<int>(std::floor(y0)) - 1);
    int ix1 = std::min(c.w, static_cast<int>(std::ceil(x1)) + 1), iy1 = std::min(c.h, static_cast<int>(std::ceil(y1)) + 1);
    for (int y = iy0; y < iy1; ++y)
        for (int x = ix0; x < ix1; ++x) f(x, y, c.px[static_cast<size_t>(y) * c.w + x]);
}

void FillRRect(Canvas c, float x0, float y0, float x1, float y1, float r, COLORREF col, float a) {
    ForRRect(c, x0, y0, x1, y1, [&](int x, int y, UINT32& p) {
        float cov = std::min(1.f, std::max(0.f, 0.5f - RRectDist(x + 0.5f, y + 0.5f, x0, y0, x1, y1, r)));
        if (cov > 0.f) Over(p, GetRValue(col), GetGValue(col), GetBValue(col), a * cov);
    });
}

void FillCircle(Canvas c, float cx, float cy, float rad, COLORREF col, float a) {
    FillRRect(c, cx - rad, cy - rad, cx + rad, cy + rad, rad, col, a);
}

// Vertical gradient fill (top colour -> bottom colour).
void FillRRectGrad(Canvas c, float x0, float y0, float x1, float y1, float r, COLORREF top, COLORREF bot, float a) {
    ForRRect(c, x0, y0, x1, y1, [&](int x, int y, UINT32& p) {
        float cov = std::min(1.f, std::max(0.f, 0.5f - RRectDist(x + 0.5f, y + 0.5f, x0, y0, x1, y1, r)));
        if (cov <= 0.f) return;
        float t = std::min(1.f, std::max(0.f, (y + 0.5f - y0) / (y1 - y0)));
        Over(p, GetRValue(top) + (GetRValue(bot) - GetRValue(top)) * t,
                GetGValue(top) + (GetGValue(bot) - GetGValue(top)) * t,
                GetBValue(top) + (GetBValue(bot) - GetBValue(top)) * t, a * cov);
    });
}

// Liquid-glass light on a rounded shape: thin specular rim (brightest on the
// edges facing the top-left light and the opposite edge) + a soft top gloss.
void GlassLight(Canvas c, float x0, float y0, float x1, float y1, float r, float strength) {
    const float lx = -0.6f, ly = -0.8f, rimW = 1.6f;
    const float glossH = (y1 - y0) * 0.5f;
    ForRRect(c, x0, y0, x1, y1, [&](int x, int y, UINT32& p) {
        float px = x + 0.5f, py = y + 0.5f;
        float d = RRectDist(px, py, x0, y0, x1, y1, r);
        float cov = std::min(1.f, std::max(0.f, 0.5f - d));
        if (cov <= 0.f) return;
        float a = 0.f;
        float in = -d;
        if (in < rimW + 1.f) {
            float gx = RRectDist(px + 1, py, x0, y0, x1, y1, r) - RRectDist(px - 1, py, x0, y0, x1, y1, r);
            float gy = RRectDist(px, py + 1, x0, y0, x1, y1, r) - RRectDist(px, py - 1, x0, y0, x1, y1, r);
            float gl = std::sqrt(gx * gx + gy * gy);
            float lit = gl > 0.001f ? std::fabs((gx * lx + gy * ly) / gl) : 0.f;
            a += std::max(0.f, 1.f - in / rimW) * (0.30f + 0.70f * lit) * 0.85f;
        }
        if (py - y0 < glossH) a += 0.10f * (1.f - (py - y0) / glossH);
        Over(p, 255, 255, 255, a * strength * cov);
    });
}

// Text through a white-on-black GDI mask, so it composites onto the alpha
// buffer with clean anti-aliased edges (GDI itself can't write alpha).
void DrawTextOn(Canvas c, Surface& mask, const wchar_t* text, RECT r, HFONT font, COLORREF col, float a, UINT fmt) {
    RECT rr{ std::max(0L, r.left), std::max(0L, r.top), std::min(static_cast<LONG>(c.w), r.right),
             std::min(static_cast<LONG>(c.h), r.bottom) };
    if (rr.right <= rr.left || rr.bottom <= rr.top) return;
    for (LONG y = rr.top; y < rr.bottom; ++y)
        ZeroMemory(&mask.px[static_cast<size_t>(y) * mask.w + rr.left], sizeof(UINT32) * (rr.right - rr.left));
    HGDIOBJ of = SelectObject(mask.dc, font);
    SetBkMode(mask.dc, TRANSPARENT);
    SetTextColor(mask.dc, RGB(255, 255, 255));
    DrawTextW(mask.dc, text, -1, &r, fmt | DT_NOPREFIX);
    SelectObject(mask.dc, of);
    GdiFlush();
    for (LONG y = rr.top; y < rr.bottom; ++y) {
        for (LONG x = rr.left; x < rr.right; ++x) {
            UINT32 m = mask.px[static_cast<size_t>(y) * mask.w + x];
            UINT32 cov = std::max({ (m >> 16) & 0xFF, (m >> 8) & 0xFF, m & 0xFF });
            if (cov) Over(c.px[static_cast<size_t>(y) * c.w + x], GetRValue(col), GetGValue(col), GetBValue(col), a * cov / 255.f);
        }
    }
}

COLORREF AccentColour(); // below

// Liquid-glass lettering: the glyphs become translucent tinted glass with a
// bright rim on the edges facing the top-left light, a darker inner edge on
// the far side, a gloss over the top half and a soft shadow for depth.
// Edge directions come from the slightly blurred glyph coverage.
void DrawGlassText(Canvas c, Surface& mask, const wchar_t* text, RECT r, HFONT font, float a, UINT fmt,
                   float tintMix = 0.22f, float bodyA = 0.62f) {
    RECT rr{ std::max(0L, r.left), std::max(0L, r.top), std::min(static_cast<LONG>(c.w), r.right),
             std::min(static_cast<LONG>(c.h), r.bottom) };
    const int W = rr.right - rr.left, H = rr.bottom - rr.top;
    if (W <= 2 || H <= 2 || a <= 0.f) return;

    for (LONG y = rr.top; y < rr.bottom; ++y)
        ZeroMemory(&mask.px[static_cast<size_t>(y) * mask.w + rr.left], sizeof(UINT32) * W);
    HGDIOBJ of = SelectObject(mask.dc, font);
    SetBkMode(mask.dc, TRANSPARENT);
    SetTextColor(mask.dc, RGB(255, 255, 255));
    DrawTextW(mask.dc, text, -1, &r, fmt | DT_NOPREFIX);
    SelectObject(mask.dc, of);
    GdiFlush();

    std::vector<float> M(static_cast<size_t>(W) * H);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            UINT32 m = mask.px[static_cast<size_t>(y + rr.top) * mask.w + x + rr.left];
            M[static_cast<size_t>(y) * W + x] = std::max({ (m >> 16) & 0xFF, (m >> 8) & 0xFF, m & 0xFF }) / 255.f;
        }
    std::vector<float> B = M;
    BoxBlurPlane(B, W, H, 1);

    const COLORREF acc = AccentColour();
    const float br = 255.f * (1 - tintMix) + GetRValue(acc) * tintMix;
    const float bg = 255.f * (1 - tintMix) + GetGValue(acc) * tintMix;
    const float bb = 255.f * (1 - tintMix) + GetBValue(acc) * tintMix;
    const float lx = -0.6f, ly = -0.8f;
    auto at = [&](const std::vector<float>& P, int x, int y) {
        return P[static_cast<size_t>(std::min(H - 1, std::max(0, y))) * W + std::min(W - 1, std::max(0, x))];
    };

    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            UINT32& p = c.px[static_cast<size_t>(y + rr.top) * c.w + x + rr.left];
            float sh = at(M, x - 1, y - 2);                        // shadow: glyph shifted down-right
            float m = M[static_cast<size_t>(y) * W + x];
            if (sh > m) Over(p, 0, 0, 0, (sh - m) * 0.38f * a);
            if (m <= 0.f) continue;

            float gx = (at(B, x + 1, y) - at(B, x - 1, y)) * 0.5f, gy = (at(B, x, y + 1) - at(B, x, y - 1)) * 0.5f;
            float gl = std::sqrt(gx * gx + gy * gy);
            float lit = gl > 0.01f ? (-gx * lx - gy * ly) / gl : 0.f; // outward normal = -gradient
            float edge = std::min(1.f, gl * 2.2f);
            float yrel = (y + 0.5f) / H;

            Over(p, br, bg, bb, bodyA * m * a);                                           // glass body
            Over(p, GetRValue(acc) * 0.2f, GetGValue(acc) * 0.2f, GetBValue(acc) * 0.2f,
                 edge * std::max(-lit, 0.f) * 0.35f * m * a);                             // inner edge
            Over(p, 255, 255, 255, edge * std::max(lit, 0.f) * 0.95f * m * a);            // specular rim
            if (yrel < 0.55f) Over(p, 255, 255, 255, 0.30f * (1.f - yrel / 0.55f) * m * a); // gloss
        }
    }
}

// A glass capsule/disc: tinted translucent body + rim light + gloss.
void GlassBlob(Canvas c, float x0, float y0, float x1, float y1, float r, float tintA, float whiteA, float a) {
    FillRRect(c, x0, y0, x1, y1, r, RGB(255, 255, 255), whiteA * a);
    if (tintA > 0.f) FillRRect(c, x0, y0, x1, y1, r, AccentColour(), tintA * a);
    GlassLight(c, x0, y0, x1, y1, r, a);
}

int TextWidth(const wchar_t* text, HFONT font) {
    HDC dc = GetDC(nullptr);
    HGDIOBJ of = SelectObject(dc, font);
    SIZE sz{};
    GetTextExtentPoint32W(dc, text, static_cast<int>(wcslen(text)), &sz);
    SelectObject(dc, of);
    ReleaseDC(nullptr, dc);
    return sz.cx;
}

HFONT MakeFont(int px, int weight, const wchar_t* face) {
    return CreateFontW(-px, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                       CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, face);
}

// The glass tint, brightened so it reads as a colour even when the tint is dark
// (e.g. 85,0,0 -> 220,0,0). Near-black/near-white tints give a neutral light grey.
COLORREF AccentColour() {
    int r = g_glassCfg.tintR, g = g_glassCfg.tintG, b = g_glassCfg.tintB;
    int m = std::max({ r, g, b });
    if (m < 10) return RGB(205, 205, 215);
    float k = 220.f / m;
    return RGB(std::min(255, static_cast<int>(r * k)), std::min(255, static_cast<int>(g * k)),
               std::min(255, static_cast<int>(b * k)));
}

// ---- logo: a liquid-glass tile in the tint colour with an Inter "R" ------

// Inter (C:\REICO\fonts, loaded privately): an open-source near-twin of Apple's
// SF Pro, used for all of REICO's own text. GDI sees each static weight file
// as its own family, so the family name picks the weight.
const wchar_t* kInterFiles[] = { L"C:\\REICO\\fonts\\Inter-Regular.ttf", L"C:\\REICO\\fonts\\Inter-Medium.ttf",
                                 L"C:\\REICO\\fonts\\Inter-SemiBold.ttf", L"C:\\REICO\\fonts\\Inter-Bold.ttf" };
bool g_interLoaded = false;

void LoadInter() {
    int n = 0;
    for (const wchar_t* f : kInterFiles) n += AddFontResourceExW(f, FR_PRIVATE, nullptr) > 0;
    g_interLoaded = n == 4;
    if (!g_interLoaded) Log(L"Inter fonts missing in C:\\REICO\\fonts - using Segoe UI");
}

void UnloadInter() {
    for (const wchar_t* f : kInterFiles) RemoveFontResourceExW(f, FR_PRIVATE, nullptr);
}

// weight: 400 regular, 500 medium, 600 semibold, 700 bold
HFONT InterFont(int px, int weight) {
    if (!g_interLoaded) return MakeFont(px, weight, L"Segoe UI");
    if (weight >= 700) return MakeFont(px, FW_BOLD, L"Inter");
    if (weight >= 600) return MakeFont(px, FW_NORMAL, L"Inter SemiBold");
    if (weight >= 500) return MakeFont(px, FW_NORMAL, L"Inter Medium");
    return MakeFont(px, FW_NORMAL, L"Inter");
}

void DrawLogo(Canvas c, Surface& mask, float x, float y, float size, HFONT font) {
    COLORREF acc = AccentColour();
    auto shade = [&](float k) {
        return RGB(std::min(255, static_cast<int>(GetRValue(acc) * k)), std::min(255, static_cast<int>(GetGValue(acc) * k)),
                   std::min(255, static_cast<int>(GetBValue(acc) * k)));
    };
    float r = size * 0.26f;
    FillRRectGrad(c, x, y, x + size, y + size, r, shade(1.1f), shade(0.45f), 1.f);
    GlassLight(c, x, y, x + size, y + size, r, 1.f);
    RECT tr{ static_cast<LONG>(x), static_cast<LONG>(y + size * 0.02f), static_cast<LONG>(x + size), static_cast<LONG>(y + size) };
    DrawTextOn(c, mask, L"R", tr, font, RGB(255, 255, 255), 0.96f, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

// Writes the logo, in the current tint, as a multi-size .ico (256/48/32/16).
// reico.rc embeds C:\REICO\reico.ico, so the next build's program icon matches the tint.
void WriteLogoIco(const wchar_t* path) {
    const int sizes[] = { 256, 48, 32, 16 };
    std::vector<std::vector<BYTE>> images;
    for (int sz : sizes) {
        const int f = sz >= 128 ? 2 : 4, S = sz * f; // drawn big, averaged down: smooth edges
        Surface s, m;
        if (!SurfCreate(s, S, S) || !SurfCreate(m, S, S)) { SurfFree(s); SurfFree(m); return; }
        HFONT font = InterFont(static_cast<int>(S * 0.62f), 700);
        DrawLogo(s.cv(), m, f * 0.5f, f * 0.5f, static_cast<float>(S - f), font);
        DeleteObject(font);
        BITMAPINFOHEADER bh{};
        bh.biSize = sizeof(bh); bh.biWidth = sz; bh.biHeight = sz * 2; bh.biPlanes = 1; bh.biBitCount = 32;
        int maskRow = ((sz + 31) / 32) * 4;
        std::vector<BYTE> img(sizeof(bh) + static_cast<size_t>(sz) * sz * 4 + static_cast<size_t>(maskRow) * sz, 0);
        memcpy(img.data(), &bh, sizeof(bh));
        for (int y = 0; y < sz; ++y)
            for (int x = 0; x < sz; ++x) {
                UINT32 a = 0, r = 0, g = 0, b = 0;
                for (int yy = 0; yy < f; ++yy)
                    for (int xx = 0; xx < f; ++xx) {
                        UINT32 p = s.px[static_cast<size_t>(y * f + yy) * S + x * f + xx];
                        a += p >> 24; r += (p >> 16) & 0xFF; g += (p >> 8) & 0xFF; b += p & 0xFF;
                    }
                UINT32 n = f * f; a /= n; r /= n; g /= n; b /= n;
                if (a) { r = std::min(255u, r * 255 / a); g = std::min(255u, g * 255 / a); b = std::min(255u, b * 255 / a); }
                UINT32 px = (a << 24) | (r << 16) | (g << 8) | b; // .ico wants straight alpha, rows bottom-up
                memcpy(img.data() + sizeof(bh) + (static_cast<size_t>(sz - 1 - y) * sz + x) * 4, &px, 4);
            }
        SurfFree(s);
        SurfFree(m);
        images.push_back(std::move(img));
    }
    std::ofstream out(fs::path(path), std::ios::binary);
    WORD hdr[3] = { 0, 1, static_cast<WORD>(images.size()) };
    out.write(reinterpret_cast<const char*>(hdr), sizeof(hdr));
    DWORD offset = 6 + 16 * static_cast<DWORD>(images.size());
    for (size_t i = 0; i < images.size(); ++i) {
        BYTE e[16] = {};
        e[0] = e[1] = sizes[i] == 256 ? 0 : static_cast<BYTE>(sizes[i]);
        e[4] = 1; e[6] = 32;
        DWORD len = static_cast<DWORD>(images[i].size());
        memcpy(e + 8, &len, 4);
        memcpy(e + 12, &offset, 4);
        out.write(reinterpret_cast<const char*>(e), 16);
        offset += len;
    }
    for (const auto& img : images) out.write(reinterpret_cast<const char*>(img.data()), img.size());
}

// Rendered big, then averaged down, so the small tray icon stays smooth.
HICON MakeLogoIcon(int size) {
    const int S = 64;
    if (size <= 0 || S % size) size = 16;
    Surface s, m;
    if (!SurfCreate(s, S, S) || !SurfCreate(m, S, S)) { SurfFree(s); SurfFree(m); return nullptr; }
    HFONT font = InterFont(40, 700);
    DrawLogo(s.cv(), m, 1, 1, S - 2, font);
    DeleteObject(font);

    Surface out;
    if (!SurfCreate(out, size, size)) { SurfFree(s); SurfFree(m); return nullptr; }
    const int f = S / size;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            UINT32 sa = 0, sr = 0, sg = 0, sb = 0;
            for (int yy = 0; yy < f; ++yy)
                for (int xx = 0; xx < f; ++xx) {
                    UINT32 p = s.px[(y * f + yy) * S + x * f + xx];
                    sa += p >> 24; sr += (p >> 16) & 0xFF; sg += (p >> 8) & 0xFF; sb += p & 0xFF;
                }
            UINT32 n = f * f, a = sa / n;
            // icons want straight (not premultiplied) colour
            UINT32 r = a ? std::min(255u, sr / n * 255 / a) : 0, g = a ? std::min(255u, sg / n * 255 / a) : 0,
                   b = a ? std::min(255u, sb / n * 255 / a) : 0;
            out.px[y * size + x] = (a << 24) | (r << 16) | (g << 8) | b;
        }
    }
    SelectObject(out.dc, out.old);
    out.old = nullptr;

    ICONINFO ii{};
    ii.fIcon = TRUE;
    ii.hbmColor = out.bmp;
    ii.hbmMask = CreateBitmap(size, size, 1, 1, nullptr);
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(ii.hbmMask);
    SurfFree(out);
    SurfFree(s);
    SurfFree(m);
    return icon;
}

FILETIME ConfigStamp() {
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    GetFileAttributesExW(GLASS_TXT, GetFileExInfoStandard, &fa);
    return fa.ftLastWriteTime;
}

// Rewrites key=value lines in place (comments and order kept); missing keys are appended.
bool WriteConfigValues(const fs::path& path, const std::vector<std::pair<std::string, std::string>>& kv) {
    std::vector<std::string> lines;
    {
        std::ifstream in(path);
        std::string l;
        while (std::getline(in, l)) {
            if (!l.empty() && l.back() == '\r') l.pop_back();
            lines.push_back(l);
        }
    }
    for (const auto& p : kv) {
        bool found = false;
        for (auto& l : lines) {
            if (l.rfind(p.first + "=", 0) == 0) { l = p.first + "=" + p.second; found = true; break; }
        }
        if (!found) lines.push_back(p.first + "=" + p.second);
    }
    std::ofstream out(path, std::ios::trunc);
    for (const auto& l : lines) out << l << "\n";
    return static_cast<bool>(out);
}

// Icon styling works on pixels extracted once from each HICON - re-extract
// them so the new settings start from the original icon, not the old style.
void ReapplyIconStyle() {
    for (auto& it : items)
        if (it.hIcon) it.alpha = ExtractIconAlpha(it.hIcon);
    ApplyIconTint();
}

void ReloadConfig() {
    bool wasOn = g_glassOn;
    if (wasOn) GlassSetEnabled(false); // undo with the OLD settings first

    g_glassCfg = GlassConfig{};
    LoadGlassConfig();
    g_cfgStamp = ConfigStamp();

    if (g_glassCfg.radius > 0 || g_glassCfg.liquid) SetTimer(g_hwnd, GLASS_TIMER_ID, 500, nullptr);
    else KillTimer(g_hwnd, GLASS_TIMER_ID);
    if (wasOn) GlassSetEnabled(true);

    ReapplyIconStyle();
    Render(g_hwnd);
    RefreshTrayLogo(); // logo follows the tint
    PanelOnReload();
    DockOnReload();
    Log(L"Settings: glass.txt reloaded");
}

std::vector<std::wstring> ListPresets() {
    std::vector<std::wstring> names;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(PRESET_DIR, ec))
        if (e.path().extension() == L".txt") names.push_back(e.path().stem().wstring());
    std::sort(names.begin(), names.end());
    return names;
}

fs::path PresetPath(const std::wstring& name) { return fs::path(PRESET_DIR) / (name + L".txt"); }

// key=value lines of a settings file (comments skipped); class= lines are kept as a list
std::vector<std::pair<std::string, std::string>> ReadConfigValues(const fs::path& p) {
    std::vector<std::pair<std::string, std::string>> kv;
    std::ifstream in(p);
    std::string l;
    while (std::getline(in, l)) {
        if (!l.empty() && l.back() == '\r') l.pop_back();
        size_t eq = l.find('=');
        if (l.empty() || l[0] == '#' || eq == std::string::npos) continue;
        kv.push_back({ l.substr(0, eq), l.substr(eq + 1) });
    }
    return kv;
}

// A preset only changes the settings it contains: settings added after the preset
// was saved (e.g. dock=, nowPlaying=) keep their current values instead of vanishing.
void ApplyPreset(const std::wstring& name) {
    auto before = ReadConfigValues(GLASS_TXT);
    if (!CopyFileW(PresetPath(name).c_str(), GLASS_TXT, FALSE)) {
        LogF(L"Preset '%s' could not be applied (error %lu)", name.c_str(), GetLastError());
        return;
    }
    auto now = ReadConfigValues(GLASS_TXT);
    std::vector<std::pair<std::string, std::string>> keep;
    for (const auto& kv : before)
        if (kv.first != "class" &&
            std::none_of(now.begin(), now.end(), [&](const auto& n) { return n.first == kv.first; }) &&
            std::none_of(keep.begin(), keep.end(), [&](const auto& n) { return n.first == kv.first; }))
            keep.push_back(kv);
    if (!keep.empty()) WriteConfigValues(GLASS_TXT, keep);
    ReloadConfig();
}

// First run: a "My setup" copy of the current glass.txt plus three starting looks.
void SeedPresets() {
    std::error_code ec;
    if (fs::exists(PRESET_DIR, ec)) return;
    fs::create_directories(PRESET_DIR, ec);

    struct Seed { const wchar_t* name; std::vector<std::pair<std::string, std::string>> kv; };
    const Seed seeds[] = {
        { L"My setup", {} },
        { L"Red glass", { { "tint", "85,0,0" }, { "tintAlpha", "170" }, { "windowAlpha", "190" },
                          { "iconTint", "1" }, { "iconBrightness", "300" }, { "iconGlassAmount", "65" } } },
        { L"Clear glass", { { "tint", "255,255,255" }, { "tintAlpha", "45" }, { "windowAlpha", "220" },
                            { "iconTint", "0" }, { "iconGlassAmount", "85" } } },
        { L"Dark smoke", { { "tint", "15,15,20" }, { "tintAlpha", "190" }, { "windowAlpha", "215" },
                           { "iconTint", "1" }, { "iconBrightness", "260" }, { "iconGlassAmount", "60" } } },
    };
    for (const auto& s : seeds) {
        fs::path p = PresetPath(s.name);
        if (CopyFileW(GLASS_TXT, p.c_str(), TRUE) && !s.kv.empty()) WriteConfigValues(p, s.kv);
    }
}

// ---- settings window ------------------------------------------------

enum {
    IDC_LIQUID = 101, IDC_TINTBTN, IDC_SWATCH, IDC_ICONGLASS, IDC_ICONTINT, IDC_HOVER,
    IDC_PRESETS, IDC_P_APPLY, IDC_P_DELETE, IDC_P_NAME, IDC_P_SAVE,
    // sliders; each one's value label is id + 100
    IDC_TINTA = 150, IDC_WALPHA, IDC_GLASSAMT, IDC_BRIGHT, IDC_EDGE,
};

struct SliderDef { int id; const wchar_t* label; int lo, hi; };
const SliderDef kSliders[] = {
    { IDC_TINTA,    L"Tint strength",  0,   255 },
    { IDC_WALPHA,   L"Window opacity", 80,  255 },
    { IDC_GLASSAMT, L"Icon glass amount (%)", 0, 100 },
    { IDC_BRIGHT,   L"Icon brightness (%)",   40, 300 },
    { IDC_EDGE,     L"Icon edge softness",    0,  24 },
};
const wchar_t* kHoverNames[] = { L"Lift", L"Shine", L"Bounce", L"None" };
const char* kHoverKeys[] = { "lift", "shine", "bounce", "none" };

HWND UiCtl(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style,
           int x, int y, int w, int h, int id) {
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, parent,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(g_uiFont), TRUE);
    return c;
}

const SliderDef* FindSlider(int id) {
    for (const auto& s : kSliders) if (s.id == id) return &s;
    return nullptr;
}

void UpdateSliderLabel(HWND dlg, int id) {
    const SliderDef* s = FindSlider(id);
    if (!s) return;
    int v = static_cast<int>(SendDlgItemMessageW(dlg, id, TBM_GETPOS, 0, 0));
    wchar_t buf[96];
    swprintf(buf, 96, L"%ls: %d", s->label, v);
    SetDlgItemTextW(dlg, id + 100, buf);
}

void SetSlider(HWND dlg, int id, int v) {
    SendDlgItemMessageW(dlg, id, TBM_SETPOS, TRUE, v);
    UpdateSliderLabel(dlg, id);
}

void FillPresetList(HWND dlg) {
    HWND cb = GetDlgItem(dlg, IDC_PRESETS);
    SendMessageW(cb, CB_RESETCONTENT, 0, 0);
    for (const auto& n : ListPresets()) SendMessageW(cb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(n.c_str()));
    SendMessageW(cb, CB_SETCURSEL, 0, 0);
}

// glass.txt -> controls (after a reload, so hand edits and presets show up)
void UiFromConfig() {
    HWND d = g_settingsWnd;
    if (!d) return;
    CheckDlgButton(d, IDC_LIQUID, g_glassCfg.liquid ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(d, IDC_ICONGLASS, g_glassCfg.iconGlass ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(d, IDC_ICONTINT, g_glassCfg.iconTint ? BST_CHECKED : BST_UNCHECKED);
    SetSlider(d, IDC_TINTA, g_glassCfg.tintAlpha);
    SetSlider(d, IDC_WALPHA, g_glassCfg.windowAlpha);
    SetSlider(d, IDC_GLASSAMT, g_glassCfg.iconGlassAmount);
    SetSlider(d, IDC_BRIGHT, g_glassCfg.iconBrightness);
    SetSlider(d, IDC_EDGE, g_glassCfg.iconEdgeBlur);
    SendDlgItemMessageW(d, IDC_HOVER, CB_SETCURSEL, g_hoverEffect, 0);
    g_uiTint = RGB(g_glassCfg.tintR, g_glassCfg.tintG, g_glassCfg.tintB);
    InvalidateRect(GetDlgItem(d, IDC_SWATCH), nullptr, TRUE);
}

// controls -> glass.txt -> live reload
void ConfigFromUi() {
    HWND d = g_settingsWnd;
    if (!d) return;
    auto pos = [&](int id) { return std::to_string(SendDlgItemMessageW(d, id, TBM_GETPOS, 0, 0)); };
    auto chk = [&](int id) { return std::string(IsDlgButtonChecked(d, id) == BST_CHECKED ? "1" : "0"); };
    int hover = static_cast<int>(SendDlgItemMessageW(d, IDC_HOVER, CB_GETCURSEL, 0, 0));
    if (hover < 0 || hover > HOVER_NONE) hover = HOVER_LIFT;

    WriteConfigValues(GLASS_TXT, {
        { "liquid", chk(IDC_LIQUID) },
        { "tint", std::to_string(GetRValue(g_uiTint)) + "," + std::to_string(GetGValue(g_uiTint)) + "," +
                  std::to_string(GetBValue(g_uiTint)) },
        { "tintAlpha", pos(IDC_TINTA) },
        { "windowAlpha", pos(IDC_WALPHA) },
        { "iconGlass", chk(IDC_ICONGLASS) },
        { "iconTint", chk(IDC_ICONTINT) },
        { "iconGlassAmount", pos(IDC_GLASSAMT) },
        { "iconBrightness", pos(IDC_BRIGHT) },
        { "iconEdgeBlur", pos(IDC_EDGE) },
        { "hoverEffect", kHoverKeys[hover] },
    });
    ReloadConfig();
}

void ScheduleApply() { SetTimer(g_trayWnd, CFG_APPLY_TIMER_ID, 250, nullptr); }

void PickTintColour(HWND owner) {
    typedef BOOL (WINAPI *PFN_ChooseColorW)(LPCHOOSECOLORW);
    static PFN_ChooseColorW chooseColor = nullptr;
    if (!chooseColor) {
        HMODULE m = LoadLibraryW(L"comdlg32.dll"); // runtime: no extra link flag needed
        if (m) chooseColor = reinterpret_cast<PFN_ChooseColorW>(reinterpret_cast<void*>(GetProcAddress(m, "ChooseColorW")));
        if (!chooseColor) return;
    }
    static COLORREF custom[16]{};
    CHOOSECOLORW cc{};
    cc.lStructSize = sizeof(cc);
    cc.hwndOwner = owner;
    cc.rgbResult = g_uiTint;
    cc.lpCustColors = custom;
    cc.Flags = CC_FULLOPEN | CC_RGBINIT;
    if (chooseColor(&cc)) {
        g_uiTint = cc.rgbResult;
        InvalidateRect(GetDlgItem(owner, IDC_SWATCH), nullptr, TRUE);
        ScheduleApply();
    }
}

std::wstring SelectedPreset(HWND d) {
    HWND cb = GetDlgItem(d, IDC_PRESETS);
    int i = static_cast<int>(SendMessageW(cb, CB_GETCURSEL, 0, 0));
    if (i < 0) return L"";
    wchar_t buf[MAX_PATH]{};
    SendMessageW(cb, CB_GETLBTEXT, i, reinterpret_cast<LPARAM>(buf));
    return buf;
}

LRESULT CALLBACK SettingsProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_HSCROLL: {
        int id = GetDlgCtrlID(reinterpret_cast<HWND>(l));
        UpdateSliderLabel(h, id);
        ScheduleApply();
        return 0;
    }

    case WM_COMMAND: {
        int id = LOWORD(w), code = HIWORD(w);
        if ((id == IDC_LIQUID || id == IDC_ICONGLASS || id == IDC_ICONTINT) && code == BN_CLICKED) ScheduleApply();
        else if (id == IDC_HOVER && code == CBN_SELCHANGE) ScheduleApply();
        else if (id == IDC_TINTBTN && code == BN_CLICKED) PickTintColour(h);
        else if (id == IDC_P_APPLY && code == BN_CLICKED) {
            std::wstring n = SelectedPreset(h);
            if (!n.empty()) { ApplyPreset(n); UiFromConfig(); }
        } else if (id == IDC_P_DELETE && code == BN_CLICKED) {
            std::wstring n = SelectedPreset(h);
            if (!n.empty() && MessageBoxW(h, (L"Delete preset \"" + n + L"\"?").c_str(), L"REICO",
                                          MB_YESNO | MB_ICONQUESTION) == IDYES) {
                DeleteFileW(PresetPath(n).c_str());
                FillPresetList(h);
            }
        } else if (id == IDC_P_SAVE && code == BN_CLICKED) {
            wchar_t buf[64]{};
            GetDlgItemTextW(h, IDC_P_NAME, buf, 64);
            std::wstring n;
            for (wchar_t c : std::wstring(buf))
                if (!wcschr(L"\\/:*?\"<>|", c)) n += c; // keep it a valid file name
            while (!n.empty() && n.back() == L' ') n.pop_back();
            if (n.empty()) { MessageBoxW(h, L"Type a name for the preset first.", L"REICO", MB_OK); return 0; }
            std::error_code ec;
            fs::create_directories(PRESET_DIR, ec);
            if (CopyFileW(GLASS_TXT, PresetPath(n).c_str(), FALSE)) {
                FillPresetList(h);
                SendDlgItemMessageW(h, IDC_PRESETS, CB_SELECTSTRING, static_cast<WPARAM>(-1),
                                    reinterpret_cast<LPARAM>(n.c_str()));
                SetDlgItemTextW(h, IDC_P_NAME, L"");
            }
        }
        return 0;
    }

    case WM_CTLCOLORSTATIC:
        if (GetDlgCtrlID(reinterpret_cast<HWND>(l)) == IDC_SWATCH) {
            if (g_swatchBrush) DeleteObject(g_swatchBrush);
            g_swatchBrush = CreateSolidBrush(g_uiTint);
            return reinterpret_cast<LRESULT>(g_swatchBrush);
        }
        break;

    case WM_DESTROY:
        g_settingsWnd = nullptr;
        return 0;
    }
    return DefWindowProcW(h, msg, w, l);
}

void OpenSettings() {
    if (g_settingsWnd) {
        ShowWindow(g_settingsWnd, SW_RESTORE);
        SetForegroundWindow(g_settingsWnd);
        return;
    }

    RECT rc{ 0, 0, 366, 540 };
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRect(&rc, style, FALSE);
    HWND d = CreateWindowExW(0, L"REICOSettings", L"REICO settings", style, CW_USEDEFAULT, CW_USEDEFAULT,
                             rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!d) return;
    g_settingsWnd = d;
    if (g_logoIcon) {
        SendMessageW(d, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(g_logoIcon));
        SendMessageW(d, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(g_logoIcon));
    }

    auto section = [&](int y, const wchar_t* t) { UiCtl(d, L"STATIC", t, 0, 16, y, 330, 18, -1); };
    auto slider = [&](int y, int id) {
        UiCtl(d, L"STATIC", L"", 0, 16, y, 330, 18, id + 100);
        HWND tb = UiCtl(d, TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, 10, y + 18, 340, 26, id);
        const SliderDef* s = FindSlider(id);
        SendMessageW(tb, TBM_SETRANGE, TRUE, MAKELPARAM(s->lo, s->hi));
    };

    section(10, L"Windows");
    UiCtl(d, L"BUTTON", L"Liquid glass", BS_AUTOCHECKBOX | WS_TABSTOP, 16, 30, 200, 20, IDC_LIQUID);
    UiCtl(d, L"BUTTON", L"Tint colour...", BS_PUSHBUTTON | WS_TABSTOP, 16, 56, 110, 24, IDC_TINTBTN);
    UiCtl(d, L"STATIC", L"", SS_SUNKEN, 134, 58, 50, 20, IDC_SWATCH);
    slider(88, IDC_TINTA);
    slider(136, IDC_WALPHA);

    section(190, L"Icons");
    UiCtl(d, L"BUTTON", L"Glass icons", BS_AUTOCHECKBOX | WS_TABSTOP, 16, 210, 150, 20, IDC_ICONGLASS);
    UiCtl(d, L"BUTTON", L"Tint icons", BS_AUTOCHECKBOX | WS_TABSTOP, 180, 210, 150, 20, IDC_ICONTINT);
    slider(236, IDC_GLASSAMT);
    slider(284, IDC_BRIGHT);
    slider(332, IDC_EDGE);
    UiCtl(d, L"STATIC", L"Hover effect", 0, 16, 388, 90, 18, -1);
    HWND hov = UiCtl(d, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, 110, 384, 120, 200, IDC_HOVER);
    for (const wchar_t* n : kHoverNames) SendMessageW(hov, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(n));

    section(424, L"Presets");
    UiCtl(d, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 16, 444, 200, 300, IDC_PRESETS);
    UiCtl(d, L"BUTTON", L"Apply", BS_PUSHBUTTON | WS_TABSTOP, 224, 443, 60, 24, IDC_P_APPLY);
    UiCtl(d, L"BUTTON", L"Delete", BS_PUSHBUTTON | WS_TABSTOP, 290, 443, 60, 24, IDC_P_DELETE);
    UiCtl(d, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 16, 476, 200, 22, IDC_P_NAME);
    UiCtl(d, L"BUTTON", L"Save current as", BS_PUSHBUTTON | WS_TABSTOP, 224, 475, 126, 24, IDC_P_SAVE);
    UiCtl(d, L"STATIC", L"Changes apply live. Hand edits to glass.txt are picked up too.",
          0, 16, 512, 340, 18, -1);

    FillPresetList(d);
    UiFromConfig();
    ShowWindow(d, SW_SHOW);
    SetForegroundWindow(d);
}

// ---- glass panel: the left-click tray menu ------------------------------
//
// A per-pixel-alpha layered popup, fully drawn by us: a frosted backdrop (the
// screen behind it, captured and blurred when it opens), a dark tint veil,
// liquid-glass rim light, then the controls. Opens with a fade + slide up,
// closes with a fade when it loses focus or on Esc. Changes write glass.txt
// through the same path as the settings window, so everything stays in sync.

const UINT_PTR PANEL_ANIM_TIMER_ID = 1, PANEL_APPLY_TIMER_ID = 2;
const int PANEL_COL_W = 312, PANEL_GAP = 28, PANEL_PAD = 20, PANEL_R = 22;
const int PANEL_W = 2 * PANEL_COL_W + PANEL_GAP + 2 * PANEL_PAD;  // two columns side by side
const int PANEL_PREVIEW = 42;                                     // slider preview tile size

enum PanelCtlKind { CK_ACTION, CK_TOGGLE, CK_SLIDER, CK_SWATCH, CK_SEGMENT, CK_CHIP, CK_LINK, CK_SECTION, CK_LABEL, CK_WHEEL };
enum {
    P_REFRESH = 1, P_EDIT, P_GLASS, P_LIQUID, P_ICONGLASS, P_ICONTINT,
    P_TINTA, P_WALPHA, P_GLASSAMT, P_BRIGHT, P_EDGE, P_HOVER, P_SETTINGS, P_EXIT, P_NOWPLAY, P_DOCK, P_BTNGAMES, P_BTNTASK, P_BTNNOTES,
    P_DOWNLOADS, P_WHEEL, P_TINTV,
    P_SWATCH = 100, P_PRESET = 200,
};

struct PanelCtl {
    int kind, id;
    RECT r;
    std::wstring label;
    int lo = 0, hi = 0;     // slider range / preset index
    COLORREF colour = 0;    // swatch colour
    float hot = 0.f;        // animated hover 0..1
    float on = 0.f;         // animated toggle state / segment position
};

struct PanelVals {
    bool liquid, iconGlass, iconTint, nowPlaying, dock, btnGames, btnTaskMgr, btnNotes, downloads;
    int tintA, wAlpha, glassAmt, bright, edge, hover;
    COLORREF tint;
    float hue, sat;  // tint as hue (0..360) + saturation (0..1), for the colour wheel
    int tintV;       // tint brightness, 0..100
};

void RgbToHsv(COLORREF c, float& h, float& s, float& v) {
    float r = GetRValue(c) / 255.f, g = GetGValue(c) / 255.f, b = GetBValue(c) / 255.f;
    float mx = std::max({ r, g, b }), mn = std::min({ r, g, b }), d = mx - mn;
    v = mx;
    s = mx > 0.f ? d / mx : 0.f;
    if (d <= 0.f) h = 0.f;
    else if (mx == r) h = 60.f * std::fmod((g - b) / d, 6.f);
    else if (mx == g) h = 60.f * ((b - r) / d + 2.f);
    else h = 60.f * ((r - g) / d + 4.f);
    if (h < 0.f) h += 360.f;
}

COLORREF HsvToRgb(float h, float s, float v) {
    float c = v * s, x = c * (1.f - std::fabs(std::fmod(h / 60.f, 2.f) - 1.f)), m = v - c;
    float r = 0, g = 0, b = 0;
    if (h < 60) { r = c; g = x; } else if (h < 120) { r = x; g = c; } else if (h < 180) { g = c; b = x; }
    else if (h < 240) { g = x; b = c; } else if (h < 300) { r = x; b = c; } else { r = c; b = x; }
    auto to8 = [](float f) { return static_cast<BYTE>(std::min(255.f, std::max(0.f, f * 255.f + 0.5f))); };
    return RGB(to8(r + m), to8(g + m), to8(b + m));
}

HWND g_panel = nullptr;
Surface g_pnl, g_pnlMask;              // frame buffer + text mask
std::vector<UINT32> g_pnlBlur;          // blurred screen behind the panel (opaque)
std::vector<UINT32> g_pnlBase;          // card + header, redrawn only when the tint changes
std::vector<PanelCtl> g_ctls;
std::vector<std::wstring> g_pnlPresets;
PanelVals g_pv{};
int g_pnlX = 0, g_pnlY = 0, g_pnlH = 0;
bool g_pnlVisible = false, g_pnlClosing = false, g_pnlTimerOn = false;
DWORD g_pnlAnimStart = 0, g_pnlLastClose = 0;
float g_pnlOpenT = 0.f;
int g_pnlHot = -1, g_pnlPressed = -1, g_pnlDrag = -1; // indexes into g_ctls
bool g_pnlMoving = false;              // dragging the panel by its header
POINT g_pnlMoveOff{};

// Waits until the screen shows the latest frame (so a capture doesn't see what we just hid).
void DwmFlushIfAvailable() {
    typedef HRESULT (WINAPI *PFN_DwmFlush)();
    static PFN_DwmFlush flush = reinterpret_cast<PFN_DwmFlush>(
        reinterpret_cast<void*>(GetProcAddress(LoadLibraryW(L"dwmapi.dll"), "DwmFlush")));
    if (flush) { flush(); flush(); } else Sleep(50);
}
HFONT g_fBody = nullptr, g_fSmall = nullptr, g_fBtn = nullptr, g_fTitle = nullptr, g_fLogo = nullptr;

const COLORREF kSwatches[] = { RGB(85, 0, 0), RGB(0, 150, 150), RGB(30, 70, 170),
                               RGB(100, 40, 150), RGB(15, 15, 20), RGB(255, 255, 255) };

void PanelLoadVals() {
    g_pv.liquid = g_glassCfg.liquid;
    g_pv.iconGlass = g_glassCfg.iconGlass;
    g_pv.iconTint = g_glassCfg.iconTint;
    g_pv.nowPlaying = g_glassCfg.nowPlaying;
    g_pv.dock = g_glassCfg.dock;
    g_pv.btnGames = g_glassCfg.btnGames;
    g_pv.btnTaskMgr = g_glassCfg.btnTaskMgr;
    g_pv.btnNotes = g_glassCfg.btnNotes;
    g_pv.downloads = g_glassCfg.downloads;
    g_pv.tintA = g_glassCfg.tintAlpha;
    g_pv.wAlpha = g_glassCfg.windowAlpha;
    g_pv.glassAmt = g_glassCfg.iconGlassAmount;
    g_pv.bright = g_glassCfg.iconBrightness;
    g_pv.edge = g_glassCfg.iconEdgeBlur;
    g_pv.hover = g_hoverEffect;
    g_pv.tint = RGB(g_glassCfg.tintR, g_glassCfg.tintG, g_glassCfg.tintB);
    float v = 0.f;
    RgbToHsv(g_pv.tint, g_pv.hue, g_pv.sat, v);
    g_pv.tintV = static_cast<int>(std::lround(v * 100.f));
}

int* PanelSliderVal(int id) {
    switch (id) {
    case P_TINTV: return &g_pv.tintV;
    case P_TINTA: return &g_pv.tintA;
    case P_WALPHA: return &g_pv.wAlpha;
    case P_GLASSAMT: return &g_pv.glassAmt;
    case P_BRIGHT: return &g_pv.bright;
    case P_EDGE: return &g_pv.edge;
    }
    return nullptr;
}

bool* PanelToggleVal(int id) {
    switch (id) {
    case P_LIQUID: return &g_pv.liquid;
    case P_ICONGLASS: return &g_pv.iconGlass;
    case P_ICONTINT: return &g_pv.iconTint;
    case P_NOWPLAY: return &g_pv.nowPlaying;
    case P_DOCK: return &g_pv.dock;
    case P_BTNGAMES: return &g_pv.btnGames;
    case P_BTNTASK: return &g_pv.btnTaskMgr;
    case P_BTNNOTES: return &g_pv.btnNotes;
    case P_DOWNLOADS: return &g_pv.downloads;
    }
    return nullptr;
}

void PanelApply() {
    auto b = [](bool v) { return std::string(v ? "1" : "0"); };
    WriteConfigValues(GLASS_TXT, {
        { "liquid", b(g_pv.liquid) },
        { "tint", std::to_string(GetRValue(g_pv.tint)) + "," + std::to_string(GetGValue(g_pv.tint)) + "," +
                  std::to_string(GetBValue(g_pv.tint)) },
        { "tintAlpha", std::to_string(g_pv.tintA) },
        { "windowAlpha", std::to_string(g_pv.wAlpha) },
        { "iconGlass", b(g_pv.iconGlass) },
        { "iconTint", b(g_pv.iconTint) },
        { "iconGlassAmount", std::to_string(g_pv.glassAmt) },
        { "iconBrightness", std::to_string(g_pv.bright) },
        { "iconEdgeBlur", std::to_string(g_pv.edge) },
        { "hoverEffect", kHoverKeys[g_pv.hover] },
        { "nowPlaying", b(g_pv.nowPlaying) },
        { "dock", b(g_pv.dock) },
        { "btnGames", b(g_pv.btnGames) },
        { "btnTaskMgr", b(g_pv.btnTaskMgr) },
        { "btnNotes", b(g_pv.btnNotes) },
        { "downloads", b(g_pv.downloads) },
    });
    ReloadConfig();
    UiFromConfig();
}

void PanelScheduleApply(UINT ms) { SetTimer(g_panel, PANEL_APPLY_TIMER_ID, ms, nullptr); }

// A wide two-column box: windows on the left, icons + the rest on the right,
// the header (logo, title, quick buttons) across the top.
void PanelLayout() {
    g_ctls.clear();
    const int L1 = PANEL_PAD, R1 = PANEL_COL_W + PANEL_PAD;          // left column
    const int L2 = R1 + PANEL_GAP, R2 = PANEL_W - PANEL_PAD;          // right column
    const int SR = PANEL_PREVIEW + 12;                                // room for a slider's preview tile
    auto add = [&](int kind, int id, RECT r, const std::wstring& label, int lo = 0, int hi = 0, COLORREF c = 0) {
        PanelCtl k{ kind, id, r, label, lo, hi, c };
        g_ctls.push_back(k);
    };

    // quick buttons, right side of the header
    const wchar_t* qa[] = { L"Refresh", L"Edit layout", L"Glass" };
    const int qid[] = { P_REFRESH, P_EDIT, P_GLASS };
    const int bw = 100;
    for (int i = 0; i < 3; ++i) {
        int x0 = R2 - (3 - i) * bw - (2 - i) * 8;
        add(CK_ACTION, qid[i], { x0, 20, x0 + bw, 54 }, qa[i]);
    }

    // left column: window glass
    int y = 80;
    add(CK_SECTION, 0, { L1, y, R1, y + 16 }, L"WINDOWS"); y += 22;
    add(CK_TOGGLE, P_LIQUID, { L1, y, R1, y + 28 }, L"Liquid glass"); y += 34;
    add(CK_SLIDER, P_TINTA, { L1, y, R1 - SR, y + 42 }, L"Tint strength", 0, 255); y += 50;
    add(CK_SLIDER, P_WALPHA, { L1, y, R1 - SR, y + 42 }, L"Window opacity", 80, 255); y += 50;
    add(CK_LABEL, 0, { L1, y, L1 + 90, y + 28 }, L"Tint colour");
    int sx = R1 - 6 * 30 + 4;
    for (int i = 0; i < 6; ++i) add(CK_SWATCH, P_SWATCH + i, { sx + i * 30, y, sx + i * 30 + 26, y + 28 }, L"", 0, 0, kSwatches[i]);
    y += 28 + 10;
    add(CK_WHEEL, P_WHEEL, { L1, y, R1, y + 116 }, L"Custom colour"); y += 116 + 10;
    add(CK_SLIDER, P_TINTV, { L1, y, R1 - SR, y + 42 }, L"Tint brightness", 0, 100); y += 50;
    const int leftEnd = y;

    // right column: icons, hover, extras, presets
    y = 80;
    add(CK_SECTION, 0, { L2, y, R2, y + 16 }, L"ICONS"); y += 22;
    add(CK_TOGGLE, P_ICONGLASS, { L2, y, R2, y + 28 }, L"Glass icons"); y += 32;
    add(CK_TOGGLE, P_ICONTINT, { L2, y, R2, y + 28 }, L"Tint icons"); y += 34;
    add(CK_SLIDER, P_GLASSAMT, { L2, y, R2 - SR, y + 42 }, L"Glass amount", 0, 100); y += 50;
    add(CK_SLIDER, P_BRIGHT, { L2, y, R2 - SR, y + 42 }, L"Brightness", 40, 300); y += 50;
    add(CK_SLIDER, P_EDGE, { L2, y, R2 - SR, y + 42 }, L"Edge softness", 0, 24); y += 50 + 4;

    add(CK_SECTION, 0, { L2, y, R2, y + 16 }, L"HOVER EFFECT"); y += 22;
    add(CK_SEGMENT, P_HOVER, { L2, y, R2, y + 30 }, L""); y += 30 + 16;

    add(CK_SECTION, 0, { L2, y, R2, y + 16 }, L"EXTRAS"); y += 22;
    add(CK_TOGGLE, P_NOWPLAY, { L2, y, R2, y + 28 }, L"Now playing pop-up"); y += 32;
    add(CK_TOGGLE, P_DOCK, { L2, y, R2, y + 28 }, L"Glass taskbar (top)"); y += 32;
    add(CK_TOGGLE, P_BTNGAMES, { L2, y, R2, y + 28 }, L"Game launcher button"); y += 32;
    add(CK_TOGGLE, P_BTNTASK, { L2, y, R2, y + 28 }, L"Task Manager button"); y += 32;
    add(CK_TOGGLE, P_BTNNOTES, { L2, y, R2, y + 28 }, L"Board button"); y += 32;
    add(CK_TOGGLE, P_DOWNLOADS, { L2, y, R2, y + 28 }, L"Download manager"); y += 34 + 10;

    add(CK_SECTION, 0, { L2, y, R2, y + 16 }, L"PRESETS"); y += 22;
    g_pnlPresets = ListPresets();
    int cx = L2;
    for (size_t i = 0; i < g_pnlPresets.size() && i < 12; ++i) {
        int tw = TextWidth(g_pnlPresets[i].c_str(), g_fBtn) + 26;
        if (cx + tw > R2 && cx > L2) { cx = L2; y += 32; }
        add(CK_CHIP, P_PRESET + static_cast<int>(i), { cx, y, cx + tw, y + 26 }, g_pnlPresets[i], static_cast<int>(i));
        cx += tw + 6;
    }
    if (g_pnlPresets.empty()) add(CK_LABEL, 0, { L2, y, R2, y + 26 }, L"No presets yet - save one in More settings");
    y += 26 + 16;

    // footer across the box
    y = std::max(y, leftEnd) + 4;
    add(CK_LINK, P_SETTINGS, { L1, y, L1 + 130, y + 24 }, L"More settings...");
    add(CK_LINK, P_EXIT, { R2 - 90, y, R2, y + 24 }, L"Exit REICO");
    y += 24 + 14;
    g_pnlH = y;
}

// Screen behind the panel -> blurred backdrop. Captured with a margin so the
// blur doesn't darken toward the panel edges.
void PanelCaptureBackdrop() {
    const int m = 30, W = PANEL_W + 2 * m, H = g_pnlH + 2 * m;
    Surface cap;
    g_pnlBlur.assign(static_cast<size_t>(PANEL_W) * g_pnlH, 0xFF202020);
    if (!SurfCreate(cap, W, H)) return;
    HDC sdc = GetDC(nullptr);
    BitBlt(cap.dc, 0, 0, W, H, sdc, g_pnlX - m, g_pnlY - m, SRCCOPY | CAPTUREBLT);
    ReleaseDC(nullptr, sdc);

    const size_t n = static_cast<size_t>(W) * H;
    std::vector<float> pr(n), pg(n), pb(n);
    for (size_t i = 0; i < n; ++i) {
        pr[i] = static_cast<float>((cap.px[i] >> 16) & 0xFF);
        pg[i] = static_cast<float>((cap.px[i] >> 8) & 0xFF);
        pb[i] = static_cast<float>(cap.px[i] & 0xFF);
    }
    for (int pass = 0; pass < 3; ++pass) { // three box passes ~ a smooth gaussian
        BoxBlurPlane(pr, W, H, 10);
        BoxBlurPlane(pg, W, H, 10);
        BoxBlurPlane(pb, W, H, 10);
    }
    for (int y = 0; y < g_pnlH; ++y)
        for (int x = 0; x < PANEL_W; ++x) {
            size_t s = static_cast<size_t>(y + m) * W + x + m;
            g_pnlBlur[static_cast<size_t>(y) * PANEL_W + x] = 0xFF000000 |
                (static_cast<UINT32>(pr[s]) << 16) | (static_cast<UINT32>(pg[s]) << 8) | static_cast<UINT32>(pb[s]);
        }
    SurfFree(cap);
}

// Card (frosted backdrop + tint veil + glass light) and the header.
void PanelBuildBase() {
    const int W = PANEL_W, H = g_pnlH;
    g_pnlBase.assign(static_cast<size_t>(W) * H, 0);
    Canvas c{ g_pnlBase.data(), W, H };

    // veil: a deep version of the tint, so white text always reads
    COLORREF veil = RGB(GetRValue(g_pv.tint) * 3 / 10 + 14, GetGValue(g_pv.tint) * 3 / 10 + 14, GetBValue(g_pv.tint) * 3 / 10 + 18);
    const float va = 0.58f;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            float cov = std::min(1.f, std::max(0.f, 0.5f - RRectDist(x + 0.5f, y + 0.5f, 0.5f, 0.5f, W - 0.5f, H - 0.5f, PANEL_R)));
            if (cov <= 0.f) continue;
            UINT32 b = g_pnlBlur[static_cast<size_t>(y) * W + x];
            float r = ((b >> 16) & 0xFF) * (1 - va) + GetRValue(veil) * va;
            float g = ((b >> 8) & 0xFF) * (1 - va) + GetGValue(veil) * va;
            float bl = (b & 0xFF) * (1 - va) + GetBValue(veil) * va;
            Over(c.px[static_cast<size_t>(y) * W + x], r, g, bl, cov);
        }
    GlassLight(c, 0.5f, 0.5f, W - 0.5f, H - 0.5f, PANEL_R, 1.f);

    DrawLogo(c, g_pnlMask, PANEL_PAD, 18, 44, g_fLogo);
    DrawTextOn(c, g_pnlMask, L"REICO", { 74, 16, W - PANEL_PAD, 46 }, g_fTitle, RGB(255, 255, 255), 0.97f, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    DrawTextOn(c, g_pnlMask, L"liquid desktop", { 75, 44, W - PANEL_PAD, 60 }, g_fSmall, RGB(255, 255, 255), 0.55f, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
}

float PanelCtlTarget(const PanelCtl& k) {
    switch (k.kind) {
    case CK_TOGGLE: { bool* v = PanelToggleVal(k.id); return v && *v ? 1.f : 0.f; }
    case CK_ACTION: return (k.id == P_EDIT && editMode) || (k.id == P_GLASS && g_glassOn) ? 1.f : 0.f;
    case CK_SEGMENT: return static_cast<float>(g_pv.hover);
    case CK_SWATCH: return k.colour == g_pv.tint ? 1.f : 0.f;
    }
    return 0.f;
}

// Eases every control toward its target; returns true while anything still moves.
bool PanelAnimateCtls(bool snap) {
    bool moving = false;
    for (size_t i = 0; i < g_ctls.size(); ++i) {
        PanelCtl& k = g_ctls[i];
        float ht = (static_cast<int>(i) == g_pnlHot || static_cast<int>(i) == g_pnlDrag) ? 1.f : 0.f;
        float ot = PanelCtlTarget(k);
        if (snap) { k.hot = ht; k.on = ot; continue; }
        k.hot += (ht - k.hot) * 0.3f;
        k.on += (ot - k.on) * 0.28f;
        if (std::fabs(ht - k.hot) < 0.01f) k.hot = ht; else moving = true;
        if (std::fabs(ot - k.on) < 0.01f) k.on = ot; else moving = true;
    }
    return moving;
}

// ---- slider previews: a small tile next to each slider showing what it changes ----

std::vector<UINT32> IconToSize(const AlphaIcon& a, int size);                                       // dock section
void BlitIconSmooth(Canvas c, const std::vector<UINT32>& px, int n, float x0, float y0, float size, float alpha);

// A mini "wallpaper" (colourful gradient + checks) so tint and transparency are visible on it.
void PreviewBackdrop(Canvas c, float x0, float y0, float x1, float y1, float r) {
    ForRRect(c, x0, y0, x1, y1, [&](int x, int y, UINT32& p) {
        float cov = Clamp01(0.5f - RRectDist(x + 0.5f, y + 0.5f, x0, y0, x1, y1, r));
        if (cov <= 0.f) return;
        float u = (x - x0) / (x1 - x0), v = (y - y0) / (y1 - y0);
        float R = 40 + 190 * u, G = 110 + 70 * v, B = 210 - 150 * u;
        if (((x - static_cast<int>(x0)) / 6 + (y - static_cast<int>(y0)) / 6) & 1) { R *= 0.7f; G *= 0.7f; B *= 0.7f; }
        Over(p, R, G, B, cov);
    });
}

// A glass window over the mini wallpaper: tint layer (tint strength) under the
// window's content (window opacity), with the glass rim.
void DrawWindowPreview(Canvas c, float x0, float y0, float x1, float y1, int tintA, int wAlpha) {
    const float r = 9.f;
    PreviewBackdrop(c, x0, y0, x1, y1, r);
    FillRRect(c, x0, y0, x1, y1, r, g_pv.tint, tintA / 255.f);
    float wa = wAlpha / 255.f; // window content: a title bar + a few lines of "text"
    FillRRect(c, x0, y0, x1, y0 + 9, r, RGB(30, 30, 34), 0.55f * wa);
    for (int i = 0; i < 3; ++i)
        FillRRect(c, x0 + 6, y0 + 15 + i * 7, x1 - 8 - i * 6, y0 + 18 + i * 7, 1.5f, RGB(240, 240, 240), 0.9f * wa);
    GlassLight(c, x0, y0, x1, y1, r, 1.f);
}

// One of your desktop icons styled with the panel's current (not yet applied) values.
std::vector<UINT32> g_pvIcon;
std::wstring g_pvIconKey;

const std::vector<UINT32>& PreviewIcon() {
    wchar_t key[128];
    swprintf(key, 128, L"%d %d %d %d %d %06lx", g_pv.glassAmt, g_pv.bright, g_pv.edge, g_pv.iconGlass, g_pv.iconTint,
             static_cast<unsigned long>(g_pv.tint));
    if (key == g_pvIconKey) return g_pvIcon;
    g_pvIconKey = key;
    g_pvIcon.clear();
    for (const auto& it : items) {
        if (!it.hIcon) continue;
        AlphaIcon a = ExtractIconAlpha(it.hIcon);
        if (!a.valid) continue;
        a.pixels = IconToSize(a, 48);
        a.w = a.h = 48;
        GlassConfig saved = g_glassCfg; // style with the panel's values, then put the real ones back
        g_glassCfg.iconGlassAmount = g_pv.glassAmt;
        g_glassCfg.iconBrightness = g_pv.bright;
        g_glassCfg.iconEdgeBlur = g_pv.edge;
        g_glassCfg.iconGlass = g_pv.iconGlass;
        g_glassCfg.iconTint = g_pv.iconTint;
        g_glassCfg.tintR = GetRValue(g_pv.tint);
        g_glassCfg.tintG = GetGValue(g_pv.tint);
        g_glassCfg.tintB = GetBValue(g_pv.tint);
        StyleIcon(a);
        g_glassCfg = saved;
        g_pvIcon = std::move(a.pixels);
        break;
    }
    return g_pvIcon;
}

void PanelSliderPreview(Canvas c, const PanelCtl& k) {
    float x0 = k.r.right + 12.f, y0 = k.r.top + 1.f, x1 = x0 + PANEL_PREVIEW, y1 = y0 + PANEL_PREVIEW;
    switch (k.id) {
    case P_TINTA:
    case P_WALPHA:
        DrawWindowPreview(c, x0, y0, x1, y1, g_pv.tintA, g_pv.wAlpha);
        break;
    case P_TINTV:
        FillRRect(c, x0, y0, x1, y1, 10, g_pv.tint, 1.f);
        GlassLight(c, x0, y0, x1, y1, 10, 1.f);
        break;
    case P_GLASSAMT:
    case P_BRIGHT:
    case P_EDGE: {
        PreviewBackdrop(c, x0, y0, x1, y1, 10);
        const auto& icon = PreviewIcon();
        if (!icon.empty()) BlitIconSmooth(c, icon, 48, x0 + 3, y0 + 3, PANEL_PREVIEW - 6.f, 1.f);
        GlassLight(c, x0, y0, x1, y1, 10, 0.7f);
        break;
    }
    }
}

void PanelDrawCtl(Canvas c, const PanelCtl& k) {
    const COLORREF white = RGB(255, 255, 255), acc = AccentColour();
    const float x0 = static_cast<float>(k.r.left), y0 = static_cast<float>(k.r.top);
    const float x1 = static_cast<float>(k.r.right), y1 = static_cast<float>(k.r.bottom);
    const UINT one = DT_SINGLELINE | DT_VCENTER;

    switch (k.kind) {
    case CK_SECTION:
        DrawTextOn(c, g_pnlMask, k.label.c_str(), k.r, g_fSmall, white, 0.5f, DT_LEFT | one);
        break;

    case CK_LABEL:
        DrawTextOn(c, g_pnlMask, k.label.c_str(), k.r, g_fBody, white, 0.85f, DT_LEFT | one);
        break;

    case CK_ACTION: {
        FillRRect(c, x0, y0, x1, y1, 12, white, 0.09f + 0.09f * k.hot);
        if (k.on > 0.01f) FillRRect(c, x0, y0, x1, y1, 12, acc, 0.55f * k.on);
        GlassLight(c, x0, y0, x1, y1, 12, 0.55f + 0.3f * k.hot);
        DrawTextOn(c, g_pnlMask, k.label.c_str(), k.r, g_fBtn, white, 0.95f, DT_CENTER | one);
        break;
    }

    case CK_TOGGLE: {
        RECT lr = k.r; lr.right -= 50;
        DrawTextOn(c, g_pnlMask, k.label.c_str(), lr, g_fBody, white, 0.92f, DT_LEFT | one);
        float tx1 = x1, tx0 = x1 - 42, ty0 = (y0 + y1) / 2 - 11, ty1 = ty0 + 22;
        FillRRect(c, tx0, ty0, tx1, ty1, 11, white, 0.16f + 0.06f * k.hot);
        if (k.on > 0.01f) FillRRect(c, tx0, ty0, tx1, ty1, 11, acc, 0.9f * k.on);
        GlassLight(c, tx0, ty0, tx1, ty1, 11, 0.5f);
        float kx = tx0 + 11 + (tx1 - tx0 - 22) * k.on;
        FillCircle(c, kx, (ty0 + ty1) / 2, 9, white, 0.97f);
        GlassLight(c, kx - 9, (ty0 + ty1) / 2 - 9, kx + 9, (ty0 + ty1) / 2 + 9, 9, 0.6f);
        break;
    }

    case CK_SLIDER: {
        int* v = PanelSliderVal(k.id);
        int val = v ? *v : k.lo;
        RECT lr{ k.r.left, k.r.top, k.r.right, k.r.top + 18 };
        DrawTextOn(c, g_pnlMask, k.label.c_str(), lr, g_fBody, white, 0.92f, DT_LEFT | one);
        wchar_t buf[16];
        swprintf(buf, 16, L"%d", val);
        DrawTextOn(c, g_pnlMask, buf, lr, g_fBody, white, 0.6f, DT_RIGHT | one);

        float tl = x0 + 8, tr = x1 - 8, ty = y0 + 31;
        float f = (k.hi > k.lo) ? static_cast<float>(val - k.lo) / (k.hi - k.lo) : 0.f;
        float kx = tl + (tr - tl) * f;
        FillRRect(c, tl - 3, ty - 3, tr + 3, ty + 3, 3, white, 0.18f);
        FillRRect(c, tl - 3, ty - 3, kx + 3, ty + 3, 3, acc, 0.95f);
        float kr = 8.f + 1.5f * k.hot;
        FillCircle(c, kx, ty, kr, white, 0.97f);
        GlassLight(c, kx - kr, ty - kr, kx + kr, ty + kr, kr, 0.6f);
        PanelSliderPreview(c, k); // what this slider changes, live
        break;
    }

    case CK_SWATCH: {
        float cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
        if (k.on > 0.01f) FillCircle(c, cx, cy, 13.f, white, 0.9f * k.on);
        FillCircle(c, cx, cy, 10.f + 1.f * k.hot, k.colour == RGB(255, 255, 255) ? RGB(235, 235, 240) : k.colour, 1.f);
        GlassLight(c, cx - 10, cy - 10, cx + 10, cy + 10, 10, 0.7f);
        break;
    }

    case CK_WHEEL: {
        // hue around the circle, saturation outwards (shown at full brightness so it reads)
        const float R = 56.f, cx = x0 + R + 2.f, cy = (y0 + y1) / 2.f;
        for (int y = static_cast<int>(cy - R - 1); y <= static_cast<int>(cy + R + 1); ++y)
            for (int x = static_cast<int>(cx - R - 1); x <= static_cast<int>(cx + R + 1); ++x) {
                if (x < 0 || y < 0 || x >= c.w || y >= c.h) continue;
                float dx = x + 0.5f - cx, dy = y + 0.5f - cy, d = std::sqrt(dx * dx + dy * dy);
                float cov = Clamp01(R - d + 0.5f);
                if (cov <= 0.f) continue;
                float h = std::atan2(dy, dx) * 57.29578f;
                if (h < 0.f) h += 360.f;
                COLORREF col = HsvToRgb(h, std::min(1.f, d / R), 1.f);
                Over(c.px[static_cast<size_t>(y) * c.w + x], GetRValue(col), GetGValue(col), GetBValue(col), cov);
            }
        GlassLight(c, cx - R, cy - R, cx + R, cy + R, R, 0.8f);

        // where the tint sits on the wheel: a glass ring filled with the actual colour
        float a = g_pv.hue / 57.29578f, sx = cx + std::cos(a) * g_pv.sat * R, sy = cy + std::sin(a) * g_pv.sat * R;
        float kr = 8.f + 2.f * k.hot;
        FillCircle(c, sx, sy, kr + 2.f, RGB(0, 0, 0), 0.25f);
        FillCircle(c, sx, sy, kr, white, 0.95f);
        FillCircle(c, sx, sy, kr - 2.5f, g_pv.tint, 1.f);
        GlassLight(c, sx - kr, sy - kr, sx + kr, sy + kr, kr, 0.8f);

        // preview + hex code
        float px0 = cx + R + 18.f, px1 = x1;
        DrawTextOn(c, g_pnlMask, k.label.c_str(), { static_cast<LONG>(px0), k.r.top + 6, static_cast<LONG>(px1), k.r.top + 24 },
                   g_fBody, white, 0.92f, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        FillRRect(c, px0, y0 + 30, px1, y0 + 82, 14, g_pv.tint, 1.f);
        GlassLight(c, px0, y0 + 30, px1, y0 + 82, 14, 1.f);
        wchar_t hex[16];
        swprintf(hex, 16, L"#%02X%02X%02X", GetRValue(g_pv.tint), GetGValue(g_pv.tint), GetBValue(g_pv.tint));
        DrawTextOn(c, g_pnlMask, hex, { static_cast<LONG>(px0), k.r.top + 88, static_cast<LONG>(px1), k.r.top + 106 },
                   g_fBtn, white, 0.6f, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        break;
    }

    case CK_SEGMENT: {
        FillRRect(c, x0, y0, x1, y1, 15, white, 0.08f);
        float sw = (x1 - x0) / 4.f;
        if (g_pnlHot >= 0 && &g_ctls[g_pnlHot] == &k) {
            POINT p; GetCursorPos(&p); ScreenToClient(g_panel, &p);
            int part = std::min(3, std::max(0, static_cast<int>((p.x - x0) / sw)));
            if (part != g_pv.hover) FillRRect(c, x0 + part * sw + 2, y0 + 2, x0 + (part + 1) * sw - 2, y1 - 2, 13, white, 0.06f);
        }
        float sx = x0 + k.on * sw;
        FillRRect(c, sx + 2, y0 + 2, sx + sw - 2, y1 - 2, 13, white, 0.22f);
        GlassLight(c, sx + 2, y0 + 2, sx + sw - 2, y1 - 2, 13, 0.6f);
        for (int i = 0; i < 4; ++i) {
            RECT pr{ static_cast<LONG>(x0 + i * sw), k.r.top, static_cast<LONG>(x0 + (i + 1) * sw), k.r.bottom };
            DrawTextOn(c, g_pnlMask, kHoverNames[i], pr, g_fBtn, white, i == g_pv.hover ? 0.97f : 0.6f, DT_CENTER | one);
        }
        break;
    }

    case CK_CHIP:
        FillRRect(c, x0, y0, x1, y1, 13, white, 0.10f + 0.12f * k.hot);
        GlassLight(c, x0, y0, x1, y1, 13, 0.45f + 0.3f * k.hot);
        DrawTextOn(c, g_pnlMask, k.label.c_str(), k.r, g_fBtn, white, 0.92f, DT_CENTER | one);
        break;

    case CK_LINK:
        DrawTextOn(c, g_pnlMask, k.label.c_str(), k.r, g_fBtn, k.id == P_EXIT ? RGB(255, 190, 190) : white,
                   0.6f + 0.4f * k.hot, (k.id == P_EXIT ? DT_RIGHT : DT_LEFT) | one);
        break;
    }
}

void PanelPresent() {
    float t = std::min(1.f, std::max(0.f, g_pnlOpenT));
    float e = 1.f - (1.f - t) * (1.f - t) * (1.f - t); // ease-out cubic
    memcpy(g_pnl.px, g_pnlBase.data(), sizeof(UINT32) * g_pnlBase.size());
    Canvas c = g_pnl.cv();
    for (const auto& k : g_ctls) PanelDrawCtl(c, k);

    POINT dst{ g_pnlX, g_pnlY + static_cast<int>((1.f - e) * 16.f) }, src{ 0, 0 };
    SIZE sz{ PANEL_W, g_pnlH };
    BLENDFUNCTION bf{ AC_SRC_OVER, 0, static_cast<BYTE>(255.f * e), AC_SRC_ALPHA };
    UpdateLayeredWindow(g_panel, nullptr, &dst, &sz, g_pnl.dc, &src, 0, &bf, ULW_ALPHA);
}

void PanelKick() {
    if (!g_pnlTimerOn) { SetTimer(g_panel, PANEL_ANIM_TIMER_ID, 15, nullptr); g_pnlTimerOn = true; }
}

void PanelTick() {
    DWORD el = GetTickCount() - g_pnlAnimStart;
    float dur = g_pnlClosing ? 140.f : 220.f;
    float p = std::min(1.f, el / dur);
    g_pnlOpenT = g_pnlClosing ? 1.f - p : p;
    bool moving = PanelAnimateCtls(false) || p < 1.f;
    PanelPresent();

    if (g_pnlClosing && p >= 1.f) {
        ShowWindow(g_panel, SW_HIDE);
        g_pnlVisible = false;
        g_pnlClosing = false;
        moving = false;
    }
    if (!moving) { KillTimer(g_panel, PANEL_ANIM_TIMER_ID); g_pnlTimerOn = false; }
}

void PanelClose() {
    if (!g_pnlVisible || g_pnlClosing) return;
    if (g_pnlDrag >= 0) { ReleaseCapture(); g_pnlDrag = -1; }
    g_pnlClosing = true;
    g_pnlAnimStart = GetTickCount();
    g_pnlLastClose = g_pnlAnimStart;
    PanelKick();
}

void PanelOpen() {
    if (!g_panel) return;
    if (g_pnlVisible) ShowWindow(g_panel, SW_HIDE); // so the backdrop capture doesn't include us

    PanelLoadVals();
    PanelLayout();
    if (!SurfCreate(g_pnl, PANEL_W, g_pnlH) || !SurfCreate(g_pnlMask, PANEL_W, g_pnlH)) return;

    // above the tray, wherever the taskbar is
    POINT c;
    GetCursorPos(&c);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromPoint(c, MONITOR_DEFAULTTONEAREST), &mi);
    RECT wa = mi.rcWork;
    g_pnlX = std::min(std::max(c.x - PANEL_W / 2, wa.left + 12), wa.right - PANEL_W - 12);
    g_pnlY = (c.y < wa.top) ? wa.top + 12 : wa.bottom - g_pnlH - 12;
    if (c.x < wa.left) g_pnlX = wa.left + 12;          // taskbar on the left
    else if (c.x >= wa.right) g_pnlX = wa.right - PANEL_W - 12; // taskbar on the right
    g_pnlY = std::max(g_pnlY, static_cast<int>(wa.top) + 12);

    PanelCaptureBackdrop();
    PanelBuildBase();
    g_pnlHot = g_pnlPressed = g_pnlDrag = -1;
    PanelAnimateCtls(true);

    g_pnlVisible = true;
    g_pnlClosing = false;
    g_pnlOpenT = 0.f;
    g_pnlAnimStart = GetTickCount();
    PanelPresent();
    ShowWindow(g_panel, SW_SHOW);
    SetForegroundWindow(g_panel); // needed so clicking elsewhere closes it
    PanelKick();
}

void PanelOnReload() {
    if (!g_pnlVisible || g_pnlClosing) return;
    PanelLoadVals();
    PanelBuildBase(); // veil + logo follow the tint
    PanelKick();
}

int PanelHit(POINT p) {
    for (size_t i = 0; i < g_ctls.size(); ++i) {
        const PanelCtl& k = g_ctls[i];
        if (k.kind == CK_SECTION || k.kind == CK_LABEL) continue;
        if (PtInRect(&k.r, p)) return static_cast<int>(i);
    }
    return -1;
}

void PanelSlideTo(const PanelCtl& k, int x) {
    int* v = PanelSliderVal(k.id);
    if (!v) return;
    float tl = k.r.left + 8.f, tr = k.r.right - 8.f;
    float f = std::min(1.f, std::max(0.f, (x - tl) / (tr - tl)));
    *v = k.lo + static_cast<int>(std::lround(f * (k.hi - k.lo)));
    if (k.id == P_TINTV) { // tint brightness: recolour the panel's own glass right away
        g_pv.tint = HsvToRgb(g_pv.hue, g_pv.sat, g_pv.tintV / 100.f);
        PanelBuildBase();
    }
}

// Colour wheel: angle = hue, distance from the centre = saturation.
void PanelWheelTo(const PanelCtl& k, POINT p) {
    const float R = 56.f, cx = k.r.left + R + 2.f, cy = (k.r.top + k.r.bottom) / 2.f;
    float dx = p.x - cx, dy = p.y - cy;
    g_pv.sat = std::min(1.f, std::sqrt(dx * dx + dy * dy) / R);
    g_pv.hue = std::atan2(dy, dx) * 57.29578f;
    if (g_pv.hue < 0.f) g_pv.hue += 360.f;
    if (g_pv.tintV < 5) g_pv.tintV = 50; // picking a colour on a black tint: make it visible
    g_pv.tint = HsvToRgb(g_pv.hue, g_pv.sat, g_pv.tintV / 100.f);
    PanelBuildBase(); // the panel's own glass previews it live; windows + icons follow on release
}

bool PanelInWheel(const PanelCtl& k, POINT p) {
    const float R = 56.f, cx = k.r.left + R + 2.f, cy = (k.r.top + k.r.bottom) / 2.f;
    float dx = p.x - cx, dy = p.y - cy;
    return dx * dx + dy * dy <= (R + 4.f) * (R + 4.f);
}

void PanelActivate(int idx, POINT p) {
    const PanelCtl k = g_ctls[idx]; // copy: some actions rebuild g_ctls
    if (bool* t = PanelToggleVal(k.id)) { *t = !*t; PanelScheduleApply(120); return; }

    switch (k.id) {
    case P_REFRESH: ScanDesktop(); Render(g_hwnd); return;
    case P_EDIT:
        editMode = !editMode;
        hoverIndex = -1;
        EnsureAnimTimer(g_hwnd);
        Render(g_hwnd);
        return;
    case P_GLASS: GlassSetEnabled(!g_glassOn); return;
    case P_HOVER: {
        int part = std::min(3, std::max(0, static_cast<int>((p.x - k.r.left) * 4 / std::max(1L, k.r.right - k.r.left))));
        if (part != g_pv.hover) { g_pv.hover = part; PanelScheduleApply(120); }
        return;
    }
    case P_SETTINGS: PanelClose(); OpenSettings(); return;
    case P_EXIT: PanelClose(); DestroyWindow(g_hwnd); return;
    }
    if (k.kind == CK_SWATCH && k.colour != g_pv.tint) {
        g_pv.tint = k.colour;
        float v = 0.f;
        RgbToHsv(k.colour, g_pv.hue, g_pv.sat, v); // the wheel + brightness follow the swatch
        g_pv.tintV = static_cast<int>(std::lround(v * 100.f));
        PanelBuildBase();
        PanelScheduleApply(120);
    }
    if (k.kind == CK_CHIP && k.lo < static_cast<int>(g_pnlPresets.size())) {
        ApplyPreset(g_pnlPresets[k.lo]); // reloads -> PanelOnReload refreshes the values
        UiFromConfig();
    }
}

LRESULT CALLBACK PanelProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_MOUSEACTIVATE:
        return MA_ACTIVATE;

    case WM_ACTIVATE:
        if (LOWORD(w) == WA_INACTIVE) PanelClose(); // clicked anywhere else
        return 0;

    case WM_KEYDOWN:
        if (w == VK_ESCAPE) PanelClose();
        return 0;

    case WM_MOUSEMOVE: {
        POINT p{ GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, h, 0 };
        TrackMouseEvent(&tme);
        if (g_pnlDrag >= 0) {
            if (g_ctls[g_pnlDrag].kind == CK_WHEEL) PanelWheelTo(g_ctls[g_pnlDrag], p);
            else PanelSlideTo(g_ctls[g_pnlDrag], p.x);
        }
        if (g_pnlMoving) {
            POINT sp;
            GetCursorPos(&sp);
            g_pnlX = sp.x - g_pnlMoveOff.x;
            g_pnlY = sp.y - g_pnlMoveOff.y;
            PanelPresent();
            return 0;
        }
        int hot = PanelHit(p);
        bool seg = hot >= 0 && g_ctls[hot].kind == CK_SEGMENT; // its hover follows the cursor inside it
        if (hot != g_pnlHot || g_pnlDrag >= 0 || seg) { g_pnlHot = hot; PanelKick(); }
        SetCursor(LoadCursor(nullptr, hot >= 0 ? IDC_HAND : IDC_ARROW));
        return 0;
    }

    case WM_MOUSELEAVE:
        if (g_pnlDrag < 0) { g_pnlHot = -1; PanelKick(); }
        return 0;

    case WM_LBUTTONDOWN: {
        POINT p{ GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        g_pnlPressed = PanelHit(p);
        if (g_pnlPressed >= 0 && g_ctls[g_pnlPressed].kind == CK_SLIDER) {
            g_pnlDrag = g_pnlPressed;
            SetCapture(h);
            PanelSlideTo(g_ctls[g_pnlDrag], p.x);
            PanelKick();
        } else if (g_pnlPressed >= 0 && g_ctls[g_pnlPressed].kind == CK_WHEEL && PanelInWheel(g_ctls[g_pnlPressed], p)) {
            g_pnlDrag = g_pnlPressed; // drag around the wheel; applied on release like the sliders
            SetCapture(h);
            PanelWheelTo(g_ctls[g_pnlDrag], p);
            PanelKick();
        } else if (g_pnlPressed < 0 && p.y < 70) { // empty part of the header: move the panel
            POINT sp;
            GetCursorPos(&sp);
            g_pnlMoving = true;
            g_pnlMoveOff = { sp.x - g_pnlX, sp.y - g_pnlY };
            SetCapture(h);
        }
        return 0;
    }

    case WM_LBUTTONUP: {
        POINT p{ GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        if (g_pnlMoving) {
            ReleaseCapture();
            g_pnlMoving = false;
            // the frosted backdrop was captured at the old spot: hide for a frame, re-capture here
            POINT src{ 0, 0 }, dst{ g_pnlX, g_pnlY };
            SIZE sz{ PANEL_W, g_pnlH };
            BLENDFUNCTION bf{ AC_SRC_OVER, 0, 0, AC_SRC_ALPHA };
            UpdateLayeredWindow(h, nullptr, &dst, &sz, g_pnl.dc, &src, 0, &bf, ULW_ALPHA);
            DwmFlushIfAvailable();
            PanelCaptureBackdrop();
            PanelBuildBase();
            PanelPresent();
            return 0;
        }
        if (g_pnlDrag >= 0) {
            ReleaseCapture();
            g_pnlDrag = -1;
            PanelScheduleApply(60); // sliders apply on release: a reload re-styles every icon
        } else {
            int hit = PanelHit(p);
            if (hit >= 0 && hit == g_pnlPressed) PanelActivate(hit, p);
        }
        g_pnlPressed = -1;
        PanelKick();
        return 0;
    }

    case WM_MOUSEWHEEL: {
        POINT p{ GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        ScreenToClient(h, &p);
        int i = PanelHit(p);
        if (i >= 0 && g_ctls[i].kind == CK_SLIDER) {
            PanelCtl& k = g_ctls[i];
            int step = std::max(1, (k.hi - k.lo) / 50) * (GET_WHEEL_DELTA_WPARAM(w) > 0 ? 1 : -1);
            int* v = PanelSliderVal(k.id);
            *v = std::min(k.hi, std::max(k.lo, *v + step));
            PanelScheduleApply(450);
            PanelKick();
        }
        return 0;
    }

    case WM_TIMER:
        if (w == PANEL_ANIM_TIMER_ID) PanelTick();
        else if (w == PANEL_APPLY_TIMER_ID) { KillTimer(h, PANEL_APPLY_TIMER_ID); PanelApply(); }
        return 0;
    }
    return DefWindowProcW(h, msg, w, l);
}

void PanelInit() {
    LoadInter();
    g_fBody = InterFont(14, 500);
    g_fBtn = InterFont(13, 600);
    g_fSmall = InterFont(11, 700);
    g_fTitle = InterFont(26, 700);
    g_fLogo = InterFont(28, 700);

    WNDCLASSW wc{};
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpfnWndProc = PanelProc;
    wc.lpszClassName = L"REICOPanel";
    RegisterClassW(&wc);
    g_panel = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"REICOPanel", L"REICO",
                              WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, wc.hInstance, nullptr);
}

void PanelShutdown() {
    if (g_panel) { DestroyWindow(g_panel); g_panel = nullptr; }
    SurfFree(g_pnl);
    SurfFree(g_pnlMask);
    for (HFONT* f : { &g_fBody, &g_fBtn, &g_fSmall, &g_fTitle, &g_fLogo })
        if (*f) { DeleteObject(*f); *f = nullptr; }
    UnloadInter();
}

// =====================================================================
// NOW PLAYING: a liquid-glass pop-up at the top of the screen, for 5 s
// when a song starts (or resumes, or the track changes) and again when it
// pauses/stops. Shown over everything, click-through, never takes focus.
//
// Source: Windows' global media session API (what the volume flyout uses),
// so it works for Spotify, browsers, etc. It's WinRT and MinGW ships no
// headers for it, so the few interfaces used are declared here; their IIDs
// and method order were read from Windows' own metadata (Windows.Media.Control).
// Everything is loaded at runtime (combase/shcore/ole32): no build changes.
// A worker thread polls it twice a second and posts changes to the UI thread.
// =====================================================================

namespace np {

struct IAsyncInfoX : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Id(UINT32*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Status(int*) = 0; // 0 started, 1 completed, 2 canceled, 3 error
    virtual HRESULT STDMETHODCALLTYPE get_ErrorCode(HRESULT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE Cancel() = 0;
    virtual HRESULT STDMETHODCALLTYPE Close() = 0;
};
template <class T> struct IAsyncOp : IInspectable { // IAsyncOperation<T>
    virtual HRESULT STDMETHODCALLTYPE put_Completed(IUnknown*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Completed(IUnknown**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetResults(T** result) = 0;
};
struct IStreamRef : IInspectable { // Windows.Storage.Streams.IRandomAccessStreamReference
    virtual HRESULT STDMETHODCALLTYPE OpenReadAsync(IAsyncOp<IInspectable>**) = 0;
};
struct IPlaybackInfo : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Controls(IInspectable**) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_PlaybackStatus(int*) = 0; // 0 closed 1 opened 2 changing 3 stopped 4 playing 5 paused
};
struct IMediaProps : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Title(HSTRING*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Subtitle(HSTRING*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_AlbumArtist(HSTRING*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Artist(HSTRING*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_AlbumTitle(HSTRING*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_TrackNumber(INT32*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Genres(IInspectable**) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_AlbumTrackCount(INT32*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_PlaybackType(IInspectable**) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Thumbnail(IStreamRef**) = 0;
};
struct ISession : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_SourceAppUserModelId(HSTRING*) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryGetMediaPropertiesAsync(IAsyncOp<IMediaProps>**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetTimelineProperties(IInspectable**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPlaybackInfo(IPlaybackInfo**) = 0;
    // the Try* commands return IAsyncOperation<bool>; results aren't needed, so only Release() is called
    virtual HRESULT STDMETHODCALLTYPE TryPlayAsync(IInspectable**) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryPauseAsync(IInspectable**) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryStopAsync(IInspectable**) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryRecordAsync(IInspectable**) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryFastForwardAsync(IInspectable**) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryRewindAsync(IInspectable**) = 0;
    virtual HRESULT STDMETHODCALLTYPE TrySkipNextAsync(IInspectable**) = 0;
    virtual HRESULT STDMETHODCALLTYPE TrySkipPreviousAsync(IInspectable**) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryChangeChannelUpAsync(IInspectable**) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryChangeChannelDownAsync(IInspectable**) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryTogglePlayPauseAsync(IInspectable**) = 0;
};
template <class T> struct IVectorView : IInspectable { // IReadOnlyList<T>
    virtual HRESULT STDMETHODCALLTYPE GetAt(UINT32, T**) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Size(UINT32*) = 0;
};
struct IManager : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE GetCurrentSession(ISession**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetSessions(IVectorView<ISession>**) = 0;
};
struct IManagerStatics : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE RequestAsync(IAsyncOp<IManager>**) = 0;
};

const IID IID_ManagerStatics = { 0x2050c4ee, 0x11a0, 0x57de, { 0xae, 0xd7, 0xc9, 0x7c, 0x70, 0x33, 0x82, 0x45 } };
const IID IID_AsyncInfo = { 0x00000036, 0x0000, 0x0000, { 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };
const IID IID_Stream = { 0x0000000c, 0x0000, 0x0000, { 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };
const CLSID CLSID_WicFactory = { 0xcacaf262, 0x9370, 0x4615, { 0xa1, 0x3b, 0x9f, 0x55, 0x39, 0xda, 0x4c, 0x0a } };
const IID IID_WicFactory = { 0xec5ec8a9, 0xc395, 0x4314, { 0x9c, 0x77, 0x54, 0xd7, 0xa9, 0x35, 0xff, 0x70 } };
const GUID FMT_32bppPBGRA = { 0x6fddc324, 0x4e03, 0x4bfe, { 0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9, 0x10 } };

struct Api {
    HRESULT (WINAPI* RoInitialize)(int) = nullptr;
    void (WINAPI* RoUninitialize)() = nullptr;
    HRESULT (WINAPI* RoGetActivationFactory)(HSTRING, REFIID, void**) = nullptr;
    HRESULT (WINAPI* WindowsCreateString)(LPCWSTR, UINT32, HSTRING*) = nullptr;
    HRESULT (WINAPI* WindowsDeleteString)(HSTRING) = nullptr;
    PCWSTR (WINAPI* WindowsGetStringRawBuffer)(HSTRING, UINT32*) = nullptr;
    HRESULT (WINAPI* CreateStreamOverRandomAccessStream)(IUnknown*, REFIID, void**) = nullptr;
    HRESULT (WINAPI* CoCreateInstance)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID*) = nullptr;

    bool Load() {
        HMODULE cb = LoadLibraryW(L"combase.dll"), sc = LoadLibraryW(L"shcore.dll"), ole = LoadLibraryW(L"ole32.dll");
        if (!cb || !ole) return false;
        auto get = [](HMODULE m, const char* n) { return m ? reinterpret_cast<void*>(GetProcAddress(m, n)) : nullptr; };
        RoInitialize = reinterpret_cast<decltype(RoInitialize)>(get(cb, "RoInitialize"));
        RoUninitialize = reinterpret_cast<decltype(RoUninitialize)>(get(cb, "RoUninitialize"));
        RoGetActivationFactory = reinterpret_cast<decltype(RoGetActivationFactory)>(get(cb, "RoGetActivationFactory"));
        WindowsCreateString = reinterpret_cast<decltype(WindowsCreateString)>(get(cb, "WindowsCreateString"));
        WindowsDeleteString = reinterpret_cast<decltype(WindowsDeleteString)>(get(cb, "WindowsDeleteString"));
        WindowsGetStringRawBuffer = reinterpret_cast<decltype(WindowsGetStringRawBuffer)>(get(cb, "WindowsGetStringRawBuffer"));
        CreateStreamOverRandomAccessStream = reinterpret_cast<decltype(CreateStreamOverRandomAccessStream)>(get(sc, "CreateStreamOverRandomAccessStream"));
        CoCreateInstance = reinterpret_cast<decltype(CoCreateInstance)>(get(ole, "CoCreateInstance"));
        return RoInitialize && RoUninitialize && RoGetActivationFactory && WindowsCreateString &&
               WindowsDeleteString && WindowsGetStringRawBuffer;
    }
} api;

std::wstring TakeHString(HSTRING h) {
    if (!h) return L"";
    UINT32 n = 0;
    PCWSTR p = api.WindowsGetStringRawBuffer(h, &n);
    std::wstring s(p ? p : L"", p ? n : 0);
    api.WindowsDeleteString(h);
    return s;
}

// Blocks (this is the worker thread) until the async operation finishes; takes ownership of op.
template <class T> T* Await(IAsyncOp<T>* op, DWORD timeoutMs = 3000) {
    if (!op) return nullptr;
    T* result = nullptr;
    IAsyncInfoX* info = nullptr;
    if (SUCCEEDED(op->QueryInterface(IID_AsyncInfo, reinterpret_cast<void**>(&info))) && info) {
        int st = 0;
        DWORD t0 = GetTickCount();
        while (SUCCEEDED(info->get_Status(&st)) && st == 0 && GetTickCount() - t0 < timeoutMs) Sleep(10);
        if (st == 1) op->GetResults(&result);
        else info->Cancel();
        info->Release();
    }
    op->Release();
    return result;
}

// "Spotify.exe" -> "Spotify", "Microsoft.ZuneMusic_8wekyb3d8bbwe!Microsoft.ZuneMusic" -> "ZuneMusic"
std::wstring AppName(std::wstring id) {
    size_t bang = id.rfind(L'!');
    if (bang != std::wstring::npos) id = id.substr(bang + 1);
    if (id.size() > 4 && _wcsicmp(id.c_str() + id.size() - 4, L".exe") == 0) id.resize(id.size() - 4);
    size_t dot = id.rfind(L'.');
    if (dot != std::wstring::npos && dot + 1 < id.size()) id = id.substr(dot + 1);
    if (!id.empty()) id[0] = towupper(id[0]);
    return id;
}

} // namespace np

const int NP_ART = 84; // album art size in px

struct NowPlayingInfo {
    std::wstring title, artist, app;
    bool playing = false;
    std::vector<UINT32> art; // NP_ART x NP_ART premultiplied BGRA, empty = no art
};

const UINT WM_NOWPLAYING = WM_APP + 20; // worker -> pop-up, lParam = new NowPlayingInfo (receiver deletes)
HWND g_npWnd = nullptr;
HANDLE g_npThread = nullptr, g_npWake = nullptr;
volatile bool g_npQuit = false;
enum { NPC_NONE = 0, NPC_TOGGLE, NPC_NEXT, NPC_PREV };
volatile LONG g_npCmd = NPC_NONE; // pop-up button -> worker (the session lives on the worker thread)

// Album art, centre-cropped square, scaled to NP_ART with WIC.
void LoadArt(np::IMediaProps* props, NowPlayingInfo& out) {
    using namespace np;
    if (!api.CreateStreamOverRandomAccessStream || !api.CoCreateInstance) return;
    IStreamRef* ref = nullptr;
    if (FAILED(props->get_Thumbnail(&ref)) || !ref) return;
    IAsyncOp<IInspectable>* op = nullptr;
    HRESULT hr = ref->OpenReadAsync(&op);
    ref->Release();
    if (FAILED(hr)) return;
    IInspectable* ras = Await(op);
    if (!ras) return;
    IStream* stream = nullptr;
    hr = api.CreateStreamOverRandomAccessStream(ras, IID_Stream, reinterpret_cast<void**>(&stream));
    ras->Release();
    if (FAILED(hr) || !stream) return;

    IWICImagingFactory* wic = nullptr;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICBitmapClipper* clip = nullptr;
    IWICBitmapScaler* scale = nullptr;
    IWICFormatConverter* conv = nullptr;
    UINT w = 0, h = 0;
    if (SUCCEEDED(api.CoCreateInstance(CLSID_WicFactory, nullptr, CLSCTX_INPROC_SERVER, IID_WicFactory, reinterpret_cast<void**>(&wic))) &&
        SUCCEEDED(wic->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnLoad, &dec)) &&
        SUCCEEDED(dec->GetFrame(0, &frame)) && SUCCEEDED(frame->GetSize(&w, &h)) && w && h &&
        SUCCEEDED(wic->CreateBitmapClipper(&clip))) {
        UINT s = std::min(w, h);
        WICRect rc{ static_cast<INT>((w - s) / 2), static_cast<INT>((h - s) / 2), static_cast<INT>(s), static_cast<INT>(s) };
        out.art.assign(static_cast<size_t>(NP_ART) * NP_ART, 0);
        if (!(SUCCEEDED(clip->Initialize(frame, &rc)) && SUCCEEDED(wic->CreateBitmapScaler(&scale)) &&
              SUCCEEDED(scale->Initialize(clip, NP_ART, NP_ART, WICBitmapInterpolationModeFant)) &&
              SUCCEEDED(wic->CreateFormatConverter(&conv)) &&
              SUCCEEDED(conv->Initialize(scale, FMT_32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)) &&
              SUCCEEDED(conv->CopyPixels(nullptr, NP_ART * 4, NP_ART * NP_ART * 4, reinterpret_cast<BYTE*>(out.art.data())))))
            out.art.clear();
    }
    for (IUnknown* u : std::initializer_list<IUnknown*>{ conv, scale, clip, frame, dec, wic, stream })
        if (u) u->Release();
}

void PostNowPlaying(const NowPlayingInfo& info) {
    auto* copy = new NowPlayingInfo(info);
    if (!PostMessageW(g_npWnd, WM_NOWPLAYING, 0, reinterpret_cast<LPARAM>(copy))) delete copy;
}

DWORD WINAPI NowPlayingThread(void*) {
    using namespace np;
    if (!api.Load() || FAILED(api.RoInitialize(1 /* multithreaded */))) return 0;

    IManager* mgr = nullptr;
    {
        const wchar_t* cls = L"Windows.Media.Control.GlobalSystemMediaTransportControlsSessionManager";
        HSTRING hs = nullptr;
        IManagerStatics* st = nullptr;
        if (SUCCEEDED(api.WindowsCreateString(cls, static_cast<UINT32>(wcslen(cls)), &hs))) {
            api.RoGetActivationFactory(hs, IID_ManagerStatics, reinterpret_cast<void**>(&st));
            api.WindowsDeleteString(hs);
        }
        IAsyncOp<IManager>* op = nullptr;
        if (st && SUCCEEDED(st->RequestAsync(&op))) mgr = Await(op, 5000);
        if (st) st->Release();
    }
    if (!mgr) { api.RoUninitialize(); return 0; }

    bool first = true, wasPlaying = false;
    std::wstring lastKey, followed; // followed = the media app (AUMID) the pop-up is about
    NowPlayingInfo last;

    struct Sess { ISession* s; int status; std::wstring app; };
    auto sessionsNow = [&]() {
        // Windows' "current" session alone isn't enough: a paused video in another app can
        // become current while Spotify keeps playing, hiding its song changes. So look at all.
        std::vector<Sess> out;
        auto add = [&](ISession* s) {
            if (!s) return;
            for (const auto& e : out) if (e.s == s) { s->Release(); return; }
            Sess e{ s, 0, L"" };
            IPlaybackInfo* pi = nullptr;
            if (SUCCEEDED(s->GetPlaybackInfo(&pi)) && pi) { pi->get_PlaybackStatus(&e.status); pi->Release(); }
            HSTRING h = nullptr;
            s->get_SourceAppUserModelId(&h);
            e.app = TakeHString(h);
            out.push_back(e);
        };
        ISession* cur = nullptr;
        if (SUCCEEDED(mgr->GetCurrentSession(&cur))) add(cur);
        IVectorView<ISession>* list = nullptr;
        if (SUCCEEDED(mgr->GetSessions(&list)) && list) {
            UINT32 n = 0;
            list->get_Size(&n);
            for (UINT32 i = 0; i < n && i < 32; ++i) { ISession* s = nullptr; if (SUCCEEDED(list->GetAt(i, &s))) add(s); }
            list->Release();
        }
        return out;
    };

    while (!g_npQuit) {
        bool playing = wasPlaying;
        std::vector<Sess> all = sessionsNow();

        // Follow: the followed app if it's playing, else any app that's playing, else the
        // followed app (so its pause is seen), else Windows' current one.
        const Sess* pick = nullptr;
        for (const auto& e : all) if (e.app == followed && e.status == 4) { pick = &e; break; }
        if (!pick) for (const auto& e : all) if (e.status == 4) { pick = &e; break; }
        if (!pick) for (const auto& e : all) if (e.app == followed) { pick = &e; break; }
        if (!pick && !all.empty()) pick = &all.front();

        ISession* s = pick ? pick->s : nullptr;
        if (s) {
            // a button in the pop-up was pressed: it goes to the app the pop-up shows
            if (LONG cmd = InterlockedExchange(&g_npCmd, NPC_NONE)) {
                IInspectable* op = nullptr;
                if (cmd == NPC_TOGGLE) s->TryTogglePlayPauseAsync(&op);
                else if (cmd == NPC_NEXT) s->TrySkipNextAsync(&op);
                else if (cmd == NPC_PREV) s->TrySkipPreviousAsync(&op);
                if (op) op->Release();
                for (auto& e : all) e.s->Release();
                Sleep(250); // let the player react, then read the new state right away
                continue;
            }

            int status = pick->status;
            if (status == 4) { playing = true; followed = pick->app; }
            else if (status == 0 || status == 3 || status == 5) playing = false;
            // 1 opened / 2 changing = between tracks: keep the previous state (no false "stopped")

            if (playing) {
                IAsyncOp<IMediaProps>* op = nullptr;
                IMediaProps* p = SUCCEEDED(s->TryGetMediaPropertiesAsync(&op)) ? Await(op) : nullptr;
                if (p) {
                    NowPlayingInfo cur;
                    HSTRING h = nullptr;
                    p->get_Title(&h);  cur.title = TakeHString(h); h = nullptr;
                    p->get_Artist(&h); cur.artist = TakeHString(h); h = nullptr;
                    s->get_SourceAppUserModelId(&h); cur.app = AppName(TakeHString(h));
                    cur.playing = true;
                    std::wstring key = cur.title + L"\n" + cur.artist;
                    if (cur.title.empty()) {
                        lastKey.clear(); // untitled media: no pop-ups, and no stale "Paused" for the previous song
                    } else {
                        bool newTrack = key != lastKey;
                        if (newTrack) { LoadArt(p, cur); last = cur; lastKey = key; }
                        // a new track, or playback resumed (but not for music already playing at start-up)
                        if (!first && (newTrack || !wasPlaying)) PostNowPlaying(last);
                    }
                    p->Release();
                }
            }
        } else {
            playing = false; // no media session at all = stopped
        }
        for (auto& e : all) e.s->Release();

        if (!first && wasPlaying && !playing && !lastKey.empty()) {
            NowPlayingInfo stopped = last;
            stopped.playing = false;
            PostNowPlaying(stopped);
        }
        wasPlaying = playing;
        first = false;
        WaitForSingleObject(g_npWake, 500);
    }

    mgr->Release();
    api.RoUninitialize();
    return 0;
}

// ---- the pop-up --------------------------------------------------------

const int NP_BOX_W = 520, NP_BOX_H = 156;   // window; the card morphs inside it
const int NP_CARD_W = 480, NP_CARD_H = 112, NP_CARD_R = 30, NP_TOP = 14;
const float NP_DROP = 60.f;                  // droplet size the card grows from / shrinks to
const DWORD NP_IN = 680, NP_HOLD = 5000, NP_OUT = 560;
const UINT_PTR NP_TIMER_ID = 1;

Surface g_npS, g_npMask;
std::vector<UINT32> g_npBlur;
NowPlayingInfo g_npInfo;
DWORD g_npStart = 0, g_npContentStart = 0, g_npLastTick = 0;
bool g_npVisible = false, g_npMouseIn = false;
int g_npX = 0, g_npY = 0;
int g_npHot = -1, g_npPressed = -1;          // control buttons: 0 previous, 1 play/pause, 2 next
float g_npHotT[3] = {};                      // animated hover per button
float g_npBtnX[3] = {}, g_npBtnY = 0.f, g_npBtnR[3] = { 17.f, 23.f, 17.f };
HFONT g_npTitleF = nullptr, g_npArtistF = nullptr, g_npLabelF = nullptr, g_npIconF = nullptr,
      g_npPlayF = nullptr, g_npNoteF = nullptr;

inline float EaseOut3(float t) { t = Clamp01(t); return 1.f - (1.f - t) * (1.f - t) * (1.f - t); }
inline float EaseOutBack(float t) { t = Clamp01(t); const float c = 1.9f; return 1.f + (c + 1.f) * std::pow(t - 1.f, 3.f) + c * std::pow(t - 1.f, 2.f); }
inline float Jelly(float t, float freq) { t = Clamp01(t); return 1.f - std::pow(2.f, -10.f * t) * std::cos(t * 3.14159f * freq); }
inline float EaseInBack(float t) { t = Clamp01(t); const float c = 1.6f; return (c + 1.f) * t * t * t - c * t * t; }

void NpCaptureBackdrop() {
    Surface cap;
    g_npBlur.assign(static_cast<size_t>(NP_BOX_W) * NP_BOX_H, 0xFF202020);
    const int m = 30, W = NP_BOX_W + 2 * m, H = NP_BOX_H + 2 * m;
    if (!SurfCreate(cap, W, H)) return;
    HDC sdc = GetDC(nullptr);
    BitBlt(cap.dc, 0, 0, W, H, sdc, g_npX - m, g_npY - m, SRCCOPY | CAPTUREBLT);
    ReleaseDC(nullptr, sdc);
    const size_t n = static_cast<size_t>(W) * H;
    std::vector<float> pr(n), pg(n), pb(n);
    for (size_t i = 0; i < n; ++i) {
        pr[i] = static_cast<float>((cap.px[i] >> 16) & 0xFF);
        pg[i] = static_cast<float>((cap.px[i] >> 8) & 0xFF);
        pb[i] = static_cast<float>(cap.px[i] & 0xFF);
    }
    for (int pass = 0; pass < 3; ++pass) { BoxBlurPlane(pr, W, H, 10); BoxBlurPlane(pg, W, H, 10); BoxBlurPlane(pb, W, H, 10); }
    for (int y = 0; y < NP_BOX_H; ++y)
        for (int x = 0; x < NP_BOX_W; ++x) {
            size_t s = static_cast<size_t>(y + m) * W + x + m;
            g_npBlur[static_cast<size_t>(y) * NP_BOX_W + x] = 0xFF000000 |
                (static_cast<UINT32>(pr[s]) << 16) | (static_cast<UINT32>(pg[s]) << 8) | static_cast<UINT32>(pb[s]);
        }
    SurfFree(cap);
}

void NpDrawArt(Canvas c, float x0, float y0, float sz, float r, float a) {
    if (g_npInfo.art.empty()) {
        COLORREF acc = AccentColour();
        FillRRectGrad(c, x0, y0, x0 + sz, y0 + sz, r, acc,
                      RGB(GetRValue(acc) / 3, GetGValue(acc) / 3, GetBValue(acc) / 3), a);
        RECT tr{ static_cast<LONG>(x0), static_cast<LONG>(y0), static_cast<LONG>(x0 + sz), static_cast<LONG>(y0 + sz) };
        DrawGlassText(c, g_npMask, L"♫", tr, g_npNoteF, a, DT_CENTER | DT_VCENTER | DT_SINGLELINE, 0.f, 0.8f);
    } else {
        ForRRect(c, x0, y0, x0 + sz, y0 + sz, [&](int x, int y, UINT32& p) {
            float cov = Clamp01(0.5f - RRectDist(x + 0.5f, y + 0.5f, x0, y0, x0 + sz, y0 + sz, r));
            if (cov <= 0.f) return;
            int sx = std::min(NP_ART - 1, std::max(0, static_cast<int>((x + 0.5f - x0) / sz * NP_ART)));
            int sy = std::min(NP_ART - 1, std::max(0, static_cast<int>((y + 0.5f - y0) / sz * NP_ART)));
            UINT32 s = g_npInfo.art[static_cast<size_t>(sy) * NP_ART + sx];
            float sa = (s >> 24) / 255.f;
            if (sa <= 0.f) return;
            Over(p, ((s >> 16) & 0xFF) / sa, ((s >> 8) & 0xFF) / sa, (s & 0xFF) / sa, sa * cov * a);
        });
    }
    GlassLight(c, x0, y0, x0 + sz, y0 + sz, r, 0.8f * a);
}

// One frame: the card's shape at time t, its glass body, then the content.
void NpRender(DWORD t) {
    Canvas c = g_npS.cv();
    ZeroMemory(c.px, sizeof(UINT32) * static_cast<size_t>(c.w) * c.h);

    // ---- shape over time: droplet falls in -> stretches (jelly) -> holds -> shrinks -> floats away
    float w, h, yoff = 0.f, alpha = 1.f, content = 1.f;
    if (t < NP_IN) {
        float p1 = t / 190.f;                                   // droplet appears
        float d = NP_DROP * EaseOutBack(p1);
        alpha = Clamp01(p1 * 1.4f);
        yoff = -26.f * (1.f - EaseOut3(p1));
        float p2 = (static_cast<float>(t) - 150.f) / 520.f;     // stretch into the card
        w = p2 <= 0.f ? d : NP_DROP + (NP_CARD_W - NP_DROP) * Jelly(p2, 2.2f);
        h = p2 <= 0.f ? d : NP_DROP + (NP_CARD_H - NP_DROP) * Jelly(p2, 2.9f);
        content = Clamp01((static_cast<float>(t) - 380.f) / 280.f);
    } else if (t < NP_IN + NP_HOLD) {
        w = NP_CARD_W; h = NP_CARD_H;
    } else {
        float q = static_cast<float>(t - NP_IN - NP_HOLD) / NP_OUT;
        content = 1.f - Clamp01(q / 0.28f);
        float s = EaseInBack((q - 0.12f) / 0.55f);
        w = NP_CARD_W - (NP_CARD_W - NP_DROP) * s;
        h = NP_CARD_H - (NP_CARD_H - NP_DROP) * s;
        float f = Clamp01((q - 0.62f) / 0.38f);
        yoff = -30.f * f * f;
        alpha = 1.f - f;
        float shrink = 1.f - 0.5f * f;
        w *= shrink; h *= shrink;
    }
    // content cross-fade when a new song replaces the one on screen
    if (g_npContentStart) content = std::min(content, Clamp01((GetTickCount() - g_npContentStart) / 260.f));
    w = std::max(4.f, w); h = std::max(4.f, h);

    const float cx = NP_BOX_W / 2.f;
    const float x0 = cx - w / 2.f, x1 = cx + w / 2.f, y0 = NP_TOP + yoff, y1 = y0 + h;
    const float r = std::min(h / 2.f, std::max(static_cast<float>(NP_CARD_R), h / 2.f - (h - NP_DROP)));

    // ---- glass body: frosted backdrop + tint veil + light
    COLORREF tint = RGB(g_glassCfg.tintR, g_glassCfg.tintG, g_glassCfg.tintB);
    COLORREF veil = RGB(GetRValue(tint) * 3 / 10 + 14, GetGValue(tint) * 3 / 10 + 14, GetBValue(tint) * 3 / 10 + 18);
    const float va = 0.55f;
    ForRRect(c, x0, y0, x1, y1, [&](int x, int y, UINT32& p) {
        float cov = Clamp01(0.5f - RRectDist(x + 0.5f, y + 0.5f, x0, y0, x1, y1, r));
        if (cov <= 0.f) return;
        UINT32 b = g_npBlur[static_cast<size_t>(y) * NP_BOX_W + x];
        Over(p, ((b >> 16) & 0xFF) * (1 - va) + GetRValue(veil) * va, ((b >> 8) & 0xFF) * (1 - va) + GetGValue(veil) * va,
             (b & 0xFF) * (1 - va) + GetBValue(veil) * va, cov);
    });
    GlassLight(c, x0, y0, x1, y1, r, 1.f);

    // shine sweep once the card has formed
    float sp = (static_cast<float>(t) - (NP_IN - 120.f)) / 650.f;
    if (sp > 0.f && sp < 1.f) {
        float pos = -0.2f + 1.4f * sp;
        ForRRect(c, x0, y0, x1, y1, [&](int x, int y, UINT32& p) {
            float cov = Clamp01(0.5f - RRectDist(x + 0.5f, y + 0.5f, x0, y0, x1, y1, r));
            if (cov <= 0.f) return;
            float u = (x + 0.5f - x0) / w * 0.75f + (y + 0.5f - y0) / h * 0.25f;
            float d = std::fabs(u - pos);
            if (d < 0.07f) Over(p, 255, 255, 255, (1.f - d / 0.07f) * 0.22f * cov);
        });
    }

    // ---- content (laid out on the final card size; only visible once it has formed)
    if (content > 0.01f) {
        const float ca = content;
        const float L = cx - NP_CARD_W / 2.f, T = y0 + (1.f - content) * 8.f;
        const LONG l = static_cast<LONG>(L), tp = static_cast<LONG>(T);
        const UINT one = DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS;

        // album art set in a glass frame
        GlassBlob(c, L + 10, T + 10, L + 18 + NP_ART, T + 18 + NP_ART, 22, 0.f, 0.10f, ca);
        NpDrawArt(c, L + 14, T + 14, NP_ART, 18, ca);

        // everything written is liquid glass
        const LONG tx = l + 118, tr = l + 328;
        const wchar_t* label = g_npInfo.playing ? L"Now Playing" : L"Paused";
        DrawGlassText(c, g_npMask, label, { tx, tp + 6, tr, tp + 38 }, g_npLabelF, ca, DT_LEFT | one, 0.35f, 0.7f);
        DrawGlassText(c, g_npMask, g_npInfo.title.c_str(), { tx, tp + 38, tr, tp + 66 }, g_npTitleF, ca, DT_LEFT | one);
        std::wstring sub = g_npInfo.artist;
        if (!g_npInfo.app.empty()) sub += (sub.empty() ? L"" : L"  ·  ") + g_npInfo.app;
        DrawGlassText(c, g_npMask, sub.c_str(), { tx, tp + 66, tr, tp + 88 }, g_npArtistF, ca, DT_LEFT | one, 0.15f, 0.45f);

        // beat bars: glass capsules next to the label, still while paused
        float ex = static_cast<float>(tx + TextWidth(label, g_npLabelF) + 12), eb = T + 31;
        for (int i = 0; i < 5; ++i) {
            float bh = g_npInfo.playing ? 6.f + 14.f * (0.5f + 0.5f * std::sin(t * 0.011f * (1.f + i * 0.31f) + i * 1.9f)) : 6.f;
            GlassBlob(c, ex + i * 9, eb - bh, ex + i * 9 + 6, eb, 3, 0.75f, 0.10f, ca);
        }

        // controls: previous / play-pause / next as glass discs with glass icons
        g_npBtnY = T + NP_CARD_H / 2.f;
        g_npBtnX[2] = L + NP_CARD_W - 16 - g_npBtnR[2];
        g_npBtnX[1] = g_npBtnX[2] - g_npBtnR[2] - 8 - g_npBtnR[1];
        g_npBtnX[0] = g_npBtnX[1] - g_npBtnR[1] - 8 - g_npBtnR[0];
        const wchar_t* glyph[3] = { L"", g_npInfo.playing ? L"" : L"", L"" }; // Segoe MDL2
        for (int i = 0; i < 3; ++i) {
            g_npHotT[i] += ((i == g_npHot ? 1.f : 0.f) - g_npHotT[i]) * 0.3f;
            float hot = g_npHotT[i];
            float br = g_npBtnR[i] * (1.f + 0.08f * hot - (i == g_npPressed && i == g_npHot ? 0.1f : 0.f));
            float bx = g_npBtnX[i], by = g_npBtnY;
            GlassBlob(c, bx - br, by - br, bx + br, by + br, br, i == 1 ? 0.55f + 0.2f * hot : 0.18f * hot,
                      0.12f + 0.08f * hot, ca);
            RECT gr{ static_cast<LONG>(bx - br), static_cast<LONG>(by - br), static_cast<LONG>(bx + br), static_cast<LONG>(by + br) };
            DrawGlassText(c, g_npMask, glyph[i], gr, i == 1 ? g_npPlayF : g_npIconF, ca,
                          DT_CENTER | DT_VCENTER | DT_SINGLELINE, 0.f, 0.85f);
        }

        // time left: a glass tube that drains (holds while the mouse is on the card)
        if (t >= NP_IN && t < NP_IN + NP_HOLD) {
            float left = 1.f - static_cast<float>(t - NP_IN) / NP_HOLD;
            float tl = static_cast<float>(tx), trr = L + NP_CARD_W - 18, ty = T + NP_CARD_H - 16;
            GlassBlob(c, tl, ty, trr, ty + 5, 2.5f, 0.f, 0.07f, ca);
            if (left > 0.02f) GlassBlob(c, tl, ty, tl + (trr - tl) * left, ty + 5, 2.5f, 0.8f, 0.08f, ca);
        }
    }

    POINT dst{ g_npX, g_npY }, src{ 0, 0 };
    SIZE sz{ NP_BOX_W, NP_BOX_H };
    BLENDFUNCTION bf{ AC_SRC_OVER, 0, static_cast<BYTE>(255.f * Clamp01(alpha)), AC_SRC_ALPHA };
    UpdateLayeredWindow(g_npWnd, nullptr, &dst, &sz, g_npS.dc, &src, 0, &bf, ULW_ALPHA);
}

void NpShow(NowPlayingInfo* info) {
    DWORD now = GetTickCount();
    DWORD t = now - g_npStart;
    bool holding = g_npVisible && t >= NP_IN && t < NP_IN + NP_HOLD;
    bool intro = g_npVisible && t < NP_IN;
    bool sameSong = g_npInfo.title == info->title && g_npInfo.artist == info->artist;
    g_npInfo = std::move(*info);

    if (holding) {                 // swap the content in place, restart the 5 s
        g_npStart = now - NP_IN;
        if (!sameSong) g_npContentStart = now; // cross-fade only for a different song
        return;
    }
    g_npContentStart = 0;
    if (intro) return;             // still forming: it just shows the new content

    if (!g_npVisible) {
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY), &mi);
        g_npX = (mi.rcWork.left + mi.rcWork.right) / 2 - NP_BOX_W / 2;
        g_npY = mi.rcWork.top + 4;
        NpCaptureBackdrop(); // only while hidden, so it never captures itself
    }
    g_npStart = now;
    g_npLastTick = now;
    g_npVisible = true;
    g_npHot = g_npPressed = -1;
    NpRender(0);
    SetWindowPos(g_npWnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    SetTimer(g_npWnd, NP_TIMER_ID, 15, nullptr);
}

int NpHitButton(POINT p) {
    if (!g_npVisible || GetTickCount() - g_npStart < NP_IN) return -1; // not formed yet
    for (int i = 0; i < 3; ++i) {
        float dx = p.x - g_npBtnX[i], dy = p.y - g_npBtnY;
        if (dx * dx + dy * dy <= (g_npBtnR[i] + 3) * (g_npBtnR[i] + 3)) return i;
    }
    return -1;
}

void NpCommand(int button) {
    LONG cmd = button == 0 ? NPC_PREV : button == 1 ? NPC_TOGGLE : NPC_NEXT;
    if (cmd == NPC_TOGGLE) g_npInfo.playing = !g_npInfo.playing; // flip the icon now; the player confirms it
    InterlockedExchange(&g_npCmd, cmd);
    SetEvent(g_npWake);
}

LRESULT CALLBACK NowPlayingProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_NOWPLAYING: {
        NowPlayingInfo* info = reinterpret_cast<NowPlayingInfo*>(l);
        if (g_glassCfg.nowPlaying) NpShow(info);
        delete info;
        return 0;
    }
    case WM_TIMER:
        if (w == NP_TIMER_ID) {
            DWORD now = GetTickCount(), t = now - g_npStart;
            // the countdown holds while the mouse is on the card, so the buttons can be reached
            if (g_npMouseIn && t >= NP_IN && t < NP_IN + NP_HOLD) g_npStart += now - g_npLastTick;
            g_npLastTick = now;
            t = now - g_npStart;
            if (t >= NP_IN + NP_HOLD + NP_OUT) {
                KillTimer(h, NP_TIMER_ID);
                ShowWindow(h, SW_HIDE);
                g_npVisible = g_npMouseIn = false;
            } else {
                NpRender(t);
            }
        }
        return 0;

    // Only the card's pixels receive the mouse: the transparent margins of a
    // per-pixel-alpha window are click-through by themselves.
    case WM_MOUSEMOVE: {
        POINT p{ GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, h, 0 };
        TrackMouseEvent(&tme);
        g_npMouseIn = true;
        g_npHot = NpHitButton(p);
        return 0;
    }
    case WM_MOUSELEAVE:
        g_npMouseIn = false;
        g_npHot = g_npPressed = -1;
        return 0;
    case WM_SETCURSOR:
        SetCursor(LoadCursor(nullptr, g_npHot >= 0 ? IDC_HAND : IDC_ARROW));
        return TRUE;
    case WM_LBUTTONDOWN: {
        POINT p{ GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        g_npPressed = NpHitButton(p);
        return 0;
    }
    case WM_LBUTTONUP: {
        POINT p{ GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        int b = NpHitButton(p);
        if (b >= 0 && b == g_npPressed) NpCommand(b);
        g_npPressed = -1;
        return 0;
    }
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE; // clicking it never takes focus from what you're doing
    }
    return DefWindowProcW(h, msg, w, l);
}

void NpInit() {
    // glass lettering needs some stroke weight to carry its rim light
    g_npTitleF = InterFont(21, 700);
    g_npArtistF = InterFont(14, 500);
    g_npLabelF = InterFont(16, 600);
    g_npIconF = MakeFont(15, FW_NORMAL, L"Segoe MDL2 Assets");
    g_npPlayF = MakeFont(19, FW_NORMAL, L"Segoe MDL2 Assets");
    g_npNoteF = MakeFont(40, FW_NORMAL, L"Segoe UI Symbol");
    if (!SurfCreate(g_npS, NP_BOX_W, NP_BOX_H) || !SurfCreate(g_npMask, NP_BOX_W, NP_BOX_H)) return;

    WNDCLASSW wc{};
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpfnWndProc = NowPlayingProc;
    wc.lpszClassName = L"REICONowPlaying";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    // clickable (it has controls) but never activates, above everything
    g_npWnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                              L"REICONowPlaying", L"Now playing", WS_POPUP, 0, 0, NP_BOX_W, NP_BOX_H,
                              nullptr, nullptr, wc.hInstance, nullptr);
    if (!g_npWnd) return;

    g_npWake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_npThread = CreateThread(nullptr, 0, NowPlayingThread, nullptr, 0, nullptr);
}

void NpShutdown() {
    if (g_npThread) {
        g_npQuit = true;
        SetEvent(g_npWake);
        WaitForSingleObject(g_npThread, 2000);
        CloseHandle(g_npThread);
        g_npThread = nullptr;
    }
    if (g_npWake) { CloseHandle(g_npWake); g_npWake = nullptr; }
    if (g_npWnd) { DestroyWindow(g_npWnd); g_npWnd = nullptr; }
    SurfFree(g_npS);
    SurfFree(g_npMask);
    for (HFONT* f : { &g_npTitleF, &g_npArtistF, &g_npLabelF, &g_npIconF, &g_npPlayF, &g_npNoteF })
        if (*f) { DeleteObject(*f); *f = nullptr; }
}

// =====================================================================
// DOCK: REICO's own taskbar - a rounded liquid-glass bar centred at the
// top of the screen: Start, pinned apps, open apps (grouped per app), a
// tray button and a clock.
//
// It's a separate topmost layered window registered as an "appbar" for the
// top edge, so maximized windows stop below it. Windows' own taskbar is set
// to auto-hide while the dock is on (it slides up from the bottom when you
// need the tray); its original setting is saved to C:\REICO\taskbar_state.txt
// and put back when the dock is turned off, on exit, or - after a crash - on
// the next start.
// =====================================================================

const int DOCK_STRIP = 66;              // screen height reserved at the top
const int DOCK_WIN_H = 112;             // window: the strip + room for tooltips below the bar
const int DOCK_TOP = 7, DOCK_H = 52, DOCK_R = 22;
const int DOCK_SLOT = 46, DOCK_ICON = 32, DOCK_PAD = 10;
const int DOCK_SEP = 12, DOCK_TRAY_W = 36, DOCK_CLOCK_W = 80;
const int DOCK_SIDE_X = 12;             // the round buttons start at the top-left of the strip
enum { SIDE_GAMES, SIDE_TASKMGR, SIDE_NOTES };

// x of a top-left button (game launcher, Task Manager, board), or -1 when it's turned
// off in the settings; the ones that are on are packed left to right.
int DockSideX(int which) {
    const bool on[3] = { g_glassCfg.btnGames, g_glassCfg.btnTaskMgr, g_glassCfg.btnNotes };
    if (!on[which]) return -1;
    int x = DOCK_SIDE_X;
    for (int i = 0; i < which; ++i) if (on[i]) x += DOCK_H + 8;
    return x;
}
const UINT WM_DOCK_APPBAR = WM_APP + 30;
const UINT_PTR DOCK_ANIM_TIMER = 1, DOCK_TICK_TIMER = 2, DOCK_REFRESH_TIMER = 3;
enum { DH_NONE = -1, DH_START = 1000, DH_TRAY, DH_CLOCK, DH_GAMES, DH_TASKMGR, DH_NOTES };
const wchar_t* TASKBAR_STATE_FILE = L"C:\\REICO\\taskbar_state.txt";

struct DockApp {
    std::wstring key;          // lower-case exe path ("uwp:<title>" for store apps)
    std::wstring lnk;          // pinned shortcut (launched when not running)
    std::wstring exe;          // exe path, for "new window"
    std::wstring title;
    bool pinned = false;
    std::vector<HWND> wins;    // open windows, topmost first
    std::vector<UINT32> icon;  // DOCK_ICON x DOCK_ICON, premultiplied
    float x = -1.f;            // animated slot position, dock-relative
    float ind = 0.f, appear = 0.f;
    DWORD bounce = 0;          // click bounce start
    DWORD shine = 0;           // hover shine sweep start
};

HWND g_dock = nullptr;
bool g_dockOn = false, g_dockAppbar = false, g_dockHidden = false, g_dockTimerOn = false;
Surface g_dockS, g_dockMask;
std::vector<DockApp> g_dockApps;
RECT g_dockMon{};
POINT g_dockOrigin{};
float g_dockW = 0.f;
int g_dockHot = DH_NONE;
float g_dockStartHot = 0.f, g_dockTrayHot = 0.f, g_dockClockHot = 0.f, g_dockGamesHot = 0.f, g_dockTaskHot = 0.f, g_dockNotesHot = 0.f;
DWORD g_dockHoverSince = 0;
HWINEVENTHOOK g_dockHookFg = nullptr, g_dockHookObj = nullptr;
HFONT g_dockGlyphF = nullptr, g_dockTimeF = nullptr, g_dockDateF = nullptr, g_dockTipF = nullptr, g_dockGamesF = nullptr;

// ---- taskbar auto-hide (saved + restored) -------------------------------

UINT TaskbarGetState() {
    APPBARDATA a{};
    a.cbSize = sizeof(a);
    a.hWnd = FindWindowW(L"Shell_TrayWnd", nullptr);
    return static_cast<UINT>(SHAppBarMessage(ABM_GETSTATE, &a));
}

void TaskbarSetState(UINT s) {
    APPBARDATA a{};
    a.cbSize = sizeof(a);
    a.hWnd = FindWindowW(L"Shell_TrayWnd", nullptr);
    a.lParam = s;
    SHAppBarMessage(ABM_SETSTATE, &a);
}

void TaskbarAutoHide() {
    UINT cur = TaskbarGetState();
    if (GetFileAttributesW(TASKBAR_STATE_FILE) == INVALID_FILE_ATTRIBUTES) { // keep the ORIGINAL, never overwrite
        std::ofstream(fs::path(TASKBAR_STATE_FILE)) << cur;
    }
    TaskbarSetState(ABS_AUTOHIDE | (cur & ABS_ALWAYSONTOP));
}

// Auto-hide alone still lets the taskbar slide up at the bottom edge, so while
// the dock is on the window itself is hidden too.
void TaskbarShow(bool show) {
    if (HWND tb = FindWindowW(L"Shell_TrayWnd", nullptr)) ShowWindow(tb, show ? SW_SHOWNA : SW_HIDE);
}

void TaskbarRestore() {
    std::ifstream in{ fs::path(TASKBAR_STATE_FILE) };
    UINT s = 0;
    if (!(in >> s)) return;
    in.close();
    TaskbarShow(true);
    TaskbarSetState(s);
    DeleteFileW(TASKBAR_STATE_FILE);
    Log(L"Dock: Windows taskbar restored");
}


// ---- apps: pinned shortcuts + open windows --------------------------------

std::wstring LowerStr(std::wstring s) {
    for (auto& ch : s) ch = towlower(ch);
    return s;
}

std::wstring WindowExe(HWND h) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return L"";
    wchar_t buf[MAX_PATH]{};
    DWORD n = MAX_PATH;
    std::wstring out = QueryFullProcessImageNameW(p, 0, buf, &n) ? std::wstring(buf, n) : L"";
    CloseHandle(p);
    return out;
}

// Area-average resize, so big shell icons shrink smoothly.
std::vector<UINT32> IconToSize(const AlphaIcon& a, int size) {
    std::vector<UINT32> out(static_cast<size_t>(size) * size, 0);
    if (!a.valid || a.w <= 0 || a.h <= 0) return out;
    for (int y = 0; y < size; ++y) {
        int sy0 = y * a.h / size, sy1 = std::max(sy0 + 1, (y + 1) * a.h / size);
        for (int x = 0; x < size; ++x) {
            int sx0 = x * a.w / size, sx1 = std::max(sx0 + 1, (x + 1) * a.w / size);
            UINT32 s[4] = {}, n = 0;
            for (int yy = sy0; yy < sy1; ++yy)
                for (int xx = sx0; xx < sx1; ++xx, ++n) {
                    UINT32 p = a.pixels[static_cast<size_t>(yy) * a.w + xx];
                    s[0] += p >> 24; s[1] += (p >> 16) & 0xFF; s[2] += (p >> 8) & 0xFF; s[3] += p & 0xFF;
                }
            out[static_cast<size_t>(y) * size + x] = ((s[0] / n) << 24) | ((s[1] / n) << 16) | ((s[2] / n) << 8) | (s[3] / n);
        }
    }
    return out;
}

std::vector<UINT32> IconFromHicon(HICON h) { return IconToSize(ExtractIconAlpha(h), DOCK_ICON); }

// 48 px shell icon (the jumbo list puts small icons in a corner of a 256 canvas),
// returned at size x size
std::vector<UINT32> IconForPath(const std::wstring& rawPath, int size = DOCK_ICON) {
    std::wstring path = rawPath; // the shell's icon lookup rejects forward slashes (Riot writes them)
    std::replace(path.begin(), path.end(), L'/', L'\\');
    SHFILEINFOW sfi{};
    IImageList* list = nullptr;
    HICON h = nullptr;
    if (SHGetFileInfoW(path.c_str(), 0, &sfi, sizeof(sfi), SHGFI_SYSICONINDEX) &&
        SUCCEEDED(SHGetImageList(SHIL_EXTRALARGE, IID_IImageList, reinterpret_cast<void**>(&list))) && list) {
        list->GetIcon(sfi.iIcon, ILD_TRANSPARENT, &h);
        list->Release();
    }
    if (!h) return {};
    std::vector<UINT32> v = IconToSize(ExtractIconAlpha(h), size);
    DestroyIcon(h);
    return v;
}

std::vector<UINT32> IconFromWindow(HWND h, const std::wstring& exe) {
    DWORD_PTR r = 0;
    SendMessageTimeoutW(h, WM_GETICON, ICON_BIG, 0, SMTO_ABORTIFHUNG, 100, &r);
    HICON hi = reinterpret_cast<HICON>(r);
    if (!hi) hi = reinterpret_cast<HICON>(GetClassLongPtrW(h, GCLP_HICON));
    if (hi) return IconFromHicon(hi); // not ours: not destroyed
    return IconForPath(exe);
}

// .lnk -> target exe, through the shell's IShellLink (COM, loaded at runtime)
std::wstring ResolveLnk(const std::wstring& lnk) {
    typedef HRESULT (WINAPI *PFN_CoCreate)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID*);
    static PFN_CoCreate coCreate = reinterpret_cast<PFN_CoCreate>(
        reinterpret_cast<void*>(GetProcAddress(LoadLibraryW(L"ole32.dll"), "CoCreateInstance")));
    static const CLSID CLSID_ShellLink_ = { 0x00021401, 0, 0, { 0xc0, 0, 0, 0, 0, 0, 0, 0x46 } };
    static const IID IID_IShellLinkW_ = { 0x000214f9, 0, 0, { 0xc0, 0, 0, 0, 0, 0, 0, 0x46 } };
    static const IID IID_IPersistFile_ = { 0x0000010b, 0, 0, { 0xc0, 0, 0, 0, 0, 0, 0, 0x46 } };
    if (!coCreate) return L"";
    std::wstring out;
    IShellLinkW* sl = nullptr;
    if (SUCCEEDED(coCreate(CLSID_ShellLink_, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW_, reinterpret_cast<void**>(&sl))) && sl) {
        IPersistFile* pf = nullptr;
        if (SUCCEEDED(sl->QueryInterface(IID_IPersistFile_, reinterpret_cast<void**>(&pf))) && pf) {
            wchar_t buf[MAX_PATH]{};
            if (SUCCEEDED(pf->Load(lnk.c_str(), STGM_READ)) && SUCCEEDED(sl->GetPath(buf, MAX_PATH, nullptr, 0)) && buf[0]) {
                wchar_t exp[MAX_PATH]{};
                out = ExpandEnvironmentStringsW(buf, exp, MAX_PATH) ? exp : buf;
            }
            pf->Release();
        }
        sl->Release();
    }
    return out;
}

void DockLoadPinned() {
    wchar_t appdata[MAX_PATH]{};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appdata))) return;
    fs::path dir = fs::path(appdata) / L"Microsoft\\Internet Explorer\\Quick Launch\\User Pinned\\TaskBar";
    std::vector<fs::path> lnks;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec))
        if (LowerStr(e.path().extension().wstring()) == L".lnk") lnks.push_back(e.path());
    std::sort(lnks.begin(), lnks.end());
    for (const auto& p : lnks) {
        DockApp a;
        a.pinned = true;
        a.lnk = p.wstring();
        a.exe = ResolveLnk(a.lnk);
        a.key = LowerStr(a.exe.empty() ? a.lnk : a.exe);
        a.title = p.stem().wstring();
        a.icon = IconForPath(a.lnk);
        g_dockApps.push_back(std::move(a));
    }
}

bool DockWantsWindow(HWND h) {
    if (!IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return false;
    if (GetWindowLongPtrW(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return false;
    if (GetWindowTextLengthW(h) == 0) return false;
    int cloaked = 0; // suspended store apps / other virtual desktops
    if (g_dwmGetAttr) g_dwmGetAttr(h, 14 /* DWMWA_CLOAKED */, &cloaked, sizeof(cloaked));
    if (cloaked) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId()) return false;
    wchar_t cls[64]{};
    GetClassNameW(h, cls, 64);
    for (const wchar_t* skip : { L"Progman", L"WorkerW", L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd" })
        if (_wcsicmp(cls, skip) == 0) return false;
    return true;
}

void DockKick();

// Re-reads the open windows and regroups them under their apps.
void DockRefresh() {
    for (auto& a : g_dockApps) a.wins.clear();
    std::vector<HWND> wins;
    EnumWindows([](HWND h, LPARAM l) -> BOOL {
        if (DockWantsWindow(h)) reinterpret_cast<std::vector<HWND>*>(l)->push_back(h);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&wins));

    for (HWND h : wins) {
        std::wstring exe = WindowExe(h);
        bool uwp = LowerStr(exe).find(L"applicationframehost.exe") != std::wstring::npos;
        wchar_t title[256]{};
        GetWindowTextW(h, title, 256);
        std::wstring key = uwp ? L"uwp:" + std::wstring(title) : LowerStr(exe);
        if (key.empty()) continue;
        auto it = std::find_if(g_dockApps.begin(), g_dockApps.end(), [&](const DockApp& a) { return a.key == key; });
        if (it == g_dockApps.end()) {
            DockApp a;
            a.key = key;
            a.exe = uwp ? L"" : exe;
            a.icon = uwp ? IconFromWindow(h, exe) : IconForPath(exe);
            if (a.icon.empty()) a.icon = IconFromWindow(h, exe);
            g_dockApps.push_back(std::move(a));
            it = g_dockApps.end() - 1;
        }
        it->wins.push_back(h);
    }

    g_dockApps.erase(std::remove_if(g_dockApps.begin(), g_dockApps.end(),
                                    [](const DockApp& a) { return !a.pinned && a.wins.empty(); }),
                     g_dockApps.end());
    for (auto& a : g_dockApps) {
        if (a.wins.empty()) continue;
        wchar_t t[256]{};
        GetWindowTextW(a.wins.front(), t, 256);
        a.title = t;
    }
    if (g_dockHot >= 0 && g_dockHot >= static_cast<int>(g_dockApps.size())) g_dockHot = DH_NONE;
    DockKick();
}

// ---- layout + drawing ------------------------------------------------------

float DockTargetWidth() {
    return 2.f * DOCK_PAD + DOCK_SLOT + 2.f * DOCK_SEP + g_dockApps.size() * static_cast<float>(DOCK_SLOT) +
           DOCK_TRAY_W + DOCK_CLOCK_W;
}
float DockAppX(int i) { return static_cast<float>(DOCK_PAD + DOCK_SLOT + DOCK_SEP + i * DOCK_SLOT); }

bool DockIsActive(const DockApp& a) {
    HWND fg = GetForegroundWindow();
    return std::find(a.wins.begin(), a.wins.end(), fg) != a.wins.end();
}

// Eases everything toward its target; true while something still moves.
bool DockAnimate() {
    bool moving = false;
    auto ease = [&](float& v, float target, float k, float eps) {
        v += (target - v) * k;
        if (std::fabs(target - v) < eps) v = target; else moving = true;
    };
    ease(g_dockW, DockTargetWidth(), 0.2f, 0.3f);
    for (int i = 0; i < static_cast<int>(g_dockApps.size()); ++i) {
        DockApp& a = g_dockApps[i];
        if (a.x < 0.f) a.x = DockAppX(i);
        ease(a.x, DockAppX(i), 0.22f, 0.3f);
        ease(a.ind, DockIsActive(a) ? 1.f : (a.wins.empty() ? 0.f : 0.45f), 0.25f, 0.01f);
        ease(a.appear, 1.f, 0.15f, 0.01f);
        if (a.bounce) { if (GetTickCount() - a.bounce < 520) moving = true; else a.bounce = 0; }
        if (a.shine) { if (GetTickCount() - a.shine < 650) moving = true; else a.shine = 0; }
    }
    ease(g_dockStartHot, g_dockHot == DH_START ? 1.f : 0.f, 0.3f, 0.01f);
    ease(g_dockGamesHot, g_dockHot == DH_GAMES ? 1.f : 0.f, 0.3f, 0.01f);
    ease(g_dockTaskHot, g_dockHot == DH_TASKMGR ? 1.f : 0.f, 0.3f, 0.01f);
    ease(g_dockNotesHot, g_dockHot == DH_NOTES ? 1.f : 0.f, 0.3f, 0.01f);
    ease(g_dockTrayHot, g_dockHot == DH_TRAY ? 1.f : 0.f, 0.3f, 0.01f);
    ease(g_dockClockHot, g_dockHot == DH_CLOCK ? 1.f : 0.f, 0.3f, 0.01f);
    if (g_dockHot >= 0 && GetTickCount() - g_dockHoverSince < 700) moving = true; // tooltip still on its way
    return moving;
}

// Bilinear blit of a premultiplied square icon (smooth when magnified).
void BlitIconSmooth(Canvas c, const std::vector<UINT32>& px, int n, float x0, float y0, float size, float alpha) {
    if (px.empty() || size <= 0.f) return;
    int ix0 = std::max(0, static_cast<int>(std::floor(x0))), iy0 = std::max(0, static_cast<int>(std::floor(y0)));
    int ix1 = std::min(c.w, static_cast<int>(std::ceil(x0 + size))), iy1 = std::min(c.h, static_cast<int>(std::ceil(y0 + size)));
    auto at = [&](int x, int y) { return px[static_cast<size_t>(std::min(n - 1, std::max(0, y))) * n + std::min(n - 1, std::max(0, x))]; };
    for (int y = iy0; y < iy1; ++y) {
        float v = (y + 0.5f - y0) / size * n - 0.5f;
        int vy = static_cast<int>(std::floor(v));
        float fy = v - vy;
        for (int x = ix0; x < ix1; ++x) {
            float u = (x + 0.5f - x0) / size * n - 0.5f;
            int ux = static_cast<int>(std::floor(u));
            float fx = u - ux;
            float ch[4] = {};
            UINT32 q[4] = { at(ux, vy), at(ux + 1, vy), at(ux, vy + 1), at(ux + 1, vy + 1) };
            float w[4] = { (1 - fx) * (1 - fy), fx * (1 - fy), (1 - fx) * fy, fx * fy };
            for (int k = 0; k < 4; ++k)
                for (int s = 0; s < 4; ++s) ch[s] += ((q[k] >> (24 - 8 * s)) & 0xFF) * w[k];
            float a = ch[0] / 255.f;
            if (a <= 0.004f) continue;
            Over(c.px[static_cast<size_t>(y) * c.w + x], ch[1] / a, ch[2] / a, ch[3] / a, a * alpha);
        }
    }
}

// Hover "shine": a diagonal band of light sweeping across an icon (t = 0..1),
// only where the icon has pixels - same effect as the desktop icons' Shine.
void IconShine(Canvas c, const std::vector<UINT32>& px, int n, float x0, float y0, float size, float t) {
    if (px.empty() || t <= 0.f || t >= 1.f || size <= 0.f) return;
    const float pos = -0.25f + 1.5f * t, half = 0.13f;
    int ix0 = std::max(0, static_cast<int>(x0)), iy0 = std::max(0, static_cast<int>(y0));
    int ix1 = std::min(c.w, static_cast<int>(x0 + size) + 1), iy1 = std::min(c.h, static_cast<int>(y0 + size) + 1);
    for (int y = iy0; y < iy1; ++y)
        for (int x = ix0; x < ix1; ++x) {
            float u = ((x + 0.5f - x0) + (y + 0.5f - y0)) / size * 0.5f;
            float d = std::fabs(u - pos);
            if (d >= half) continue;
            int sx = std::min(n - 1, std::max(0, static_cast<int>((x + 0.5f - x0) / size * n)));
            int sy = std::min(n - 1, std::max(0, static_cast<int>((y + 0.5f - y0) / size * n)));
            float sa = (px[static_cast<size_t>(sy) * n + sx] >> 24) / 255.f;
            if (sa > 0.f) Over(c.px[static_cast<size_t>(y) * c.w + x], 255, 255, 255, (1.f - d / half) * sa * 0.75f);
        }
}

void DrawWinLogo(Canvas c, float cx, float cy, float s, float hot); // below

void DockRender() {
    if (!g_dockS.px) return;
    Canvas c = g_dockS.cv();
    ZeroMemory(c.px, sizeof(UINT32) * static_cast<size_t>(c.w) * c.h);
    const COLORREF white = RGB(255, 255, 255);
    const float L = c.w / 2.f - g_dockW / 2.f, R = L + g_dockW, T = DOCK_TOP, B = T + DOCK_H, cy = T + DOCK_H / 2.f;
    const DWORD now = GetTickCount();

    // the glass bar
    COLORREF tint = RGB(g_glassCfg.tintR, g_glassCfg.tintG, g_glassCfg.tintB);
    COLORREF veil = RGB(GetRValue(tint) * 3 / 10 + 14, GetGValue(tint) * 3 / 10 + 14, GetBValue(tint) * 3 / 10 + 18);
    FillRRect(c, L, T, R, B, DOCK_R, veil, 0.68f);
    GlassLight(c, L, T, R, B, DOCK_R, 1.f);

    // the round glass buttons at the top-left: game launcher, Task Manager, board
    // (each can be turned off in the panel; the ones that are on pack left to right)
    {
        const wchar_t glyphs[3][2] = { { 0xE7FC, 0 }, { 0xE9D9, 0 }, { 0xE70B, 0 } }; // controller, activity graph, note
        const float hots[3] = { g_dockGamesHot, g_dockTaskHot, g_dockNotesHot };
        for (int i = 0; i < 3; ++i) {
            int bx = DockSideX(i);
            if (bx < 0) continue;
            const float x0 = static_cast<float>(bx), x1 = x0 + DOCK_H;
            FillRRect(c, x0, T, x1, B, DOCK_R, veil, 0.68f);
            if (hots[i] > 0.01f) FillRRect(c, x0, T, x1, B, DOCK_R, AccentColour(), 0.45f * hots[i]);
            GlassLight(c, x0, T, x1, B, DOCK_R, 1.f);
            DrawGlassText(c, g_dockMask, glyphs[i], { static_cast<LONG>(x0), static_cast<LONG>(T), static_cast<LONG>(x1), static_cast<LONG>(B) },
                          g_dockGamesF, 1.f, DT_CENTER | DT_VCENTER | DT_SINGLELINE, 0.f, 0.9f);
        }
    }

    // Start
    float sx = L + DOCK_PAD;
    DrawWinLogo(c, sx + DOCK_SLOT / 2.f, cy, 22.f + 2.f * g_dockStartHot, g_dockStartHot); // Start

    // separators
    float sep1 = sx + DOCK_SLOT + DOCK_SEP / 2.f, sep2 = R - DOCK_PAD - DOCK_CLOCK_W - DOCK_TRAY_W - DOCK_SEP / 2.f;
    for (float s : { sep1, sep2 }) FillRRect(c, s - 0.75f, T + 15, s + 0.75f, B - 15, 0.75f, white, 0.18f);

    // apps
    for (int i = 0; i < static_cast<int>(g_dockApps.size()); ++i) {
        const DockApp& a = g_dockApps[i];
        float ax = L + a.x, sc = 1.f, lift = 0.f;
        if (a.bounce) { // click feedback only - hovering doesn't resize anything
            float bt = (now - a.bounce) / 520.f;
            float bump = std::sin(Clamp01(bt) * 3.14159f) * (1.f - Clamp01(bt) * 0.5f);
            sc *= 1.f + 0.12f * bump;
            lift += 4.f * bump;
        }
        float sz = DOCK_ICON * sc, ix = ax + DOCK_SLOT / 2.f - sz / 2.f, iy = cy - sz / 2.f - lift - 2.f;
        BlitIconSmooth(c, a.icon, DOCK_ICON, ix, iy, sz, a.appear);
        if (a.shine) IconShine(c, a.icon, DOCK_ICON, ix, iy, sz, (now - a.shine) / 650.f); // hover: a light sweep

        // running / active indicator: a glass pill that grows for the active app
        if (a.ind > 0.02f) {
            float act = Clamp01((a.ind - 0.45f) / 0.55f);
            float pw = 5.f + 13.f * act, px = ax + DOCK_SLOT / 2.f;
            GlassBlob(c, px - pw / 2, B - 8, px + pw / 2, B - 4, 2, 0.95f * act, 0.55f * (1.f - act) + 0.1f, std::min(1.f, a.ind * 2.f));
        }
    }

    // tray button
    float tx0 = R - DOCK_PAD - DOCK_CLOCK_W - DOCK_TRAY_W;
    if (g_dockTrayHot > 0.01f) GlassBlob(c, tx0 + 2, T + 9, tx0 + DOCK_TRAY_W - 2, B - 9, 12, 0.f, 0.14f, g_dockTrayHot);
    DrawGlassText(c, g_dockMask, L"\uE712", { static_cast<LONG>(tx0), static_cast<LONG>(T), static_cast<LONG>(tx0 + DOCK_TRAY_W), static_cast<LONG>(B) },
                  g_dockGlyphF, 1.f, DT_CENTER | DT_VCENTER | DT_SINGLELINE, 0.f, 0.8f);

    // clock
    SYSTEMTIME st;
    GetLocalTime(&st);
    static const wchar_t* days[] = { L"Sun", L"Mon", L"Tue", L"Wed", L"Thu", L"Fri", L"Sat" };
    static const wchar_t* months[] = { L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun", L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec" };
    wchar_t tbuf[16], dbuf[32];
    swprintf(tbuf, 16, L"%02d:%02d", st.wHour, st.wMinute);
    swprintf(dbuf, 32, L"%ls %d %ls", days[st.wDayOfWeek % 7], st.wDay, months[(st.wMonth + 11) % 12]);
    float cx0 = R - DOCK_PAD - DOCK_CLOCK_W;
    if (g_dockClockHot > 0.01f) GlassBlob(c, cx0 + 2, T + 6, R - DOCK_PAD, B - 6, 13, 0.f, 0.12f, g_dockClockHot);
    LONG cl = static_cast<LONG>(cx0), cr = static_cast<LONG>(R - DOCK_PAD), ct = static_cast<LONG>(T);
    DrawGlassText(c, g_dockMask, tbuf, { cl, ct + 7, cr, ct + 29 }, g_dockTimeF, 1.f, DT_CENTER | DT_VCENTER | DT_SINGLELINE, 0.2f, 0.75f);
    DrawTextOn(c, g_dockMask, dbuf, { cl, ct + 28, cr, ct + 44 }, g_dockDateF, white, 0.62f, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    // tooltip: the app's window title, in a small glass pill under the bar
    if (g_dockHot >= 0 && g_dockHot < static_cast<int>(g_dockApps.size()) && now - g_dockHoverSince > 420) {
        const DockApp& a = g_dockApps[g_dockHot];
        if (!a.title.empty()) {
            float ta = Clamp01((now - g_dockHoverSince - 420) / 160.f);
            float tw = std::min(340.f, static_cast<float>(TextWidth(a.title.c_str(), g_dockTipF)) + 26.f);
            float mid = L + a.x + DOCK_SLOT / 2.f;
            float t0 = std::min(std::max(mid - tw / 2, 8.f), c.w - tw - 8.f), ty = B + 10 + (1.f - ta) * 4.f;
            FillRRect(c, t0, ty, t0 + tw, ty + 28, 14, veil, 0.85f * ta);
            GlassLight(c, t0, ty, t0 + tw, ty + 28, 14, 0.8f * ta);
            DrawTextOn(c, g_dockMask, a.title.c_str(), { static_cast<LONG>(t0 + 12), static_cast<LONG>(ty), static_cast<LONG>(t0 + tw - 12), static_cast<LONG>(ty + 28) },
                       g_dockTipF, white, 0.95f * ta, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
    }

    POINT src{ 0, 0 };
    SIZE sz{ c.w, c.h };
    BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(g_dock, nullptr, &g_dockOrigin, &sz, g_dockS.dc, &src, 0, &bf, ULW_ALPHA);
}

void DockKick() {
    if (!g_dockOn || g_dockTimerOn) return;
    SetTimer(g_dock, DOCK_ANIM_TIMER, 15, nullptr);
    g_dockTimerOn = true;
}

// ---- glass tray pop-up (drops down under the ··· button) --------------------
//
// Windows' tray icons live inside its (now hidden) taskbar at the bottom.
// Their list - owner window, id, callback message, version, icon, tooltip -
// is read from Explorer's two tray toolbars (the visible notification area
// and the overflow), which live in Explorer's process, so a small buffer is
// allocated there for the toolbar to write into. A click here is sent
// straight to the app in the same format Explorer would use for that icon's
// version, so Explorer (and its taskbar) is never involved.

struct TrayBtn {
    HWND owner;                // the app's window that receives the icon's messages
    UINT id, callback, version; // as the app registered them with Shell_NotifyIcon
    std::wstring tip;
    std::vector<UINT32> icon;  // iconN x iconN premultiplied
    int iconN = 0;
    DWORD shine = 0;
};

const int TP_CELL = 44, TP_COLS = 6, TP_PAD = 12, TP_TIP_H = 30, TP_R = 18;
const UINT_PTR TP_ANIM_TIMER = 1;

HWND g_tp = nullptr;
Surface g_tpS, g_tpMask;
std::vector<TrayBtn> g_tpBtns;
int g_tpX = 0, g_tpY = 0, g_tpW = 0, g_tpH = 0, g_tpHot = -1, g_tpPressed = -1;
bool g_tpVisible = false, g_tpClosing = false;
DWORD g_tpAnimStart = 0, g_tpLastClose = 0;

void ReadTrayToolbar(HWND tb, std::vector<TrayBtn>& out) {
    if (!tb) return;
    DWORD pid = 0;
    GetWindowThreadProcessId(tb, &pid);
    HANDLE p = OpenProcess(PROCESS_VM_OPERATION | PROCESS_VM_READ | PROCESS_VM_WRITE, FALSE, pid);
    if (!p) return;
    BYTE* rem = static_cast<BYTE*>(VirtualAllocEx(p, nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!rem) { CloseHandle(p); return; }

    // Explorer's per-icon record (x64 layout, checked against live icons: the
    // version field reads 0 for REICO's own icon, 3/4 for apps that set them)
    struct TrayData { HWND hwnd; UINT uID; UINT uCallbackMessage; DWORD flags; UINT uVersion; HICON hIcon; };

    DWORD_PTR count = 0;
    SendMessageTimeoutW(tb, TB_BUTTONCOUNT, 0, 0, SMTO_ABORTIFHUNG, 300, &count);
    for (int i = 0; i < static_cast<int>(count) && i < 64; ++i) {
        DWORD_PTR r = 0;
        SIZE_T got = 0;
        TBBUTTON b{};
        if (!SendMessageTimeoutW(tb, TB_GETBUTTON, i, reinterpret_cast<LPARAM>(rem), SMTO_ABORTIFHUNG, 300, &r) || !r ||
            !ReadProcessMemory(p, rem, &b, sizeof(b), &got) || (b.fsState & TBSTATE_HIDDEN) || !b.dwData)
            continue;
        TrayData td{};
        if (!ReadProcessMemory(p, reinterpret_cast<void*>(b.dwData), &td, sizeof(td), &got) || !IsWindow(td.hwnd))
            continue; // ghost icon of an app that's gone

        TrayBtn t;
        t.owner = td.hwnd;
        t.id = td.uID;
        t.callback = td.uCallbackMessage;
        t.version = td.uVersion;
        wchar_t tip[256]{};
        if (SendMessageTimeoutW(tb, TB_GETBUTTONTEXTW, b.idCommand, reinterpret_cast<LPARAM>(rem + 512), SMTO_ABORTIFHUNG, 300, &r) &&
            static_cast<int>(r) > 0)
            ReadProcessMemory(p, rem + 512, tip, sizeof(wchar_t) * std::min<DWORD_PTR>(r, 255), &got);
        t.tip = tip;
        // Tray icons are only 16 px. For a normal app, its own 48 px shell icon is much
        // sharper; Windows' own icons (volume, network, ...) belong to Explorer and are
        // kept as they are, at native size (scaled smoothly when drawn).
        std::wstring exe = WindowExe(td.hwnd);
        DWORD ownerPid = 0;
        GetWindowThreadProcessId(td.hwnd, &ownerPid);
        bool ours = ownerPid == GetCurrentProcessId(); // REICO's own icon: its live (tinted) logo, not the exe's
        if (!ours && !exe.empty() && LowerStr(exe).find(L"\\explorer.exe") == std::wstring::npos) {
            t.icon = IconForPath(exe);
            t.iconN = DOCK_ICON;
        }
        if (t.icon.empty() && td.hIcon) {
            AlphaIcon a = ExtractIconAlpha(td.hIcon);
            if (a.valid && a.w > DOCK_ICON) { t.icon = IconToSize(a, DOCK_ICON); t.iconN = DOCK_ICON; } // big: shrink smoothly
            else if (a.valid && a.w == a.h) { t.icon = std::move(a.pixels); t.iconN = a.w; }           // small: keep native
        }
        out.push_back(std::move(t));
    }
    VirtualFreeEx(p, rem, 0, MEM_RELEASE);
    CloseHandle(p);
}

std::vector<TrayBtn> ReadTray() {
    std::vector<TrayBtn> out;
    HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
    HWND notify = tray ? FindWindowExW(tray, nullptr, L"TrayNotifyWnd", nullptr) : nullptr;
    HWND pager = notify ? FindWindowExW(notify, nullptr, L"SysPager", nullptr) : nullptr;
    ReadTrayToolbar(pager ? FindWindowExW(pager, nullptr, L"ToolbarWindow32", nullptr) : nullptr, out); // shown on the taskbar
    HWND over = FindWindowW(L"NotifyIconOverflowWindow", nullptr);
    ReadTrayToolbar(over ? FindWindowExW(over, nullptr, L"ToolbarWindow32", nullptr) : nullptr, out);  // behind the ^ arrow

    // Explorer can list the same icon twice (e.g. in both areas, or again after it
    // re-adds icons): one entry per app window + icon id.
    std::vector<TrayBtn> unique;
    for (auto& t : out)
        if (std::none_of(unique.begin(), unique.end(), [&](const TrayBtn& u) { return u.owner == t.owner && u.id == t.id; }))
            unique.push_back(std::move(t));

    // Explorer takes a while to list a freshly started app's icon - REICO's own is
    // added directly when it isn't listed yet, so it's always there (and only once).
    if (g_trayWnd && std::none_of(unique.begin(), unique.end(), [](const TrayBtn& u) { return u.owner == g_trayWnd; })) {
        TrayBtn me{ g_trayWnd, 1, WM_TRAY, 0, L"REICO" };
        if (g_logoIcon) {
            AlphaIcon a = ExtractIconAlpha(g_logoIcon);
            if (a.valid && a.w == a.h) { me.icon = std::move(a.pixels); me.iconN = a.w; }
        }
        unique.push_back(std::move(me));
    }
    return unique;
}

// Sends the click straight to the app, as Explorer would for this icon's version:
//   version 4:  wParam = cursor x/y, lParam = MAKELPARAM(message, icon id)
//   older:      wParam = icon id,    lParam = message
// The full sequence includes NIN_SELECT (left) / WM_CONTEXTMENU (right), which
// version 3+ apps listen for instead of the raw button messages.
void TrayForward(const TrayBtn& t, bool right) {
    if (!IsWindow(t.owner) || !t.callback) return;
    DWORD pid = 0;
    GetWindowThreadProcessId(t.owner, &pid);
    AllowSetForegroundWindow(pid); // the app may bring up a window or its menu
    POINT c;
    GetCursorPos(&c);
    auto send = [&](UINT m) {
        if (t.version >= 4)
            SendNotifyMessageW(t.owner, t.callback, MAKEWPARAM(c.x, c.y), MAKELPARAM(m, t.id));
        else
            SendNotifyMessageW(t.owner, t.callback, t.id, m);
    };
    send(WM_MOUSEMOVE);
    if (right) { send(WM_RBUTTONDOWN); send(WM_RBUTTONUP); if (t.version >= 3) send(WM_CONTEXTMENU); }
    else { send(WM_LBUTTONDOWN); send(WM_LBUTTONUP); if (t.version >= 3) send(NIN_SELECT); }
}

RECT TpCell(int i) {
    int col = i % TP_COLS, row = i / TP_COLS;
    return { TP_PAD + col * TP_CELL, TP_PAD + row * TP_CELL, TP_PAD + (col + 1) * TP_CELL, TP_PAD + (row + 1) * TP_CELL };
}

int TpHit(POINT p) {
    for (int i = 0; i < static_cast<int>(g_tpBtns.size()); ++i) {
        RECT r = TpCell(i);
        if (PtInRect(&r, p)) return i;
    }
    return -1;
}

void TpRender() {
    DWORD now = GetTickCount();
    float p = Clamp01((now - g_tpAnimStart) / (g_tpClosing ? 130.f : 180.f));
    float open = g_tpClosing ? 1.f - p : EaseOut3(p);
    Canvas c = g_tpS.cv();
    ZeroMemory(c.px, sizeof(UINT32) * static_cast<size_t>(c.w) * c.h);
    const COLORREF white = RGB(255, 255, 255);

    COLORREF tint = RGB(g_glassCfg.tintR, g_glassCfg.tintG, g_glassCfg.tintB);
    COLORREF veil = RGB(GetRValue(tint) * 3 / 10 + 14, GetGValue(tint) * 3 / 10 + 14, GetBValue(tint) * 3 / 10 + 18);
    FillRRect(c, 0.5f, 0.5f, g_tpW - 0.5f, g_tpH - 0.5f, TP_R, veil, 0.93f); // dense: readable over busy windows
    GlassLight(c, 0.5f, 0.5f, g_tpW - 0.5f, g_tpH - 0.5f, TP_R, 1.f);

    for (int i = 0; i < static_cast<int>(g_tpBtns.size()); ++i) {
        TrayBtn& t = g_tpBtns[i];
        RECT r = TpCell(i);
        float x = r.left + (TP_CELL - 26) / 2.f, y = r.top + (TP_CELL - 26) / 2.f;
        if (i == g_tpHot) GlassBlob(c, r.left + 3.f, r.top + 3.f, r.right - 3.f, r.bottom - 3.f, 12, 0.f, 0.08f, 1.f);
        BlitIconSmooth(c, t.icon, t.iconN, x, y, 26.f, 1.f);
        if (t.shine) {
            float st = (now - t.shine) / 650.f;
            if (st < 1.f) IconShine(c, t.icon, t.iconN, x, y, 26.f, st); else t.shine = 0;
        }
    }

    const wchar_t* tip = (g_tpHot >= 0 && !g_tpBtns[g_tpHot].tip.empty()) ? g_tpBtns[g_tpHot].tip.c_str()
                       : g_tpBtns.empty() ? L"No tray icons" : L"Tray";
    FillRRect(c, TP_PAD, g_tpH - TP_TIP_H - 4.f, g_tpW - TP_PAD, g_tpH - TP_TIP_H - 3.f, 0.5f, white, 0.12f);
    DrawTextOn(c, g_tpMask, tip, { TP_PAD, g_tpH - TP_TIP_H - 2, g_tpW - TP_PAD, g_tpH - 6 }, g_dockTipF, white,
               g_tpHot >= 0 ? 0.95f : 0.55f, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    POINT dst{ g_tpX, g_tpY - static_cast<int>((1.f - open) * 8.f) }, src{ 0, 0 };
    SIZE sz{ g_tpW, g_tpH };
    BLENDFUNCTION bf{ AC_SRC_OVER, 0, static_cast<BYTE>(255.f * open), AC_SRC_ALPHA };
    UpdateLayeredWindow(g_tp, nullptr, &dst, &sz, g_tpS.dc, &src, 0, &bf, ULW_ALPHA);
}

// After a right-click the app's own menu takes the focus, so "lost focus" can't mean
// "close" any more: while sticky, the pop-up stays open (through the menu and after
// it) and closes only on a click outside both it and any menu, or on Esc.
bool g_tpSticky = false, g_tpMouseWasDown = false;
const UINT_PTR TP_WATCH_TIMER = 2;

void TpClose() {
    if (!g_tpVisible || g_tpClosing) return;
    g_tpClosing = true;
    g_tpSticky = false;
    KillTimer(g_tp, TP_WATCH_TIMER);
    g_tpAnimStart = g_tpLastClose = GetTickCount();
    SetTimer(g_tp, TP_ANIM_TIMER, 15, nullptr);
}

LRESULT CALLBACK TrayPopProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_ACTIVATE:
        if (LOWORD(w) == WA_INACTIVE && !g_tpSticky) TpClose();
        return 0;
    case WM_KEYDOWN:
        if (w == VK_ESCAPE) TpClose();
        return 0;
    case WM_TIMER: {
        if (w == TP_WATCH_TIMER) { // sticky: a click outside the pop-up and outside any menu closes it
            bool down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) || (GetAsyncKeyState(VK_RBUTTON) & 0x8000);
            if (down && !g_tpMouseWasDown) {
                POINT p;
                GetCursorPos(&p);
                HWND at = GetAncestor(WindowFromPoint(p), GA_ROOT);
                wchar_t cls[32]{};
                if (at) GetClassNameW(at, cls, 32);
                if (at != h && wcscmp(cls, L"#32768") != 0) TpClose(); // #32768 = a popup menu
            }
            g_tpMouseWasDown = down;
            return 0;
        }
        TpRender();
        bool shining = false;
        for (const auto& t : g_tpBtns) shining |= t.shine != 0;
        bool animating = GetTickCount() - g_tpAnimStart < 200;
        if (g_tpClosing && !animating) {
            ShowWindow(h, SW_HIDE);
            g_tpVisible = g_tpClosing = false;
            KillTimer(h, TP_ANIM_TIMER);
        } else if (!animating && !shining) {
            KillTimer(h, TP_ANIM_TIMER);
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, h, 0 };
        TrackMouseEvent(&tme);
        int hot = TpHit(POINT{ GET_X_LPARAM(l), GET_Y_LPARAM(l) });
        if (hot != g_tpHot) {
            g_tpHot = hot;
            if (hot >= 0) g_tpBtns[hot].shine = GetTickCount();
            SetTimer(h, TP_ANIM_TIMER, 15, nullptr);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        g_tpHot = -1;
        TpRender();
        return 0;
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
        g_tpPressed = TpHit(POINT{ GET_X_LPARAM(l), GET_Y_LPARAM(l) });
        return 0;
    case WM_LBUTTONUP:
    case WM_RBUTTONUP: {
        int hit = TpHit(POINT{ GET_X_LPARAM(l), GET_Y_LPARAM(l) });
        if (hit >= 0 && hit == g_tpPressed) {
            TrayBtn t = g_tpBtns[hit];
            if (msg == WM_RBUTTONUP) { // the app's menu: keep the tray open around it
                g_tpSticky = true;
                g_tpMouseWasDown = true;
                SetTimer(h, TP_WATCH_TIMER, 40, nullptr);
            } else {
                TpClose(); // left-click usually opens the app's window: get out of its way
            }
            TrayForward(t, msg == WM_RBUTTONUP);
        }
        g_tpPressed = -1;
        return 0;
    }
    }
    return DefWindowProcW(h, msg, w, l);
}

void TrayPopToggle(float buttonCenterX) {
    if (g_tpVisible && !g_tpClosing) { TpClose(); return; }
    if (GetTickCount() - g_tpLastClose < 250) return; // this click is what just closed it
    if (!g_tp) {
        WNDCLASSW wc{};
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpfnWndProc = TrayPopProc;
        wc.lpszClassName = L"REICOTrayPop";
        wc.hCursor = LoadCursor(nullptr, IDC_HAND);
        RegisterClassW(&wc);
        g_tp = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"REICOTrayPop", L"Tray", WS_POPUP,
                               0, 0, 0, 0, nullptr, nullptr, wc.hInstance, nullptr);
        if (!g_tp) return;
    }

    g_tpBtns = ReadTray();
    int n = std::max(1, static_cast<int>(g_tpBtns.size()));
    int cols = std::min(n, TP_COLS), rows = (n + TP_COLS - 1) / TP_COLS;
    g_tpW = std::max(2 * TP_PAD + cols * TP_CELL, 170);
    g_tpH = 2 * TP_PAD + rows * TP_CELL + TP_TIP_H;
    if (!SurfCreate(g_tpS, g_tpW, g_tpH) || !SurfCreate(g_tpMask, g_tpW, g_tpH)) return;

    g_tpX = std::min(std::max(static_cast<int>(buttonCenterX) - g_tpW / 2, static_cast<int>(g_dockMon.left) + 8),
                     static_cast<int>(g_dockMon.right) - g_tpW - 8);
    g_tpY = g_dockOrigin.y + DOCK_TOP + DOCK_H + 10;
    g_tpHot = g_tpPressed = -1;
    g_tpVisible = true;
    g_tpClosing = false;
    g_tpAnimStart = GetTickCount();
    TpRender();
    ShowWindow(g_tp, SW_SHOW);
    SetForegroundWindow(g_tp); // so clicking anywhere else closes it
    SetTimer(g_tp, TP_ANIM_TIMER, 15, nullptr);
}

// ---- glass calendar (drops down under the clock) ----------------------------

const int CAL_PAD = 16, CAL_CW = 34, CAL_CH = 32;
const int CAL_W = 2 * CAL_PAD + 7 * CAL_CW;
const int CAL_GRID_Y = 136, CAL_H = CAL_GRID_Y + 6 * CAL_CH + 14;
const UINT_PTR CAL_ANIM_TIMER = 1, CAL_TICK_TIMER = 2;
enum { CALH_NONE = -1, CALH_PREV = 100, CALH_NEXT = 101 };

HWND g_cal = nullptr;
Surface g_calS, g_calMask;
int g_calX = 0, g_calY = 0, g_calOffset = 0, g_calHot = CALH_NONE;
bool g_calVisible = false, g_calClosing = false;
DWORD g_calAnimStart = 0, g_calLastClose = 0;
HFONT g_calTimeF = nullptr, g_calMonthF = nullptr, g_calDayF = nullptr, g_calSmallF = nullptr, g_calArrowF = nullptr;

int DaysInMonth(int y, int m) {
    static const int d[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    return m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0) ? 29 : d[m - 1];
}
int WeekdayMon0(int y, int m, int d) { // 0 = Monday
    static const int t[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    if (m < 3) y -= 1;
    int sun0 = (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
    return (sun0 + 6) % 7;
}

RECT CalArrow(bool next) {
    return next ? RECT{ CAL_W - CAL_PAD - 30, 84, CAL_W - CAL_PAD, 112 }
                : RECT{ CAL_W - CAL_PAD - 64, 84, CAL_W - CAL_PAD - 34, 112 };
}

void CalRender() {
    DWORD now = GetTickCount();
    float p = Clamp01((now - g_calAnimStart) / (g_calClosing ? 130.f : 180.f));
    float open = g_calClosing ? 1.f - p : EaseOut3(p);
    Canvas c = g_calS.cv();
    ZeroMemory(c.px, sizeof(UINT32) * static_cast<size_t>(c.w) * c.h);
    const COLORREF white = RGB(255, 255, 255);
    COLORREF tint = RGB(g_glassCfg.tintR, g_glassCfg.tintG, g_glassCfg.tintB);
    COLORREF veil = RGB(GetRValue(tint) * 3 / 10 + 14, GetGValue(tint) * 3 / 10 + 14, GetBValue(tint) * 3 / 10 + 18);
    FillRRect(c, 0.5f, 0.5f, CAL_W - 0.5f, CAL_H - 0.5f, 20, veil, 0.93f); // dense: readable over busy windows
    GlassLight(c, 0.5f, 0.5f, CAL_W - 0.5f, CAL_H - 0.5f, 20, 1.f);

    SYSTEMTIME st;
    GetLocalTime(&st);
    static const wchar_t* dayNames[] = { L"Sunday", L"Monday", L"Tuesday", L"Wednesday", L"Thursday", L"Friday", L"Saturday" };
    static const wchar_t* monthNames[] = { L"January", L"February", L"March", L"April", L"May", L"June", L"July",
                                           L"August", L"September", L"October", L"November", L"December" };
    wchar_t buf[64];
    swprintf(buf, 64, L"%02d:%02d:%02d", st.wHour, st.wMinute, st.wSecond);
    DrawGlassText(c, g_calMask, buf, { CAL_PAD, 12, CAL_W - CAL_PAD, 50 }, g_calTimeF, 1.f, DT_LEFT | DT_VCENTER | DT_SINGLELINE, 0.2f, 0.75f);
    swprintf(buf, 64, L"%ls, %d %ls %d", dayNames[st.wDayOfWeek % 7], st.wDay, monthNames[(st.wMonth + 11) % 12], st.wYear);
    DrawTextOn(c, g_calMask, buf, { CAL_PAD, 50, CAL_W - CAL_PAD, 68 }, g_calSmallF, white, 0.7f, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    FillRRect(c, CAL_PAD, 76, CAL_W - CAL_PAD, 77, 0.5f, white, 0.12f);

    // the month being shown
    int y = st.wYear, m = st.wMonth + g_calOffset;
    while (m < 1) { m += 12; --y; }
    while (m > 12) { m -= 12; ++y; }
    swprintf(buf, 64, L"%ls %d", monthNames[m - 1], y);
    DrawGlassText(c, g_calMask, buf, { CAL_PAD, 84, CAL_W - 70, 112 }, g_calMonthF, 1.f, DT_LEFT | DT_VCENTER | DT_SINGLELINE, 0.2f, 0.7f);
    for (bool next : { false, true }) {
        RECT r = CalArrow(next);
        bool hot = g_calHot == (next ? CALH_NEXT : CALH_PREV);
        GlassBlob(c, r.left + 1.f, r.top + 1.f, r.right - 1.f, r.bottom - 1.f, 13, 0.f, hot ? 0.18f : 0.08f, 1.f);
        DrawGlassText(c, g_calMask, next ? L"" : L"", r, g_calArrowF, 1.f,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE, 0.f, 0.85f);
    }

    static const wchar_t* wd[] = { L"Mo", L"Tu", L"We", L"Th", L"Fr", L"Sa", L"Su" };
    for (int i = 0; i < 7; ++i)
        DrawTextOn(c, g_calMask, wd[i], { CAL_PAD + i * CAL_CW, 116, CAL_PAD + (i + 1) * CAL_CW, 134 }, g_calSmallF,
                   white, 0.45f, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    int first = WeekdayMon0(y, m, 1), dim = DaysInMonth(y, m);
    int py = m == 1 ? y - 1 : y, pm = m == 1 ? 12 : m - 1, pdim = DaysInMonth(py, pm);
    COLORREF acc = AccentColour();
    for (int cell = 0; cell < 42; ++cell) {
        int day = cell - first + 1;
        bool inMonth = day >= 1 && day <= dim;
        int shown = inMonth ? day : (day < 1 ? pdim + day : day - dim);
        bool today = inMonth && g_calOffset == 0 && day == st.wDay;
        float x0 = static_cast<float>(CAL_PAD + (cell % 7) * CAL_CW), y0 = static_cast<float>(CAL_GRID_Y + (cell / 7) * CAL_CH);
        float cx = x0 + CAL_CW / 2.f, cy = y0 + CAL_CH / 2.f;
        if (today) {
            FillCircle(c, cx, cy, 14.f, acc, 0.85f);
            GlassLight(c, cx - 14, cy - 14, cx + 14, cy + 14, 14, 1.f);
        } else if (cell == g_calHot) {
            GlassBlob(c, cx - 14, cy - 14, cx + 14, cy + 14, 14, 0.f, 0.12f, 1.f);
        }
        swprintf(buf, 64, L"%d", shown);
        RECT r{ static_cast<LONG>(x0), static_cast<LONG>(y0), static_cast<LONG>(x0 + CAL_CW), static_cast<LONG>(y0 + CAL_CH) };
        DrawTextOn(c, g_calMask, buf, r, g_calDayF, white, inMonth ? (today ? 1.f : 0.88f) : 0.28f,
                   DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    POINT dst{ g_calX, g_calY - static_cast<int>((1.f - open) * 8.f) }, src{ 0, 0 };
    SIZE sz{ CAL_W, CAL_H };
    BLENDFUNCTION bf{ AC_SRC_OVER, 0, static_cast<BYTE>(255.f * open), AC_SRC_ALPHA };
    UpdateLayeredWindow(g_cal, nullptr, &dst, &sz, g_calS.dc, &src, 0, &bf, ULW_ALPHA);
}

int CalHit(POINT p) {
    for (bool next : { false, true }) {
        RECT r = CalArrow(next);
        if (PtInRect(&r, p)) return next ? CALH_NEXT : CALH_PREV;
    }
    if (p.x >= CAL_PAD && p.x < CAL_PAD + 7 * CAL_CW && p.y >= CAL_GRID_Y && p.y < CAL_GRID_Y + 6 * CAL_CH)
        return ((p.y - CAL_GRID_Y) / CAL_CH) * 7 + (p.x - CAL_PAD) / CAL_CW;
    return CALH_NONE;
}

void CalClose() {
    if (!g_calVisible || g_calClosing) return;
    g_calClosing = true;
    g_calAnimStart = g_calLastClose = GetTickCount();
    SetTimer(g_cal, CAL_ANIM_TIMER, 15, nullptr);
}

LRESULT CALLBACK CalProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_ACTIVATE:
        if (LOWORD(w) == WA_INACTIVE) CalClose();
        return 0;
    case WM_KEYDOWN:
        if (w == VK_ESCAPE) CalClose();
        else if (w == VK_LEFT || w == VK_RIGHT) { g_calOffset += w == VK_RIGHT ? 1 : -1; CalRender(); }
        return 0;
    case WM_TIMER:
        if (w == CAL_TICK_TIMER) { CalRender(); return 0; } // seconds
        CalRender();
        if (GetTickCount() - g_calAnimStart >= 200) {
            KillTimer(h, CAL_ANIM_TIMER);
            if (g_calClosing) {
                ShowWindow(h, SW_HIDE);
                KillTimer(h, CAL_TICK_TIMER);
                g_calVisible = g_calClosing = false;
            }
        }
        return 0;
    case WM_MOUSEMOVE: {
        TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, h, 0 };
        TrackMouseEvent(&tme);
        int hot = CalHit(POINT{ GET_X_LPARAM(l), GET_Y_LPARAM(l) });
        if (hot != g_calHot) { g_calHot = hot; CalRender(); }
        return 0;
    }
    case WM_MOUSELEAVE:
        g_calHot = CALH_NONE;
        CalRender();
        return 0;
    case WM_LBUTTONUP: {
        int hit = CalHit(POINT{ GET_X_LPARAM(l), GET_Y_LPARAM(l) });
        if (hit == CALH_PREV) --g_calOffset;
        else if (hit == CALH_NEXT) ++g_calOffset;
        CalRender();
        return 0;
    }
    case WM_MOUSEWHEEL:
        g_calOffset += GET_WHEEL_DELTA_WPARAM(w) > 0 ? -1 : 1;
        CalRender();
        return 0;
    }
    return DefWindowProcW(h, msg, w, l);
}

void CalToggle(float buttonCenterX) {
    if (g_calVisible && !g_calClosing) { CalClose(); return; }
    if (GetTickCount() - g_calLastClose < 250) return; // this click is what just closed it
    if (!g_cal) {
        g_calTimeF = InterFont(30, 700);
        g_calMonthF = InterFont(16, 600);
        g_calDayF = InterFont(13, 500);
        g_calSmallF = InterFont(12, 500);
        g_calArrowF = MakeFont(12, FW_NORMAL, L"Segoe MDL2 Assets");
        WNDCLASSW wc{};
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpfnWndProc = CalProc;
        wc.lpszClassName = L"REICOCalendar";
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        RegisterClassW(&wc);
        g_cal = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"REICOCalendar", L"Calendar", WS_POPUP,
                                0, 0, 0, 0, nullptr, nullptr, wc.hInstance, nullptr);
        if (!g_cal || !SurfCreate(g_calS, CAL_W, CAL_H) || !SurfCreate(g_calMask, CAL_W, CAL_H)) return;
    }
    g_calOffset = 0;
    g_calHot = CALH_NONE;
    g_calX = std::min(std::max(static_cast<int>(buttonCenterX) - CAL_W / 2, static_cast<int>(g_dockMon.left) + 8),
                      static_cast<int>(g_dockMon.right) - CAL_W - 8);
    g_calY = g_dockOrigin.y + DOCK_TOP + DOCK_H + 10;
    g_calVisible = true;
    g_calClosing = false;
    g_calAnimStart = GetTickCount();
    CalRender();
    ShowWindow(g_cal, SW_SHOW);
    SetForegroundWindow(g_cal);
    SetTimer(g_cal, CAL_ANIM_TIMER, 15, nullptr);
    SetTimer(g_cal, CAL_TICK_TIMER, 1000, nullptr);
}

// Start button: the Windows logo as four glass panes.
void DrawWinLogo(Canvas c, float cx, float cy, float s, float hot) {
    float gap = s * 0.09f, q = (s - gap) / 2.f;
    for (int j = 0; j < 2; ++j)
        for (int i = 0; i < 2; ++i) {
            float x0 = cx - s / 2.f + i * (q + gap), y0 = cy - s / 2.f + j * (q + gap);
            GlassBlob(c, x0, y0, x0 + q, y0 + q, q * 0.16f, 0.55f * hot, 0.72f + 0.15f * hot, 1.f);
        }
}

// ---- game launcher (joystick button at the top-left) -------------------------
//
// Games are found where launchers record their installs, not by crawling whole
// drives (slow, and full of non-game exes): Steam (all library folders), Epic,
// Riot (Valorant / League go through the Riot Client), GOG, plus game folders
// like X:\Games, EA, Ubisoft and Rockstar on every drive. Store launchers'
// games are started through the launcher (so updates / overlays work); for
// the rest the game's main exe is picked by name and size, skipping
// uninstallers, crash reporters, anti-cheat helpers and the like.
// Scanning runs on a background thread; results are cached in C:\REICO\games.txt.

struct Game {
    std::wstring name, file, args, iconPath, source; // file = exe or launcher URL
    std::vector<UINT32> icon;                        // GL_ICON px, loaded when first drawn
    bool iconTried = false;
    DWORD shine = 0;
};

const int GL_COLS = 4, GL_ROWS = 4, GL_TW = 104, GL_TH = 112, GL_PAD = 16, GL_HEAD = 104, GL_ICON = 48;
const int GL_W = 2 * GL_PAD + GL_COLS * GL_TW, GL_H = GL_HEAD + GL_ROWS * GL_TH + 12;
const UINT WM_GAMES_FOUND = WM_APP + 40; // scan thread -> dock window, lParam = std::vector<Game>*
const UINT_PTR GL_ANIM_TIMER = 1;
const wchar_t* GAMES_CACHE = L"C:\\REICO\\games.txt";
enum { GLH_NONE = -1, GLH_REFRESH = 10000, GLH_ADD };
bool g_glDialog = false; // file picker / menu open: don't close the launcher on focus loss

HWND g_gl = nullptr;
Surface g_glS, g_glMask;
std::vector<Game> g_games;
std::vector<int> g_glView;             // indexes into g_games matching the search
std::wstring g_glFilter;
int g_glRow = 0, g_glHot = GLH_NONE, g_glPressed = GLH_NONE, g_glX = 0, g_glY = 0;
bool g_glVisible = false, g_glClosing = false, g_glScanning = false;
DWORD g_glAnimStart = 0, g_glLastClose = 0;
HFONT g_glTitleF = nullptr, g_glNameF = nullptr, g_glSmallF = nullptr, g_glGlyphF = nullptr, g_glSearchF = nullptr;

// -- finding games (scan thread: no UI here) --

std::wstring NormName(const std::wstring& s) {
    std::wstring o;
    for (wchar_t c : s) if (iswalnum(c)) o += towlower(c);
    return o;
}

std::wstring ReadTextFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::string b((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (b.size() >= 3 && static_cast<unsigned char>(b[0]) == 0xEF) b.erase(0, 3); // UTF-8 BOM
    int n = MultiByteToWideChar(CP_UTF8, 0, b.data(), static_cast<int>(b.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, b.data(), static_cast<int>(b.size()), &w[0], n);
    return w;
}

// Every value of "key" in a VDF ("key" "value") or JSON ("key": "value") text.
std::vector<std::wstring> QuotedValues(const std::wstring& t, const std::wstring& key) {
    std::vector<std::wstring> out;
    std::wstring needle = L"\"" + key + L"\"";
    for (size_t pos = t.find(needle); pos != std::wstring::npos; pos = t.find(needle, pos + 1)) {
        size_t i = pos + needle.size();
        while (i < t.size() && (iswspace(t[i]) || t[i] == L':')) ++i;
        if (i >= t.size() || t[i] != L'"') continue;
        std::wstring v;
        for (++i; i < t.size() && t[i] != L'"'; ++i) {
            if (t[i] == L'\\' && i + 1 < t.size()) { ++i; if (t[i] == L'n') { v += L' '; continue; } }
            v += t[i];
        }
        out.push_back(v);
    }
    return out;
}
std::wstring QuotedValue(const std::wstring& t, const std::wstring& key) {
    auto v = QuotedValues(t, key);
    return v.empty() ? L"" : v.front();
}

std::wstring RegString(HKEY root, const std::wstring& path, const wchar_t* value) {
    wchar_t buf[1024]{};
    DWORD sz = sizeof(buf);
    if (RegGetValueW(root, path.c_str(), value, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, buf, &sz) != ERROR_SUCCESS) return L"";
    return buf;
}

// The game's own exe in its install folder: best name match, then size; helpers skipped.
std::wstring MainExe(const fs::path& dir, const std::wstring& hint) {
    static const wchar_t* skip[] = { L"unins", L"setup", L"install", L"redist", L"crash", L"report", L"update",
                                     L"helper", L"prereq", L"dxsetup", L"vc_", L"vcredist", L"dotnet", L"cleanup",
                                     L"uploader", L"anticheat", L"battleye", L"benchmark", L"server", L"editor",
                                     L"config", L"activation", L"cefprocess", L"webhelper", L"subprocess", L"handler",
                                     L"overlay", L"bugsplat", L"sandbox", L"directx", L"physx", L"touchup", L"7z" };
    const std::wstring h = NormName(hint);
    std::error_code ec;
    std::wstring best;
    double bestScore = -1e9;
    int visited = 0;
    auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
    for (; !ec && it != fs::recursive_directory_iterator() && visited < 20000; it.increment(ec), ++visited) {
        if (it.depth() >= 3) it.disable_recursion_pending();
        const fs::path& p = it->path();
        if (LowerStr(p.extension().wstring()) != L".exe" || !it->is_regular_file(ec)) continue;
        std::wstring stem = LowerStr(p.stem().wstring()), ns = NormName(stem);
        bool bad = false;
        for (const wchar_t* s : skip) if (stem.find(s) != std::wstring::npos) { bad = true; break; }
        if (bad) continue;
        double score = std::min(60.0, static_cast<double>(it->file_size(ec)) / 1048576.0);
        if (!h.empty() && ns.size() >= 3 && (ns == h || h.find(ns) != std::wstring::npos || ns.find(h) != std::wstring::npos))
            score += 100;
        if (stem.find(L"launcher") != std::wstring::npos) score -= 30;
        if (stem.find(L"shipping") != std::wstring::npos) score += 15;
        score -= it.depth() * 8.0;
        if (score > bestScore) { bestScore = score; best = p.wstring(); }
    }
    return best;
}

void ScanSteam(std::vector<Game>& out) {
    std::wstring steam = RegString(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath");
    if (steam.empty()) return;
    std::vector<std::wstring> libs{ steam };
    for (auto& p : QuotedValues(ReadTextFile(fs::path(steam) / L"steamapps\\libraryfolders.vdf"), L"path")) libs.push_back(p);
    std::error_code ec;
    std::vector<std::wstring> seen;
    for (const auto& lib : libs) {
        fs::path apps = fs::path(lib) / L"steamapps";
        if (std::find(seen.begin(), seen.end(), LowerStr(fs::weakly_canonical(apps, ec).wstring())) != seen.end()) continue;
        seen.push_back(LowerStr(fs::weakly_canonical(apps, ec).wstring()));
        for (const auto& e : fs::directory_iterator(apps, ec)) {
            std::wstring fn = e.path().filename().wstring();
            if (fn.rfind(L"appmanifest_", 0) != 0) continue;
            std::wstring t = ReadTextFile(e.path());
            std::wstring id = QuotedValue(t, L"appid"), name = QuotedValue(t, L"name"), dir = QuotedValue(t, L"installdir");
            if (id.empty() || name.empty() || id == L"228980") continue; // Steamworks redistributables
            if (name.find(L"Proton") == 0 || name.find(L"Steam Linux Runtime") == 0 || name.find(L"Steamworks") == 0 ||
                name.find(L"SteamVR") != std::wstring::npos) continue; // tools, not games
            fs::path gd = apps / L"common" / dir;
            if (!fs::exists(gd, ec)) continue;
            out.push_back({ name, L"steam://rungameid/" + id, L"", MainExe(gd, dir), L"Steam" });
        }
    }
}

void ScanEpic(std::vector<Game>& out) {
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(L"C:\\ProgramData\\Epic\\EpicGamesLauncher\\Data\\Manifests", ec)) {
        if (LowerStr(e.path().extension().wstring()) != L".item") continue;
        std::wstring t = ReadTextFile(e.path());
        std::wstring name = QuotedValue(t, L"DisplayName"), loc = QuotedValue(t, L"InstallLocation");
        std::wstring exe = QuotedValue(t, L"LaunchExecutable"), app = QuotedValue(t, L"AppName");
        std::wstring ns = QuotedValue(t, L"CatalogNamespace"), item = QuotedValue(t, L"CatalogItemId");
        if (name.empty() || app.empty() || !fs::exists(loc, ec)) continue;
        if (name.find(L"Unreal Engine") != std::wstring::npos) continue;
        std::wstring url = L"com.epicgames.launcher://apps/" + ns + L"%3A" + item + L"%3A" + app + L"?action=launch&silent=true";
        out.push_back({ name, url, L"", (fs::path(loc) / exe).wstring(), L"Epic Games" });
    }
}

// Riot registers every game with Windows' uninstall list, e.g.
//   UninstallString: "C:\Riot Games\Riot Client\RiotClientServices.exe" --uninstall-product=valorant --uninstall-patchline=live
// which gives the Riot Client, product and patchline wherever the game itself lives.
void ScanRiot(std::vector<Game>& out) {
    const wchar_t* base = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall";
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, base, 0, KEY_READ, &k) != ERROR_SUCCESS) return;
    wchar_t sub[256];
    for (DWORD i = 0; RegEnumKeyW(k, i, sub, 256) == ERROR_SUCCESS; ++i) {
        std::wstring path = std::wstring(base) + L"\\" + sub;
        std::wstring un = RegString(HKEY_CURRENT_USER, path, L"UninstallString");
        size_t q = un.find(L"RiotClientServices.exe"), pp = un.find(L"--uninstall-product=");
        if (q == std::wstring::npos || pp == std::wstring::npos) continue;
        std::wstring rc = un.substr(un[0] == L'"' ? 1 : 0, q + wcslen(L"RiotClientServices.exe") - (un[0] == L'"' ? 1 : 0));
        auto arg = [&](size_t at) { size_t e = un.find(L' ', at); return un.substr(at, e == std::wstring::npos ? std::wstring::npos : e - at); };
        std::wstring product = arg(pp + wcslen(L"--uninstall-product="));
        size_t pl = un.find(L"--uninstall-patchline=");
        std::wstring patch = pl == std::wstring::npos ? L"live" : arg(pl + wcslen(L"--uninstall-patchline="));
        if (product.empty() || _wcsicmp(product.c_str(), L"Riot_Client") == 0) continue;
        if (patch.empty()) patch = L"live";
        std::wstring name = RegString(HKEY_CURRENT_USER, path, L"DisplayName");
        std::wstring loc = RegString(HKEY_CURRENT_USER, path, L"InstallLocation");
        std::error_code ec;
        if (name.empty() || !fs::exists(rc, ec)) continue;
        out.push_back({ name, rc, L"--launch-product=" + product + L" --launch-patchline=" + patch,
                        loc.empty() ? rc : MainExe(loc, name), L"Riot" });
    }
    RegCloseKey(k);
}

void ScanGog(std::vector<Game>& out) {
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\GOG.com\\Games", 0, KEY_READ, &k) != ERROR_SUCCESS) return;
    wchar_t sub[256];
    for (DWORD i = 0; RegEnumKeyW(k, i, sub, 256) == ERROR_SUCCESS; ++i) {
        std::wstring path = std::wstring(L"SOFTWARE\\WOW6432Node\\GOG.com\\Games\\") + sub;
        std::wstring name = RegString(HKEY_LOCAL_MACHINE, path, L"gameName"), exe = RegString(HKEY_LOCAL_MACHINE, path, L"exe");
        std::error_code ec;
        if (!name.empty() && !exe.empty() && fs::exists(exe, ec)) out.push_back({ name, exe, L"", exe, L"GOG" });
    }
    RegCloseKey(k);
}

// Plain game folders on every fixed drive: one game per sub-folder.
void ScanFolders(std::vector<Game>& out) {
    static const wchar_t* roots[] = { L"Games", L"Program Files\\EA Games", L"Program Files (x86)\\Origin Games",
                                      L"Program Files\\Rockstar Games", L"Program Files (x86)\\Ubisoft\\Ubisoft Game Launcher\\games",
                                      L"Program Files\\Ubisoft\\Ubisoft Game Launcher\\games" };
    DWORD drives = GetLogicalDrives();
    std::error_code ec;
    for (wchar_t d = L'A'; d <= L'Z'; ++d) {
        if (!(drives & (1u << (d - L'A')))) continue;
        std::wstring drive = std::wstring(1, d) + L":\\";
        if (GetDriveTypeW(drive.c_str()) != DRIVE_FIXED) continue;
        for (const wchar_t* r : roots) {
            for (const auto& e : fs::directory_iterator(fs::path(drive) / r, ec)) {
                if (!e.is_directory(ec)) continue;
                std::wstring name = e.path().filename().wstring();
                std::wstring exe = MainExe(e.path(), name);
                if (!exe.empty()) out.push_back({ name, exe, L"", exe, L"Folder" });
            }
        }
    }
}

// Three small UTF-8 files in C:\REICO:
//   games.txt         what the last scan found (name, file, args, icon exe, source per line)
//   games_added.txt   games added by hand with "+" (kept through rescans)
//   games_hidden.txt  names removed with right-click (stay gone after rescans)
const wchar_t* GAMES_ADDED = L"C:\\REICO\\games_added.txt";
const wchar_t* GAMES_HIDDEN = L"C:\\REICO\\games_hidden.txt";

void WriteUtf8(const wchar_t* path, const std::wstring& text) {
    int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string b(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), &b[0], n, nullptr, nullptr);
    std::ofstream(fs::path(path), std::ios::binary) << b;
}

void SaveGames(const wchar_t* path, const std::vector<Game>& games) {
    std::wstring all;
    for (const auto& g : games) all += g.name + L"\t" + g.file + L"\t" + g.args + L"\t" + g.iconPath + L"\t" + g.source + L"\n";
    WriteUtf8(path, all);
}

std::vector<Game> LoadGames(const wchar_t* path) {
    std::vector<Game> out;
    std::wstringstream ss(ReadTextFile(path));
    std::wstring line;
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        std::vector<std::wstring> f;
        size_t s = 0;
        for (size_t t; (t = line.find(L'\t', s)) != std::wstring::npos; s = t + 1) f.push_back(line.substr(s, t - s));
        f.push_back(line.substr(s));
        if (f.size() >= 5 && !f[0].empty()) out.push_back({ f[0], f[1], f[2], f[3], f[4] });
    }
    return out;
}

std::vector<std::wstring> LoadHidden() {
    std::vector<std::wstring> out;
    std::wstringstream ss(ReadTextFile(GAMES_HIDDEN));
    std::wstring line;
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        if (!line.empty()) out.push_back(line);
    }
    return out;
}

// scanned + added - removed, one entry per game, sorted by name
std::vector<Game> ComposeGames(std::vector<Game> scanned) {
    std::vector<Game> all = LoadGames(GAMES_ADDED); // added first: they win a name clash
    for (auto& g : scanned) all.push_back(std::move(g));
    std::vector<std::wstring> hidden = LoadHidden();
    std::vector<Game> out;
    for (auto& g : all) {
        std::wstring n = NormName(g.name);
        if (std::find(hidden.begin(), hidden.end(), n) != hidden.end()) continue;
        if (std::any_of(out.begin(), out.end(), [&](const Game& u) { return NormName(u.name) == n; })) continue;
        out.push_back(std::move(g));
    }
    std::sort(out.begin(), out.end(), [](const Game& a, const Game& b) { return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0; });
    return out;
}

// Background: either a real scan (the rescan button) or just reading the saved list.
// Icons are loaded here too, so opening the launcher never waits on the shell.
DWORD WINAPI GameLoadThread(void* doScan) {
    std::vector<Game> scanned;
    if (doScan) {
        ScanSteam(scanned);
        ScanEpic(scanned);
        ScanRiot(scanned);
        ScanGog(scanned);
        ScanFolders(scanned);
        // one entry per game (a store copy wins over the same game found as a plain folder)
        std::vector<Game> unique;
        for (auto& g : scanned) {
            std::wstring n = NormName(g.name);
            if (std::none_of(unique.begin(), unique.end(), [&](const Game& u) { return NormName(u.name) == n; }))
                unique.push_back(std::move(g));
        }
        scanned = std::move(unique);
        SaveGames(GAMES_CACHE, scanned);
    } else {
        scanned = LoadGames(GAMES_CACHE);
    }
    auto* games = new std::vector<Game>(ComposeGames(std::move(scanned)));

    typedef HRESULT (WINAPI *PFN_CoInitEx)(LPVOID, DWORD);
    if (auto init = reinterpret_cast<PFN_CoInitEx>(reinterpret_cast<void*>(GetProcAddress(LoadLibraryW(L"ole32.dll"), "CoInitializeEx"))))
        init(nullptr, COINIT_APARTMENTTHREADED);
    for (auto& g : *games) {
        g.icon = IconForPath(g.iconPath.empty() ? g.file : g.iconPath, GL_ICON);
        g.iconTried = true;
    }
    if (!g_dock || !PostMessageW(g_dock, WM_GAMES_FOUND, 0, reinterpret_cast<LPARAM>(games))) delete games;
    return 0;
}

bool g_glScanRunning = false; // a real scan (not just loading the list) is in progress

void GlStartLoad(bool scan) {
    if (g_glScanning) return;
    g_glScanning = true;
    g_glScanRunning = scan;
    if (HANDLE t = CreateThread(nullptr, 0, GameLoadThread, scan ? reinterpret_cast<void*>(1) : nullptr, 0, nullptr)) CloseHandle(t);
    else g_glScanning = g_glScanRunning = false;
}

// -- the panel --

void GlRebuildView() {
    g_glView.clear();
    std::wstring f = LowerStr(g_glFilter);
    for (int i = 0; i < static_cast<int>(g_games.size()); ++i)
        if (f.empty() || LowerStr(g_games[i].name).find(f) != std::wstring::npos) g_glView.push_back(i);
    int rows = (static_cast<int>(g_glView.size()) + GL_COLS - 1) / GL_COLS;
    g_glRow = std::max(0, std::min(g_glRow, rows - GL_ROWS));
}

RECT GlTile(int slot) { // slot = position on screen, 0 .. COLS*ROWS-1
    int col = slot % GL_COLS, row = slot / GL_COLS;
    return { GL_PAD + col * GL_TW, GL_HEAD + row * GL_TH, GL_PAD + (col + 1) * GL_TW, GL_HEAD + (row + 1) * GL_TH };
}
RECT GlRefreshRect() { return { GL_W - GL_PAD - 34, 16, GL_W - GL_PAD, 50 }; }
RECT GlAddRect() { return { GL_W - GL_PAD - 76, 16, GL_W - GL_PAD - 42, 50 }; }

int GlHit(POINT p) {
    RECT rr = GlRefreshRect(), ar = GlAddRect();
    if (PtInRect(&rr, p)) return GLH_REFRESH;
    if (PtInRect(&ar, p)) return GLH_ADD;
    for (int s = 0; s < GL_COLS * GL_ROWS; ++s) {
        RECT r = GlTile(s);
        int idx = g_glRow * GL_COLS + s;
        if (PtInRect(&r, p) && idx < static_cast<int>(g_glView.size())) return idx;
    }
    return GLH_NONE;
}

void GlRender() {
    DWORD now = GetTickCount();
    float p = Clamp01((now - g_glAnimStart) / (g_glClosing ? 130.f : 200.f));
    float open = g_glClosing ? 1.f - p : EaseOut3(p);
    Canvas c = g_glS.cv();
    ZeroMemory(c.px, sizeof(UINT32) * static_cast<size_t>(c.w) * c.h);
    const COLORREF white = RGB(255, 255, 255);
    COLORREF tint = RGB(g_glassCfg.tintR, g_glassCfg.tintG, g_glassCfg.tintB);
    COLORREF veil = RGB(GetRValue(tint) * 3 / 10 + 14, GetGValue(tint) * 3 / 10 + 14, GetBValue(tint) * 3 / 10 + 18);
    FillRRect(c, 0.5f, 0.5f, GL_W - 0.5f, GL_H - 0.5f, 22, veil, 0.9f);
    GlassLight(c, 0.5f, 0.5f, GL_W - 0.5f, GL_H - 0.5f, 22, 1.f);

    // header: title, count, add (+) and rescan buttons
    DrawGlassText(c, g_glMask, L"Games", { GL_PAD, 12, GL_W - 90, 44 }, g_glTitleF, 1.f, DT_LEFT | DT_VCENTER | DT_SINGLELINE, 0.25f, 0.75f);
    wchar_t buf[64];
    if (g_glScanRunning) swprintf(buf, 64, L"Searching for games\u2026");
    else swprintf(buf, 64, L"%d game%ls", static_cast<int>(g_games.size()), g_games.size() == 1 ? L"" : L"s");
    DrawTextOn(c, g_glMask, buf, { GL_PAD + 1, 42, GL_W - 90, 58 }, g_glSmallF, white, 0.55f, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    for (int b = 0; b < 2; ++b) {
        RECT br = b ? GlRefreshRect() : GlAddRect();
        bool hot = g_glHot == (b ? GLH_REFRESH : GLH_ADD);
        GlassBlob(c, static_cast<float>(br.left), static_cast<float>(br.top), static_cast<float>(br.right), static_cast<float>(br.bottom), 17,
                  hot ? 0.5f : 0.f, hot ? 0.16f : 0.09f, 1.f);
        DrawGlassText(c, g_glMask, b ? L"\uE72C" : L"\uE710", br, g_glGlyphF, 1.f, DT_CENTER | DT_VCENTER | DT_SINGLELINE, 0.f, 0.85f);
    }

    // search field (type anywhere while the launcher is open)
    FillRRect(c, GL_PAD, 64, GL_W - GL_PAD, 94, 15, white, 0.07f);
    GlassLight(c, GL_PAD, 64, GL_W - GL_PAD, 94, 15, 0.5f);
    DrawTextOn(c, g_glMask, L"\uE721", { GL_PAD + 6, 64, GL_PAD + 32, 94 }, g_glGlyphF, white, 0.5f, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    DrawTextOn(c, g_glMask, g_glFilter.empty() ? L"Type to search" : g_glFilter.c_str(), { GL_PAD + 34, 64, GL_W - GL_PAD - 12, 94 },
               g_glSearchF, white, g_glFilter.empty() ? 0.4f : 0.95f, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    // the grid
    for (int s = 0; s < GL_COLS * GL_ROWS; ++s) {
        int vi = g_glRow * GL_COLS + s;
        if (vi >= static_cast<int>(g_glView.size())) break;
        Game& g = g_games[g_glView[vi]];
        RECT r = GlTile(s);
        float x0 = static_cast<float>(r.left), y0 = static_cast<float>(r.top);
        if (vi == g_glHot) GlassBlob(c, x0 + 4, y0 + 4, r.right - 4.f, r.bottom - 4.f, 16, 0.25f, 0.10f, 1.f);
        if (!g.iconTried) {
            g.iconTried = true;
            g.icon = IconForPath(g.iconPath.empty() ? g.file : g.iconPath, GL_ICON);
        }
        float ix = x0 + (GL_TW - GL_ICON) / 2.f, iy = y0 + 14;
        if (g.icon.empty()) { // no icon: a glass tile with a controller
            GlassBlob(c, ix, iy, ix + GL_ICON, iy + GL_ICON, 12, 0.6f, 0.1f, 1.f);
            DrawGlassText(c, g_glMask, L"\uE7FC", { static_cast<LONG>(ix), static_cast<LONG>(iy), static_cast<LONG>(ix + GL_ICON), static_cast<LONG>(iy + GL_ICON) },
                          g_glGlyphF, 1.f, DT_CENTER | DT_VCENTER | DT_SINGLELINE, 0.f, 0.85f);
        } else {
            BlitIconSmooth(c, g.icon, GL_ICON, ix, iy, GL_ICON, 1.f);
        }
        if (g.shine) {
            float st = (now - g.shine) / 650.f;
            if (st < 1.f) IconShine(c, g.icon, GL_ICON, ix, iy, GL_ICON, st); else g.shine = 0;
        }
        DrawTextOn(c, g_glMask, g.name.c_str(), { r.left + 6, r.top + 68, r.right - 6, r.top + 88 }, g_glNameF, white, 0.95f,
                   DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        DrawTextOn(c, g_glMask, g.source.c_str(), { r.left + 6, r.top + 87, r.right - 6, r.top + 102 }, g_glSmallF, white, 0.42f,
                   DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    if (g_glView.empty()) {
        const wchar_t* msg = g_glScanRunning ? L"Looking through Steam, Epic, Riot, GOG and game folders\u2026"
                           : g_glScanning ? L"Loading\u2026"
                           : !g_glFilter.empty() ? L"No game matches that" : L"No games yet - press \u27F3 to search or + to add one";
        DrawTextOn(c, g_glMask, msg, { GL_PAD, GL_HEAD + 40, GL_W - GL_PAD, GL_HEAD + 80 }, g_glSearchF, white, 0.6f,
                   DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    // scroll position: a slim glass pill on the right
    int rows = (static_cast<int>(g_glView.size()) + GL_COLS - 1) / GL_COLS;
    if (rows > GL_ROWS) {
        float trackT = GL_HEAD + 6.f, trackB = GL_H - 18.f, len = (trackB - trackT) * GL_ROWS / rows;
        float top = trackT + (trackB - trackT - len) * g_glRow / std::max(1, rows - GL_ROWS);
        GlassBlob(c, GL_W - 9.f, top, GL_W - 5.f, top + len, 2, 0.6f, 0.2f, 1.f);
    }

    POINT dst{ g_glX, g_glY - static_cast<int>((1.f - open) * 8.f) }, src{ 0, 0 };
    SIZE sz{ GL_W, GL_H };
    BLENDFUNCTION bf{ AC_SRC_OVER, 0, static_cast<BYTE>(255.f * open), AC_SRC_ALPHA };
    UpdateLayeredWindow(g_gl, nullptr, &dst, &sz, g_glS.dc, &src, 0, &bf, ULW_ALPHA);
}

void GlKick() { if (g_gl) SetTimer(g_gl, GL_ANIM_TIMER, 15, nullptr); }

void GlClose() {
    if (!g_glVisible || g_glClosing) return;
    g_glClosing = true;
    g_glAnimStart = g_glLastClose = GetTickCount();
    GlKick();
}

// A nice name for a hand-added exe: its "File description" (e.g. "VALORANT"), else
// the product name, else the file name.
// Engine bootstrappers etc. describe themselves generically ("BootstrapPackagedGame").
bool GenericGameName(const std::wstring& s) {
    std::wstring n = NormName(s);
    for (const wchar_t* g : { L"bootstrappackagedgame", L"game", L"launcher", L"application", L"unity", L"unrealengine",
                              L"win64", L"shipping", L"client", L"gamelauncher", L"setup" })
        if (n == g) return true;
    return n.size() < 2;
}

std::wstring FriendlyExeName(const std::wstring& path) {
    std::wstring stem = fs::path(path).stem().wstring();
    if (GenericGameName(stem)) stem = fs::path(path).parent_path().filename().wstring(); // e.g. ...\MyGame\Game.exe
    if (LowerStr(fs::path(path).extension().wstring()) != L".exe") return stem;
    typedef DWORD (WINAPI *PFN_Size)(LPCWSTR, LPDWORD);
    typedef BOOL (WINAPI *PFN_Info)(LPCWSTR, DWORD, DWORD, LPVOID);
    typedef BOOL (WINAPI *PFN_Query)(LPCVOID, LPCWSTR, LPVOID*, PUINT);
    HMODULE v = LoadLibraryW(L"version.dll"); // runtime: no extra link flag needed
    auto size = v ? reinterpret_cast<PFN_Size>(reinterpret_cast<void*>(GetProcAddress(v, "GetFileVersionInfoSizeW"))) : nullptr;
    auto info = v ? reinterpret_cast<PFN_Info>(reinterpret_cast<void*>(GetProcAddress(v, "GetFileVersionInfoW"))) : nullptr;
    auto query = v ? reinterpret_cast<PFN_Query>(reinterpret_cast<void*>(GetProcAddress(v, "VerQueryValueW"))) : nullptr;
    if (!size || !info || !query) return stem;
    DWORD n = size(path.c_str(), nullptr);
    if (!n) return stem;
    std::vector<BYTE> data(n);
    if (!info(path.c_str(), 0, n, data.data())) return stem;
    struct Lang { WORD lang, cp; }* langs = nullptr;
    UINT len = 0;
    if (!query(data.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<LPVOID*>(&langs), &len) || len < sizeof(Lang)) return stem;
    for (const wchar_t* field : { L"FileDescription", L"ProductName" }) {
        wchar_t key[96];
        swprintf(key, 96, L"\\StringFileInfo\\%04x%04x\\%ls", langs[0].lang, langs[0].cp, field);
        wchar_t* val = nullptr;
        if (query(data.data(), key, reinterpret_cast<LPVOID*>(&val), &len) && val && len > 1) {
            std::wstring s(val);
            while (!s.empty() && iswspace(s.back())) s.pop_back();
            if (!s.empty() && !GenericGameName(s)) return s;
        }
    }
    return stem;
}

void GlLaunch(const Game& g);

// "+": pick an exe (or a shortcut) that the scan didn't find. Kept in games_added.txt.
void GlAddGame() {
    typedef BOOL (WINAPI *PFN_GetOpen)(LPOPENFILENAMEW);
    static PFN_GetOpen getOpen = reinterpret_cast<PFN_GetOpen>(
        reinterpret_cast<void*>(GetProcAddress(LoadLibraryW(L"comdlg32.dll"), "GetOpenFileNameW")));
    if (!getOpen) return;
    wchar_t file[MAX_PATH] = L"";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_gl;
    ofn.lpstrFilter = L"Games (*.exe, shortcuts)\0*.exe;*.lnk;*.url\0All files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"Add a game to REICO";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_NODEREFERENCELINKS;
    g_glDialog = true;
    BOOL ok = getOpen(&ofn);
    g_glDialog = false;

    if (ok) {
        Game g{ FriendlyExeName(file), file, L"", file, L"Added" };
        std::wstring n = NormName(g.name);
        std::vector<Game> added = LoadGames(GAMES_ADDED);
        added.erase(std::remove_if(added.begin(), added.end(), [&](const Game& a) { return NormName(a.name) == n; }), added.end());
        added.push_back(g);
        SaveGames(GAMES_ADDED, added);
        std::vector<std::wstring> hidden = LoadHidden(); // adding it back un-removes it
        hidden.erase(std::remove(hidden.begin(), hidden.end(), n), hidden.end());
        std::wstring h;
        for (const auto& x : hidden) h += x + L"\n";
        WriteUtf8(GAMES_HIDDEN, h);

        g.icon = IconForPath(file, GL_ICON);
        g.iconTried = true;
        g_games.erase(std::remove_if(g_games.begin(), g_games.end(), [&](const Game& x) { return NormName(x.name) == n; }), g_games.end());
        g_games.push_back(g);
        std::sort(g_games.begin(), g_games.end(), [](const Game& a, const Game& b) { return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0; });
        g_glFilter.clear();
        GlRebuildView();
        Log(L"Games: added " + g.name);
    }
    ShowWindow(g_gl, SW_SHOW);
    SetForegroundWindow(g_gl);
    GlKick();
}

// Right-click -> remove: an added game is dropped from games_added.txt, a scanned one
// is remembered in games_hidden.txt so a rescan doesn't bring it back.
void GlRemoveGame(int viewIndex) {
    Game g = g_games[g_glView[viewIndex]];
    std::wstring n = NormName(g.name);
    if (g.source == L"Added") {
        std::vector<Game> added = LoadGames(GAMES_ADDED);
        added.erase(std::remove_if(added.begin(), added.end(), [&](const Game& a) { return NormName(a.name) == n; }), added.end());
        SaveGames(GAMES_ADDED, added);
    } else {
        std::vector<std::wstring> hidden = LoadHidden();
        if (std::find(hidden.begin(), hidden.end(), n) == hidden.end()) hidden.push_back(n);
        std::wstring h;
        for (const auto& x : hidden) h += x + L"\n";
        WriteUtf8(GAMES_HIDDEN, h);
    }
    g_games.erase(std::remove_if(g_games.begin(), g_games.end(), [&](const Game& x) { return NormName(x.name) == n; }), g_games.end());
    g_glHot = GLH_NONE;
    GlRebuildView();
    Log(L"Games: removed " + g.name);
}

void GlLaunch(const Game& g) {
    bool isExe = LowerStr(fs::path(g.file).extension().wstring()) == L".exe";
    std::wstring dir = isExe ? fs::path(g.file).parent_path().wstring() : L"";
    AllowSetForegroundWindow(ASFW_ANY);
    ShellExecuteW(nullptr, L"open", g.file.c_str(), g.args.empty() ? nullptr : g.args.c_str(),
                  dir.empty() ? nullptr : dir.c_str(), SW_SHOWNORMAL); // games needing admin get Windows' prompt
    Log(L"Games: launched " + g.name);
}

LRESULT CALLBACK GameLauncherProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_ACTIVATE:
        if (LOWORD(w) == WA_INACTIVE && !g_glDialog) GlClose();
        return 0;
    case WM_TIMER: {
        GlRender();
        bool shining = false;
        for (const auto& g : g_games) shining |= g.shine != 0;
        bool animating = GetTickCount() - g_glAnimStart < 220;
        if (g_glClosing && !animating) {
            ShowWindow(h, SW_HIDE);
            g_glVisible = g_glClosing = false;
            KillTimer(h, GL_ANIM_TIMER);
        } else if (!animating && !shining && !g_glScanning) {
            KillTimer(h, GL_ANIM_TIMER);
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, h, 0 };
        TrackMouseEvent(&tme);
        int hot = GlHit(POINT{ GET_X_LPARAM(l), GET_Y_LPARAM(l) });
        if (hot != g_glHot) {
            g_glHot = hot;
            if (hot >= 0 && hot < static_cast<int>(g_glView.size())) g_games[g_glView[hot]].shine = GetTickCount();
            GlKick();
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        g_glHot = GLH_NONE;
        GlKick();
        return 0;
    case WM_SETCURSOR:
        SetCursor(LoadCursor(nullptr, g_glHot != GLH_NONE ? IDC_HAND : IDC_ARROW));
        return TRUE;
    case WM_LBUTTONDOWN:
        g_glPressed = GlHit(POINT{ GET_X_LPARAM(l), GET_Y_LPARAM(l) });
        return 0;
    case WM_LBUTTONUP: {
        int hit = GlHit(POINT{ GET_X_LPARAM(l), GET_Y_LPARAM(l) });
        if (hit == g_glPressed) {
            if (hit == GLH_REFRESH) { GlStartLoad(true); GlKick(); }
            else if (hit == GLH_ADD) GlAddGame();
            else if (hit >= 0 && hit < static_cast<int>(g_glView.size())) {
                Game g = g_games[g_glView[hit]];
                GlClose();
                GlLaunch(g);
            }
        }
        g_glPressed = GLH_NONE;
        return 0;
    }
    case WM_RBUTTONUP: { // right-click a game: launch / remove
        int hit = GlHit(POINT{ GET_X_LPARAM(l), GET_Y_LPARAM(l) });
        if (hit < 0 || hit >= static_cast<int>(g_glView.size())) return 0;
        HMENU m = CreatePopupMenu();
        AppendMenuW(m, MF_STRING, 1, L"Launch");
        AppendMenuW(m, MF_STRING, 2, L"Remove from launcher");
        POINT pt;
        GetCursorPos(&pt);
        g_glDialog = true;
        int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, h, nullptr);
        g_glDialog = false;
        DestroyMenu(m);
        if (cmd == 1) { Game g = g_games[g_glView[hit]]; GlClose(); GlLaunch(g); }
        else if (cmd == 2) GlRemoveGame(hit);
        GlKick();
        return 0;
    }
    case WM_MOUSEWHEEL: {
        int rows = (static_cast<int>(g_glView.size()) + GL_COLS - 1) / GL_COLS;
        g_glRow = std::max(0, std::min(g_glRow + (GET_WHEEL_DELTA_WPARAM(w) > 0 ? -1 : 1), rows - GL_ROWS));
        g_glHot = GLH_NONE;
        GlKick();
        return 0;
    }
    case WM_KEYDOWN:
        if (w == VK_ESCAPE) {
            if (!g_glFilter.empty()) { g_glFilter.clear(); GlRebuildView(); GlKick(); }
            else GlClose();
        } else if (w == VK_RETURN && !g_glView.empty()) { // Enter: the first match
            Game g = g_games[g_glView.front()];
            GlClose();
            GlLaunch(g);
        }
        return 0;
    case WM_CHAR:
        if (w == VK_BACK) { if (!g_glFilter.empty()) g_glFilter.pop_back(); }
        else if (w >= 32 && g_glFilter.size() < 40) g_glFilter += static_cast<wchar_t>(w);
        else return 0;
        g_glRow = 0;
        GlRebuildView();
        GlKick();
        return 0;
    }
    return DefWindowProcW(h, msg, w, l);
}

void GameLauncherToggle() {
    if (g_glVisible && !g_glClosing) { GlClose(); return; }
    if (GetTickCount() - g_glLastClose < 250) return; // this click is what just closed it
    if (!g_gl) {
        g_glTitleF = InterFont(22, 700);
        g_glNameF = InterFont(12, 600);
        g_glSmallF = InterFont(11, 500);
        g_glSearchF = InterFont(13, 500);
        g_glGlyphF = MakeFont(16, FW_NORMAL, L"Segoe MDL2 Assets");
        WNDCLASSW wc{};
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpfnWndProc = GameLauncherProc;
        wc.lpszClassName = L"REICOGames";
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        RegisterClassW(&wc);
        g_gl = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"REICOGames", L"Games", WS_POPUP,
                               0, 0, 0, 0, nullptr, nullptr, wc.hInstance, nullptr);
        if (!g_gl || !SurfCreate(g_glS, GL_W, GL_H) || !SurfCreate(g_glMask, GL_W, GL_H)) return;
    }
    g_glFilter.clear();
    g_glRow = 0;
    g_glHot = g_glPressed = GLH_NONE;
    GlRebuildView();
    g_glX = g_dockOrigin.x + DOCK_SIDE_X;
    g_glY = g_dockOrigin.y + DOCK_TOP + DOCK_H + 10;
    g_glVisible = true;
    g_glClosing = false;
    g_glAnimStart = GetTickCount();
    GlRender();
    ShowWindow(g_gl, SW_SHOW);
    SetForegroundWindow(g_gl); // keyboard search + close on click-away
    GlKick();
}

// ---- the board (notes button): photos, notes and bucket lists ---------------
//
// A big glass board in the middle of the screen. Ctrl+V pastes a picture as an
// Instax-style photo (white frame, thick bottom edge, a slight tilt, a shadow;
// type to caption it). "+ Note" adds a glass sticky note, "+ List" a bucket list
// (type + Enter adds an item, click the circle to tick it off). Everything can be
// dragged around; right-click deletes. Saved as you go to the C:\REICO\board
// folder (board.txt + one .bmp per photo). It's a normal window (not always-on-top), so
// you can switch to another app to copy something and come back to paste.
// Typing is deliberately simple: type, Backspace, Enter - no caret moving.

enum { BI_PHOTO, BI_NOTE, BI_LIST };
struct BoardEntry { std::wstring text; bool done = false; };
struct BoardItem {
    int kind = BI_NOTE;
    float x = 0, y = 0, rot = 0;      // top-left on the board, tilt in degrees (photos)
    int w = 230, h = 120;             // size on the board (h is laid out from the content)
    std::wstring text;                // note text / photo caption / list title
    std::vector<BoardEntry> entries;  // list items
    std::wstring file;                // photo file name in C:\REICO\board
    std::vector<UINT32> photo;        // photo pixels (opaque BGRA)
    int pw = 0, ph = 0;
    std::vector<UINT32> card;         // rendered photo card (frame + photo + caption), unrotated
    bool cardDirty = true;
};

const wchar_t* BOARD_DIR = L"C:\\REICO\\board";
const UINT_PTR BD_ANIM_TIMER = 1, BD_CARET_TIMER = 2, BD_SAVE_TIMER = 3;
const int BD_HEAD = 64, BD_PHOTO_W = 220;

HWND g_bdWnd = nullptr;
Surface g_bdS, g_bdMask;
std::vector<UINT32> g_bdBase;
std::vector<BoardItem> g_bd;            // drawing order: last = on top
int g_bdW = 0, g_bdH = 0, g_bdX = 0, g_bdY = 0;
int g_bdFocus = -1, g_bdDrag = -1, g_bdHotBtn = -1;
POINT g_bdDragOff{};
bool g_bdDragMoved = false;
std::wstring g_bdPending;               // list item being typed
bool g_bdEditTitle = false;             // typing goes to the focused list's title instead
bool g_bdMoveWin = false;               // dragging the whole board by its header
POINT g_bdMoveOff{};
bool g_bdVisible = false, g_bdClosing = false, g_bdLoaded = false, g_bdCaretOn = true;
DWORD g_bdAnimStart = 0;
HFONT g_bdTitleF = nullptr, g_bdTextF = nullptr, g_bdListTitleF = nullptr, g_bdBtnF = nullptr, g_bdCapF = nullptr,
      g_bdGlyphF = nullptr, g_bdHintF = nullptr;

// -- storage --

std::wstring BdEsc(const std::wstring& s) {
    std::wstring o;
    for (wchar_t c : s) { if (c == L'\\') o += L"\\\\"; else if (c == L'\n') o += L"\\n"; else if (c == L'\t') o += L"\\t"; else o += c; }
    return o;
}
std::wstring BdUnesc(const std::wstring& s) {
    std::wstring o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == L'\\' && i + 1 < s.size()) { wchar_t n = s[++i]; o += n == L'n' ? L'\n' : n == L't' ? L'\t' : n; }
        else o += s[i];
    }
    return o;
}

bool SaveBmp32(const std::wstring& path, const std::vector<UINT32>& px, int w, int h) {
    BITMAPFILEHEADER fh{}; BITMAPINFOHEADER ih{};
    ih.biSize = sizeof(ih); ih.biWidth = w; ih.biHeight = -h; ih.biPlanes = 1; ih.biBitCount = 32;
    fh.bfType = 0x4D42; fh.bfOffBits = sizeof(fh) + sizeof(ih); fh.bfSize = fh.bfOffBits + static_cast<DWORD>(w) * h * 4;
    std::ofstream o(fs::path(path), std::ios::binary);
    o.write(reinterpret_cast<const char*>(&fh), sizeof(fh));
    o.write(reinterpret_cast<const char*>(&ih), sizeof(ih));
    o.write(reinterpret_cast<const char*>(px.data()), static_cast<std::streamsize>(px.size()) * 4);
    return static_cast<bool>(o);
}
bool LoadBmp32(const std::wstring& path, std::vector<UINT32>& px, int& w, int& h) { // only reads what SaveBmp32 writes
    std::ifstream in(fs::path(path), std::ios::binary);
    BITMAPFILEHEADER fh{}; BITMAPINFOHEADER ih{};
    if (!in.read(reinterpret_cast<char*>(&fh), sizeof(fh)) || !in.read(reinterpret_cast<char*>(&ih), sizeof(ih))) return false;
    if (fh.bfType != 0x4D42 || ih.biBitCount != 32 || ih.biHeight >= 0 || ih.biWidth <= 0 || ih.biWidth > 4096 || -ih.biHeight > 4096) return false;
    w = ih.biWidth; h = -ih.biHeight;
    px.resize(static_cast<size_t>(w) * h);
    return static_cast<bool>(in.read(reinterpret_cast<char*>(px.data()), static_cast<std::streamsize>(px.size()) * 4));
}

void BdSave() {
    std::error_code ec;
    fs::create_directories(BOARD_DIR, ec);
    std::wstring t;
    wchar_t num[96];
    for (const auto& it : g_bd) {
        swprintf(num, 96, L"%.0f\t%.0f\t", it.x, it.y);
        if (it.kind == BI_PHOTO) {
            wchar_t r[32]; swprintf(r, 32, L"%.1f", it.rot);
            t += L"P\t" + std::wstring(num) + r + L"\t" + it.file + L"\t" + BdEsc(it.text) + L"\n";
        } else if (it.kind == BI_NOTE) {
            t += L"N\t" + std::wstring(num) + std::to_wstring(it.w) + L"\t" + BdEsc(it.text) + L"\n";
        } else {
            t += L"L\t" + std::wstring(num) + std::to_wstring(it.w) + L"\t" + BdEsc(it.text) + L"\n";
            for (const auto& e : it.entries) t += std::wstring(L"E\t") + (e.done ? L"1" : L"0") + L"\t" + BdEsc(e.text) + L"\n";
        }
    }
    WriteUtf8(L"C:\\REICO\\board\\board.txt", t);
}
void BdSaveSoon() { if (g_bdWnd) SetTimer(g_bdWnd, BD_SAVE_TIMER, 600, nullptr); }

void BdLoad() {
    g_bd.clear();
    std::wstringstream ss(ReadTextFile(L"C:\\REICO\\board\\board.txt"));
    std::wstring line;
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        std::vector<std::wstring> f;
        size_t s = 0;
        for (size_t p; (p = line.find(L'\t', s)) != std::wstring::npos; s = p + 1) f.push_back(line.substr(s, p - s));
        f.push_back(line.substr(s));
        if (f[0] == L"E" && f.size() >= 3 && !g_bd.empty() && g_bd.back().kind == BI_LIST) {
            g_bd.back().entries.push_back({ BdUnesc(f[2]), f[1] == L"1" });
            continue;
        }
        if (f.size() < 5) continue;
        BoardItem it;
        it.x = static_cast<float>(_wtof(f[1].c_str()));
        it.y = static_cast<float>(_wtof(f[2].c_str()));
        if (f[0] == L"P" && f.size() >= 6) {
            it.kind = BI_PHOTO;
            it.rot = static_cast<float>(_wtof(f[3].c_str()));
            it.file = f[4];
            it.text = BdUnesc(f[5]);
            if (!LoadBmp32(std::wstring(BOARD_DIR) + L"\\" + it.file, it.photo, it.pw, it.ph)) continue;
        } else if (f[0] == L"N" || f[0] == L"L") {
            it.kind = f[0] == L"N" ? BI_NOTE : BI_LIST;
            it.w = std::max(160, _wtoi(f[3].c_str()));
            it.text = BdUnesc(f[4]);
        } else continue;
        g_bd.push_back(std::move(it));
    }
    g_bdLoaded = true;
}

// -- pictures: clipboard bitmap or an image file, scaled down to at most 640 px --

std::vector<UINT32> ScaleImage(const std::vector<UINT32>& src, int sw, int sh, int& dw, int& dh, int maxSide) {
    float k = std::min(1.f, static_cast<float>(maxSide) / std::max(sw, sh));
    dw = std::max(1, static_cast<int>(sw * k)); dh = std::max(1, static_cast<int>(sh * k));
    std::vector<UINT32> out(static_cast<size_t>(dw) * dh);
    for (int y = 0; y < dh; ++y) {
        int y0 = y * sh / dh, y1 = std::max(y0 + 1, (y + 1) * sh / dh);
        for (int x = 0; x < dw; ++x) {
            int x0 = x * sw / dw, x1 = std::max(x0 + 1, (x + 1) * sw / dw);
            UINT32 r = 0, g = 0, b = 0, n = 0;
            for (int yy = y0; yy < y1; ++yy)
                for (int xx = x0; xx < x1; ++xx, ++n) {
                    UINT32 p = src[static_cast<size_t>(yy) * sw + xx];
                    r += (p >> 16) & 0xFF; g += (p >> 8) & 0xFF; b += p & 0xFF;
                }
            out[static_cast<size_t>(y) * dw + x] = 0xFF000000 | ((r / n) << 16) | ((g / n) << 8) | (b / n);
        }
    }
    return out;
}

bool ImageFromBitmap(HBITMAP hb, std::vector<UINT32>& px, int& w, int& h) {
    BITMAP bm{};
    if (!GetObject(hb, sizeof(bm), &bm) || bm.bmWidth <= 0 || bm.bmHeight <= 0) return false;
    w = bm.bmWidth; h = bm.bmHeight;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    px.resize(static_cast<size_t>(w) * h);
    HDC dc = GetDC(nullptr);
    int ok = GetDIBits(dc, hb, 0, h, px.data(), &bi, DIB_RGB_COLORS);
    ReleaseDC(nullptr, dc);
    for (auto& p : px) p |= 0xFF000000; // clipboard pictures: opaque
    return ok != 0;
}

bool ImageFromFile(const std::wstring& path, std::vector<UINT32>& px, int& w, int& h) {
    typedef HRESULT (WINAPI *PFN_CoCreate)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID*);
    static PFN_CoCreate coCreate = reinterpret_cast<PFN_CoCreate>(
        reinterpret_cast<void*>(GetProcAddress(LoadLibraryW(L"ole32.dll"), "CoCreateInstance")));
    if (!coCreate) return false;
    IWICImagingFactory* wic = nullptr;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* conv = nullptr;
    UINT uw = 0, uh = 0;
    bool ok = SUCCEEDED(coCreate(np::CLSID_WicFactory, nullptr, CLSCTX_INPROC_SERVER, np::IID_WicFactory, reinterpret_cast<void**>(&wic))) &&
              SUCCEEDED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &dec)) &&
              SUCCEEDED(dec->GetFrame(0, &frame)) && SUCCEEDED(frame->GetSize(&uw, &uh)) && uw && uh && uw <= 16000 && uh <= 16000 &&
              SUCCEEDED(wic->CreateFormatConverter(&conv)) &&
              SUCCEEDED(conv->Initialize(frame, np::FMT_32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom));
    if (ok) {
        w = static_cast<int>(uw); h = static_cast<int>(uh);
        px.resize(static_cast<size_t>(w) * h);
        ok = SUCCEEDED(conv->CopyPixels(nullptr, w * 4, w * h * 4, reinterpret_cast<BYTE*>(px.data())));
        for (auto& p : px) { // transparent parts become white paper
            UINT32 a = p >> 24, inv = 255 - a;
            p = 0xFF000000 | (std::min(255u, ((p >> 16) & 0xFF) + inv) << 16) | (std::min(255u, ((p >> 8) & 0xFF) + inv) << 8) | std::min(255u, (p & 0xFF) + inv);
        }
    }
    for (IUnknown* u : std::initializer_list<IUnknown*>{ conv, frame, dec, wic }) if (u) u->Release();
    return ok;
}

// -- layout + drawing --

int BdTextHeight(const std::wstring& t, HFONT f, int width) {
    RECT r{ 0, 0, width, 0 };
    HGDIOBJ of = SelectObject(g_bdMask.dc, f);
    DrawTextW(g_bdMask.dc, t.empty() ? L" " : t.c_str(), -1, &r, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(g_bdMask.dc, of);
    return r.bottom;
}

void BdLayout(BoardItem& it) {
    if (it.kind == BI_PHOTO) {
        int ph = it.pw > 0 ? std::min(300, std::max(150, BD_PHOTO_W * it.ph / it.pw)) : 200;
        it.w = BD_PHOTO_W + 24;
        it.h = 12 + ph + 50;
    } else if (it.kind == BI_NOTE) {
        it.h = std::max(120, BdTextHeight(it.text, g_bdTextF, it.w - 32) + 36);
    } else {
        it.h = 52 + static_cast<int>(it.entries.size()) * 30 + 38;
    }
}

// cover-fit, bilinear: fills x,y,w,h with the picture, cropping the overflow
void BlitImageCover(Canvas c, const std::vector<UINT32>& src, int sw, int sh, float x0, float y0, float w, float h) {
    float k = std::max(w / sw, h / sh), ox = (sw * k - w) / 2.f, oy = (sh * k - h) / 2.f;
    for (int y = std::max(0, static_cast<int>(y0)); y < std::min(c.h, static_cast<int>(y0 + h)); ++y)
        for (int x = std::max(0, static_cast<int>(x0)); x < std::min(c.w, static_cast<int>(x0 + w)); ++x) {
            float u = (x - x0 + ox + 0.5f) / k - 0.5f, v = (y - y0 + oy + 0.5f) / k - 0.5f;
            int ux = std::max(0, std::min(sw - 2, static_cast<int>(u))), vy = std::max(0, std::min(sh - 2, static_cast<int>(v)));
            float fx = Clamp01(u - ux), fy = Clamp01(v - vy);
            float ch[3] = {};
            for (int s = 0; s < 3; ++s) {
                auto at = [&](int xx, int yy) { return static_cast<float>((src[static_cast<size_t>(yy) * sw + xx] >> (16 - 8 * s)) & 0xFF); };
                ch[s] = at(ux, vy) * (1 - fx) * (1 - fy) + at(ux + 1, vy) * fx * (1 - fy) + at(ux, vy + 1) * (1 - fx) * fy + at(ux + 1, vy + 1) * fx * fy;
            }
            Over(c.px[static_cast<size_t>(y) * c.w + x], ch[0], ch[1], ch[2], 1.f);
        }
}

// The Instax look: off-white frame, thick bottom edge with the caption, rendered once.
void BdRenderCard(BoardItem& it, bool caret) {
    BdLayout(it);
    Surface s, m;
    if (!SurfCreate(s, it.w, it.h) || !SurfCreate(m, it.w, it.h)) return;
    FillRRect(s.cv(), 0.5f, 0.5f, it.w - 0.5f, it.h - 0.5f, 3.f, RGB(250, 249, 244), 1.f);
    int ph = it.h - 12 - 50;
    BlitImageCover(s.cv(), it.photo, it.pw, it.ph, 12.f, 12.f, static_cast<float>(BD_PHOTO_W), static_cast<float>(ph));
    FillRRect(s.cv(), 12.f, 12.f, 12.f + BD_PHOTO_W, 12.f + ph, 0.f, RGB(0, 0, 0), 0.04f); // a touch of print sheen
    std::wstring cap = it.text + (caret ? L"|" : L"");
    DrawTextOn(s.cv(), m, cap.c_str(), { 12, 12 + ph + 4, it.w - 12, it.h - 4 }, g_bdCapF, RGB(55, 55, 60), 0.92f,
               DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    it.card.assign(s.px, s.px + static_cast<size_t>(it.w) * it.h);
    it.cardDirty = false;
    SurfFree(s);
    SurfFree(m);
}

// board point -> the card's own (unrotated) coordinates
void BdToCard(const BoardItem& it, float bx, float by, float& u, float& v) {
    float cx = it.x + it.w / 2.f, cy = it.y + it.h / 2.f, a = -it.rot * 3.14159265f / 180.f;
    float dx = bx - cx, dy = by - cy;
    u = dx * std::cos(a) - dy * std::sin(a) + it.w / 2.f;
    v = dx * std::sin(a) + dy * std::cos(a) + it.h / 2.f;
}

void BdDrawPhoto(Canvas c, BoardItem& it, bool focused) {
    if (it.cardDirty || focused) BdRenderCard(it, focused && g_bdCaretOn);
    if (it.card.empty()) return;
    float cx = it.x + it.w / 2.f, cy = it.y + it.h / 2.f, rad = std::sqrt(static_cast<float>(it.w * it.w + it.h * it.h)) / 2.f + 12.f;
    auto sample = [&](float u, float v, float out[4]) { // bilinear, premultiplied
        out[0] = out[1] = out[2] = out[3] = 0.f;
        if (u < -1 || v < -1 || u > it.w || v > it.h) return;
        int ux = static_cast<int>(std::floor(u - 0.5f)), vy = static_cast<int>(std::floor(v - 0.5f));
        float fx = u - 0.5f - ux, fy = v - 0.5f - vy;
        for (int k = 0; k < 4; ++k) {
            int xx = ux + (k & 1), yy = vy + (k >> 1);
            if (xx < 0 || yy < 0 || xx >= it.w || yy >= it.h) continue;
            float wgt = ((k & 1) ? fx : 1 - fx) * ((k >> 1) ? fy : 1 - fy);
            UINT32 p = it.card[static_cast<size_t>(yy) * it.w + xx];
            out[0] += (p >> 24) * wgt; out[1] += ((p >> 16) & 0xFF) * wgt; out[2] += ((p >> 8) & 0xFF) * wgt; out[3] += (p & 0xFF) * wgt;
        }
    };
    for (int y = std::max(0, static_cast<int>(cy - rad)); y < std::min(c.h, static_cast<int>(cy + rad + 10)); ++y)
        for (int x = std::max(0, static_cast<int>(cx - rad)); x < std::min(c.w, static_cast<int>(cx + rad + 8)); ++x) {
            UINT32& p = c.px[static_cast<size_t>(y) * c.w + x];
            float u, v, s[4];
            BdToCard(it, x + 0.5f - 5.f, y + 0.5f - 8.f, u, v); // soft drop shadow, down-right
            sample(u, v, s);
            if (s[0] > 0.f) Over(p, 0, 0, 0, s[0] / 255.f * 0.32f);
            BdToCard(it, x + 0.5f, y + 0.5f, u, v);
            sample(u, v, s);
            if (s[0] > 0.f) { float a = s[0] / 255.f; Over(p, s[1] / a, s[2] / a, s[3] / a, a); }
        }
}

COLORREF BdVeil() {
    COLORREF tint = RGB(g_glassCfg.tintR, g_glassCfg.tintG, g_glassCfg.tintB);
    return RGB(GetRValue(tint) * 3 / 10 + 14, GetGValue(tint) * 3 / 10 + 14, GetBValue(tint) * 3 / 10 + 18);
}

void BdDrawNote(Canvas c, BoardItem& it, bool focused) {
    const COLORREF white = RGB(255, 255, 255), acc = AccentColour();
    BdLayout(it);
    float x0 = it.x, y0 = it.y, x1 = it.x + it.w, y1 = it.y + it.h;
    FillRRect(c, x0 + 4, y0 + 6, x1 + 4, y1 + 6, 18, RGB(0, 0, 0), 0.22f); // shadow
    FillRRect(c, x0, y0, x1, y1, 18, BdVeil(), 0.85f);
    FillRRect(c, x0, y0, x1, y1, 18, acc, focused ? 0.22f : 0.14f);
    GlassLight(c, x0, y0, x1, y1, 18, focused ? 1.f : 0.8f);
    std::wstring t = it.text + (focused && g_bdCaretOn ? L"|" : L"");
    if (it.text.empty() && !focused) t = L"Click and type\u2026";
    DrawTextOn(c, g_bdMask, t.c_str(), { static_cast<LONG>(x0 + 16), static_cast<LONG>(y0 + 16), static_cast<LONG>(x1 - 16), static_cast<LONG>(y1 - 12) },
               g_bdTextF, white, it.text.empty() && !focused ? 0.45f : 0.95f, DT_LEFT | DT_WORDBREAK);
}

RECT BdListRow(const BoardItem& it, int i) {
    LONG top = static_cast<LONG>(it.y) + 52 + i * 30;
    return { static_cast<LONG>(it.x) + 14, top, static_cast<LONG>(it.x) + it.w - 14, top + 28 };
}

void BdDrawList(Canvas c, BoardItem& it, bool focused) {
    const COLORREF white = RGB(255, 255, 255), acc = AccentColour();
    BdLayout(it);
    float x0 = it.x, y0 = it.y, x1 = it.x + it.w, y1 = it.y + it.h;
    FillRRect(c, x0 + 4, y0 + 6, x1 + 4, y1 + 6, 18, RGB(0, 0, 0), 0.22f);
    FillRRect(c, x0, y0, x1, y1, 18, BdVeil(), 0.88f);
    if (focused) FillRRect(c, x0, y0, x1, y1, 18, acc, 0.12f);
    GlassLight(c, x0, y0, x1, y1, 18, focused ? 1.f : 0.8f);
    bool editingTitle = focused && g_bdEditTitle;
    RECT titleR{ static_cast<LONG>(x0 + 16), static_cast<LONG>(y0 + 12), static_cast<LONG>(x1 - 16), static_cast<LONG>(y0 + 44) };
    if (editingTitle) { // a soft field behind the title while renaming
        FillRRect(c, x0 + 10, y0 + 10, x1 - 10, y0 + 46, 10, RGB(255, 255, 255), 0.08f);
        GlassLight(c, x0 + 10, y0 + 10, x1 - 10, y0 + 46, 10, 0.5f);
    }
    std::wstring title = editingTitle ? it.text + (g_bdCaretOn ? L"|" : L"") : (it.text.empty() ? L"Bucket list" : it.text);
    if (editingTitle && it.text.empty() && !g_bdCaretOn) title = L"Name this list";
    DrawGlassText(c, g_bdMask, title.c_str(), titleR, g_bdListTitleF, editingTitle && it.text.empty() ? 0.5f : 1.f,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, 0.25f, 0.75f);
    for (int i = 0; i < static_cast<int>(it.entries.size()); ++i) {
        const BoardEntry& e = it.entries[i];
        RECT r = BdListRow(it, i);
        float cx = r.left + 11.f, cy = (r.top + r.bottom) / 2.f;
        if (e.done) {
            FillCircle(c, cx, cy, 9.f, acc, 0.95f);
            GlassLight(c, cx - 9, cy - 9, cx + 9, cy + 9, 9, 0.9f);
            DrawTextOn(c, g_bdMask, L"\uE73E", { r.left + 2, r.top, r.left + 21, r.bottom }, g_bdGlyphF, white, 1.f,
                       DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        } else {
            FillCircle(c, cx, cy, 9.f, white, 0.10f);
            GlassLight(c, cx - 9, cy - 9, cx + 9, cy + 9, 9, 0.9f);
        }
        RECT tr{ r.left + 30, r.top, r.right, r.bottom };
        DrawTextOn(c, g_bdMask, e.text.c_str(), tr, g_bdTextF, white, e.done ? 0.45f : 0.95f, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        if (e.done) { // crossed off
            int tw = std::min(static_cast<int>(tr.right - tr.left), TextWidth(e.text.c_str(), g_bdTextF));
            FillRRect(c, tr.left - 2.f, cy - 0.75f, tr.left + tw + 2.f, cy + 0.75f, 0.75f, white, 0.6f);
        }
    }
    RECT add = BdListRow(it, static_cast<int>(it.entries.size()));
    add.top += 4; add.bottom += 4;
    std::wstring line = focused && !g_bdEditTitle ? (L"+  " + g_bdPending + (g_bdCaretOn ? L"|" : L"")) : L"+  Add item";
    DrawTextOn(c, g_bdMask, line.c_str(), { add.left + 2, add.top, add.right, add.bottom }, g_bdTextF, white,
               focused && !g_bdPending.empty() ? 0.95f : 0.45f, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

// header buttons: 0 + Note, 1 + List, 2 Paste photo, 3 close
RECT BdBtn(int i) {
    if (i == 3) return { g_bdW - 52, 16, g_bdW - 18, 50 };
    int x = 150 + i * 132;
    return { x, 16, x + 122, 50 };
}

void BdBuildBase() {
    g_bdBase.assign(static_cast<size_t>(g_bdW) * g_bdH, 0);
    Canvas c{ g_bdBase.data(), g_bdW, g_bdH };
    FillRRect(c, 0.5f, 0.5f, g_bdW - 0.5f, g_bdH - 0.5f, 26, BdVeil(), 0.9f);
    for (int y = BD_HEAD + 20; y < g_bdH - 10; y += 26) // a faint dot grid, like a pinboard
        for (int x = 20; x < g_bdW - 10; x += 26) FillCircle(c, static_cast<float>(x), static_cast<float>(y), 1.2f, RGB(255, 255, 255), 0.07f);
    GlassLight(c, 0.5f, 0.5f, g_bdW - 0.5f, g_bdH - 0.5f, 26, 1.f);
    FillRRect(c, 18, BD_HEAD - 1.f, g_bdW - 18.f, static_cast<float>(BD_HEAD), 0.5f, RGB(255, 255, 255), 0.12f);
    DrawGlassText(c, g_bdMask, L"Board", { 22, 10, 140, 56 }, g_bdTitleF, 1.f, DT_LEFT | DT_VCENTER | DT_SINGLELINE, 0.25f, 0.75f);
    DrawTextOn(c, g_bdMask, L"Ctrl+V pastes a photo  \u00B7  drag to arrange  \u00B7  right-click to delete",
               { 560, 16, g_bdW - 70, 50 }, g_bdHintF, RGB(255, 255, 255), 0.45f, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

void BdRender() {
    if (!g_bdS.px) return;
    DWORD now = GetTickCount();
    float p = Clamp01((now - g_bdAnimStart) / (g_bdClosing ? 140.f : 220.f));
    float open = g_bdClosing ? 1.f - p : EaseOut3(p);
    memcpy(g_bdS.px, g_bdBase.data(), g_bdBase.size() * 4);
    Canvas c = g_bdS.cv();
    const COLORREF white = RGB(255, 255, 255);

    static const wchar_t* labels[] = { L"+  Note", L"+  List", L"Paste photo" };
    for (int i = 0; i < 4; ++i) {
        RECT r = BdBtn(i);
        bool hot = g_bdHotBtn == i;
        GlassBlob(c, static_cast<float>(r.left), static_cast<float>(r.top), static_cast<float>(r.right), static_cast<float>(r.bottom),
                  17, hot ? 0.5f : 0.f, hot ? 0.16f : 0.09f, 1.f);
        if (i < 3) DrawTextOn(c, g_bdMask, labels[i], r, g_bdBtnF, white, 0.95f, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        else DrawGlassText(c, g_bdMask, L"\uE711", r, g_bdGlyphF, 1.f, DT_CENTER | DT_VCENTER | DT_SINGLELINE, 0.f, 0.85f);
    }

    if (g_bd.empty())
        DrawTextOn(c, g_bdMask, L"Your board is empty - paste a photo with Ctrl+V, or add a note or a list above",
                   { 20, g_bdH / 2 - 20, g_bdW - 20, g_bdH / 2 + 20 }, g_bdTextF, white, 0.5f, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    for (int i = 0; i < static_cast<int>(g_bd.size()); ++i) {
        bool f = i == g_bdFocus;
        if (g_bd[i].kind == BI_PHOTO) BdDrawPhoto(c, g_bd[i], f);
        else if (g_bd[i].kind == BI_NOTE) BdDrawNote(c, g_bd[i], f);
        else BdDrawList(c, g_bd[i], f);
    }

    POINT dst{ g_bdX, g_bdY + static_cast<int>((1.f - open) * 14.f) }, src{ 0, 0 };
    SIZE sz{ g_bdW, g_bdH };
    BLENDFUNCTION bf{ AC_SRC_OVER, 0, static_cast<BYTE>(255.f * open), AC_SRC_ALPHA };
    UpdateLayeredWindow(g_bdWnd, nullptr, &dst, &sz, g_bdS.dc, &src, 0, &bf, ULW_ALPHA);
}

// -- editing --

int BdHit(POINT p) { // topmost item under the point
    for (int i = static_cast<int>(g_bd.size()) - 1; i >= 0; --i) {
        const BoardItem& it = g_bd[i];
        if (it.kind == BI_PHOTO) {
            float u, v;
            BdToCard(it, p.x + 0.5f, p.y + 0.5f, u, v);
            if (u >= 0 && v >= 0 && u < it.w && v < it.h) return i;
        } else if (p.x >= it.x && p.y >= it.y && p.x < it.x + it.w && p.y < it.y + it.h) {
            return i;
        }
    }
    return -1;
}

int BdBringToFront(int i) {
    if (i < 0 || i == static_cast<int>(g_bd.size()) - 1) return i;
    BoardItem it = std::move(g_bd[i]);
    g_bd.erase(g_bd.begin() + i);
    g_bd.push_back(std::move(it));
    return static_cast<int>(g_bd.size()) - 1;
}

POINT BdDropPoint(int w, int h) { // where new things land: around the middle, a little scattered
    static int n = 0;
    ++n;
    return { std::max(20, g_bdW / 2 - w / 2 + ((n * 47) % 160) - 80), std::max(BD_HEAD + 12, g_bdH / 2 - h / 2 + ((n * 31) % 120) - 60) };
}

void BdKick() { if (g_bdWnd) SetTimer(g_bdWnd, BD_ANIM_TIMER, 15, nullptr); }

void BdFocus(int i) {
    if (g_bdFocus >= 0 && g_bdFocus < static_cast<int>(g_bd.size())) {
        BoardItem& old = g_bd[g_bdFocus];
        if (old.kind == BI_PHOTO) old.cardDirty = true;
        if (old.kind == BI_LIST && old.text.empty()) old.text = L"Bucket list"; // left the title blank
    }
    if (i != g_bdFocus) { g_bdPending.clear(); g_bdEditTitle = false; }
    g_bdFocus = i;
    g_bdCaretOn = true;
    if (g_bdWnd) {
        if (i >= 0) SetTimer(g_bdWnd, BD_CARET_TIMER, 530, nullptr);
        else KillTimer(g_bdWnd, BD_CARET_TIMER);
    }
}

void BdAddPhoto(std::vector<UINT32> px, int w, int h) {
    int dw = 0, dh = 0;
    std::vector<UINT32> small = ScaleImage(px, w, h, dw, dh, 640);
    std::error_code ec;
    fs::create_directories(BOARD_DIR, ec);
    wchar_t name[64];
    swprintf(name, 64, L"photo_%llu.bmp", static_cast<unsigned long long>(GetTickCount64()));
    if (!SaveBmp32(std::wstring(BOARD_DIR) + L"\\" + name, small, dw, dh)) return;
    BoardItem it;
    it.kind = BI_PHOTO;
    it.file = name;
    it.photo = std::move(small);
    it.pw = dw; it.ph = dh;
    it.rot = static_cast<float>(static_cast<int>(GetTickCount() % 11) - 5); // -5..+5 degrees, like it was tossed on
    BdLayout(it);
    POINT at = BdDropPoint(it.w, it.h);
    it.x = static_cast<float>(at.x); it.y = static_cast<float>(at.y);
    g_bd.push_back(std::move(it));
    BdFocus(static_cast<int>(g_bd.size()) - 1);
    BdSaveSoon();
}

void BdAddNote(const std::wstring& text) {
    BoardItem it;
    it.kind = BI_NOTE;
    it.w = 240;
    it.text = text;
    BdLayout(it);
    POINT at = BdDropPoint(it.w, it.h);
    it.x = static_cast<float>(at.x); it.y = static_cast<float>(at.y);
    g_bd.push_back(std::move(it));
    BdFocus(static_cast<int>(g_bd.size()) - 1);
    BdSaveSoon();
}

void BdAddList() {
    BoardItem it;
    it.kind = BI_LIST;
    it.w = 280;
    BdLayout(it);
    POINT at = BdDropPoint(it.w, it.h);
    it.x = static_cast<float>(at.x); it.y = static_cast<float>(at.y);
    g_bd.push_back(std::move(it));
    BdFocus(static_cast<int>(g_bd.size()) - 1);
    g_bdEditTitle = true; // name it first; Enter moves on to adding items (blank = "Bucket list")
    BdSaveSoon();
}

void BdDelete(int i) {
    if (i < 0 || i >= static_cast<int>(g_bd.size())) return;
    if (g_bd[i].kind == BI_PHOTO) DeleteFileW((std::wstring(BOARD_DIR) + L"\\" + g_bd[i].file).c_str());
    g_bd.erase(g_bd.begin() + i);
    BdFocus(-1);
    BdSaveSoon();
}

// Ctrl+V: a picture (copied image, or an image file copied in Explorer) or text.
void BdPaste() {
    if (!OpenClipboard(g_bdWnd)) return;
    std::vector<UINT32> px;
    int w = 0, h = 0;
    bool image = false;
    if (HANDLE hb = GetClipboardData(CF_BITMAP)) image = ImageFromBitmap(static_cast<HBITMAP>(hb), px, w, h);
    std::wstring file, text;
    if (!image) {
        if (HANDLE hd = GetClipboardData(CF_HDROP)) {
            wchar_t f[MAX_PATH]{};
            if (DragQueryFileW(static_cast<HDROP>(hd), 0, f, MAX_PATH)) file = f;
        }
        if (HANDLE ht = GetClipboardData(CF_UNICODETEXT)) {
            if (const wchar_t* s = static_cast<const wchar_t*>(GlobalLock(ht))) { text = s; GlobalUnlock(ht); }
        }
    }
    CloseClipboard();

    if (!image && !file.empty()) {
        std::wstring ext = LowerStr(fs::path(file).extension().wstring());
        if (ext == L".png" || ext == L".jpg" || ext == L".jpeg" || ext == L".bmp" || ext == L".gif" || ext == L".tif" ||
            ext == L".tiff" || ext == L".webp" || ext == L".heic")
            image = ImageFromFile(file, px, w, h);
    }
    if (image) { BdAddPhoto(std::move(px), w, h); BdKick(); return; }
    if (text.empty()) return;
    text.erase(std::remove(text.begin(), text.end(), L'\r'), text.end());
    if (g_bdFocus >= 0 && g_bd[g_bdFocus].kind == BI_NOTE) g_bd[g_bdFocus].text += text;
    else if (g_bdFocus >= 0 && g_bd[g_bdFocus].kind == BI_LIST) {
        std::wstringstream ss(text);
        std::wstring line;
        while (std::getline(ss, line)) if (!line.empty()) g_bd[g_bdFocus].entries.push_back({ line, false }); // one item per line
    } else if (g_bdFocus >= 0 && g_bd[g_bdFocus].kind == BI_PHOTO) {
        g_bd[g_bdFocus].text += text.substr(0, 60 - std::min<size_t>(60, g_bd[g_bdFocus].text.size()));
        g_bd[g_bdFocus].cardDirty = true;
    } else BdAddNote(text);
    BdSaveSoon();
    BdKick();
}

void BdType(wchar_t ch) {
    if (g_bdFocus < 0 || g_bdFocus >= static_cast<int>(g_bd.size())) return;
    BoardItem& it = g_bd[g_bdFocus];
    bool back = ch == VK_BACK, enter = ch == VK_RETURN;
    if (!back && !enter && ch < 32) return;
    if (it.kind == BI_NOTE) {
        if (back) { if (!it.text.empty()) it.text.pop_back(); }
        else it.text += enter ? L'\n' : ch;
    } else if (it.kind == BI_PHOTO) {
        if (back) { if (!it.text.empty()) it.text.pop_back(); }
        else if (!enter && it.text.size() < 60) it.text += ch;
        it.cardDirty = true;
    } else if (g_bdEditTitle) { // renaming the list
        if (enter) { g_bdEditTitle = false; if (it.text.empty()) it.text = L"Bucket list"; }
        else if (back) { if (!it.text.empty()) it.text.pop_back(); }
        else if (it.text.size() < 40) it.text += ch;
    } else {
        if (enter) { if (!g_bdPending.empty()) { it.entries.push_back({ g_bdPending, false }); g_bdPending.clear(); } }
        else if (back) { if (!g_bdPending.empty()) g_bdPending.pop_back(); }
        else if (g_bdPending.size() < 120) g_bdPending += ch;
    }
    g_bdCaretOn = true;
    BdSaveSoon();
    BdKick();
}

void BdClose() {
    if (!g_bdVisible || g_bdClosing) return;
    if (g_bdFocus >= 0 && g_bdFocus < static_cast<int>(g_bd.size()) && g_bd[g_bdFocus].kind == BI_LIST && !g_bdPending.empty()) {
        g_bd[g_bdFocus].entries.push_back({ g_bdPending, false }); // don't lose a half-typed item
        g_bdPending.clear();
    }
    BdFocus(-1);
    BdSave();
    g_bdClosing = true;
    g_bdAnimStart = GetTickCount();
    BdKick();
}

LRESULT CALLBACK BoardProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_TIMER:
        if (w == BD_SAVE_TIMER) { KillTimer(h, BD_SAVE_TIMER); BdSave(); return 0; }
        if (w == BD_CARET_TIMER) { g_bdCaretOn = !g_bdCaretOn; BdRender(); return 0; }
        BdRender();
        if (GetTickCount() - g_bdAnimStart >= 240) {
            KillTimer(h, BD_ANIM_TIMER);
            if (g_bdClosing) { ShowWindow(h, SW_HIDE); g_bdVisible = g_bdClosing = false; }
        }
        return 0;

    case WM_MOUSEMOVE: {
        POINT p{ GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        if (g_bdMoveWin) {
            POINT sp;
            GetCursorPos(&sp);
            g_bdX = sp.x - g_bdMoveOff.x;
            g_bdY = sp.y - g_bdMoveOff.y;
            BdRender();
            return 0;
        }
        if (g_bdDrag >= 0) {
            BoardItem& it = g_bd[g_bdDrag];
            it.x = static_cast<float>(std::max(-it.w / 2, std::min(g_bdW - it.w / 2, static_cast<int>(p.x - g_bdDragOff.x))));
            it.y = static_cast<float>(std::max(BD_HEAD - 10, std::min(g_bdH - 30, static_cast<int>(p.y - g_bdDragOff.y))));
            g_bdDragMoved = true;
            BdRender();
            return 0;
        }
        int hot = -1;
        for (int i = 0; i < 4; ++i) { RECT r = BdBtn(i); if (PtInRect(&r, p)) hot = i; }
        if (hot != g_bdHotBtn) { g_bdHotBtn = hot; BdRender(); }
        TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, h, 0 };
        TrackMouseEvent(&tme);
        return 0;
    }
    case WM_MOUSELEAVE:
        if (g_bdHotBtn >= 0) { g_bdHotBtn = -1; BdRender(); }
        return 0;

    case WM_LBUTTONDOWN: {
        POINT p{ GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        for (int i = 0; i < 4; ++i) {
            RECT r = BdBtn(i);
            if (!PtInRect(&r, p)) continue;
            if (i == 0) BdAddNote(L"");
            else if (i == 1) BdAddList();
            else if (i == 2) BdPaste();
            else BdClose();
            BdRender();
            return 0;
        }
        if (p.y < BD_HEAD) { // the header bar moves the whole board
            POINT sp;
            GetCursorPos(&sp);
            g_bdMoveWin = true;
            g_bdMoveOff = { sp.x - g_bdX, sp.y - g_bdY };
            SetCapture(h);
            return 0;
        }
        int hit = BdHit(p);
        if (hit < 0) { BdFocus(-1); BdRender(); return 0; }
        // a list's tick circle: toggle without dragging
        if (g_bd[hit].kind == BI_LIST) {
            for (int i = 0; i < static_cast<int>(g_bd[hit].entries.size()); ++i) {
                RECT r = BdListRow(g_bd[hit], i);
                if (p.x >= r.left && p.x < r.left + 24 && p.y >= r.top && p.y < r.bottom) {
                    g_bd[hit].entries[i].done = !g_bd[hit].entries[i].done;
                    BdFocus(BdBringToFront(hit));
                    BdSaveSoon();
                    BdRender();
                    return 0;
                }
            }
        }
        hit = BdBringToFront(hit);
        BdFocus(hit);
        if (g_bd[hit].kind == BI_LIST) // clicking a list's title renames it; anywhere else adds items
            g_bdEditTitle = p.y < g_bd[hit].y + 46;
        g_bdDrag = hit;
        g_bdDragMoved = false;
        g_bdDragOff = { p.x - static_cast<LONG>(g_bd[hit].x), p.y - static_cast<LONG>(g_bd[hit].y) };
        SetCapture(h);
        BdRender();
        return 0;
    }
    case WM_LBUTTONUP:
        if (g_bdMoveWin) { ReleaseCapture(); g_bdMoveWin = false; }
        if (g_bdDrag >= 0) { ReleaseCapture(); g_bdDrag = -1; if (g_bdDragMoved) BdSaveSoon(); }
        return 0;

    case WM_RBUTTONUP: {
        POINT p{ GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        int hit = BdHit(p);
        if (hit < 0) return 0;
        int entry = -1;
        if (g_bd[hit].kind == BI_LIST)
            for (int i = 0; i < static_cast<int>(g_bd[hit].entries.size()); ++i) { RECT r = BdListRow(g_bd[hit], i); if (PtInRect(&r, p)) entry = i; }
        HMENU m = CreatePopupMenu();
        if (entry >= 0) AppendMenuW(m, MF_STRING, 1, L"Remove this item");
        AppendMenuW(m, MF_STRING, 2, g_bd[hit].kind == BI_PHOTO ? L"Delete photo" : g_bd[hit].kind == BI_NOTE ? L"Delete note" : L"Delete list");
        int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, g_bdX + p.x, g_bdY + p.y, 0, h, nullptr);
        DestroyMenu(m);
        if (cmd == 1) { g_bd[hit].entries.erase(g_bd[hit].entries.begin() + entry); BdSaveSoon(); }
        else if (cmd == 2) BdDelete(hit);
        BdRender();
        return 0;
    }

    case WM_KEYDOWN:
        if (w == 'V' && (GetKeyState(VK_CONTROL) & 0x8000)) { BdPaste(); return 0; }
        if (w == VK_ESCAPE) { if (g_bdFocus >= 0) { BdFocus(-1); BdRender(); } else BdClose(); }
        return 0;
    case WM_CHAR:
        if (w == 22) return 0; // Ctrl+V arrives here too as ^V
        BdType(static_cast<wchar_t>(w));
        return 0;
    case WM_SETCURSOR:
        SetCursor(LoadCursor(nullptr, IDC_ARROW));
        return TRUE;
    }
    return DefWindowProcW(h, msg, w, l);
}

void BoardToggle() {
    if (g_bdVisible && !g_bdClosing) {
        if (GetForegroundWindow() == g_bdWnd) BdClose();
        else SetForegroundWindow(g_bdWnd); // open but behind something: bring it back
        return;
    }
    if (!g_bdWnd) {
        typedef HRESULT (WINAPI *PFN_CoInitEx)(LPVOID, DWORD); // WIC, for pasted image files
        if (auto init = reinterpret_cast<PFN_CoInitEx>(reinterpret_cast<void*>(GetProcAddress(LoadLibraryW(L"ole32.dll"), "CoInitializeEx"))))
            init(nullptr, COINIT_APARTMENTTHREADED);
        g_bdTitleF = InterFont(24, 700);
        g_bdTextF = InterFont(14, 500);
        g_bdListTitleF = InterFont(17, 700);
        g_bdBtnF = InterFont(13, 600);
        g_bdCapF = InterFont(15, 500);
        g_bdHintF = InterFont(12, 500);
        g_bdGlyphF = MakeFont(13, FW_NORMAL, L"Segoe MDL2 Assets");
        WNDCLASSW wc{};
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpfnWndProc = BoardProc;
        wc.lpszClassName = L"REICOBoard";
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        RegisterClassW(&wc);
        g_bdWnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW, L"REICOBoard", L"Board", WS_POPUP,
                                  0, 0, 0, 0, nullptr, nullptr, wc.hInstance, nullptr);
        if (!g_bdWnd) return;
    }
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY), &mi);
    RECT wa = mi.rcWork;
    g_bdW = std::min(1280, static_cast<int>(wa.right - wa.left) - 120);
    g_bdH = std::min(800, static_cast<int>(wa.bottom - wa.top) - 60);
    g_bdX = (wa.left + wa.right) / 2 - g_bdW / 2;
    g_bdY = (wa.top + wa.bottom) / 2 - g_bdH / 2;
    if (!SurfCreate(g_bdS, g_bdW, g_bdH) || !SurfCreate(g_bdMask, g_bdW, g_bdH)) return;
    if (!g_bdLoaded) BdLoad();
    for (auto& it : g_bd) { it.cardDirty = true; BdLayout(it); }
    BdBuildBase();
    BdFocus(-1);
    g_bdVisible = true;
    g_bdClosing = false;
    g_bdAnimStart = GetTickCount();
    BdRender();
    ShowWindow(g_bdWnd, SW_SHOW);
    SetForegroundWindow(g_bdWnd);
    BdKick();
}

void BoardShutdown() {
    if (g_bdVisible) BdSave();
    if (g_bdWnd) { DestroyWindow(g_bdWnd); g_bdWnd = nullptr; }
    SurfFree(g_bdS);
    SurfFree(g_bdMask);
    for (HFONT* f : { &g_bdTitleF, &g_bdTextF, &g_bdListTitleF, &g_bdBtnF, &g_bdCapF, &g_bdGlyphF, &g_bdHintF })
        if (*f) { DeleteObject(*f); *f = nullptr; }
}

// ---- input -----------------------------------------------------------------

// ---- download manager: fast multi-connection downloads from the browser -------
//
// The REICO Downloads extension (C:\REICO\extension) offers every browser download to
// REICO through native messaging: Chrome/Edge/Brave start "REICO.exe chrome-extension://..."
// with the download as JSON on stdin. That short-lived process (DlHostMain) hands it to
// the running REICO (WM_COPYDATA to the tray window) and answers {"ok":true} - REICO took
// it and the browser cancels its own download - or {"ok":false}, and the browser
// downloads it as usual (REICO not running, download manager turned off).
//
// What makes it faster than the browser:
//  - several connections at once (HTTP byte ranges): servers and CDNs often cap the speed
//    of each connection, and one TCP connection rarely fills a fast or far-away link;
//  - dynamic splitting, like IDM: a connection that finishes its part takes over the
//    second half of the biggest part still left, so there's no slow tail at the end;
//  - it starts downloading the moment it's caught, while the "where to save" card waits;
//  - a part that stalls or drops is retried from the exact byte it stopped at, a server
//    that refuses extra connections just gets fewer, and connections are kept alive;
//  - the file is sparse while downloading, so writing far into it doesn't make Windows
//    zero-fill everything before that point first.
// The file downloads as "<name>.reico-part" and is deleted automatically if REICO is
// closed or the download fails (delete-on-close), so nothing half-done is left behind.

const ULONG_PTR DL_COPYDATA_ID = 0x52454443;     // 'REDC'
const wchar_t* DL_EXT_ID = L"hnocgodkmndlcgkpebfpaibkhdocnijc"; // fixed by the "key" in extension/manifest.json
const wchar_t* DL_HOST_JSON = L"C:\\REICO\\reico_host.json";
const wchar_t* DL_DIR_TXT = L"C:\\REICO\\downloads.txt";
const int DL_CONNS = 8;                          // connections per download
const long long DL_MIN_SPLIT = 1 << 20;          // never split a part below 2 x 1 MB
enum { DS_RUN, DS_DONE, DS_FAIL, DS_CANCEL };

struct DlSeg { long long start, pos, end, reqEnd; bool owned; }; // [pos, end) still to fetch; reqEnd = end of its open request
struct Download {
    std::wstring url, referrer, cookies, ua;      // the request (worker threads)
    std::wstring host, object;                    // cracked url
    INTERNET_PORT port = 0;
    bool https = false;
    HINTERNET session = nullptr, conn = nullptr;
    CRITICAL_SECTION cs;                          // guards name, part, error, segs, file
    std::wstring name, part, error;
    std::vector<DlSeg> segs;
    HANDLE file = INVALID_HANDLE_VALUE;
    std::atomic<long long> size{ 0 }, got{ 0 };
    std::atomic<int> state{ DS_RUN }, threads{ 0 }, conns{ 0 };
    std::atomic<bool> ranges{ false };
    std::atomic<DWORD> doneAt{ 0 };
    // UI thread only
    std::wstring dir, partDir, final;             // chosen folder, folder of the .reico-part, finished file
    bool confirmed = false, moving = false;
    std::atomic<bool> moved{ false };             // set by DlMover; `final` is valid once it's true
    DWORD born = 0, savedAt = 0;
    DWORD sampleT[32] = {}; long long sampleB[32] = {}; int samples = 0;
    double speed = 0;
};

HWND g_dlWnd = nullptr;
Surface g_dlS, g_dlMask;
std::vector<Download*> g_dls, g_dlGone;  // cards on screen (oldest first) / dismissed, freed once their threads stop
std::wstring g_dlDir;                    // default folder (downloads.txt, else the user's Downloads)
HFONT g_dlNameF = nullptr, g_dlSmallF = nullptr, g_dlBtnF = nullptr, g_dlGlyphF = nullptr;
enum { DA_GO = 1, DA_CANCEL, DA_FOLDER, DA_OPEN, DA_SHOW, DA_CLOSE, DA_RETRY };
struct DlHit { RECT r; Download* d; int act; };
std::vector<DlHit> g_dlHits;
DlHit g_dlHot{}, g_dlPressed{};
bool g_dlHover = false;
POINT g_dlPos{};
const UINT_PTR DL_TIMER = 1;
const int DL_W = 430, DL_ASK_H = 170, DL_CARD_H = 104, DL_GAP = 10, DL_MAX_CARDS = 5;

// -- small helpers --

std::wstring DlWide(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
    return w;
}

void DlPutUtf8(std::string& out, unsigned u) {
    if (u < 0x80) out += static_cast<char>(u);
    else if (u < 0x800) { out += static_cast<char>(0xC0 | (u >> 6)); out += static_cast<char>(0x80 | (u & 63)); }
    else if (u < 0x10000) { out += static_cast<char>(0xE0 | (u >> 12)); out += static_cast<char>(0x80 | ((u >> 6) & 63)); out += static_cast<char>(0x80 | (u & 63)); }
    else { out += static_cast<char>(0xF0 | (u >> 18)); out += static_cast<char>(0x80 | ((u >> 12) & 63));
           out += static_cast<char>(0x80 | ((u >> 6) & 63)); out += static_cast<char>(0x80 | (u & 63)); }
}

// A string field of the flat JSON object the extension sends.
std::wstring DlJsonStr(const std::string& j, const char* key) {
    std::string pat = std::string("\"") + key + "\"";
    size_t p = j.find(pat);
    if (p == std::string::npos) return L"";
    p = j.find_first_not_of(" \t\r\n", p + pat.size());
    if (p == std::string::npos || j[p] != ':') return L"";
    p = j.find_first_not_of(" \t\r\n", p + 1);
    if (p == std::string::npos || j[p] != '"') return L"";
    std::string out;
    for (++p; p < j.size() && j[p] != '"'; ++p) {
        if (j[p] != '\\') { out += j[p]; continue; }
        if (++p >= j.size()) break;
        switch (j[p]) {
        case 'n': out += '\n'; break;
        case 't': out += '\t'; break;
        case 'r': out += '\r'; break;
        case 'b': case 'f': break;
        case 'u': {
            if (p + 4 >= j.size()) return L"";
            unsigned u = std::strtoul(j.substr(p + 1, 4).c_str(), nullptr, 16);
            p += 4;
            if (u >= 0xD800 && u < 0xDC00 && p + 6 < j.size() && j[p + 1] == '\\' && j[p + 2] == 'u') { // surrogate pair
                unsigned lo = std::strtoul(j.substr(p + 3, 4).c_str(), nullptr, 16);
                if (lo >= 0xDC00 && lo < 0xE000) { u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00); p += 6; }
            }
            DlPutUtf8(out, u);
            break;
        }
        default: out += j[p];
        }
    }
    return DlWide(out);
}

long long DlJsonNum(const std::string& j, const char* key) {
    size_t p = j.find(std::string("\"") + key + "\"");
    if (p == std::string::npos || (p = j.find(':', p)) == std::string::npos) return 0;
    return std::max(0LL, std::atoll(j.c_str() + p + 1));
}

// Header values come from the browser: no control characters (no header injection).
std::wstring DlHeaderSafe(std::wstring s) {
    s.erase(std::remove_if(s.begin(), s.end(), [](wchar_t c) { return c < 32; }), s.end());
    return s.substr(0, 16384);
}

// "%E2%9C%93.zip" -> "✓.zip"
std::wstring DlPercentDecode(const std::wstring& s) {
    std::string b;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == L'%' && i + 2 < s.size() && iswxdigit(s[i + 1]) && iswxdigit(s[i + 2])) {
            b += static_cast<char>(std::wcstoul(s.substr(i + 1, 2).c_str(), nullptr, 16));
            i += 2;
        } else {
            DlPutUtf8(b, s[i]);
        }
    }
    return DlWide(b);
}

std::wstring DlCleanName(std::wstring n) {
    for (auto& c : n) if (c < 32 || wcschr(L"\\/:*?\"<>|", c)) c = L'_';
    while (!n.empty() && (n.back() == L'.' || n.back() == L' ')) n.pop_back();
    while (!n.empty() && n.front() == L' ') n.erase(0, 1);
    if (n.size() > 150) {
        std::wstring ext = fs::path(n).extension().wstring();
        if (ext.size() > 12) ext.clear();
        n = n.substr(0, 150 - ext.size()) + ext;
    }
    return n.empty() ? L"download" : n;
}

// dir\name, or dir\name (2).ext ... if that's taken
std::wstring DlUnique(const std::wstring& dir, const std::wstring& name, const wchar_t* suffix = L"") {
    fs::path p(name);
    std::wstring stem = p.stem().wstring(), ext = p.extension().wstring();
    for (int i = 1; i < 1000; ++i) {
        std::wstring f = dir + L"\\" + (i == 1 ? name : stem + L" (" + std::to_wstring(i) + L")" + ext) + suffix;
        if (GetFileAttributesW(f.c_str()) == INVALID_FILE_ATTRIBUTES) return f;
    }
    return dir + L"\\" + name + suffix;
}

std::wstring DlBytes(double b) {
    wchar_t s[32];
    if (b < 1024) swprintf(s, 32, L"%.0f B", b);
    else if (b < 1024 * 1024) swprintf(s, 32, L"%.0f KB", b / 1024);
    else if (b < 1024.0 * 1024 * 1024) swprintf(s, 32, L"%.1f MB", b / (1024 * 1024));
    else swprintf(s, 32, L"%.2f GB", b / (1024.0 * 1024 * 1024));
    return s;
}

std::wstring DlTime(long long sec) {
    wchar_t s[32];
    if (sec < 60) swprintf(s, 32, L"%lld s", sec);
    else if (sec < 3600) swprintf(s, 32, L"%lld min %lld s", sec / 60, sec % 60);
    else swprintf(s, 32, L"%lld h %lld min", sec / 3600, sec % 3600 / 60);
    return s;
}

// -- the default folder --

void DlLoadDir() {
    std::wstring t = ReadTextFile(DL_DIR_TXT);
    while (!t.empty() && iswspace(t.back())) t.pop_back();
    if (!t.empty() && GetFileAttributesW(t.c_str()) != INVALID_FILE_ATTRIBUTES) { g_dlDir = t; return; }
    static const GUID kDownloads = { 0x374DE290, 0x123F, 0x4565, { 0x91, 0x64, 0x39, 0xC4, 0x92, 0x5E, 0x46, 0x7B } };
    typedef void (WINAPI *PFN_Free)(LPVOID);
    auto coFree = reinterpret_cast<PFN_Free>(reinterpret_cast<void*>(GetProcAddress(LoadLibraryW(L"ole32.dll"), "CoTaskMemFree")));
    PWSTR p = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(kDownloads, 0, nullptr, &p)) && p) {
        g_dlDir = p;
        if (coFree) coFree(p);
    } else {
        wchar_t prof[MAX_PATH];
        g_dlDir = GetEnvironmentVariableW(L"USERPROFILE", prof, MAX_PATH) ? std::wstring(prof) + L"\\Downloads" : L"C:\\";
    }
}

bool DlPickFolder(std::wstring& dir) {
    static const CLSID kFileOpenDialog = { 0xDC1C5A9C, 0xE88A, 0x4DDE, { 0xA5, 0xA1, 0x60, 0xF8, 0x2A, 0x20, 0xAE, 0xF7 } };
    static const IID kIFileOpenDialog = { 0xD57C7288, 0xD4AD, 0x4768, { 0xBE, 0x02, 0x9D, 0x96, 0x95, 0x32, 0xD9, 0x60 } };
    static const IID kIShellItem = { 0x43826D1E, 0xE718, 0x42EE, { 0xBC, 0x55, 0xA1, 0xE2, 0x61, 0xC3, 0x7B, 0xFE } };
    typedef HRESULT (WINAPI *PFN_CoInitEx)(LPVOID, DWORD);
    typedef HRESULT (WINAPI *PFN_CoCreate)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID*);
    typedef void (WINAPI *PFN_Free)(LPVOID);
    HMODULE ole = LoadLibraryW(L"ole32.dll");
    auto init = reinterpret_cast<PFN_CoInitEx>(reinterpret_cast<void*>(GetProcAddress(ole, "CoInitializeEx")));
    auto coCreate = reinterpret_cast<PFN_CoCreate>(reinterpret_cast<void*>(GetProcAddress(ole, "CoCreateInstance")));
    auto coFree = reinterpret_cast<PFN_Free>(reinterpret_cast<void*>(GetProcAddress(ole, "CoTaskMemFree")));
    if (!init || !coCreate || !coFree) return false;
    init(nullptr, COINIT_APARTMENTTHREADED);
    IFileOpenDialog* fd = nullptr;
    if (FAILED(coCreate(kFileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, kIFileOpenDialog, reinterpret_cast<void**>(&fd)))) return false;
    DWORD o = 0;
    fd->GetOptions(&o);
    fd->SetOptions(o | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    fd->SetTitle(L"Save the download to");
    IShellItem* start = nullptr;
    if (SUCCEEDED(SHCreateItemFromParsingName(dir.c_str(), nullptr, kIShellItem, reinterpret_cast<void**>(&start)))) {
        fd->SetFolder(start);
        start->Release();
    }
    bool ok = false;
    IShellItem* it = nullptr;
    if (SUCCEEDED(fd->Show(g_dlWnd)) && SUCCEEDED(fd->GetResult(&it))) {
        PWSTR p = nullptr;
        if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) { dir = p; coFree(p); ok = true; }
        it->Release();
    }
    fd->Release();
    return ok;
}

// -- the engine (worker threads; nothing here touches the UI) --

void DlFail(Download* d, const std::wstring& why) {
    EnterCriticalSection(&d->cs);
    if (d->state == DS_RUN) { d->error = why; d->state = DS_FAIL; }
    LeaveCriticalSection(&d->cs);
}

bool DlCrack(Download* d) {
    URL_COMPONENTS u{};
    u.dwStructSize = sizeof(u);
    u.dwHostNameLength = u.dwUrlPathLength = u.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(d->url.c_str(), 0, 0, &u) || !u.dwHostNameLength) return false;
    std::wstring host(u.lpszHostName, u.dwHostNameLength);
    std::wstring object = std::wstring(u.lpszUrlPath, u.dwUrlPathLength) + std::wstring(u.lpszExtraInfo, u.dwExtraInfoLength);
    if (host != d->host || u.nPort != d->port || !d->conn) {
        if (d->conn) WinHttpCloseHandle(d->conn);
        d->conn = WinHttpConnect(d->session, host.c_str(), u.nPort, 0);
    }
    d->host = host;
    d->object = object.empty() ? L"/" : object;
    d->port = u.nPort;
    d->https = u.nScheme == INTERNET_SCHEME_HTTPS;
    return d->conn != nullptr;
}

std::wstring DlQuery(HINTERNET r, DWORD what) {
    DWORD sz = 0;
    WinHttpQueryHeaders(r, what, WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER, &sz, WINHTTP_NO_HEADER_INDEX);
    if (!sz) return L"";
    std::wstring s(sz / sizeof(wchar_t) + 1, L'\0');
    if (!WinHttpQueryHeaders(r, what, WINHTTP_HEADER_NAME_BY_INDEX, &s[0], &sz, WINHTTP_NO_HEADER_INDEX)) return L"";
    s.resize(sz / sizeof(wchar_t));
    return s;
}

// GET bytes [from, to] (to < 0: to the end). Returns the answered request, status in `status`.
HINTERNET DlRequest(Download* d, long long from, long long to, DWORD& status) {
    status = 0;
    HINTERNET r = WinHttpOpenRequest(d->conn, L"GET", d->object.c_str(), nullptr, WINHTTP_NO_REFERER,
                                     WINHTTP_DEFAULT_ACCEPT_TYPES, d->https ? WINHTTP_FLAG_SECURE : 0);
    if (!r) return nullptr;
    std::wstring hdr = L"Accept-Encoding: identity\r\nRange: bytes=" + std::to_wstring(from) + L"-" +
                       (to >= 0 ? std::to_wstring(to) : L"") + L"\r\n";
    if (!d->referrer.empty()) hdr += L"Referer: " + d->referrer + L"\r\n";
    if (!d->cookies.empty()) hdr += L"Cookie: " + d->cookies + L"\r\n";
    DWORD sz = sizeof(status);
    if (WinHttpSendRequest(r, hdr.c_str(), static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(r, nullptr) &&
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz,
                            WINHTTP_NO_HEADER_INDEX))
        return r;
    WinHttpCloseHandle(r);
    return nullptr;
}

// Reads segment i from request r until the segment is complete (true) or the stream fails.
bool DlPump(Download* d, int i, HINTERNET r, std::vector<char>& buf) {
    for (;;) {
        if (d->state != DS_RUN) return false;
        DWORD n = 0;
        if (!WinHttpReadData(r, buf.data(), static_cast<DWORD>(buf.size()), &n)) return false;
        EnterCriticalSection(&d->cs);
        DlSeg& s = d->segs[i];
        if (n == 0 && s.end == LLONG_MAX) { s.end = s.pos; d->size = s.pos; } // size unknown: the stream's end is the file's end
        long long off = s.pos, take = std::min<long long>(n, s.end - s.pos); // the end may have moved: another connection took the rest
        s.pos += take;
        bool fin = s.pos >= s.end;
        LeaveCriticalSection(&d->cs);
        if (take > 0) {
            OVERLAPPED ov{};
            ov.Offset = static_cast<DWORD>(off);
            ov.OffsetHigh = static_cast<DWORD>(off >> 32);
            DWORD wr = 0;
            if (!WriteFile(d->file, buf.data(), static_cast<DWORD>(take), &wr, &ov) || wr != take) {
                DlFail(d, L"couldn't write the file - is the disk full?");
                return false;
            }
            d->got += take;
        }
        if (fin) return true;
        if (n == 0) return false; // the server closed early
    }
}

// A free connection picks its next part: a part nobody is fetching, else the second half
// of the biggest part still left. -1 = nothing worth taking.
int DlClaim(Download* d) {
    if (!d->ranges) return -1;
    EnterCriticalSection(&d->cs);
    int best = -1;
    long long bestLeft = 0;
    for (int k = 0; k < static_cast<int>(d->segs.size()); ++k) {
        DlSeg& s = d->segs[k];
        if (!s.owned && s.pos < s.end) { s.owned = true; LeaveCriticalSection(&d->cs); return k; }
        if (s.owned && s.end - s.pos > bestLeft) { bestLeft = s.end - s.pos; best = k; }
    }
    int got = -1;
    if (best >= 0 && bestLeft >= 2 * DL_MIN_SPLIT) {
        long long mid = d->segs[best].pos + bestLeft / 2, end = d->segs[best].end;
        d->segs[best].end = mid;
        d->segs.push_back({ mid, mid, end, end, true });
        got = static_cast<int>(d->segs.size()) - 1;
    }
    LeaveCriticalSection(&d->cs);
    return got;
}

// A connection gives up its part. If it got nothing yet and the connection before it is
// still streaming those bytes (its request reaches that far), they go back to it; else
// the part waits for the next free connection.
void DlRelease(Download* d, int i) {
    EnterCriticalSection(&d->cs);
    DlSeg& p = d->segs[i];
    p.owned = false;
    for (auto& o : d->segs)
        if (&o != &p && o.owned && o.end == p.pos && o.reqEnd >= p.end) { o.end = p.end; p.end = p.pos; break; }
    LeaveCriticalSection(&d->cs);
}

// One connection's life: fetch its part, then keep taking more until nothing is left.
void DlWorkLoop(Download* d, int i, HINTERNET r) {
    std::vector<char> buf(256 * 1024);
    int fails = 0;
    while (d->state == DS_RUN) {
        if (i < 0 && (i = DlClaim(d)) < 0) break;
        if (!r) {
            EnterCriticalSection(&d->cs);
            long long from = d->segs[i].pos, to = d->segs[i].end;
            d->segs[i].reqEnd = to;
            LeaveCriticalSection(&d->cs);
            if (from >= to) { i = -1; continue; }
            DWORD st = 0;
            r = DlRequest(d, from, to - 1, st);
            if (!r || st != 206) {
                if (r) { WinHttpCloseHandle(r); r = nullptr; }
                if (st) { // answered, but not with the part: the server won't take another connection
                    EnterCriticalSection(&d->cs);
                    d->error = L"the server answered " + std::to_wstring(st);
                    LeaveCriticalSection(&d->cs);
                }
                // others still going: this server wants fewer connections, so this one bows out
                if ((st && d->conns > 0) || ++fails > 6) { DlRelease(d, i); break; }
                Sleep(700 * fails);
                continue;
            }
        }
        EnterCriticalSection(&d->cs);
        long long before = d->segs[i].pos;
        LeaveCriticalSection(&d->cs);
        ++d->conns;
        bool fin = DlPump(d, i, r, buf);
        --d->conns;
        WinHttpCloseHandle(r);
        r = nullptr;
        if (fin) { i = -1; fails = 0; continue; }
        if (d->state != DS_RUN) break;
        if (!d->ranges) { DlFail(d, L"the connection dropped and this server can't resume"); break; }
        EnterCriticalSection(&d->cs);
        if (d->segs[i].pos > before) fails = 0; // it got somewhere: a fresh try
        LeaveCriticalSection(&d->cs);
        if (++fails > 6) { DlRelease(d, i); break; }
        Sleep(700 * fails); // then resume this part from where it stopped
    }
    if (r) WinHttpCloseHandle(r);
}

// The last thread out closes the file: done (keep it) or not (delete-on-close drops it).
void DlFinish(Download* d) {
    EnterCriticalSection(&d->cs);
    bool complete = d->file != INVALID_HANDLE_VALUE && !d->segs.empty();
    for (const auto& s : d->segs) complete = complete && s.pos >= s.end;
    complete = complete && d->state == DS_RUN;
    if (d->file != INVALID_HANDLE_VALUE) {
        if (complete) {
            FILE_SET_SPARSE_BUFFER sp{ FALSE };
            DWORD br = 0;
            DeviceIoControl(d->file, FSCTL_SET_SPARSE, &sp, sizeof(sp), nullptr, 0, &br, nullptr);
            FILE_DISPOSITION_INFO keep{ FALSE };
            SetFileInformationByHandle(d->file, FileDispositionInfo, &keep, sizeof(keep));
        }
        CloseHandle(d->file);
        d->file = INVALID_HANDLE_VALUE;
    }
    if (complete) { d->doneAt = GetTickCount(); d->state = DS_DONE; }
    else if (d->state == DS_RUN) { d->state = DS_FAIL; if (d->error.empty()) d->error = L"the server stopped sending it"; }
    LeaveCriticalSection(&d->cs);
}

void DlThreadDone(Download* d) { if (--d->threads == 0) DlFinish(d); }

DWORD WINAPI DlWorker(void* p) {
    Download* d = static_cast<Download*>(p);
    DlWorkLoop(d, -1, nullptr);
    DlThreadDone(d);
    return 0;
}

// First connection: follows redirects, learns the size and whether the server can send
// parts, creates the file, starts the other connections, then downloads like the rest.
DWORD WINAPI DlMain(void* p) {
    Download* d = static_cast<Download*>(p);
    d->session = WinHttpOpen(d->ua.empty() ? L"Mozilla/5.0" : d->ua.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                             WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!d->session) d->session = WinHttpOpen(L"Mozilla/5.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET r = nullptr;
    DWORD st = 0;
    if (d->session) {
        WinHttpSetTimeouts(d->session, 0, 15000, 15000, 10000); // a stalled connection gives up after 10 s and resumes
        DWORD never = WINHTTP_OPTION_REDIRECT_POLICY_NEVER, conns = 32;
        WinHttpSetOption(d->session, WINHTTP_OPTION_REDIRECT_POLICY, &never, sizeof(never)); // redirects by hand: see below
        WinHttpSetOption(d->session, WINHTTP_OPTION_MAX_CONNS_PER_SERVER, &conns, sizeof(conns));
        for (int hop = 0; hop < 10 && d->state == DS_RUN; ++hop) {
            if (!DlCrack(d)) { DlFail(d, L"that link can't be opened"); break; }
            for (int t = 0; t < 3 && !(r = DlRequest(d, 0, -1, st)) && d->state == DS_RUN; ++t) Sleep(1000);
            if (!r) { DlFail(d, L"couldn't connect to " + d->host); break; }
            if (st < 300 || st >= 400) break;
            std::wstring loc = DlQuery(r, WINHTTP_QUERY_LOCATION);
            WinHttpCloseHandle(r);
            r = nullptr;
            if (loc.empty()) { DlFail(d, L"the server answered " + std::to_wstring(st)); break; }
            if (loc[0] == L'/')
                loc = (d->https ? L"https://" : L"http://") + d->host + L":" + std::to_wstring(d->port) + loc;
            std::wstring oldHost = d->host;
            d->url = loc;
            if (DlCrack(d) && d->host != oldHost) d->cookies.clear(); // a site's cookies never go to another host
        }
    } else {
        DlFail(d, L"Windows' internet service isn't available");
    }

    if (r && (st == 200 || st == 206) && d->state == DS_RUN) {
        long long total = 0;
        if (st == 206) {
            std::wstring cr = DlQuery(r, WINHTTP_QUERY_CONTENT_RANGE); // "bytes 0-999/1000"
            size_t sl = cr.rfind(L'/');
            if (sl != std::wstring::npos && sl + 1 < cr.size() && cr[sl + 1] != L'*') total = _wtoi64(cr.c_str() + sl + 1);
        } else {
            total = _wtoi64(DlQuery(r, WINHTTP_QUERY_CONTENT_LENGTH).c_str());
        }
        d->ranges = st == 206 && total > 0;
        d->size = total;

        EnterCriticalSection(&d->cs);
        std::wstring name = d->name;
        LeaveCriticalSection(&d->cs);
        if (name.empty()) { // Content-Disposition, else the last part of the URL
            std::wstring cd = DlQuery(r, WINHTTP_QUERY_CONTENT_DISPOSITION);
            size_t k;
            if ((k = cd.find(L"filename*=")) != std::wstring::npos) { // filename*=UTF-8''name%20here.zip
                std::wstring v = cd.substr(k + 10);
                v = v.substr(0, v.find(L';'));
                size_t q = v.find(L"''");
                name = DlPercentDecode(q == std::wstring::npos ? v : v.substr(q + 2));
            } else if ((k = cd.find(L"filename=")) != std::wstring::npos) {
                std::wstring v = cd.substr(k + 9);
                if (!v.empty() && v[0] == L'"') v = v.substr(1, v.find(L'"', 1) - 1);
                else v = v.substr(0, v.find(L';'));
                name = v;
            }
            if (name.empty()) {
                std::wstring path = d->object.substr(0, d->object.find(L'?'));
                name = DlPercentDecode(path.substr(path.rfind(L'/') + 1));
            }
        }
        name = DlCleanName(name);

        ULARGE_INTEGER freeB{};
        std::wstring part = DlUnique(d->partDir, name, L".reico-part");
        HANDLE f = INVALID_HANDLE_VALUE;
        if (total > 0 && GetDiskFreeSpaceExW(d->partDir.c_str(), &freeB, nullptr, nullptr) && freeB.QuadPart < static_cast<ULONGLONG>(total)) {
            DlFail(d, L"not enough free space - it needs " + DlBytes(static_cast<double>(total)));
        } else if ((f = CreateFileW(part.c_str(), GENERIC_WRITE | DELETE, FILE_SHARE_READ, nullptr, CREATE_NEW,
                                    FILE_ATTRIBUTE_NORMAL, nullptr)) == INVALID_HANDLE_VALUE) {
            DlFail(d, L"couldn't create the file in " + d->partDir);
        } else {
            FILE_DISPOSITION_INFO drop{ TRUE };  // until it's complete: gone if REICO closes or it fails
            SetFileInformationByHandle(f, FileDispositionInfo, &drop, sizeof(drop));
            DWORD br = 0;
            DeviceIoControl(f, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &br, nullptr);
            if (total > 0) {
                LARGE_INTEGER li;
                li.QuadPart = total;
                SetFilePointerEx(f, li, nullptr, FILE_BEGIN);
                SetEndOfFile(f);
            }
            long long end = total > 0 ? total : LLONG_MAX;
            EnterCriticalSection(&d->cs);
            d->name = name;
            d->part = part;
            d->file = f;
            d->segs = { { 0, 0, end, end, true } };
            LeaveCriticalSection(&d->cs);
            if (d->ranges)
                for (int k = 1; k < DL_CONNS; ++k) {
                    ++d->threads;
                    HANDLE t = CreateThread(nullptr, 0, DlWorker, d, 0, nullptr);
                    if (t) CloseHandle(t); else --d->threads;
                }
            DlWorkLoop(d, 0, r);
            r = nullptr;
        }
    } else if (r && d->state == DS_RUN) {
        DlFail(d, st == 401 || st == 403 ? L"the server refused it (" + std::to_wstring(st) + L") - try the browser's own download"
                                         : L"the server answered " + std::to_wstring(st));
    }
    if (r) WinHttpCloseHandle(r);
    DlThreadDone(d);
    return 0;
}

// -- the cards (UI thread) --

void DlRender();

void DlShow() {
    if (!g_dlWnd) {
        g_dlNameF = InterFont(14, 600);
        g_dlSmallF = InterFont(12, 500);
        g_dlBtnF = InterFont(13, 600);
        g_dlGlyphF = MakeFont(16, FW_NORMAL, L"Segoe MDL2 Assets");
        g_dlWnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"REICODownloads",
                                  L"Downloads", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        int h = (DL_ASK_H + DL_GAP) * DL_MAX_CARDS;
        if (!g_dlWnd || !SurfCreate(g_dlS, DL_W, h) || !SurfCreate(g_dlMask, DL_W, h)) return;
    }
    SetTimer(g_dlWnd, DL_TIMER, 100, nullptr);
    DlRender();
    ShowWindow(g_dlWnd, SW_SHOWNOACTIVATE);
}

bool DlStart(Download* d) {
    InitializeCriticalSection(&d->cs);
    d->born = GetTickCount();
    d->threads = 1;
    HANDLE t = CreateThread(nullptr, 0, DlMain, d, 0, nullptr);
    if (!t) { DeleteCriticalSection(&d->cs); delete d; return false; }
    CloseHandle(t);
    g_dls.push_back(d);
    DlShow();
    return true;
}

// A download from the extension (JSON). false = let the browser download it.
bool DlAdd(const std::string& json) {
    if (!g_glassCfg.downloads) return false;
    std::wstring url = DlHeaderSafe(DlJsonStr(json, "url"));
    if ((_wcsnicmp(url.c_str(), L"http://", 7) && _wcsnicmp(url.c_str(), L"https://", 8)) || url.size() > 8192) return false;
    if (g_dlDir.empty()) DlLoadDir();
    Download* d = new Download();
    d->url = url;
    std::wstring name = DlJsonStr(json, "name");
    if (!name.empty()) d->name = DlCleanName(name);
    d->referrer = DlHeaderSafe(DlJsonStr(json, "referrer"));
    d->cookies = DlHeaderSafe(DlJsonStr(json, "cookies"));
    d->ua = DlHeaderSafe(DlJsonStr(json, "ua"));
    d->size = DlJsonNum(json, "size");
    d->dir = d->partDir = g_dlDir;
    Log(L"Downloads: caught " + url);
    return DlStart(d);
}

void DlRetry(const Download* o) {
    Download* d = new Download();
    d->url = o->url; d->referrer = o->referrer; d->cookies = o->cookies; d->ua = o->ua;
    EnterCriticalSection(const_cast<CRITICAL_SECTION*>(&o->cs));
    d->name = o->name;
    LeaveCriticalSection(const_cast<CRITICAL_SECTION*>(&o->cs));
    d->dir = d->partDir = o->dir;
    d->confirmed = true;
    DlStart(d);
}

// Off the screen; freed by DlTick once its threads have stopped.
void DlDismiss(Download* d) {
    EnterCriticalSection(&d->cs);
    if (d->state == DS_RUN) d->state = DS_CANCEL;              // the threads stop, the part file deletes itself
    else if (d->state == DS_DONE && !d->moving) DeleteFileW(d->part.c_str()); // finished, but you said no
    LeaveCriticalSection(&d->cs);
    g_dls.erase(std::remove(g_dls.begin(), g_dls.end(), d), g_dls.end());
    g_dlGone.push_back(d);
    if (g_dlHot.d == d) g_dlHot = DlHit{};
}

// Finished and confirmed: from "<name>.reico-part" to its real name in the chosen folder.
// Its own thread: to another drive it's a full copy, and antivirus may hold the file a while.
DWORD WINAPI DlMover(void* p) {
    Download* d = static_cast<Download*>(p);
    std::wstring dest = DlUnique(d->dir, d->name);
    if (MoveFileExW(d->part.c_str(), dest.c_str(), MOVEFILE_COPY_ALLOWED)) {
        d->final = dest;
        d->savedAt = GetTickCount();
    } else {
        DeleteFileW(d->part.c_str());
        EnterCriticalSection(&d->cs);
        d->error = L"couldn't save it to " + d->dir;
        LeaveCriticalSection(&d->cs);
        d->state = DS_FAIL;
    }
    d->moved = true;
    --d->threads;
    return 0;
}

void DlMove(Download* d) {
    d->moving = true;
    ++d->threads;
    HANDLE t = CreateThread(nullptr, 0, DlMover, d, 0, nullptr);
    if (t) CloseHandle(t); else DlMover(d);
}

void DlButton(Canvas c, Download* d, int act, RECT r, const wchar_t* label, bool primary, float a, HFONT f) {
    bool hot = g_dlHot.d == d && g_dlHot.act == act;
    GlassBlob(c, static_cast<float>(r.left), static_cast<float>(r.top), static_cast<float>(r.right), static_cast<float>(r.bottom),
              (r.bottom - r.top) / 2.f, primary ? (hot ? 0.8f : 0.6f) : (hot ? 0.35f : 0.f), hot ? 0.2f : 0.1f, a);
    DrawTextOn(c, g_dlMask, label, r, f, RGB(255, 255, 255), 0.95f * a, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    g_dlHits.push_back({ r, d, act });
}

void DlDrawCard(Canvas c, Download* d, int y, int h, float a) {
    const COLORREF white = RGB(255, 255, 255);
    COLORREF tint = RGB(g_glassCfg.tintR, g_glassCfg.tintG, g_glassCfg.tintB);
    COLORREF veil = RGB(GetRValue(tint) * 3 / 10 + 14, GetGValue(tint) * 3 / 10 + 14, GetBValue(tint) * 3 / 10 + 18);
    FillRRect(c, 0.5f, y + 0.5f, DL_W - 0.5f, y + h - 0.5f, 20, veil, 0.9f * a);
    GlassLight(c, 0.5f, y + 0.5f, DL_W - 0.5f, y + h - 0.5f, 20, a);

    EnterCriticalSection(&d->cs);
    std::wstring name = d->name, err = d->error;
    std::vector<DlSeg> segs = d->segs;
    LeaveCriticalSection(&d->cs);
    int st = d->state;
    double size = static_cast<double>(d->size), got = static_cast<double>(d->got);
    bool done = st == DS_DONE && (d->moved || !d->confirmed); // confirmed: done once it's saved

    GlassBlob(c, 16, y + 16.f, 54, y + 54.f, 19, 0.55f, 0.1f, a);
    DrawGlassText(c, g_dlMask, st == DS_FAIL ? L"\uE783" : done ? L"\uE73E" : L"\uE896", { 16, y + 16, 54, y + 54 }, g_dlGlyphF, a,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE, 0.f, 0.85f);
    if (name.empty()) name = st == DS_FAIL ? d->url : L"Connecting\u2026";
    DrawTextOn(c, g_dlMask, name.c_str(), { 66, y + 14, DL_W - (d->confirmed ? 54 : 16), y + 34 },
               g_dlNameF, white, 0.97f * a, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    std::wstring line;
    if (st == DS_FAIL) line = L"Failed: " + err;
    else if (done && d->confirmed) {
        double secs = std::max<DWORD>(1, (d->doneAt - d->born) / 1000);
        line = L"Done \u00B7 " + DlBytes(size) + L" in " + DlTime(static_cast<long long>(secs)) + L" \u00B7 " + DlBytes(size / secs) + L"/s";
    } else if (!d->confirmed) line = (size > 0 ? DlBytes(size) : std::wstring(L"Size unknown")) + L" \u00B7 " + d->host; // progress is below
    else {
        line = size > 0 ? DlBytes(got) + L" of " + DlBytes(size) : DlBytes(got);
        if (d->speed > 0) line += L" \u00B7 " + DlBytes(d->speed) + L"/s";
        if (d->speed > 0 && size > got) line += L" \u00B7 " + DlTime(static_cast<long long>((size - got) / d->speed)) + L" left";
    }
    DrawTextOn(c, g_dlMask, line.c_str(), { 66, y + 34, DL_W - 16, y + 52 }, g_dlSmallF, white, 0.62f * a,
               DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    auto bar = [&](float y0) { // every connection's part, as it fills in
        FillRRect(c, 16, y0, DL_W - 16.f, y0 + 8, 4, white, 0.12f * a);
        if (size <= 0) return;
        const float x0 = 16, w = DL_W - 32.f;
        for (const auto& s : segs) {
            float s0 = x0 + w * static_cast<float>(s.start / size), s1 = x0 + w * static_cast<float>(std::min<double>(s.pos, size) / size);
            if (s1 - s0 >= 1.f) FillRRect(c, s0, y0, s1, y0 + 8, std::min(4.f, (s1 - s0) / 2), AccentColour(), 0.95f * a);
        }
        GlassLight(c, 16, y0, DL_W - 16.f, y0 + 8, 4, 0.6f * a);
    };

    if (!d->confirmed) { // where to save + Download / Cancel
        FillRRect(c, 16, y + 64.f, DL_W - 16.f, y + 98.f, 17, white, 0.07f * a);
        GlassLight(c, 16, y + 64.f, DL_W - 16.f, y + 98.f, 17, 0.5f * a);
        DrawTextOn(c, g_dlMask, L"\uED25", { 24, y + 64, 50, y + 98 }, g_dlGlyphF, white, 0.6f * a, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        DrawTextOn(c, g_dlMask, d->dir.c_str(), { 54, y + 64, DL_W - 112, y + 98 }, g_dlSmallF, white, 0.9f * a,
                   DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_PATH_ELLIPSIS);
        DlButton(c, d, DA_FOLDER, { DL_W - 104, y + 69, DL_W - 21, y + 93 }, L"Change", false, a, g_dlSmallF);
        std::wstring pre = st == DS_FAIL ? L"" : done ? L"Already downloaded - choose where to keep it"
                                         : L"Already downloading" + (size > 0 ? L" \u00B7 " + std::to_wstring(static_cast<int>(got * 100 / size)) + L"%" : L"") +
                                           (d->speed > 0 ? L" \u00B7 " + DlBytes(d->speed) + L"/s" : L"");
        DrawTextOn(c, g_dlMask, pre.c_str(), { 18, y + 104, DL_W - 16, y + 122 }, g_dlSmallF, white, 0.55f * a,
                   DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        if (st != DS_FAIL) DlButton(c, d, DA_GO, { DL_W - 148, y + 128, DL_W - 16, y + 158 }, L"Download", true, a, g_dlBtnF);
        DlButton(c, d, DA_CANCEL, { DL_W - 260, y + 128, DL_W - 156, y + 158 }, L"Cancel", false, a, g_dlBtnF);
        return;
    }
    DlButton(c, d, DA_CLOSE, { DL_W - 46, y + 12, DL_W - 16, y + 42 }, L"\uE711", false, a, g_dlGlyphF);
    if (done) {
        DlButton(c, d, DA_OPEN, { 16, y + 62, 104, y + 92 }, L"Open", true, a, g_dlBtnF);
        DlButton(c, d, DA_SHOW, { 112, y + 62, 252, y + 92 }, L"Show in folder", false, a, g_dlBtnF);
    } else if (st == DS_FAIL) {
        DlButton(c, d, DA_RETRY, { 16, y + 62, 104, y + 92 }, L"Retry", true, a, g_dlBtnF);
    } else {
        bar(y + 62.f);
        int n = d->conns;
        std::wstring info = (st == DS_DONE ? std::wstring(L"Saving…")
                             : n > 0 ? std::to_wstring(n) + (n == 1 ? L" connection" : L" connections") : std::wstring(L"Connecting")) +
                            L" \u00B7 saving to " + fs::path(d->dir).filename().wstring();
        DrawTextOn(c, g_dlMask, info.c_str(), { 16, y + 76, DL_W - 16, y + 94 }, g_dlSmallF, white, 0.5f * a,
                   DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
}

void DlRender() {
    if (!g_dlWnd) return;
    DWORD now = GetTickCount();
    Canvas c = g_dlS.cv();
    ZeroMemory(c.px, sizeof(UINT32) * static_cast<size_t>(c.w) * c.h);
    g_dlHits.clear();
    int y = 0, shown = 0;
    for (auto it = g_dls.rbegin(); it != g_dls.rend() && shown < DL_MAX_CARDS; ++it, ++shown) { // newest on top
        Download* d = *it;
        int h = d->confirmed ? DL_CARD_H : DL_ASK_H;
        DlDrawCard(c, d, y, h, EaseOut3((now - d->born) / 220.f));
        y += h + DL_GAP;
    }
    RECT wa{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0); // below the dock when it's on
    g_dlPos = { wa.right - DL_W - 16, wa.top + 12 };
    POINT src{ 0, 0 };
    SIZE sz{ DL_W, std::max(1, y - DL_GAP) };
    BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(g_dlWnd, nullptr, &g_dlPos, &sz, g_dlS.dc, &src, 0, &bf, ULW_ALPHA);
}

void DlTick() {
    DWORD now = GetTickCount();
    for (size_t k = 0; k < g_dls.size(); ++k) {
        Download* d = g_dls[k];
        int n = std::min(d->samples, 32), slot = d->samples % 32; // speed over the last ~3 s
        d->sampleT[slot] = now;
        d->sampleB[slot] = d->got;
        ++d->samples;
        int oldest = n < 32 ? 0 : (slot + 1) % 32;
        DWORD dt = now - d->sampleT[oldest];
        d->speed = dt >= 300 ? (d->got - d->sampleB[oldest]) * 1000.0 / dt : d->speed;
        if (d->state == DS_DONE && d->confirmed && !d->moving) DlMove(d);
    }
    for (size_t k = 0; k < g_dls.size(); ++k) { // finished cards leave by themselves after 15 s
        Download* d = g_dls[k];
        if (d->state == DS_DONE && d->moved && !g_dlHover && now - d->savedAt > 15000) { DlDismiss(d); --k; }
    }
    for (size_t k = 0; k < g_dlGone.size(); ++k) {
        Download* d = g_dlGone[k];
        if (d->threads != 0) continue;
        if (d->conn) WinHttpCloseHandle(d->conn);
        if (d->session) WinHttpCloseHandle(d->session);
        DeleteCriticalSection(&d->cs);
        delete d;
        g_dlGone.erase(g_dlGone.begin() + k--);
    }
    if (!g_dls.empty()) DlRender();
    else {
        ShowWindow(g_dlWnd, SW_HIDE);
        if (g_dlGone.empty()) KillTimer(g_dlWnd, DL_TIMER);
    }
}

DlHit DlHitAt(POINT p) {
    for (const auto& h : g_dlHits) if (PtInRect(&h.r, p)) return h;
    return DlHit{};
}

void DlAction(const DlHit& h) {
    Download* d = h.d;
    if (std::find(g_dls.begin(), g_dls.end(), d) == g_dls.end()) return;
    switch (h.act) {
    case DA_GO: d->confirmed = true; break;
    case DA_CANCEL: case DA_CLOSE: DlDismiss(d); break;
    case DA_FOLDER: {
        std::wstring dir = d->dir;
        if (DlPickFolder(dir)) { d->dir = g_dlDir = dir; WriteUtf8(DL_DIR_TXT, dir); }
        break;
    }
    case DA_OPEN: ShellExecuteW(nullptr, L"open", d->final.c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
    case DA_SHOW: ShellExecuteW(nullptr, L"open", L"explorer.exe", (L"/select,\"" + d->final + L"\"").c_str(), nullptr, SW_SHOWNORMAL); break;
    case DA_RETRY: DlRetry(d); DlDismiss(d); break;
    }
    DlRender();
}

LRESULT CALLBACK DlProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_TIMER: DlTick(); return 0;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE; // don't take focus from the browser
    case WM_MOUSEMOVE: {
        TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, h, 0 };
        TrackMouseEvent(&tme);
        g_dlHover = true;
        DlHit hit = DlHitAt(POINT{ GET_X_LPARAM(l), GET_Y_LPARAM(l) });
        if (hit.d != g_dlHot.d || hit.act != g_dlHot.act) { g_dlHot = hit; DlRender(); }
        return 0;
    }
    case WM_MOUSELEAVE: g_dlHover = false; g_dlHot = DlHit{}; DlRender(); return 0;
    case WM_SETCURSOR: SetCursor(LoadCursor(nullptr, g_dlHot.d ? IDC_HAND : IDC_ARROW)); return TRUE;
    case WM_LBUTTONDOWN: g_dlPressed = DlHitAt(POINT{ GET_X_LPARAM(l), GET_Y_LPARAM(l) }); return 0;
    case WM_LBUTTONUP: {
        DlHit hit = DlHitAt(POINT{ GET_X_LPARAM(l), GET_Y_LPARAM(l) });
        if (hit.d && hit.d == g_dlPressed.d && hit.act == g_dlPressed.act) DlAction(hit);
        g_dlPressed = DlHit{};
        return 0;
    }
    }
    return DefWindowProcW(h, msg, w, l);
}

void DlInit() {
    WNDCLASSW wc{};
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpfnWndProc = DlProc;
    wc.lpszClassName = L"REICODownloads";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);

    // the native-messaging host the extension talks to: this exe, for Chrome, Edge, Brave and Chromium
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring esc;
    for (const wchar_t* p = exe; *p; ++p) { if (*p == L'\\' || *p == L'"') esc += L'\\'; esc += *p; }
    WriteUtf8(DL_HOST_JSON, L"{\n  \"name\": \"com.reico.downloads\",\n  \"description\": \"REICO download manager\",\n  \"path\": \"" + esc +
                            L"\",\n  \"type\": \"stdio\",\n  \"allowed_origins\": [\"chrome-extension://" + DL_EXT_ID + L"/\"]\n}\n");
    for (const wchar_t* b : { L"Google\\Chrome", L"Microsoft\\Edge", L"BraveSoftware\\Brave-Browser", L"Chromium" }) {
        HKEY k;
        std::wstring key = std::wstring(L"Software\\") + b + L"\\NativeMessagingHosts\\com.reico.downloads";
        if (RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) == ERROR_SUCCESS) {
            RegSetValueExW(k, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(DL_HOST_JSON), static_cast<DWORD>((wcslen(DL_HOST_JSON) + 1) * sizeof(wchar_t)));
            RegCloseKey(k);
        }
    }
}

// "REICO.exe chrome-extension://..." - the browser's native-messaging call. Reads one
// message (4-byte length + JSON), hands it to the running REICO, answers, exits.
int DlHostMain() {
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE), out = GetStdHandle(STD_OUTPUT_HANDLE);
    auto readAll = [&](void* b, DWORD n) {
        for (DWORD got = 0, k = 0; got < n; got += k)
            if (!ReadFile(in, static_cast<char*>(b) + got, n - got, &k, nullptr) || !k) return false;
        return true;
    };
    UINT32 len = 0;
    bool ok = false;
    if (readAll(&len, 4) && len > 0 && len < (1u << 20)) {
        std::string json(len, '\0');
        HWND tray = FindWindowW(L"REICOTray", nullptr);
        if (readAll(&json[0], len) && tray) {
            COPYDATASTRUCT cd{ DL_COPYDATA_ID, len, &json[0] };
            DWORD_PTR res = 0;
            ok = SendMessageTimeoutW(tray, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&cd), SMTO_ABORTIFHUNG, 5000, &res) && res == 1;
        }
    }
    const char* reply = ok ? "{\"ok\":true}" : "{\"ok\":false}";
    UINT32 n = static_cast<UINT32>(strlen(reply));
    DWORD w = 0;
    WriteFile(out, &n, 4, &w, nullptr);
    WriteFile(out, reply, n, &w, nullptr);
    return 0;
}

int DockHit(POINT p) {
    const int sideHit[3] = { DH_GAMES, DH_TASKMGR, DH_NOTES };
    for (int i = 0; i < 3; ++i) {
        int bx = DockSideX(i);
        if (bx >= 0 && p.y >= DOCK_TOP && p.y <= DOCK_TOP + DOCK_H && p.x >= bx && p.x <= bx + DOCK_H) return sideHit[i];
    }
    const float L = g_dockS.w / 2.f - g_dockW / 2.f, R = L + g_dockW;
    if (p.y < DOCK_TOP || p.y > DOCK_TOP + DOCK_H || p.x < L || p.x > R) return DH_NONE;
    float rel = p.x - L;
    if (rel < DOCK_PAD + DOCK_SLOT) return DH_START;
    if (p.x >= R - DOCK_PAD - DOCK_CLOCK_W) return DH_CLOCK;
    if (p.x >= R - DOCK_PAD - DOCK_CLOCK_W - DOCK_TRAY_W) return DH_TRAY;
    for (int i = 0; i < static_cast<int>(g_dockApps.size()); ++i)
        if (rel >= DockAppX(i) && rel < DockAppX(i) + DOCK_SLOT) return i;
    return DH_NONE;
}

void SendKeys(std::initializer_list<WORD> keys) {
    std::vector<INPUT> in;
    for (WORD k : keys) { INPUT i{}; i.type = INPUT_KEYBOARD; i.ki.wVk = k; in.push_back(i); }
    for (auto it = keys.end(); it != keys.begin();) {
        INPUT i{}; i.type = INPUT_KEYBOARD; i.ki.wVk = *--it; i.ki.dwFlags = KEYEVENTF_KEYUP; in.push_back(i);
    }
    SendInput(static_cast<UINT>(in.size()), in.data(), sizeof(INPUT));
}

void ActivateWindow(HWND h) {
    if (IsIconic(h)) ShowWindow(h, SW_RESTORE);
    if (!SetForegroundWindow(h)) SwitchToThisWindow(h, TRUE);
}

void DockLaunch(const DockApp& a) {
    const std::wstring& target = !a.lnk.empty() ? a.lnk : a.exe;
    if (!target.empty()) ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// Like the real taskbar: open it / bring it to front / minimize it if it's already in front.
void DockClickApp(int i) {
    DockApp& a = g_dockApps[i];
    a.bounce = GetTickCount();
    DockKick();
    if (a.wins.empty()) { DockLaunch(a); return; }
    HWND fg = GetForegroundWindow();
    bool front = std::find(a.wins.begin(), a.wins.end(), fg) != a.wins.end();
    if (!front) ActivateWindow(a.wins.front());
    else if (a.wins.size() > 1) ActivateWindow(a.wins.back()); // several windows: cycle through them
    else ShowWindow(fg, SW_MINIMIZE);
}

void DockAppMenu(int i) {
    DockApp a = g_dockApps[i]; // copy: the list may refresh while the menu is open
    HMENU m = CreatePopupMenu();
    if (!a.lnk.empty() || !a.exe.empty()) AppendMenuW(m, MF_STRING, 1, L"New window");
    if (!a.wins.empty()) AppendMenuW(m, MF_STRING, 2, a.wins.size() > 1 ? L"Close all windows" : L"Close window");
    POINT p;
    GetCursorPos(&p);
    SetForegroundWindow(g_trayWnd); // menus need a foreground owner to close on click-away
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, p.x, p.y, 0, g_trayWnd, nullptr);
    PostMessageW(g_trayWnd, WM_NULL, 0, 0);
    DestroyMenu(m);
    if (cmd == 1) DockLaunch(a);
    else if (cmd == 2) for (HWND h : a.wins) PostMessageW(h, WM_CLOSE, 0, 0);
}

void DockAppbarSetPos() {
    APPBARDATA abd{};
    abd.cbSize = sizeof(abd);
    abd.hWnd = g_dock;
    abd.uEdge = ABE_TOP;
    abd.rc = { g_dockMon.left, g_dockMon.top, g_dockMon.right, g_dockMon.top + DOCK_STRIP };
    SHAppBarMessage(ABM_QUERYPOS, &abd);
    abd.rc.bottom = abd.rc.top + DOCK_STRIP;
    SHAppBarMessage(ABM_SETPOS, &abd);
    g_dockOrigin = { abd.rc.left, abd.rc.top };
    SetWindowPos(g_dock, HWND_TOPMOST, abd.rc.left, abd.rc.top, g_dockMon.right - g_dockMon.left, DOCK_WIN_H,
                 SWP_NOACTIVATE);
}

void DockDisable();
void DockEnable();

LRESULT CALLBACK DockProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_DOCK_APPBAR:
        if (w == ABN_POSCHANGED) DockAppbarSetPos();
        else if (w == ABN_FULLSCREENAPP) {
            // Windows also sends this when the desktop gets focus (closing / minimizing an
            // app), which made the dock blink - so only trust REICO's own check, which
            // knows the desktop isn't a fullscreen app.
            bool fs = IsForegroundWindowFullscreen();
            if (fs != g_dockHidden) { g_dockHidden = fs; ShowWindow(h, fs ? SW_HIDE : SW_SHOWNOACTIVATE); }
        }
        return 0;

    case WM_TIMER:
        if (w == DOCK_ANIM_TIMER) {
            bool moving = DockAnimate();
            DockRender();
            if (!moving) { KillTimer(h, DOCK_ANIM_TIMER); g_dockTimerOn = false; }
        } else if (w == DOCK_TICK_TIMER) {
            bool fs = IsForegroundWindowFullscreen(); // games / videos: get out of the way
            if (fs != g_dockHidden) { g_dockHidden = fs; ShowWindow(h, fs ? SW_HIDE : SW_SHOWNOACTIVATE); }
            if (!fs) DockRender(); // clock

            // backstop for the show hook (DockEventProc): keep Windows' taskbar hidden
            HWND tb = FindWindowW(L"Shell_TrayWnd", nullptr);
            if (tb && IsWindowVisible(tb)) TaskbarShow(false);
        } else if (w == DOCK_REFRESH_TIMER) {
            KillTimer(h, DOCK_REFRESH_TIMER);
            DockRefresh();
        }
        return 0;

    case WM_MOUSEMOVE: {
        POINT p{ GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, h, 0 };
        TrackMouseEvent(&tme);
        int hot = DockHit(p);
        if (hot != g_dockHot) {
            g_dockHot = hot;
            g_dockHoverSince = GetTickCount();
            if (hot >= 0 && hot < static_cast<int>(g_dockApps.size())) g_dockApps[hot].shine = g_dockHoverSince;
            DockKick();
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        g_dockHot = DH_NONE;
        DockKick();
        return 0;
    case WM_SETCURSOR:
        SetCursor(LoadCursor(nullptr, IDC_ARROW));
        return TRUE;

    case WM_LBUTTONUP: {
        int hit = DockHit(POINT{ GET_X_LPARAM(l), GET_Y_LPARAM(l) });
        if (hit == DH_GAMES) GameLauncherToggle();
        else if (hit == DH_NOTES) BoardToggle();
        else if (hit == DH_TASKMGR) { AllowSetForegroundWindow(ASFW_ANY); ShellExecuteW(nullptr, L"open", L"taskmgr.exe", nullptr, nullptr, SW_SHOWNORMAL); }
        else if (hit == DH_START) SendKeys({ VK_LWIN });
        else if (hit == DH_TRAY) // glass tray pop-up right under the button
            TrayPopToggle(g_dockOrigin.x + g_dockS.w / 2.f + g_dockW / 2.f - DOCK_PAD - DOCK_CLOCK_W - DOCK_TRAY_W / 2.f);
        else if (hit == DH_CLOCK) // glass calendar right under the clock
            CalToggle(g_dockOrigin.x + g_dockS.w / 2.f + g_dockW / 2.f - DOCK_PAD - DOCK_CLOCK_W / 2.f);
        else if (hit >= 0 && hit < static_cast<int>(g_dockApps.size())) DockClickApp(hit);
        return 0;
    }
    case WM_MBUTTONUP: {
        int hit = DockHit(POINT{ GET_X_LPARAM(l), GET_Y_LPARAM(l) });
        if (hit >= 0 && hit < static_cast<int>(g_dockApps.size())) DockLaunch(g_dockApps[hit]);
        return 0;
    }
    case WM_RBUTTONUP: {
        int hit = DockHit(POINT{ GET_X_LPARAM(l), GET_Y_LPARAM(l) });
        if (hit >= 0 && hit < static_cast<int>(g_dockApps.size())) DockAppMenu(hit);
        return 0;
    }
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;

    case WM_GAMES_FOUND: { // the background game scan finished
        std::unique_ptr<std::vector<Game>> found(reinterpret_cast<std::vector<Game>*>(l));
        g_games = std::move(*found);
        g_glScanning = g_glScanRunning = false;
        GlRebuildView();
        if (g_glVisible) GlKick();
        LogF(L"Games: %d found", static_cast<int>(g_games.size()));
        return 0;
    }

    case WM_DISPLAYCHANGE: // resolution changed: rebuild at the new size
        if (g_dockOn) { DockDisable(); DockEnable(); }
        return 0;
    }
    return DefWindowProcW(h, msg, w, l);
}

// Windows came, went or changed focus: refresh shortly (several events arrive in bursts).
void CALLBACK DockEventProc(HWINEVENTHOOK, DWORD ev, HWND hwnd, LONG idObject, LONG idChild, DWORD, DWORD) {
    if (!g_dockOn || !hwnd || idObject != OBJID_WINDOW || idChild != CHILDID_SELF) return;
    if (ev != EVENT_SYSTEM_FOREGROUND && GetAncestor(hwnd, GA_ROOT) != hwnd) return;
    if (ev == EVENT_OBJECT_SHOW) { // Windows trying to bring its taskbar / tray overflow back: hide it right away
        wchar_t cls[64]{};
        GetClassNameW(hwnd, cls, 64);
        if (_wcsicmp(cls, L"Shell_TrayWnd") == 0 || _wcsicmp(cls, L"NotifyIconOverflowWindow") == 0) {
            ShowWindow(hwnd, SW_HIDE);
            return;
        }
    }
    SetTimer(g_dock, DOCK_REFRESH_TIMER, ev == EVENT_SYSTEM_FOREGROUND ? 30 : 150, nullptr);
}

void DockEnable() {
    if (g_dockOn) return;
    if (!g_dock) {
        // COM for resolving pinned shortcuts (harmless if already initialised)
        typedef HRESULT (WINAPI *PFN_CoInitEx)(LPVOID, DWORD);
        if (auto init = reinterpret_cast<PFN_CoInitEx>(reinterpret_cast<void*>(GetProcAddress(LoadLibraryW(L"ole32.dll"), "CoInitializeEx"))))
            init(nullptr, COINIT_APARTMENTTHREADED);

        g_dockGlyphF = MakeFont(17, FW_NORMAL, L"Segoe MDL2 Assets");
        g_dockTimeF = InterFont(16, 600);
        g_dockDateF = InterFont(11, 500);
        g_dockTipF = InterFont(13, 500);
        g_dockGamesF = MakeFont(22, FW_NORMAL, L"Segoe MDL2 Assets");

        WNDCLASSW wc{};
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpfnWndProc = DockProc;
        wc.lpszClassName = L"REICODock";
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        RegisterClassW(&wc);
        g_dock = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"REICODock",
                                 L"REICO dock", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, wc.hInstance, nullptr);
        if (!g_dock) return;
    }

    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY), &mi);
    g_dockMon = mi.rcMonitor;
    int W = g_dockMon.right - g_dockMon.left;
    if (!SurfCreate(g_dockS, W, DOCK_WIN_H) || !SurfCreate(g_dockMask, W, DOCK_WIN_H)) return;

    TaskbarAutoHide();
    TaskbarShow(false);
    APPBARDATA abd{};
    abd.cbSize = sizeof(abd);
    abd.hWnd = g_dock;
    abd.uCallbackMessage = WM_DOCK_APPBAR;
    g_dockAppbar = SHAppBarMessage(ABM_NEW, &abd) != 0;
    DockAppbarSetPos();

    g_dockOn = true;
    g_dockApps.clear();
    DockLoadPinned();
    DockRefresh();
    if (g_games.empty()) GlStartLoad(false); // the saved list + icons, in the background (no scan)
    g_dockW = 80.f; // grows open from a small pill
    g_dockHidden = false;
    ShowWindow(g_dock, SW_SHOWNOACTIVATE);

    g_dockHookFg = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, DockEventProc, 0, 0,
                                   WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    g_dockHookObj = SetWinEventHook(EVENT_OBJECT_DESTROY, EVENT_OBJECT_HIDE, nullptr, DockEventProc, 0, 0,
                                    WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    SetTimer(g_dock, DOCK_TICK_TIMER, 1000, nullptr);
    DockKick();
    Log(L"Dock: on");
}

void DockDisable() {
    if (!g_dockOn) return;
    g_dockOn = false;
    if (g_dockHookFg) { UnhookWinEvent(g_dockHookFg); g_dockHookFg = nullptr; }
    if (g_dockHookObj) { UnhookWinEvent(g_dockHookObj); g_dockHookObj = nullptr; }
    KillTimer(g_dock, DOCK_ANIM_TIMER);
    KillTimer(g_dock, DOCK_TICK_TIMER);
    KillTimer(g_dock, DOCK_REFRESH_TIMER);
    g_dockTimerOn = false;
    if (g_dockAppbar) {
        APPBARDATA abd{};
        abd.cbSize = sizeof(abd);
        abd.hWnd = g_dock;
        SHAppBarMessage(ABM_REMOVE, &abd);
        g_dockAppbar = false;
    }
    ShowWindow(g_dock, SW_HIDE);
    TaskbarRestore();
    Log(L"Dock: off");
}

void DockOnReload() {
    if (g_glassCfg.dock) { DockEnable(); DockKick(); }
    else DockDisable();
}

void DockShutdown() {
    DockDisable();
    if (g_tp) { DestroyWindow(g_tp); g_tp = nullptr; }
    SurfFree(g_tpS);
    SurfFree(g_tpMask);
    BoardShutdown(); // saves the board
    if (g_gl) { DestroyWindow(g_gl); g_gl = nullptr; }
    SurfFree(g_glS);
    SurfFree(g_glMask);
    for (HFONT* f : { &g_glTitleF, &g_glNameF, &g_glSmallF, &g_glGlyphF, &g_glSearchF })
        if (*f) { DeleteObject(*f); *f = nullptr; }
    if (g_cal) { DestroyWindow(g_cal); g_cal = nullptr; }
    SurfFree(g_calS);
    SurfFree(g_calMask);
    for (HFONT* f : { &g_calTimeF, &g_calMonthF, &g_calDayF, &g_calSmallF, &g_calArrowF })
        if (*f) { DeleteObject(*f); *f = nullptr; }
    if (g_dock) { DestroyWindow(g_dock); g_dock = nullptr; }
    SurfFree(g_dockS);
    SurfFree(g_dockMask);
    for (HFONT* f : { &g_dockGlyphF, &g_dockTimeF, &g_dockDateF, &g_dockTipF, &g_dockGamesF })
        if (*f) { DeleteObject(*f); *f = nullptr; }
}

// ---- tray -------------------------------------------------------------

void AddTrayIcon() {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_trayWnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAY;
    nid.hIcon = g_logoIcon ? g_logoIcon : LoadIcon(nullptr, IDI_APPLICATION);
    wcscpy(nid.szTip, L"REICO - click for the glass menu, right-click for more");
    Shell_NotifyIconW(NIM_ADD, &nid);
}

// Tint changed: redraw the logo in the new colour.
void RefreshTrayLogo() {
    if (!g_trayWnd) return;
    HICON old = g_logoIcon;
    g_logoIcon = MakeLogoIcon(GetSystemMetrics(SM_CXSMICON) > 16 ? 32 : 16);
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_trayWnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON;
    nid.hIcon = g_logoIcon;
    Shell_NotifyIconW(NIM_MODIFY, &nid);
    if (g_settingsWnd) {
        SendMessageW(g_settingsWnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(g_logoIcon));
        SendMessageW(g_settingsWnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(g_logoIcon));
    }
    if (old) DestroyIcon(old);

    // The program file's own icon is baked in when it's built: keep reico.ico in the
    // current tint so the next build matches (only rewritten when the tint changes).
    static COLORREF written = CLR_INVALID;
    COLORREF tint = RGB(g_glassCfg.tintR, g_glassCfg.tintG, g_glassCfg.tintB);
    if (tint != written) { WriteLogoIco(L"C:\\REICO\\reico.ico"); written = tint; }
}

void ShowTrayMenu() {
    HMENU m = CreatePopupMenu(), pm = CreatePopupMenu();
    std::vector<std::wstring> presets = ListPresets();
    for (size_t i = 0; i < presets.size(); ++i)
        AppendMenuW(pm, MF_STRING, 1000 + i, presets[i].c_str());
    if (presets.empty()) AppendMenuW(pm, MF_STRING | MF_GRAYED, 0, L"(none yet)");

    AppendMenuW(m, MF_STRING, 1, L"Settings...");
    AppendMenuW(m, MF_POPUP, reinterpret_cast<UINT_PTR>(pm), L"Presets");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (g_glassOn ? MF_CHECKED : 0), 2, L"Window glass\tCtrl+Shift+F6");
    AppendMenuW(m, MF_STRING | (editMode ? MF_CHECKED : 0), 3, L"Edit icon layout\tF2");
    AppendMenuW(m, MF_STRING, 4, L"Rescan desktop\tCtrl+Shift+F5");
    AppendMenuW(m, MF_STRING, 5, L"Open glass.txt");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, 6, L"Exit REICO");
    SetMenuDefaultItem(m, 1, FALSE);

    POINT p;
    GetCursorPos(&p);
    SetForegroundWindow(g_trayWnd); // documented requirement, or the menu won't close on click-away
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, p.x, p.y, 0, g_trayWnd, nullptr);
    PostMessageW(g_trayWnd, WM_NULL, 0, 0);
    DestroyMenu(m); // also destroys the submenu

    if (cmd >= 1000 && cmd < 1000 + static_cast<int>(presets.size())) {
        ApplyPreset(presets[cmd - 1000]);
        UiFromConfig();
        return;
    }
    switch (cmd) {
    case 1: OpenSettings(); break;
    case 2: GlassSetEnabled(!g_glassOn); break;
    case 3:
        editMode = !editMode;
        hoverIndex = -1;
        EnsureAnimTimer(g_hwnd);
        Render(g_hwnd);
        break;
    case 4: ScanDesktop(); Render(g_hwnd); break;
    case 5: ShellExecuteW(nullptr, L"open", L"notepad.exe", GLASS_TXT, nullptr, SW_SHOWNORMAL); break;
    case 6: DestroyWindow(g_hwnd); break; // WM_DESTROY cleans up glass, tray and saves the layout
    }
}

LRESULT CALLBACK TrayProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    if (g_taskbarCreatedMsg && msg == g_taskbarCreatedMsg) {
        AddTrayIcon();
        return 0;
    }
    switch (msg) {
    case WM_COPYDATA: { // a browser download, from the REICO Downloads extension (see DlHostMain)
        auto* cd = reinterpret_cast<const COPYDATASTRUCT*>(l);
        return cd->dwData == DL_COPYDATA_ID && cd->cbData < (1u << 20) &&
               DlAdd(std::string(static_cast<const char*>(cd->lpData), cd->cbData)) ? 1 : 0;
    }
    case WM_TRAY:
        if (LOWORD(l) == WM_RBUTTONUP) {
            PanelClose();
            ShowTrayMenu();
        } else if (LOWORD(l) == WM_LBUTTONUP) {
            // A click on the tray icon while the panel is open first deactivates
            // (= closes) it; don't immediately reopen on that same click.
            if (g_pnlVisible && !g_pnlClosing) PanelClose();
            else if (GetTickCount() - g_pnlLastClose > 300) PanelOpen();
        }
        return 0;

    case WM_TIMER:
        if (w == CFG_WATCH_TIMER_ID) {
            FILETIME t = ConfigStamp();
            if (CompareFileTime(&t, &g_cfgStamp) != 0) { // edited outside REICO (e.g. Notepad)
                ReloadConfig();
                UiFromConfig();
            }
        } else if (w == CFG_APPLY_TIMER_ID) {
            KillTimer(h, CFG_APPLY_TIMER_ID);
            ConfigFromUi();
        }
        return 0;
    }
    return DefWindowProcW(h, msg, w, l);
}

void UiInit() {
    HINSTANCE inst = GetModuleHandleW(nullptr);

    // trackbars live in comctl32; loaded at runtime so the build needs no extra library
    typedef BOOL (WINAPI *PFN_InitCC)(const INITCOMMONCONTROLSEX*);
    if (HMODULE cc = LoadLibraryW(L"comctl32.dll")) {
        auto init = reinterpret_cast<PFN_InitCC>(reinterpret_cast<void*>(GetProcAddress(cc, "InitCommonControlsEx")));
        INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_BAR_CLASSES };
        if (init) init(&icc);
    }
    g_uiFont = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));

    WNDCLASSW wc{};
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpfnWndProc = TrayProc;
    wc.lpszClassName = L"REICOTray";
    RegisterClassW(&wc);

    PanelInit(); // also loads the Inter fonts the logo uses
    NpInit();    // now-playing pop-up + its media watcher thread
    DlInit();    // download manager window + the browser extension's native-messaging host
    if (g_glassCfg.dock) DockEnable();
    else TaskbarRestore(); // a previous run ended without turning the dock off: undo its auto-hide
    g_logoIcon = MakeLogoIcon(GetSystemMetrics(SM_CXSMICON) > 16 ? 32 : 16);

    wc.lpfnWndProc = SettingsProc;
    wc.lpszClassName = L"REICOSettings";
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION); // the logo is set per window (it's redrawn on tint changes)
    RegisterClassW(&wc);

    g_trayWnd = CreateWindowExW(WS_EX_TOOLWINDOW, L"REICOTray", L"REICO tray", WS_POPUP,
                                0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
    g_taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");
    AddTrayIcon();

    SeedPresets();
    g_cfgStamp = ConfigStamp();
    SetTimer(g_trayWnd, CFG_WATCH_TIMER_ID, 1000, nullptr);
}

// Safe to call more than once (WM_DESTROY, and again after the message loop).
void UiShutdown() {
    if (!g_trayWnd) return;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_trayWnd;
    nid.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    if (g_settingsWnd) DestroyWindow(g_settingsWnd);
    for (Download* d : g_dls) // finished but never answered: don't leave the file behind
        if (d->state == DS_DONE && !d->moving) DeleteFileW(d->part.c_str());
    DockShutdown(); // also gives Windows its own taskbar setting back
    NpShutdown();
    PanelShutdown();
    DestroyWindow(g_trayWnd);
    g_trayWnd = nullptr;
    if (g_swatchBrush) { DeleteObject(g_swatchBrush); g_swatchBrush = nullptr; }
    if (g_logoIcon) { DestroyIcon(g_logoIcon); g_logoIcon = nullptr; }
}

// ---------------------------------------------------------------------

LRESULT CALLBACK WndProc(
    HWND h,
    UINT msg,
    WPARAM w,
    LPARAM l
) {
    switch (msg) {

    case WM_KEYDOWN:

        if (w == VK_F2) {
            editMode = !editMode;
            hoverIndex = -1;
            LogF(L"F2 pressed: editMode=%d", editMode);
            EnsureAnimTimer(h);
            Render(h);
            return 0;
        }

        if (w == VK_F5) {
            Log(L"F5 pressed: rescanning");
            ScanDesktop();
            Render(h);
            return 0;
        }

        if (w == VK_ESCAPE) {
            PostQuitMessage(0);
            return 0;
        }

        break;


    case WM_LBUTTONDOWN: {

        POINT p{
            GET_X_LPARAM(l),
            GET_Y_LPARAM(l)
        };

        int k = Hit(p);
        bool onHandle = (k >= 0) ? HitHandle(items[k], p) : false;

        LogF(L"LDOWN (%ld,%ld) editMode=%d hit=%d onHandle=%d",
             p.x, p.y, editMode, k, onHandle);

        if (editMode && k >= 0) {

            selected = k;

            if (onHandle) {
                resizing = true;
                resizeStartW = items[k].w;
                resizeStartH = items[k].h;
                resizeStartMouse = p;
                LogF(L"  -> RESIZE start, item=%d startW=%d startH=%d",
                     k, resizeStartW, resizeStartH);
            } else {
                dragging = true;
                dragOffset.x = p.x - items[k].x;
                dragOffset.y = p.y - items[k].y;
                LogF(L"  -> DRAG start, item=%d offset=(%ld,%ld)",
                     k, dragOffset.x, dragOffset.y);
            }

            SetCapture(h);

        } else if (!editMode && k >= 0) {

            LogF(L"  -> single click on item %d (ignored, double-click to launch)", k);

        } else {
            LogF(L"  -> no item hit at this point");
        }

        Render(h);

        return 0;
    }


    case WM_LBUTTONDBLCLK: {

        // Double-click is the only way to launch now - single click no
        // longer does anything but a bit of visual feedback.
        if (!editMode) {
            POINT p{ GET_X_LPARAM(l), GET_Y_LPARAM(l) };
            int k = Hit(p);
            if (k >= 0) {
                items[k].pulseStart = GetTickCount();
                EnsureAnimTimer(h);
                Launch(items[k]);
                Render(h);
            }
        }

        return 0;
    }


    case WM_MOUSEMOVE: {

        POINT p{
            GET_X_LPARAM(l),
            GET_Y_LPARAM(l)
        };

        if (editMode && resizing && selected >= 0) {

            // Locked-aspect resize: combine horizontal and vertical
            // mouse movement into one scale factor, applied to both
            // width and height together, so icons stretch evenly
            // instead of distorting on one axis only.
            int dx = static_cast<int>(p.x - resizeStartMouse.x);
            int dy = static_cast<int>(p.y - resizeStartMouse.y);
            int delta = (dx + dy) / 2;

            int newW = std::max(MIN_SIZE, resizeStartW + delta);
            double ratio = static_cast<double>(resizeStartH) / static_cast<double>(resizeStartW);
            int newH = std::max(MIN_SIZE, static_cast<int>(std::lround(newW * ratio)));

            items[selected].w = newW;
            items[selected].h = newH;

            if (GetTickCount64() - lastLayoutSaveTick >= LAYOUT_AUTOSAVE_MS) {
                Save();
                lastLayoutSaveTick = GetTickCount64();
            }

            Render(h);

        } else if (editMode && dragging && selected >= 0) {

            items[selected].x =
                std::max(0, static_cast<int>(p.x - dragOffset.x));

            items[selected].y =
                std::max(0, static_cast<int>(p.y - dragOffset.y));

            if (GetTickCount64() - lastLayoutSaveTick >= LAYOUT_AUTOSAVE_MS) {
                Save();
                lastLayoutSaveTick = GetTickCount64();
            }

            Render(h);

        } else if (editMode) {

            int k = Hit(p);
            if (k >= 0 && HitHandle(items[k], p))
                SetCursor(LoadCursor(nullptr, IDC_SIZENWSE));
            else
                SetCursor(LoadCursor(nullptr, IDC_ARROW));

        } else {

            // FIX #3: hover tracking outside edit mode. The actual
            // easing/redraw happens in the animation timer so it stays
            // smooth even while the mouse holds still over an icon.
            int k = Hit(p);
            if (k != hoverIndex) {
                SetHover(k);
                EnsureAnimTimer(h);
            }
        }

        return 0;
    }


    case WM_HOTKEY:

        if (w == HOTKEY_RESCAN_ID) {
            Log(L"Ctrl+Shift+F5 hotkey: rescanning");
            ScanDesktop();
            Render(h);
            return 0;
        }

        if (w == HOTKEY_GLASS_ID) {
            GlassSetEnabled(!g_glassOn);
            return 0;
        }

        break;


    case WM_TIMER:

    if (w == ANIM_TIMER_ID) {

        if (!g_overlayHidden) {
            // A true per-pixel-alpha layered window is automatically
            // click/input-through over fully transparent pixels, so once
            // the cursor leaves an icon onto the transparent desktop
            // background we stop receiving WM_MOUSEMOVE there entirely -
            // nothing would ever clear hoverIndex. Poll the real cursor
            // position instead of waiting for a message that may never
            // come, so hover always corrects itself every frame while
            // this timer is running.
            if (!editMode) {
                POINT cp;
                GetCursorPos(&cp);
                ScreenToClient(h, &cp);
                SetHover(Hit(cp));
            }

            Render(h);
        }

        StopAnimTimerIfIdle(h);

    } else if (w == FULLSCREEN_TIMER_ID) {

        UpdateFullscreenVisibility(h);

    } else if (w == ICON_REFRESH_TIMER_ID) {

        RefreshMissingIcons(h);

    } else if (w == GLASS_TIMER_ID) {

        GlassTick();

    } else if (w == GLASS_FAST_TIMER_ID) {

        GlassFastTick();

    }

    return 0;


    case WM_LBUTTONUP:

        LogF(L"LUP dragging=%d resizing=%d", dragging, resizing);

        if (dragging || resizing) {
            dragging = false;
            resizing = false;
            ReleaseCapture();
            Save();
            lastLayoutSaveTick = GetTickCount64();
        }

        Render(h);

        return 0;


    case WM_RBUTTONDOWN:

        if (editMode) {

            POINT p{
                GET_X_LPARAM(l),
                GET_Y_LPARAM(l)
            };

            int k = Hit(p);

            if (k >= 0) {

                selected = k;

                HMENU menu = CreatePopupMenu();
                AppendMenuW(menu, MF_STRING, 1, L"Hide from overlay");
                AppendMenuW(menu, MF_STRING, 2, L"Launch");

                int command = TrackPopupMenu(
                    menu,
                    TPM_RETURNCMD | TPM_RIGHTBUTTON,
                    p.x, p.y, 0, h, nullptr
                );

                DestroyMenu(menu);

                if (command == 1) {

                    if (items[k].hIcon)
                        DestroyIcon(items[k].hIcon);

                    items.erase(items.begin() + k);
                    selected = -1;

                } else if (command == 2) {

                    Launch(items[k]);
                }

                Save();

                Render(h);

            } else {

                HMENU menu = CreatePopupMenu();
                AppendMenuW(menu, MF_STRING, 1, L"Rescan Desktop (F5)");

                int command = TrackPopupMenu(
                    menu,
                    TPM_RETURNCMD | TPM_RIGHTBUTTON,
                    p.x, p.y, 0, h, nullptr
                );

                DestroyMenu(menu);

                if (command == 1) {
                    ScanDesktop();
                    Render(h);
                }
            }
        }

        return 0;


    case WM_ERASEBKGND:
        return 1;


    case WM_PAINT: {
        // Actual pixels are pushed by Render()/UpdateLayeredWindow, not
        // through normal WM_PAINT - just validate the region.
        PAINTSTRUCT ps;
        BeginPaint(h, &ps);
        EndPaint(h, &ps);
        return 0;
    }


    case WM_DESTROY:

        UnregisterHotKey(h, HOTKEY_RESCAN_ID);
        UiShutdown();
        GlassShutdown(h);

        if (g_animTimerOn) {
            KillTimer(h, ANIM_TIMER_ID);
            g_animTimerOn = false;
        }
        KillTimer(h, FULLSCREEN_TIMER_ID);
        KillTimer(h, ICON_REFRESH_TIMER_ID);

        Save();
        FreeIcons();
        DestroyBackBuffer();
        PostQuitMessage(0);

        return 0;
    }

    return DefWindowProcW(
        h,
        msg,
        w,
        l
    );
}


int WINAPI wWinMain(
    HINSTANCE instance,
    HINSTANCE,
    PWSTR,
    int
) {

    // started by the browser for the REICO Downloads extension: hand over the download and exit
    if (wcsstr(GetCommandLineW(), L"chrome-extension://")) return DlHostMain();

    Log(L"App starting...");

    EnsureRunOnStartup();

    LoadGlassConfig(); // before the first scan, so icons get tinted from the start

    ScanDesktop();

    const wchar_t CLASS_NAME[] =
        L"REICODesktop";

    WNDCLASSW wc{};

    wc.lpfnWndProc =
        WndProc;

    wc.hInstance =
        instance;

    wc.lpszClassName =
        CLASS_NAME;

    wc.hCursor =
        LoadCursor(
            nullptr,
            IDC_ARROW
        );

    // CS_DBLCLKS is required to receive WM_LBUTTONDBLCLK (fix #3).
    wc.style = CS_DBLCLKS;

    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1)); // the REICO logo from reico.rc

    RegisterClassW(&wc);

    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);

    HWND hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_LAYERED,
        CLASS_NAME,
        L"REICO",
        WS_POPUP,
        0,
        0,
        screenW,
        screenH,
        nullptr,
        nullptr,
        instance,
        nullptr
    );

    if (!hwnd) {
        Log(L"CreateWindowExW FAILED");
        return 1;
    }

    g_hwnd = hwnd;

    // FIX #2: no more SetLayeredWindowAttributes/color-key. The window
    // is composited purely through UpdateLayeredWindow + ULW_ALPHA,
    // called from Render().
    CreateBackBuffer(screenW, screenH);

    ShowWindow(
        hwnd,
        SW_SHOW
    );

    // FIX #6 (overlay drawn on top of other windows): this is a desktop-
    // icon replacement, not a normal app window, so it must always sit
    // behind every other top-level window - never covering a window the
    // user clicks on/restores. A freshly-shown window is inserted at the
    // TOP of the z-order by default, so pin it to the bottom immediately.
    SetWindowPos(hwnd, HWND_BOTTOM, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    Log(L"Window created and shown");

    // Always-on background check: hides the overlay while a true
    // fullscreen app has focus, restores it automatically once that
    // app is gone or is only maximized (not fullscreen).
    SetTimer(hwnd, FULLSCREEN_TIMER_ID, 500, nullptr);

// Give Windows Explorer/Shell32 a moment to finish resolving
// desktop icons, then automatically retry (auto-F5) any missing icons.
SetTimer(hwnd, ICON_REFRESH_TIMER_ID, 1500, nullptr);

// FIX #7: Ctrl+Shift+F5 works as a manual rescan no matter which window
// currently has focus (see comment at HOTKEY_RESCAN_ID above).
if (!RegisterHotKey(hwnd, HOTKEY_RESCAN_ID, MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, VK_F5)) {
    LogF(L"RegisterHotKey (Ctrl+Shift+F5) failed, error %lu", GetLastError());
}

// Translucent teal glass for Explorer + cmd (see GLASS section above).
// Toggle with Ctrl+Shift+F6; tweak C:\\REICO\\glass.txt.
GlassInit(hwnd);

// Tray icon + settings window + live reload of glass.txt + presets.
UiInit();

Render(hwnd);

    MSG msg{};

    while (GetMessageW(
        &msg,
        nullptr,
        0,
        0
    )) {

        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    UiShutdown(); // Esc quits without WM_DESTROY - don't leave a dead tray icon behind

    return 0;
}
