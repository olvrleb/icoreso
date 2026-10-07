# icoreso — Crisp Taskbar Icons

A [Windhawk](https://windhawk.net) mod for Windows 10/11 that makes taskbar
icons (pinned shortcuts and running apps such as File Explorer, Discord,
Steam, …) sharp instead of blurry or pixelated.

![Default scaling vs. this mod](docs/comparison.png)

## Why taskbar icons look blurry

Most apps ship a 256×256 image in their `.ico`/`.exe`, but Windows rarely uses
it for the taskbar:

| Situation | What Windows does | Result |
|---|---|---|
| Taskbar needs 30 px (125% scaling), icon has 16/32/48/256 | Takes the **32 px** image and shrinks it 6% | Every edge smeared over 2 px |
| Running app sends a 32 px window icon, taskbar needs 36 px (150%) | **Upscales** 32 → 36 | Soft and pixelated |
| `DrawIconEx` / `CopyImage` stretching | Nearest-neighbour style sampling | Jagged edges |

## What the mod does

Windows always shrinks the icon to the taskbar size itself. So the mod does
the shrinking first — from the 256 px image, with a Lanczos-3 filter — and
hands Windows an icon that already has the exact final pixel size (24 px at
100%, 30 px at 125%, 36 px at 150%). Windows' own downscale then has nothing
left to do.

Inside `explorer.exe` (which hosts the taskbar) it hooks:

- **`IWICImagingFactory::CreateBitmapFromHICON`** – where the taskbar turns an
  icon (often 32 px) into a bitmap that it then shrinks itself. The mod hands
  it an icon at the exact final size, rendered straight from the 256 px image
  when it knows which file the icon came from.
- **`PrivateExtractIconsW` / `LoadImageW`** – shortcut and file icons are
  rendered from a high-resolution image even when the file has the requested
  size (hand-made 16 px images are kept).
- **`WM_GETICON` (`SendMessage*`) / `GetClassLongPtrW`** – running app icons
  that aren't exactly the taskbar's pixel size are replaced with the app's exe
  icon at that size (verified by a pixel comparison), or a high-quality
  resample of the window icon.
- **`DrawIconEx` / `CopyImage`** – stretched icons are resampled with the
  high-quality filter.

The taskbar-specific changes check that the caller is the taskbar
(`Taskbar.dll`, `Taskbar.View.dll` or `explorer.exe`), so Alt+Tab and Start are
unaffected. No files on disk are modified.

## Install

1. Install [Windhawk](https://windhawk.net).
2. *Create a new mod*, replace the template with
   [`mods/taskbar-crisp-icons.wh.cpp`](mods/taskbar-crisp-icons.wh.cpp), then
   *Compile* and *Enable*.
3. That's it: the mod clears the icon cache and restarts Explorer by itself
   once (open File Explorer windows close). It does this again after you
   change an icon setting.

If icons still look unchanged, enable *Debug logging* in the mod's settings
and check the log (*Advanced → Show log output*) for `CreateBitmapFromHICON`,
`Re-rendered` and `Window icon` lines.

### Settings

| Setting | Default | |
|---|---|---|
| Re-render shortcut, pinned and file icons | on | |
| Always render from a high-resolution image | on | Ignore exact-size images (except 16 px) |
| Hand the taskbar icons at its exact pixel size | on | Replaces the taskbar's own downscale |
| Fix running app icons | on | |
| Restart Explorer automatically | on | Once per enable / setting change |
| High-quality icon stretching | on | |
| Taskbar icon size | 24 | Logical px at 100%; use 16 for small taskbar buttons |
| Source image preference | Auto | Auto / nearest larger / largest |
| Resampling filter | Lanczos-3 | Lanczos-3, Catmull-Rom, Mitchell, area average |

## Limitations

- The result can't be finer than the screen: at 100% scaling a taskbar icon is
  24×24 physical pixels, and the mod makes them a perfectly anti-aliased render
  of the 256 px artwork. Higher display scaling gives it more pixels to use.
- Detail that isn't in the icon file can't be created: an app that only ships
  a 32 px icon is upscaled smoothly, not sharpened.
- Store/UWP apps use scale-specific PNG assets and are unaffected.

## Tests

`tests/run_tests.sh` cross-compiles the rendering pipeline with MinGW and runs
it under Wine (needs `mingw-w64`, `wine`, Python + Pillow). It checks `.ico`
and PE icon-group parsing, source-image selection, the hooked
`LoadImageW`/`PrivateExtractIconsW`/`CopyImage`/`DrawIconEx` paths, the
taskbar-size `CreateBitmapFromHICON` replacement and the resampler.
