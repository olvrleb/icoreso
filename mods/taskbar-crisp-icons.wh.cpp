// ==WindhawkMod==
// @id              taskbar-crisp-icons
// @name            Crisp Taskbar Icons
// @description     Stops Windows from blurry/pixelated icon scaling on the taskbar by re-rendering icons at the exact pixel size from the highest resolution image in the .ico/.exe, using a high-quality resampling filter
// @version         1.1.0
// @author          olvrleb
// @github          https://github.com/olvrleb
// @include         explorer.exe
// @compilerOptions -lgdi32 -luser32 -lole32 -lwindowscodecs -lshell32
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Crisp Taskbar Icons

Windows often shows taskbar icons (pinned shortcuts and running apps such as
File Explorer, Discord, Steam, ...) slightly blurry or pixelated, especially
at display scaling other than 100%. The reason is almost never that the app
lacks a good icon - most `.ico`/`.exe` files ship a 256x256 image - but how
Windows picks and scales the image:

* The taskbar asks for an icon of e.g. 30x30 px (24 px at 125% scaling).
  The `.ico` has 16, 32, 48 and 256 px images, so Windows takes the **32 px**
  image and squeezes it to 30 px with a cheap filter. A 6% downscale smears
  every edge over two pixels - blurry.
* Running windows hand the taskbar a 32x32 icon (`WM_GETICON`). At 150%
  scaling the taskbar needs 36x36, so the 32 px icon is **upscaled** -
  blurry/pixelated.
* `DrawIconEx` / `CopyImage` stretch icons with nearest-neighbour style
  sampling - jagged.

This mod fixes all of these inside `explorer.exe` (which hosts the taskbar).
The idea: Windows always shrinks the icon itself, so the mod does the
shrinking first, with a high-quality filter, and hands Windows an icon that
already has the exact final pixel size. Windows' own downscale then has
nothing left to do.

1. **The taskbar's own downscale** (`IWICImagingFactory::CreateBitmapFromHICON`):
   the taskbar converts icons (often 32 px) to bitmaps and shrinks them to its
   icon size (24 px at 100%) itself. The mod hands it an icon rendered at the
   exact final size instead - straight from the 256 px image when it knows
   which file the icon came from.
2. **Shortcut, pinned and file icons** (`PrivateExtractIconsW`, `LoadImageW`):
   rendered from a high-resolution image (by default the smallest one at
   least 2x the target, usually 256 px) with a Lanczos-3 filter in
   premultiplied alpha, even when the file has an image of the requested
   size. Hand-made 16 px images are kept.
3. **Running app icons** (`WM_GETICON`, `GetClassLongPtrW`): any window icon
   that isn't exactly the taskbar's pixel size is replaced with the app's
   `.exe` icon rendered at that size (after a pixel comparison confirms it is
   the same icon), or else a high-quality resample of the window's icon.
4. **Icon stretching** (`DrawIconEx`, `CopyImage`): any icon drawn or copied
   at a different size is resampled with the high-quality filter.

Changes 1 and 3 only apply to the taskbar's own code, not Alt+Tab, Start or
File Explorer windows.

## After enabling

The mod clears Explorer's icon cache and restarts Explorer by itself, once,
right after it is enabled and after each icon setting change (turn off
*Restart Explorer automatically* to do this yourself). Open File Explorer
windows close when this happens.

If icons still look unchanged, turn on *Debug logging* and check the log for
`CreateBitmapFromHICON`, `Re-rendered` and `Window icon` lines.

## Notes

* Nothing on disk is modified - no `.ico` or `.exe` files are touched.
* The result can't be finer than the screen: at 100% scaling a taskbar icon
  is 24x24 physical pixels. The mod makes those pixels a perfectly
  anti-aliased render of the 256 px artwork; at 125%/150% scaling it gets
  30/36 px to work with and looks correspondingly finer.
* A source image can't contain detail it doesn't have: an app whose icon
  file only contains a 32 px image will be upscaled smoothly, not magically
  sharpened.
* With taskbars on several monitors with different scaling, the primary
  monitor's size is used for the taskbar downscale fix.
* Store/UWP apps already ship scale-specific PNGs and are unaffected.
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- improveExtractedIcons: true
  $name: Re-render shortcut, pinned and file icons
  $description: Render icons from the best (largest) image in the .ico/.exe instead of letting Windows scale the nearest size
- alwaysUseLargeSource: true
  $name: Always render from a high-resolution image
  $description: Even when the .ico/.exe has an image of exactly the requested size, render from a larger one. Hand-made 16 px images are still used as is
- fixTaskbarScaling: true
  $name: Hand the taskbar icons at its exact pixel size
  $description: The taskbar converts icons to bitmaps and shrinks them itself (e.g. 32 px to 24 px) with a blurry filter. This gives it icons already rendered at the final size, so it has nothing left to shrink
- upgradeWindowIcons: true
  $name: Fix running app icons
  $description: When a running app gives the taskbar an icon that isn't exactly the taskbar's pixel size, replace it with the app's exe icon rendered at that size (or a high-quality resample of the app's icon)
- autoRestartExplorer: true
  $name: Restart Explorer automatically
  $description: Clears the icon cache and restarts Windows Explorer once when the mod is enabled or an icon setting changes, so the new icons show up. Open File Explorer windows will close
- improveIconStretching: true
  $name: High-quality icon stretching
  $description: Resample icons drawn or copied at a different size (DrawIconEx, CopyImage) with the selected filter
- taskbarIconSize: 24
  $name: Taskbar icon size (logical pixels)
  $description: Size of taskbar icons at 100% scaling. 24 for default Windows 10/11 taskbars, 16 for small taskbar buttons. It is multiplied by the monitor's scaling factor
- sourcePreference: auto
  $name: Source image preference
  $description: Which image of the .ico/.exe to scale from when the exact size isn't available
  $options:
  - auto: Auto - smallest image at least 2x the target size (sharpest)
  - nearestLarger: Nearest larger image
  - largest: Always the largest image
- filter: lanczos3
  $name: Resampling filter
  $options:
  - lanczos3: Lanczos-3 (sharpest)
  - catmullRom: Catmull-Rom bicubic (sharp)
  - mitchell: Mitchell bicubic (smooth)
  - box: Area average (soft, no ringing)
- refreshOnLoad: true
  $name: Refresh shell icons when the mod loads
  $description: Asks Explorer to reload its in-memory icons so the change is visible without restarting
- debugLogging: false
  $name: Debug logging
*/
// ==/WindhawkModSettings==

#include <windows.h>
#include <shlobj.h>
#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <list>
#include <memory>
#include <string>
#include <vector>

#ifndef RESOURCE_ENUM_LN
#define RESOURCE_ENUM_LN 0x0001
#endif

////////////////////////////////////////////////////////////////////////////////
// Settings

enum class SourcePreference { Auto, NearestLarger, Largest };
enum class Filter { Lanczos3, CatmullRom, Mitchell, Box };

struct Settings {
    bool improveExtractedIcons = true;
    bool alwaysUseLargeSource = true;
    bool fixTaskbarScaling = true;
    bool upgradeWindowIcons = true;
    bool autoRestartExplorer = true;
    bool improveIconStretching = true;
    int taskbarIconSize = 24;
    SourcePreference sourcePreference = SourcePreference::Auto;
    Filter filter = Filter::Lanczos3;
    bool refreshOnLoad = true;
    bool debugLogging = false;
};

static Settings g_settings;

#define LOG(...)                       \
    do {                               \
        if (g_settings.debugLogging) { \
            Wh_Log(__VA_ARGS__);       \
        }                              \
    } while (0)

// Prevents our own processing from re-entering the hooks.
static thread_local bool g_inHook = false;

struct HookGuard {
    bool active;
    HookGuard() : active(!g_inHook) { g_inHook = true; }
    ~HookGuard() {
        if (active) {
            g_inHook = false;
        }
    }
};

////////////////////////////////////////////////////////////////////////////////
// Pixel helpers

