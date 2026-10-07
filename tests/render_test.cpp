// Exercises the mod's rendering pipeline outside Windhawk (runs under Wine or
// Windows). Build:
//   x86_64-w64-mingw32-g++ -std=c++20 -O2 -include windhawk_stub.h
//       render_test.cpp -o render_test.exe -lgdi32 -luser32 -lshell32
// Usage: render_test.exe <icon.ico> <icon.exe> <outdir>
#include "../mods/taskbar-crisp-icons.wh.cpp"

#include <cstdio>

static int g_failures = 0;
#define CHECK(cond)                                                \
    do {                                                           \
        if (!(cond)) {                                             \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                    #cond);                                        \
            g_failures++;                                          \
        }                                                          \
    } while (0)

static void Dump(const IconPixels& p, const wchar_t* dir, const char* name) {
    wchar_t path[MAX_PATH];
    swprintf(path, MAX_PATH, L"%ls\\%hs_%dx%d.bgra", dir, name, p.w, p.h);
    FILE* f = _wfopen(path, L"wb");
    if (f) {
        fwrite(p.px.data(), 4, p.px.size(), f);
        fclose(f);
    }
}

static IconPixels Pixels(HICON icon) {
    IconPixels p;
    CHECK(GetIconPixels(icon, p));
    return p;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: render_test <ico> <exe> <outdir>\n");
        return 2;
    }
    const wchar_t* ico = argv[1];
    const wchar_t* exe = argv[2];
    const wchar_t* out = argv[3];
    LoadSettings();
    PrivateExtractIconsW_Original = PrivateExtractIconsW;
    LoadImageW_Original = LoadImageW;
    CopyImage_Original = CopyImage;
    DrawIconEx_Original = DrawIconEx;

    // Group parsing of a .ico file.
    {
        IconGroup g;
        CHECK(LoadGroupFromFile(ico, 0, g));
        CHECK(g.entries.size() == 4);
        bool exact;
        const IconEntry* e = PickEntry(g, 30, 30, &exact);
        CHECK(e && e->w == 256 && !exact);  // auto: >= 2x target
        e = PickEntry(g, 24, 24, &exact);
        CHECK(e && e->w == 48 && !exact);
        e = PickEntry(g, 32, 32, &exact);
        CHECK(e && e->w == 32 && exact);
        e = PickEntry(g, 300, 300, &exact);
        CHECK(e && e->w == 256 && !exact);  // upscale from largest

        // Exact sizes are left to Windows.
        CHECK(RenderGroupIcon(g, 32, 32) == nullptr);

        for (int size : {20, 24, 30, 36, 40, 64}) {
            HICON icon = RenderGroupIcon(g, size, size);
            CHECK(icon != nullptr);
            if (icon) {
                IconPixels p = Pixels(icon);
                CHECK(p.w == size && p.h == size);
                Dump(p, out, "mod");
                DestroyIcon(icon);
            }
            HICON plain = (HICON)LoadImageW(nullptr, ico, IMAGE_ICON, size,
                                            size, LR_LOADFROMFILE);
            if (plain) {
                Dump(Pixels(plain), out, "windows");
                DestroyIcon(plain);
            }
        }
    }

    // Hooked LoadImageW from a .ico file.
    {
        HICON icon = (HICON)LoadImageW_Hook(nullptr, ico, IMAGE_ICON, 30, 30,
                                            LR_LOADFROMFILE);
        CHECK(icon != nullptr);
        IconPixels p = Pixels(icon);
        CHECK(p.w == 30);
        DestroyIcon(icon);
    }

    // Hooked PrivateExtractIconsW from a PE file, two sizes at once.
    {
        HICON icons[2] = {};
        UINT ids[2] = {};
        UINT n = PrivateExtractIconsW_Hook(exe, 0, MAKELONG(30, 36),
                                           MAKELONG(30, 36), icons, ids, 2, 0);
        CHECK(n == 2);
        if (n == 2) {
            IconPixels a = Pixels(icons[0]);
            IconPixels b = Pixels(icons[1]);
            CHECK(a.w == 30 && b.w == 36);
            Dump(a, out, "pe");
            Dump(b, out, "pe");
            DestroyIcon(icons[0]);
            DestroyIcon(icons[1]);
        }
        HICON neg = nullptr;
        n = PrivateExtractIconsW_Hook(exe, -1, 30, 30, &neg, nullptr, 1, 0);
        CHECK(n == 1 && neg);
        if (neg) {
            CHECK(Pixels(neg).w == 30);
            DestroyIcon(neg);
        }
    }

    // Resampling sanity: same size is identity, downscale keeps opacity.
    {
        IconGroup g;
        LoadGroupFromFile(ico, 0, g);
        IconPixels big, same;
        CHECK(RenderGroup(g, 256, 256, false, big));
        same = ResamplePixels(big, 256, 256, Filter::Lanczos3);
        CHECK(PixelDifference(big, same) < 0.002);
        IconPixels at32;
        CHECK(RenderGroup(g, 32, 32, false, at32));
        IconPixels down = ResamplePixels(big, 32, 32, Filter::Lanczos3);
        double diff = PixelDifference(at32, down);
        printf("diff(32 entry, 256->32 lanczos) = %.4f\n", diff);
        CHECK(diff < 0.06);  // window-icon similarity threshold holds
        IconPixels box = ResamplePixels(big, 32, 32, Filter::Box);
        CHECK(PixelDifference(at32, box) < 0.06);
        IconPixels up = ResamplePixels(at32, 64, 64, Filter::Box);
        CHECK(up.w == 64);
    }

    // CopyImage / DrawIconEx hooks.
    {
        IconGroup g;
        LoadGroupFromFile(ico, 0, g);
        IconPixels p48;
        RenderGroup(g, 48, 48, false, p48);
        HICON src = CreateIconFromPixels(p48);
        HICON copy = (HICON)CopyImage_Hook(src, IMAGE_ICON, 30, 30, 0);
        CHECK(copy && Pixels(copy).w == 30);
        Dump(Pixels(copy), out, "copyimage");
        DestroyIcon(copy);

        HDC screen = GetDC(nullptr);
        HDC dc = CreateCompatibleDC(screen);
        HBITMAP bmp = CreateCompatibleBitmap(screen, 64, 64);
        SelectObject(dc, bmp);
        CHECK(DrawIconEx_Hook(dc, 0, 0, src, 30, 30, 0, nullptr, DI_NORMAL));
        CHECK(DrawIconEx_Hook(dc, 0, 0, src, 30, 30, 0, nullptr, DI_NORMAL));
        DeleteDC(dc);
        DeleteObject(bmp);
        ReleaseDC(nullptr, screen);
        DestroyIcon(src);
        g_stretchCache.Clear(true);
    }

    printf("%s (%d failures)\n", g_failures ? "FAILED" : "PASSED", g_failures);
    return g_failures ? 1 : 0;
}