// Straight-alpha BGRA pixels, top-down.
struct IconPixels {
    int w = 0;
    int h = 0;
    bool isIcon = true;
    std::vector<DWORD> px;
};

static bool GetIconPixels(HICON icon, IconPixels& out) {
    ICONINFO ii{};
    if (!GetIconInfo(icon, &ii)) {
        return false;
    }

    bool ok = false;
    BITMAP bm{};
    // Monochrome icons (no color bitmap) are left alone.
    if (ii.hbmColor && GetObjectW(ii.hbmColor, sizeof(bm), &bm) &&
        bm.bmWidth > 0 && bm.bmHeight > 0 && bm.bmWidth <= 1024 &&
        bm.bmHeight <= 1024) {
        int w = bm.bmWidth;
        int h = bm.bmHeight;
        out.w = w;
        out.h = h;
        out.isIcon = ii.fIcon != FALSE;
        out.px.assign((size_t)w * h, 0);

        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        BITMAPINFO biMask = bi;

        HDC dc = GetDC(nullptr);
        ok = GetDIBits(dc, ii.hbmColor, 0, h, out.px.data(), &bi,
                       DIB_RGB_COLORS) == h;

        bool hasAlpha = false;
        for (DWORD p : out.px) {
            if (p & 0xFF000000) {
                hasAlpha = true;
                break;
            }
        }

        if (ok && !hasAlpha) {
            // Old-style icon: transparency comes from the AND mask.
            std::vector<DWORD> mask((size_t)w * h, 0);
            bool gotMask = ii.hbmMask &&
                           GetDIBits(dc, ii.hbmMask, 0, h, mask.data(),
                                     &biMask, DIB_RGB_COLORS) == h;
            for (size_t i = 0; i < out.px.size(); i++) {
                bool transparent = gotMask && (mask[i] & 0x00FFFFFF);
                out.px[i] = transparent ? 0 : (out.px[i] | 0xFF000000);
            }
        }
        ReleaseDC(nullptr, dc);
    }

    if (ii.hbmColor) {
        DeleteObject(ii.hbmColor);
    }
    if (ii.hbmMask) {
        DeleteObject(ii.hbmMask);
    }
    return ok;
}

static HICON CreateIconFromPixels(const IconPixels& p) {
    if (p.w <= 0 || p.h <= 0) {
        return nullptr;
    }

    BITMAPV5HEADER bh{};
    bh.bV5Size = sizeof(bh);
    bh.bV5Width = p.w;
    bh.bV5Height = -p.h;
    bh.bV5Planes = 1;
    bh.bV5BitCount = 32;
    bh.bV5Compression = BI_BITFIELDS;
    bh.bV5RedMask = 0x00FF0000;
    bh.bV5GreenMask = 0x0000FF00;
    bh.bV5BlueMask = 0x000000FF;
    bh.bV5AlphaMask = 0xFF000000;

    void* bits = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(dc, (BITMAPINFO*)&bh, DIB_RGB_COLORS,
                                     &bits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    if (!color) {
        return nullptr;
    }
    memcpy(bits, p.px.data(), p.px.size() * sizeof(DWORD));
    GdiFlush();

    // AND mask: 1 = transparent. Rows are WORD aligned.
    int stride = ((p.w + 15) / 16) * 2;
    std::vector<BYTE> maskBits((size_t)stride * p.h, 0);
    for (int y = 0; y < p.h; y++) {
        for (int x = 0; x < p.w; x++) {
            if ((p.px[(size_t)y * p.w + x] >> 24) == 0) {
                maskBits[(size_t)y * stride + x / 8] |= (BYTE)(0x80 >> (x % 8));
            }
        }
    }
    HBITMAP mask = CreateBitmap(p.w, p.h, 1, 1, maskBits.data());

    HICON icon = nullptr;
    if (mask) {
        ICONINFO ii{};
        ii.fIcon = p.isIcon;
        ii.hbmColor = color;
        ii.hbmMask = mask;
        icon = CreateIconIndirect(&ii);
        DeleteObject(mask);
    }
    DeleteObject(color);
    return icon;
}

static uint64_t HashBytes(uint64_t h, const void* data, size_t size) {
    const BYTE* b = (const BYTE*)data;
    for (size_t i = 0; i < size; i++) {
        h ^= b[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static uint64_t HashPixels(const IconPixels& p, int a, int b, int c) {
    uint64_t h = 14695981039346656037ULL;
    int header[] = {p.w, p.h, p.isIcon, a, b, c};
    h = HashBytes(h, header, sizeof(header));
    return HashBytes(h, p.px.data(), p.px.size() * sizeof(DWORD));
}

////////////////////////////////////////////////////////////////////////////////
// High-quality resampling (separable, premultiplied alpha)

static double Sinc(double x) {
    if (x == 0.0) {
        return 1.0;
    }
    x *= 3.14159265358979323846;
    return std::sin(x) / x;
}

static double Cubic(double x, double B, double C) {
    if (x < 1.0) {
        return ((12 - 9 * B - 6 * C) * x * x * x +
                (-18 + 12 * B + 6 * C) * x * x + (6 - 2 * B)) /
               6.0;
    }
    if (x < 2.0) {
        return ((-B - 6 * C) * x * x * x + (6 * B + 30 * C) * x * x +
                (-12 * B - 48 * C) * x + (8 * B + 24 * C)) /
               6.0;
    }
    return 0.0;
}

static double FilterSupport(Filter f) {
    switch (f) {
        case Filter::Lanczos3:
            return 3.0;
        case Filter::Box:
            return 0.5;
        default:
            return 2.0;
    }
}

static double FilterWeight(Filter f, double x) {
    x = std::fabs(x);
    switch (f) {
        case Filter::Lanczos3:
            return x < 3.0 ? Sinc(x) * Sinc(x / 3.0) : 0.0;
        case Filter::CatmullRom:
            return Cubic(x, 0.0, 0.5);
        case Filter::Mitchell:
            return Cubic(x, 1.0 / 3.0, 1.0 / 3.0);
        case Filter::Box:
            return x <= 0.5 ? 1.0 : 0.0;
    }
    return 0.0;
}

struct Contrib {
    int start = 0;
    std::vector<float> weights;
};

static std::vector<Contrib> ComputeContribs(int srcSize, int dstSize,
                                            Filter f) {
    double scale = (double)dstSize / srcSize;
    // Box upscaling is nearest neighbour, which is exactly what we avoid.
    if (f == Filter::Box && scale > 1.0) {
        f = Filter::CatmullRom;
    }
    // When shrinking, stretch the kernel so it covers every source pixel.
    double filterScale = scale < 1.0 ? 1.0 / scale : 1.0;
    double support = FilterSupport(f) * filterScale;

    std::vector<Contrib> contribs(dstSize);
    for (int i = 0; i < dstSize; i++) {
        double center = (i + 0.5) / scale;
        int first = (int)std::floor(center - support);
        int last = (int)std::ceil(center + support);
        first = std::max(first, 0);
        last = std::min(last, srcSize - 1);

        Contrib& c = contribs[i];
        c.start = first;
        double sum = 0.0;
        for (int j = first; j <= last; j++) {
            double w;
            if (f == Filter::Box) {
                // Exact area coverage of source pixel j.
                double left = std::max(center - 0.5 * filterScale, (double)j);
                double right =
                    std::min(center + 0.5 * filterScale, (double)j + 1);
                w = std::max(0.0, right - left);
            } else {
                w = FilterWeight(f, (j + 0.5 - center) / filterScale);
            }
            c.weights.push_back((float)w);
            sum += w;
        }
        if (sum != 0.0) {
            for (float& w : c.weights) {
                w = (float)(w / sum);
            }
        }
    }
    return contribs;
}

static IconPixels ResamplePixels(const IconPixels& src, int dw, int dh,
                                 Filter f) {
    int sw = src.w;
    int sh = src.h;

    // To premultiplied float.
    std::vector<float> in((size_t)sw * sh * 4);
    for (size_t i = 0; i < src.px.size(); i++) {
        DWORD p = src.px[i];
        float a = (float)(p >> 24) / 255.0f;
        in[i * 4 + 0] = (float)(p & 0xFF) / 255.0f * a;
        in[i * 4 + 1] = (float)((p >> 8) & 0xFF) / 255.0f * a;
        in[i * 4 + 2] = (float)((p >> 16) & 0xFF) / 255.0f * a;
        in[i * 4 + 3] = a;
    }

    std::vector<Contrib> cx = ComputeContribs(sw, dw, f);
    std::vector<Contrib> cy = ComputeContribs(sh, dh, f);

    // Horizontal pass: sw x sh -> dw x sh.
    std::vector<float> tmp((size_t)dw * sh * 4, 0.0f);
    for (int y = 0; y < sh; y++) {
        const float* row = &in[(size_t)y * sw * 4];
        float* out = &tmp[(size_t)y * dw * 4];
        for (int x = 0; x < dw; x++) {
            const Contrib& c = cx[x];
            float acc[4] = {};
            for (size_t k = 0; k < c.weights.size(); k++) {
                const float* s = &row[(size_t)(c.start + k) * 4];
                float w = c.weights[k];
                acc[0] += s[0] * w;
                acc[1] += s[1] * w;
                acc[2] += s[2] * w;
                acc[3] += s[3] * w;
            }
            memcpy(&out[(size_t)x * 4], acc, sizeof(acc));
        }
    }

    // Vertical pass: dw x sh -> dw x dh, then back to straight-alpha BGRA.
    IconPixels dst;
    dst.w = dw;
    dst.h = dh;
    dst.isIcon = src.isIcon;
    dst.px.assign((size_t)dw * dh, 0);
    for (int y = 0; y < dh; y++) {
        const Contrib& c = cy[y];
        for (int x = 0; x < dw; x++) {
            float acc[4] = {};
            for (size_t k = 0; k < c.weights.size(); k++) {
                const float* s = &tmp[((size_t)(c.start + k) * dw + x) * 4];
                float w = c.weights[k];
                acc[0] += s[0] * w;
                acc[1] += s[1] * w;
                acc[2] += s[2] * w;
                acc[3] += s[3] * w;
            }

            // Clamp away filter overshoot (ringing) in premultiplied space.
            float a = std::min(std::max(acc[3], 0.0f), 1.0f);
            BYTE A = (BYTE)std::lround(a * 255.0f);
            if (A == 0) {
                continue;
            }
            BYTE ch[3];
            for (int i = 0; i < 3; i++) {
                float v = std::min(std::max(acc[i], 0.0f), a) / a;
                ch[i] = (BYTE)std::lround(v * 255.0f);
            }
            dst.px[(size_t)y * dw + x] =
                ((DWORD)A << 24) | ((DWORD)ch[2] << 16) | ((DWORD)ch[1] << 8) |
                ch[0];
        }
    }
    return dst;
}

// Mean absolute difference of premultiplied channels, 0..1.
static double PixelDifference(const IconPixels& a, const IconPixels& b) {
    if (a.w != b.w || a.h != b.h || a.px.empty()) {
        return 1.0;
    }
    double total = 0.0;
    for (size_t i = 0; i < a.px.size(); i++) {
        DWORD pa = a.px[i];
        DWORD pb = b.px[i];
        int aa = pa >> 24;
        int ab = pb >> 24;
        total += std::abs(aa - ab);
        for (int shift = 0; shift <= 16; shift += 8) {
            int ca = ((pa >> shift) & 0xFF) * aa / 255;
            int cb = ((pb >> shift) & 0xFF) * ab / 255;
            total += std::abs(ca - cb);
        }
    }
    return total / (a.px.size() * 4.0 * 255.0);
}

////////////////////////////////////////////////////////////////////////////////
// Icon groups (.ico files and RT_GROUP_ICON resources)

#pragma pack(push, 2)
struct GRPICONDIRENTRY {
    BYTE bWidth;
    BYTE bHeight;
    BYTE bColorCount;
    BYTE bReserved;
    WORD wPlanes;
    WORD wBitCount;
    DWORD dwBytesInRes;
    WORD nId;
};
#pragma pack(pop)

struct ICONDIRENTRY {
    BYTE bWidth;
    BYTE bHeight;
    BYTE bColorCount;
    BYTE bReserved;
    WORD wPlanes;
    WORD wBitCount;
    DWORD dwBytesInRes;
    DWORD dwImageOffset;
};

struct ICONDIR {
    WORD idReserved;
    WORD idType;
    WORD idCount;
};

struct IconEntry {
    int w = 0;
    int h = 0;
    int bpp = 0;
    const BYTE* data = nullptr;
    DWORD size = 0;
};

struct IconGroup {
    std::vector<IconEntry> entries;
    // Backing memory: either a data-file module or a copy of an .ico file.
    HMODULE module = nullptr;
    std::vector<BYTE> fileData;

    IconGroup() = default;
    IconGroup(const IconGroup&) = delete;
    IconGroup& operator=(const IconGroup&) = delete;
    ~IconGroup() {
        if (module) {
            FreeLibrary(module);
        }
    }
};

// Reads the real dimensions and bit depth from the image data, which is more
// reliable than the directory (it says 0 for 256 px and larger images).
static bool ParseEntryData(IconEntry& e) {
    static const BYTE kPng[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    if (e.size >= 24 && memcmp(e.data, kPng, 8) == 0) {
        auto be32 = [](const BYTE* b) {
            return (int)(((DWORD)b[0] << 24) | ((DWORD)b[1] << 16) |
                         ((DWORD)b[2] << 8) | b[3]);
        };
        e.w = be32(e.data + 16);
        e.h = be32(e.data + 20);
        e.bpp = 32;
    } else if (e.size >= sizeof(BITMAPINFOHEADER)) {
        BITMAPINFOHEADER bih;
        memcpy(&bih, e.data, sizeof(bih));
        if (bih.biSize < sizeof(BITMAPINFOHEADER)) {
            return false;
        }
        e.w = bih.biWidth;
        e.h = std::abs(bih.biHeight) / 2;
        e.bpp = bih.biBitCount * bih.biPlanes;
    } else {
        return false;
    }
    return e.w > 0 && e.h > 0 && e.w <= 1024 && e.h <= 1024;
}

static bool LoadGroupFromModule(HMODULE module, LPCWSTR name, IconGroup& g) {
    HRSRC res = FindResourceW(module, name, MAKEINTRESOURCEW(14) /* RT_GROUP_ICON */);
    if (!res) {
        return false;
    }
    HGLOBAL global = LoadResource(module, res);
    const BYTE* dir = global ? (const BYTE*)LockResource(global) : nullptr;
    DWORD dirSize = SizeofResource(module, res);
    if (!dir || dirSize < sizeof(ICONDIR)) {
        return false;
    }

    ICONDIR header;
    memcpy(&header, dir, sizeof(header));
    if (header.idType != 1 ||
        dirSize < sizeof(ICONDIR) + header.idCount * sizeof(GRPICONDIRENTRY)) {
        return false;
    }

    for (WORD i = 0; i < header.idCount; i++) {
        GRPICONDIRENTRY de;
        memcpy(&de, dir + sizeof(ICONDIR) + i * sizeof(GRPICONDIRENTRY),
               sizeof(de));
        HRSRC iconRes = FindResourceW(module, MAKEINTRESOURCEW(de.nId),
                                      MAKEINTRESOURCEW(3) /* RT_ICON */);
        if (!iconRes) {
            continue;
        }
        HGLOBAL iconGlobal = LoadResource(module, iconRes);
        IconEntry e;
        e.data = iconGlobal ? (const BYTE*)LockResource(iconGlobal) : nullptr;
        e.size = SizeofResource(module, iconRes);
        if (e.data && ParseEntryData(e)) {
            g.entries.push_back(e);
        }
    }
    return !g.entries.empty();
}

static bool LoadGroupFromIcoData(IconGroup& g) {
    const std::vector<BYTE>& d = g.fileData;
    if (d.size() < sizeof(ICONDIR)) {
        return false;
    }
    ICONDIR header;
    memcpy(&header, d.data(), sizeof(header));
    if (header.idReserved != 0 || header.idType != 1 ||
        d.size() < sizeof(ICONDIR) + header.idCount * sizeof(ICONDIRENTRY)) {
        return false;
    }
    for (WORD i = 0; i < header.idCount; i++) {
        ICONDIRENTRY de;
        memcpy(&de, d.data() + sizeof(ICONDIR) + i * sizeof(ICONDIRENTRY),
               sizeof(de));
        if (de.dwImageOffset >= d.size() ||
            de.dwBytesInRes > d.size() - de.dwImageOffset) {
            continue;
        }
        IconEntry e;
        e.data = d.data() + de.dwImageOffset;
        e.size = de.dwBytesInRes;
        if (ParseEntryData(e)) {
            g.entries.push_back(e);
        }
    }
    return !g.entries.empty();
}

static bool ReadFileData(LPCWSTR path, std::vector<BYTE>& out) {
    HANDLE file = CreateFileW(path, GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE |
                                  FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    bool ok = false;
    LARGE_INTEGER size;
    if (GetFileSizeEx(file, &size) && size.QuadPart > 0 &&
        size.QuadPart <= 16 * 1024 * 1024) {
        out.resize((size_t)size.QuadPart);
        DWORD read = 0;
        ok = ReadFile(file, out.data(), (DWORD)out.size(), &read, nullptr) &&
             read == out.size();
    }
    CloseHandle(file);
    return ok;
}

struct EnumGroupContext {
    int target;
    int current;
    bool found;
    bool isId;
    WORD id;
    std::wstring name;
};

static BOOL CALLBACK EnumGroupProc(HMODULE, LPCWSTR, LPWSTR name,
                                   LONG_PTR param) {
    auto* ctx = (EnumGroupContext*)param;
    if (ctx->current++ != ctx->target) {
        return TRUE;
    }
    ctx->found = true;
    ctx->isId = IS_INTRESOURCE(name);
    if (ctx->isId) {
        ctx->id = (WORD)(ULONG_PTR)name;
    } else {
        ctx->name = name;
    }
    return FALSE;
}

// Same index semantics as ExtractIconEx: index >= 0 is the n-th icon group,
// a negative index is a resource id.
static bool LoadGroupFromFile(LPCWSTR path, int index, IconGroup& g) {
    HMODULE module = LoadLibraryExW(
        path, nullptr,
        LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (module) {
        g.module = module;
        if (index < 0) {
            return LoadGroupFromModule(module, MAKEINTRESOURCEW(-index), g);
        }
        EnumGroupContext ctx{index, 0, false, false, 0, {}};
        EnumResourceNamesExW(module, MAKEINTRESOURCEW(14) /* RT_GROUP_ICON */,
                             EnumGroupProc, (LONG_PTR)&ctx, RESOURCE_ENUM_LN,
                             0);
        if (!ctx.found) {
            return false;
        }
        return LoadGroupFromModule(
            module, ctx.isId ? MAKEINTRESOURCEW(ctx.id) : ctx.name.c_str(), g);
    }

    // Not a PE file - maybe a plain .ico, which only has index 0.
    if (index != 0 || !ReadFileData(path, g.fileData)) {
        return false;
    }
    return LoadGroupFromIcoData(g);
}

// Whether an image of exactly the requested size should be used as is.
// Small images are usually hand-tuned and crisper than any downscale.
static bool UseExactSize(int size) {
    return !g_settings.alwaysUseLargeSource || size <= 16;
}

static const IconEntry* PickEntry(const IconGroup& g, int cx, int cy,
                                  bool preferExact, bool* exact) {
    *exact = false;
    int maxBpp = 0;
    for (const IconEntry& e : g.entries) {
        maxBpp = std::max(maxBpp, e.bpp);
    }
    // Ignore low-color images if true-color ones exist.
    int minBpp = std::min(maxBpp, 24);
    int target = std::max(cx, cy);

    auto better = [](const IconEntry* a, const IconEntry& b, bool smaller) {
        if (!a) {
            return true;
        }
        if (b.w != a->w) {
            return smaller ? b.w < a->w : b.w > a->w;
        }
        return b.bpp > a->bpp;
    };

    const IconEntry* exactEntry = nullptr;
    const IconEntry* largest = nullptr;
    const IconEntry* nearestLarger = nullptr;
    const IconEntry* nearestDouble = nullptr;
    for (const IconEntry& e : g.entries) {
        if (e.bpp < minBpp) {
            continue;
        }
        if (e.w == cx && e.h == cy && better(exactEntry, e, true)) {
            exactEntry = &e;
        }
        if (better(largest, e, false)) {
            largest = &e;
        }
        if (e.w >= target && better(nearestLarger, e, true)) {
            nearestLarger = &e;
        }
        if (e.w >= target * 2 && better(nearestDouble, e, true)) {
            nearestDouble = &e;
        }
    }

    if (exactEntry && preferExact) {
        *exact = true;
        return exactEntry;
    }
    switch (g_settings.sourcePreference) {
        case SourcePreference::Auto:
            if (nearestDouble) {
                return nearestDouble;
            }
            [[fallthrough]];
        case SourcePreference::NearestLarger:
            if (nearestLarger) {
                return nearestLarger;
            }
            [[fallthrough]];
        case SourcePreference::Largest:
            break;
    }
    return largest;
}

enum class ExactSize {
    Skip,       // return false if the exact size is used (Windows has it)
    Use,        // use the exact size if available
    BySetting,  // use it only if UseExactSize() says so
};

// Renders the group at exactly cx x cy.
static bool RenderGroup(const IconGroup& g, int cx, int cy, ExactSize mode,
                        IconPixels& out) {
    bool preferExact = mode == ExactSize::Use ||
                       UseExactSize(std::max(cx, cy));
    bool exact;
    const IconEntry* e = PickEntry(g, cx, cy, preferExact, &exact);
    if (!e || (exact && mode == ExactSize::Skip)) {
        return false;
    }

    // CreateIconFromResourceEx wants DWORD-aligned data.
    std::vector<DWORD> aligned((e->size + 3) / 4);
    memcpy(aligned.data(), e->data, e->size);
    HICON native = CreateIconFromResourceEx((PBYTE)aligned.data(), e->size,
                                            TRUE, 0x00030000, 0, 0,
                                            LR_DEFAULTCOLOR);
    if (!native) {
        return false;
    }
    IconPixels src;
    bool ok = GetIconPixels(native, src);
    DestroyIcon(native);
    if (!ok) {
        return false;
    }

    if (src.w == cx && src.h == cy) {
        out = std::move(src);
    } else {
        out = ResamplePixels(src, cx, cy, g_settings.filter);
    }
    return true;
}

static HICON RenderGroupIcon(const IconGroup& g, int cx, int cy) {
    IconPixels p;
    if (!RenderGroup(g, cx, cy, ExactSize::Skip, p)) {
        return nullptr;
    }
    return CreateIconFromPixels(p);
}

////////////////////////////////////////////////////////////////////////////////
// Cache of icons we created, keyed by content hash

class IconCache {
   public:
    explicit IconCache(size_t capacity) : capacity_(capacity) {}

    bool Lookup(uint64_t key, HICON* icon) {
        AcquireSRWLockExclusive(&lock_);
        bool found = false;
        for (auto it = items_.begin(); it != items_.end(); ++it) {
            if (it->key == key) {
                *icon = it->icon;
                items_.splice(items_.begin(), items_, it);
                found = true;
                break;
            }
        }
        ReleaseSRWLockExclusive(&lock_);
        return found;
    }

    // Returns the icon to use (an existing one if another thread won).
    HICON Insert(uint64_t key, HICON icon) {
        AcquireSRWLockExclusive(&lock_);
        for (const Item& item : items_) {
            if (item.key == key) {
                HICON existing = item.icon;
                ReleaseSRWLockExclusive(&lock_);
                if (icon && icon != existing) {
                    DestroyIcon(icon);
                }
                return existing;
            }
        }
        items_.push_front({key, icon});
        HICON evicted = nullptr;
        if (items_.size() > capacity_) {
            evicted = items_.back().icon;
            items_.pop_back();
        }
        ReleaseSRWLockExclusive(&lock_);
        if (evicted) {
            DestroyIcon(evicted);
        }
        return icon;
    }

    void Clear(bool destroyIcons) {
        AcquireSRWLockExclusive(&lock_);
        std::list<Item> items;
        items.swap(items_);
        ReleaseSRWLockExclusive(&lock_);
        for (const Item& item : items) {
            if (destroyIcons && item.icon) {
                DestroyIcon(item.icon);
            }
        }
    }

   private:
    struct Item {
        uint64_t key;
        HICON icon;  // nullptr caches "no improvement possible"
    };
    SRWLOCK lock_ = SRWLOCK_INIT;
    std::list<Item> items_;
    size_t capacity_;
};

static IconCache g_stretchCache(128);
static IconCache g_windowIconCache(256);

// Returns a cached, high-quality resampled copy of an icon (owned by the
// cache), or nullptr if not applicable.
static HICON GetStretchedIcon(const IconPixels& px, int cx, int cy) {
    uint64_t key = HashPixels(px, cx, cy, (int)g_settings.filter);
    HICON icon;
    if (g_stretchCache.Lookup(key, &icon)) {
        return icon;
    }
    icon = CreateIconFromPixels(ResamplePixels(px, cx, cy, g_settings.filter));
    return g_stretchCache.Insert(key, icon);
}

////////////////////////////////////////////////////////////////////////////////
// Window icon upgrade

using GetDpiForMonitor_t = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
static GetDpiForMonitor_t g_getDpiForMonitor = nullptr;

static int GetTaskbarIconPixelSize(HWND hwnd) {
    UINT dpi = 0;
    if (g_getDpiForMonitor) {
        HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);
        UINT dpiY;
        if (FAILED(g_getDpiForMonitor(monitor, 0 /* MDT_EFFECTIVE_DPI */, &dpi,
                                      &dpiY))) {
            dpi = 0;
        }
    }
    if (!dpi) {
        HDC dc = GetDC(nullptr);
        dpi = GetDeviceCaps(dc, LOGPIXELSX);
        ReleaseDC(nullptr, dc);
    }
    return MulDiv(g_settings.taskbarIconSize, dpi, 96);
}

static bool GetProcessImagePath(DWORD pid, std::wstring& path) {
    HANDLE process =
        OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) {
        return false;
    }
    WCHAR buffer[MAX_PATH * 2];
    DWORD size = ARRAYSIZE(buffer);
    bool ok = QueryFullProcessImageNameW(process, 0, buffer, &size);
    CloseHandle(process);
    if (ok) {
        path.assign(buffer, size);
    }
    return ok;
}

////////////////////////////////////////////////////////////////////////////////
// Where icons came from
//
// Icons get copied around (image lists, CopyIcon, ...) before the taskbar
// draws them, so they're recognized by their pixels. Knowing the source file
// lets the final taskbar-sized icon be rendered straight from the 256 px
// image instead of from an intermediate 32 px copy.

struct IconSource {
    uint64_t hash;
    std::wstring file;
    int index;
};

static SRWLOCK g_sourcesLock = SRWLOCK_INIT;
static std::list<IconSource> g_sources;

static uint64_t HashForSource(const IconPixels& p) {
    return HashPixels(p, 0x50524353, 0, 0);
}

static void RememberIconSource(HICON icon, LPCWSTR file, int index) {
    IconPixels p;
    if (!GetIconPixels(icon, p)) {
        return;
    }
    uint64_t hash = HashForSource(p);
    AcquireSRWLockExclusive(&g_sourcesLock);
    for (auto it = g_sources.begin(); it != g_sources.end(); ++it) {
        if (it->hash == hash) {
            g_sources.erase(it);
            break;
        }
    }
    g_sources.push_front({hash, file, index});
    if (g_sources.size() > 512) {
        g_sources.pop_back();
    }
    ReleaseSRWLockExclusive(&g_sourcesLock);
}

static bool FindIconSource(const IconPixels& p, std::wstring& file,
                           int& index) {
    uint64_t hash = HashForSource(p);
    bool found = false;
    AcquireSRWLockShared(&g_sourcesLock);
    for (const IconSource& s : g_sources) {
        if (s.hash == hash) {
            file = s.file;
            index = s.index;
            found = true;
            break;
        }
    }
    ReleaseSRWLockShared(&g_sourcesLock);
    return found;
}

////////////////////////////////////////////////////////////////////////////////
// Taskbar detection

// True if the code at `address` belongs to the taskbar (Taskbar.dll and
// Taskbar.View.dll on Windows 11, explorer.exe itself on Windows 10). Keeps
// Alt+Tab, Start and File Explorer windows out of the taskbar-only changes.
static bool IsTaskbarCode(void* address) {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)address, &module) ||
        !module) {
        return false;
    }
    return module == GetModuleHandleW(nullptr) ||
           module == GetModuleHandleW(L"Taskbar.dll") ||
           module == GetModuleHandleW(L"Taskbar.View.dll");
}

static std::wstring ModuleNameOf(void* address) {
    HMODULE module = nullptr;
    WCHAR path[MAX_PATH] = L"?";
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)address, &module)) {
        GetModuleFileNameW(module, path, ARRAYSIZE(path));
    }
    const WCHAR* name = wcsrchr(path, L'\\');
    return name ? name + 1 : path;
}

static HWND PrimaryTaskbarWindow() {
    return FindWindowW(L"Shell_TrayWnd", nullptr);
}

// Renders an icon at exactly size x size for the taskbar: from its source
// file's best image if known, otherwise by resampling its pixels. Owned by
// the cache.
static HICON GetTaskbarSizedIcon(const IconPixels& px, int size) {
    uint64_t key = HashPixels(px, size, 0x54424152,
                              (int)g_settings.filter * 16 +
                                  (int)g_settings.sourcePreference * 2 +
                                  g_settings.alwaysUseLargeSource);
    HICON icon;
    if (g_stretchCache.Lookup(key, &icon)) {
        return icon;
    }

    icon = nullptr;
    std::wstring file;
    int index;
    if (FindIconSource(px, file, index)) {
        IconGroup group;
        IconPixels rendered;
        if (LoadGroupFromFile(file.c_str(), index, group) &&
            RenderGroup(group, size, size, ExactSize::BySetting, rendered)) {
            LOG(L"Taskbar icon %dpx -> %dpx from source %s,%d", px.w, size,
                file.c_str(), index);
            icon = CreateIconFromPixels(rendered);
        }
    }
    if (!icon) {
        LOG(L"Taskbar icon %dpx -> %dpx resampled", px.w, size);
        icon = CreateIconFromPixels(
            ResamplePixels(px, size, size, g_settings.filter));
    }
    return g_stretchCache.Insert(key, icon);
}

// Called with the icon a running window gave the taskbar. Unless it already
// is exactly the taskbar's pixel size, returns the app's exe icon rendered at
// that size (if the window icon is the exe icon), or else a high-quality
// resample of the window icon. The returned icon is owned by the cache.
static HICON UpgradeWindowIcon(HWND hwnd, HICON icon, void* caller) {
    if (!icon || !g_settings.upgradeWindowIcons || g_inHook) {
        return icon;
    }
    HookGuard guard;

    if (!IsTaskbarCode(caller)) {
        return icon;
    }

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid || pid == GetCurrentProcessId()) {
        return icon;
    }

    int target = GetTaskbarIconPixelSize(hwnd);
    IconPixels original;
    if (!GetIconPixels(icon, original) || !original.isIcon ||
        original.w == target || original.w != original.h) {
        return icon;
    }

    uint64_t key = HashPixels(original, target, (int)pid,
                              (int)g_settings.filter * 16 +
                                  (int)g_settings.sourcePreference * 2 +
                                  g_settings.alwaysUseLargeSource);
    HICON upgraded;
    if (g_windowIconCache.Lookup(key, &upgraded)) {
        return upgraded ? upgraded : icon;
    }

    upgraded = nullptr;
    std::wstring exePath;
    IconGroup group;
    IconPixels exeAtOriginalSize;
    if (GetProcessImagePath(pid, exePath) &&
        LoadGroupFromFile(exePath.c_str(), 0, group) &&
        RenderGroup(group, original.w, original.h, ExactSize::Use,
                    exeAtOriginalSize)) {
        double diff = PixelDifference(original, exeAtOriginalSize);
        LOG(L"Window icon %dpx -> %dpx, exe %s, difference %d/1000", original.w,
            target, exePath.c_str(), (int)(diff * 1000));
        IconPixels exeAtTarget;
        if (diff < 0.06 && RenderGroup(group, target, target,
                                       ExactSize::BySetting, exeAtTarget)) {
            upgraded = CreateIconFromPixels(exeAtTarget);
        }
    }
    if (!upgraded) {
        // Custom window icon: at least scale it properly.
        LOG(L"Window icon %dpx -> %dpx resampled", original.w, target);
        upgraded = CreateIconFromPixels(
            ResamplePixels(original, target, target, g_settings.filter));
    }

    upgraded = g_windowIconCache.Insert(key, upgraded);
    return upgraded ? upgraded : icon;
}

////////////////////////////////////////////////////////////////////////////////
// Hooks: icon extraction

using PrivateExtractIconsW_t = UINT(WINAPI*)(LPCWSTR, int, int, int, HICON*,
                                             UINT*, UINT, UINT);
static PrivateExtractIconsW_t PrivateExtractIconsW_Original;

static UINT WINAPI PrivateExtractIconsW_Hook(LPCWSTR fileName, int index,
                                             int cxIcon, int cyIcon,
                                             HICON* icons, UINT* iconIds,
                                             UINT count, UINT flags) {
    UINT result = PrivateExtractIconsW_Original(fileName, index, cxIcon, cyIcon,
                                                icons, iconIds, count, flags);
    if (!g_settings.improveExtractedIcons || g_inHook || !fileName || !icons ||
        result == 0 || result == (UINT)-1 ||
        (flags & (LR_MONOCHROME | LR_SHARED | LR_VGACOLOR))) {
        return result;
    }
    HookGuard guard;

    // HIWORDs may hold a second size; icons then alternate between the two.
    int cx1 = LOWORD(cxIcon), cy1 = LOWORD(cyIcon);
    int cx2 = HIWORD(cxIcon), cy2 = HIWORD(cyIcon);
    bool twoSizes = cx2 && cy2;

    std::unique_ptr<IconGroup> group;
    int loadedGroup = -1;
    UINT n = std::min(result, count);
    for (UINT i = 0; i < n; i++) {
        if (!icons[i]) {
            continue;
        }
        int groupOffset = twoSizes ? (int)(i / 2) : (int)i;
        if (index < 0 && groupOffset != 0) {
            break;
        }
        int cx = (twoSizes && (i & 1)) ? cx2 : cx1;
        int cy = (twoSizes && (i & 1)) ? cy2 : cy1;
        if (cx <= 0 || cy <= 0) {
            continue;
        }

        if (loadedGroup != groupOffset) {
            loadedGroup = groupOffset;
            int groupIndex = index < 0 ? index : index + groupOffset;
            group = std::make_unique<IconGroup>();
            if (!LoadGroupFromFile(fileName, groupIndex, *group)) {
                group.reset();
            }
        }
        if (!group) {
            continue;
        }

        HICON better = RenderGroupIcon(*group, cx, cy);
        if (better) {
            LOG(L"Re-rendered %s,%d at %dx%d", fileName, index + groupOffset,
                cx, cy);
            DestroyIcon(icons[i]);
            icons[i] = better;
        }
        RememberIconSource(icons[i], fileName,
                           index < 0 ? index : index + groupOffset);
    }
    return result;
}

using LoadImageW_t = HANDLE(WINAPI*)(HINSTANCE, LPCWSTR, UINT, int, int, UINT);
static LoadImageW_t LoadImageW_Original;

static HANDLE WINAPI LoadImageW_Hook(HINSTANCE instance, LPCWSTR name,
                                     UINT type, int cx, int cy, UINT flags) {
    HANDLE result =
        LoadImageW_Original(instance, name, type, cx, cy, flags);
    const UINT allowedFlags =
        LR_LOADFROMFILE | LR_CREATEDIBSECTION | LR_LOADTRANSPARENT;
    if (!result || type != IMAGE_ICON || !g_settings.improveExtractedIcons ||
        g_inHook || cx <= 0 || cy <= 0 || !name || (flags & ~allowedFlags)) {
        return result;
    }
    HookGuard guard;

    IconGroup group;
    bool loaded;
    if (flags & LR_LOADFROMFILE) {
        loaded = ReadFileData(name, group.fileData) &&
                 LoadGroupFromIcoData(group);
    } else {
        loaded = instance && LoadGroupFromModule(instance, name, group);
    }
    if (!loaded) {
        return result;
    }

    HICON better = RenderGroupIcon(group, cx, cy);
    if (better) {
        DestroyIcon((HICON)result);
        result = better;
    }

    if (flags & LR_LOADFROMFILE) {
        RememberIconSource((HICON)result, name, 0);
    } else if (IS_INTRESOURCE(name)) {
        WCHAR modulePath[MAX_PATH];
        if (GetModuleFileNameW(instance, modulePath, ARRAYSIZE(modulePath))) {
            RememberIconSource((HICON)result, modulePath,
                               -(int)(ULONG_PTR)name);
        }
    }
    return result;
}

////////////////////////////////////////////////////////////////////////////////
// Hooks: icon stretching

using CopyImage_t = HANDLE(WINAPI*)(HANDLE, UINT, int, int, UINT);
static CopyImage_t CopyImage_Original;

static HANDLE WINAPI CopyImage_Hook(HANDLE image, UINT type, int cx, int cy,
                                    UINT flags) {
    const UINT allowedFlags =
        LR_COPYDELETEORG | LR_COPYRETURNORG | LR_CREATEDIBSECTION;
    if (type != IMAGE_ICON || !image || !g_settings.improveIconStretching ||
        g_inHook || cx <= 0 || cy <= 0 || (flags & ~allowedFlags)) {
        return CopyImage_Original(image, type, cx, cy, flags);
    }
    HookGuard guard;

    IconPixels px;
    if (!GetIconPixels((HICON)image, px) || !px.isIcon ||
        (px.w == cx && px.h == cy)) {
        return CopyImage_Original(image, type, cx, cy, flags);
    }

    // The caller owns the result, so don't hand out a cached icon.
    HICON scaled =
        CreateIconFromPixels(ResamplePixels(px, cx, cy, g_settings.filter));
    if (!scaled) {
        return CopyImage_Original(image, type, cx, cy, flags);
    }
    if (flags & LR_COPYDELETEORG) {
        DestroyIcon((HICON)image);
    }
    return scaled;
}

using DrawIconEx_t = BOOL(WINAPI*)(HDC, int, int, HICON, int, int, UINT, HBRUSH,
                                   UINT);
static DrawIconEx_t DrawIconEx_Original;

static BOOL WINAPI DrawIconEx_Hook(HDC dc, int x, int y, HICON icon, int cx,
                                   int cy, UINT step, HBRUSH brush,
                                   UINT flags) {
    if (!icon || !g_settings.improveIconStretching || g_inHook || cx <= 0 ||
        cy <= 0 || step != 0 || (flags & DI_NORMAL) != DI_NORMAL ||
        (flags & (DI_COMPAT | DI_DEFAULTSIZE))) {
        return DrawIconEx_Original(dc, x, y, icon, cx, cy, step, brush, flags);
    }

    HICON scaled = nullptr;
    {
        HookGuard guard;
        IconPixels px;
        if (GetIconPixels(icon, px) && px.isIcon &&
            (px.w != cx || px.h != cy)) {
            scaled = GetStretchedIcon(px, cx, cy);
        }
    }
    return DrawIconEx_Original(dc, x, y, scaled ? scaled : icon, cx, cy, step,
                               brush, flags);
}

////////////////////////////////////////////////////////////////////////////////
// Hooks: window icons (WM_GETICON / class icon)

using SendMessageW_t = LRESULT(WINAPI*)(HWND, UINT, WPARAM, LPARAM);
static SendMessageW_t SendMessageW_Original;

static LRESULT WINAPI SendMessageW_Hook(HWND hwnd, UINT msg, WPARAM wParam,
                                        LPARAM lParam) {
    LRESULT result = SendMessageW_Original(hwnd, msg, wParam, lParam);
    if (msg == WM_GETICON && wParam == ICON_BIG && result) {
        result = (LRESULT)UpgradeWindowIcon(hwnd, (HICON)result,
                                            __builtin_return_address(0));
    }
    return result;
}

using SendMessageTimeoutW_t = LRESULT(WINAPI*)(HWND, UINT, WPARAM, LPARAM,
                                               UINT, UINT, PDWORD_PTR);
static SendMessageTimeoutW_t SendMessageTimeoutW_Original;

static LRESULT WINAPI SendMessageTimeoutW_Hook(HWND hwnd, UINT msg,
                                               WPARAM wParam, LPARAM lParam,
                                               UINT flags, UINT timeout,
                                               PDWORD_PTR resultOut) {
    LRESULT ret = SendMessageTimeoutW_Original(hwnd, msg, wParam, lParam,
                                               flags, timeout, resultOut);
    if (ret && msg == WM_GETICON && wParam == ICON_BIG && resultOut &&
        *resultOut && hwnd != HWND_BROADCAST) {
        *resultOut = (DWORD_PTR)UpgradeWindowIcon(
            hwnd, (HICON)*resultOut, __builtin_return_address(0));
    }
    return ret;
}

// The taskbar queries icons asynchronously with SendMessageCallbackW, so the
// callback is wrapped to see the result.
struct GetIconCallbackContext {
    SENDASYNCPROC callback;
    ULONG_PTR data;
    void* caller;
};

static volatile LONG g_pendingCallbacks = 0;
static volatile LONG g_unloaded = 0;

static void CALLBACK GetIconCallback(HWND hwnd, UINT msg, ULONG_PTR data,
                                     LRESULT result) {
    auto* ctx = (GetIconCallbackContext*)data;
    InterlockedDecrement(&g_pendingCallbacks);
    if (!g_unloaded && result) {
        result = (LRESULT)UpgradeWindowIcon(hwnd, (HICON)result, ctx->caller);
    }
    SENDASYNCPROC callback = ctx->callback;
    ULONG_PTR originalData = ctx->data;
    delete ctx;
    callback(hwnd, msg, originalData, result);
}

using SendMessageCallbackW_t = BOOL(WINAPI*)(HWND, UINT, WPARAM, LPARAM,
                                             SENDASYNCPROC, ULONG_PTR);
static SendMessageCallbackW_t SendMessageCallbackW_Original;

static BOOL WINAPI SendMessageCallbackW_Hook(HWND hwnd, UINT msg,
                                             WPARAM wParam, LPARAM lParam,
                                             SENDASYNCPROC callback,
                                             ULONG_PTR data) {
    if (msg != WM_GETICON || wParam != ICON_BIG || !callback ||
        hwnd == HWND_BROADCAST || !g_settings.upgradeWindowIcons) {
        return SendMessageCallbackW_Original(hwnd, msg, wParam, lParam,
                                             callback, data);
    }

    auto* ctx = new GetIconCallbackContext{callback, data,
                                           __builtin_return_address(0)};
    InterlockedIncrement(&g_pendingCallbacks);
    BOOL ok = SendMessageCallbackW_Original(hwnd, msg, wParam, lParam,
                                           GetIconCallback, (ULONG_PTR)ctx);
    if (!ok) {
        InterlockedDecrement(&g_pendingCallbacks);
        delete ctx;
    }
    return ok;
}

#ifdef _WIN64
using GetClassLongPtrW_t = ULONG_PTR(WINAPI*)(HWND, int);
static GetClassLongPtrW_t GetClassLongPtrW_Original;

static ULONG_PTR WINAPI GetClassLongPtrW_Hook(HWND hwnd, int index) {
    ULONG_PTR result = GetClassLongPtrW_Original(hwnd, index);
    if (index == GCLP_HICON && result) {
        result = (ULONG_PTR)UpgradeWindowIcon(hwnd, (HICON)result,
                                              __builtin_return_address(0));
    }
    return result;
}
#else
using GetClassLongW_t = DWORD(WINAPI*)(HWND, int);
static GetClassLongW_t GetClassLongW_Original;

static DWORD WINAPI GetClassLongW_Hook(HWND hwnd, int index) {
    DWORD result = GetClassLongW_Original(hwnd, index);
    if (index == GCL_HICON && result) {
        result = (DWORD)(ULONG_PTR)UpgradeWindowIcon(
            hwnd, (HICON)result, __builtin_return_address(0));
    }
    return result;
}
#endif

////////////////////////////////////////////////////////////////////////////////
// Hook: the taskbar's own downscale
//
// The taskbar turns icons into bitmaps with
// IWICImagingFactory::CreateBitmapFromHICON and then shrinks the bitmap to
// its icon size itself (e.g. 32 px -> 24 px) with a cheap filter. Handing it
// an icon that already has the final pixel size leaves nothing to shrink.

using CreateBitmapFromHICON_t = HRESULT(STDMETHODCALLTYPE*)(void*, HICON,
                                                            IWICBitmap**);
static CreateBitmapFromHICON_t CreateBitmapFromHICON_Original;
// Position of CreateBitmapFromHICON in the IWICImagingFactory vtable.
static constexpr int kCreateBitmapFromHICONSlot = 22;

static HRESULT STDMETHODCALLTYPE CreateBitmapFromHICON_Hook(void* self,
                                                            HICON icon,
                                                            IWICBitmap** out) {
    void* caller = __builtin_return_address(0);
    HICON replacement = nullptr;
    if (icon && g_settings.fixTaskbarScaling && !g_inHook) {
        HookGuard guard;
        bool taskbar = IsTaskbarCode(caller);
        IconPixels px;
        if (GetIconPixels(icon, px) && px.isIcon && px.w == px.h) {
            int target = GetTaskbarIconPixelSize(PrimaryTaskbarWindow());
            LOG(L"CreateBitmapFromHICON %dpx from %s, taskbar size %dpx%s",
                px.w, ModuleNameOf(caller).c_str(), target,
                taskbar ? L"" : L" (not taskbar, ignored)");
            // Only shrink: small icons (badges, buttons) stay as they are.
            if (taskbar && px.w > target && px.w <= 256) {
                replacement = GetTaskbarSizedIcon(px, target);
            }
        }
    }
    return CreateBitmapFromHICON_Original(self, replacement ? replacement : icon,
                                         out);
}

static void HookWicCreateBitmapFromHICON() {
    HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IWICImagingFactory* factory = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                   CLSCTX_INPROC_SERVER,
                                   IID_IWICImagingFactory,
                                   (void**)&factory))) {
        void** vtable = *(void***)factory;
        Wh_SetFunctionHook(vtable[kCreateBitmapFromHICONSlot],
                           (void*)CreateBitmapFromHICON_Hook,
                           (void**)&CreateBitmapFromHICON_Original);
        factory->Release();
    } else {
        Wh_Log(L"Couldn't create a WIC factory, taskbar scaling fix disabled");
    }
    if (SUCCEEDED(init)) {
        CoUninitialize();
    }
}

////////////////////////////////////////////////////////////////////////////////
// Mod lifecycle

static void LoadSettings() {
    g_settings.improveExtractedIcons =
        Wh_GetIntSetting(L"improveExtractedIcons");
    g_settings.alwaysUseLargeSource = Wh_GetIntSetting(L"alwaysUseLargeSource");
    g_settings.fixTaskbarScaling = Wh_GetIntSetting(L"fixTaskbarScaling");
    g_settings.upgradeWindowIcons = Wh_GetIntSetting(L"upgradeWindowIcons");
    g_settings.autoRestartExplorer = Wh_GetIntSetting(L"autoRestartExplorer");
    g_settings.improveIconStretching =
        Wh_GetIntSetting(L"improveIconStretching");
    g_settings.taskbarIconSize = Wh_GetIntSetting(L"taskbarIconSize");
    if (g_settings.taskbarIconSize < 8 || g_settings.taskbarIconSize > 256) {
        g_settings.taskbarIconSize = 24;
    }
    g_settings.refreshOnLoad = Wh_GetIntSetting(L"refreshOnLoad");
    g_settings.debugLogging = Wh_GetIntSetting(L"debugLogging");

    PCWSTR pref = Wh_GetStringSetting(L"sourcePreference");
    if (wcscmp(pref, L"nearestLarger") == 0) {
        g_settings.sourcePreference = SourcePreference::NearestLarger;
    } else if (wcscmp(pref, L"largest") == 0) {
        g_settings.sourcePreference = SourcePreference::Largest;
    } else {
        g_settings.sourcePreference = SourcePreference::Auto;
    }
    Wh_FreeStringSetting(pref);

    PCWSTR filter = Wh_GetStringSetting(L"filter");
    if (wcscmp(filter, L"catmullRom") == 0) {
        g_settings.filter = Filter::CatmullRom;
    } else if (wcscmp(filter, L"mitchell") == 0) {
        g_settings.filter = Filter::Mitchell;
    } else if (wcscmp(filter, L"box") == 0) {
        g_settings.filter = Filter::Box;
    } else {
        g_settings.filter = Filter::Lanczos3;
    }
    Wh_FreeStringSetting(filter);
}

static void RefreshShellIcons() {
    // Makes Explorer drop its in-memory icon caches and re-extract.
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
}

// Bump when a change to the rendering should trigger another restart.
static constexpr int kRenderVersion = 2;

// Identifies everything that changes how icons look. Explorer is restarted
// when it differs from the value saved at the last restart.
static int RenderStamp() {
    int values[] = {kRenderVersion,
                    g_settings.improveExtractedIcons,
                    g_settings.alwaysUseLargeSource,
                    g_settings.fixTaskbarScaling,
                    g_settings.upgradeWindowIcons,
                    g_settings.improveIconStretching,
                    g_settings.taskbarIconSize,
                    (int)g_settings.sourcePreference,
                    (int)g_settings.filter};
    uint64_t h = HashBytes(14695981039346656037ULL, values, sizeof(values));
    return (int)(h & 0x7FFFFFFF) | 1;
}

static bool IsShellProcess() {
    HWND tray = PrimaryTaskbarWindow();
    DWORD pid = 0;
    return tray && GetWindowThreadProcessId(tray, &pid) &&
           pid == GetCurrentProcessId();
}

// Clears the icon cache and restarts Explorer once per rendering change. The
// saved stamp makes sure the restarted Explorer doesn't restart again.
static void RestartExplorerIfNeeded() {
    if (!g_settings.autoRestartExplorer || !IsShellProcess()) {
        return;
    }
    int stamp = RenderStamp();
    if (Wh_GetIntValue(L"restartStamp", 0) == stamp) {
        return;
    }
    if (!Wh_SetIntValue(L"restartStamp", stamp)) {
        Wh_Log(L"Couldn't save restart state, not restarting Explorer");
        return;
    }

    // A detached cmd.exe outlives this Explorer: it kills Explorer, deletes
    // the icon cache while nothing holds it, starts Explorer again unless
    // Windows already did, and finally refreshes the icon cache.
    WCHAR command[] =
        L"cmd.exe /d /s /c \""
        L"ping -n 2 127.0.0.1 >nul"
        L" & taskkill /f /im explorer.exe >nul 2>&1"
        L" & ping -n 2 127.0.0.1 >nul"
        L" & del /f /q /a \"%LOCALAPPDATA%\\Microsoft\\Windows\\Explorer\\"
        L"iconcache_*.db\" >nul 2>&1"
        L" & del /f /q /a \"%LOCALAPPDATA%\\IconCache.db\" >nul 2>&1"
        L" & (tasklist /fi \"imagename eq explorer.exe\" | find /i "
        L"\"explorer.exe\" >nul || start \"\" \"%WINDIR%\\explorer.exe\")"
        L" & ping -n 4 127.0.0.1 >nul"
        L" & ie4uinit.exe -show\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, command, nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP |
                           CREATE_BREAKAWAY_FROM_JOB,
                       nullptr, nullptr, &si, &pi) ||
        CreateProcessW(nullptr, command, nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP,
                       nullptr, nullptr, &si, &pi)) {
        Wh_Log(L"Restarting Explorer to apply the new icons");
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    } else {
        Wh_Log(L"Couldn't start the Explorer restart: %u", GetLastError());
    }
}

BOOL Wh_ModInit() {
    Wh_Log(L"Init");
    g_unloaded = 0;
    LoadSettings();

    HMODULE shcore = LoadLibraryW(L"shcore.dll");
    if (shcore) {
        g_getDpiForMonitor = (GetDpiForMonitor_t)(void*)GetProcAddress(
            shcore, "GetDpiForMonitor");
    }

    Wh_SetFunctionHook((void*)PrivateExtractIconsW,
                       (void*)PrivateExtractIconsW_Hook,
                       (void**)&PrivateExtractIconsW_Original);
    Wh_SetFunctionHook((void*)LoadImageW, (void*)LoadImageW_Hook,
                       (void**)&LoadImageW_Original);
    Wh_SetFunctionHook((void*)CopyImage, (void*)CopyImage_Hook,
                       (void**)&CopyImage_Original);
    Wh_SetFunctionHook((void*)DrawIconEx, (void*)DrawIconEx_Hook,
                       (void**)&DrawIconEx_Original);
    Wh_SetFunctionHook((void*)SendMessageW, (void*)SendMessageW_Hook,
                       (void**)&SendMessageW_Original);
    Wh_SetFunctionHook((void*)SendMessageTimeoutW,
                       (void*)SendMessageTimeoutW_Hook,
                       (void**)&SendMessageTimeoutW_Original);
    Wh_SetFunctionHook((void*)SendMessageCallbackW,
                       (void*)SendMessageCallbackW_Hook,
                       (void**)&SendMessageCallbackW_Original);
#ifdef _WIN64
    Wh_SetFunctionHook((void*)GetClassLongPtrW, (void*)GetClassLongPtrW_Hook,
                       (void**)&GetClassLongPtrW_Original);
#else
    Wh_SetFunctionHook((void*)GetClassLongW, (void*)GetClassLongW_Hook,
                       (void**)&GetClassLongW_Original);
#endif
    HookWicCreateBitmapFromHICON();

    return TRUE;
}

void Wh_ModAfterInit() {
    if (g_settings.refreshOnLoad) {
        RefreshShellIcons();
    }
    RestartExplorerIfNeeded();
}

void Wh_ModUninit() {
    Wh_Log(L"Uninit");
    g_unloaded = 1;

    // A WM_GETICON sent to a hung app may still call GetIconCallback later.
    // Keep our code loaded in that case; the callback then just forwards.
    if (g_pendingCallbacks > 0) {
        HMODULE self;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_PIN,
                           (LPCWSTR)&GetIconCallback, &self);
    }

    g_stretchCache.Clear(true);
    // The taskbar may still display window icons we handed out; leak them.
    g_windowIconCache.Clear(false);

    if (g_settings.refreshOnLoad) {
        RefreshShellIcons();
    }
}

void Wh_ModSettingsChanged() {
    Wh_Log(L"SettingsChanged");
    LoadSettings();
    g_stretchCache.Clear(true);
    g_windowIconCache.Clear(false);
    if (g_settings.refreshOnLoad) {
        RefreshShellIcons();
    }
    RestartExplorerIfNeeded();
}
