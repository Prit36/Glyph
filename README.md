<p align="center">
  <img src="assets/Glyph.svg" width="112" height="112" alt="Glyph icon">
</p>

# Glyph

**Text within reach.** Select and copy text from anywhere on your Windows desktop—even when the app, image, video, or webpage doesn't let you select it.

Press **Ctrl+Alt+T**, select the text, and copy. Press **Esc** to return to your desktop.

Glyph is a small native C++20 tray app powered by Windows' offline OCR. It has no bundled OCR models, third-party runtime, network requests, or background screen recording.

[Download the latest release](https://github.com/Prit36/Glyph/releases/latest)

## Features

- Select individual characters, words, multiple lines, or a rectangular region.
- Add separate selections and copy them together.
- Preserve the screen's original fonts and text, with connected translucent selection highlights.
- Capture multiple monitors, including mixed DPI and negative monitor coordinates.
- Configure the activation shortcut and OCR language from the tray menu.
- Enable start at login if you want it; it is off by default.
- Stay idle without polling, timers, or repeated screen capture.

## Get started

**Requirements:** Windows 10 version 2004 or later, or Windows 11; x64; an installed Windows OCR language.

1. Download the Windows x64 ZIP from [Releases](https://github.com/Prit36/Glyph/releases/latest) and extract it.
2. Run `Glyph.exe`. Glyph appears in your notification area.
3. Press **Ctrl+Alt+T**. The desktop freezes while OCR runs; text becomes selectable as recognition completes for each monitor.
4. Drag to select text. Press **Ctrl+C** to copy, or **Enter** to copy and exit.

The portable executable does not require an administrator account. Windows officially supports this OCR API in desktop apps with package identity; unpackaged OCR works on some systems but is not guaranteed. If OCR is unavailable, use the packaged installation below.

### Packaged installation

The release includes an **unsigned MSIX** and scripts for local installation. Double-clicking the unsigned package alone will not install it.

Install the Windows SDK with SignTool. Open PowerShell **as Administrator under your own account**, change to the extracted folder, and run:

```powershell
.\install.ps1
```

The script creates a local signing certificate, signs a copy of the package, imports its public certificate into `LocalMachine\TrustedPeople`, and installs Glyph for the current user. Launch Glyph from Start. This is a local signing workflow, not a publicly trusted publisher signature.

Quit Glyph before uninstalling. Use Windows Settings or `uninstall.ps1`. Optional switches:

```powershell
.\uninstall.ps1 -RemoveSettings
# Requires Administrator PowerShell:
.\uninstall.ps1 -RemoveSettings -RemoveSigningCertificate
```

## Controls

| Action | Control |
|---|---|
| Select characters across words and lines | Drag |
| Select a word | Double-click |
| Select words in a rectangle | Alt-drag |
| Add a separate selection | Ctrl-drag |
| Add a rectangular selection | Ctrl+Alt-drag |
| Select all recognized text | Ctrl+A |
| Copy and keep OCR mode open | Ctrl+C or Copy button |
| Copy and exit | Enter |
| Exit without copying | Esc, close button, or activation shortcut again |

Click the tray icon to capture. Its context menu offers OCR language, settings, reload settings, start at login, help, and quit. Launching a second copy activates the existing instance. `Glyph.exe --capture` opens directly in OCR mode.

## Settings

Open **Settings** from the tray, edit `%APPDATA%\Glyph\settings.ini`, save, and select **Reload settings**.

```ini
[Glyph]
hotkey=Ctrl+Alt+T
language=auto
```

Shortcuts accept Ctrl, Alt, Shift, Win, letters, digits, Space, and F1–F24. Use at least one modifier for letters and digits. Some shortcuts are reserved or already registered by another application; Glyph reports conflicts.

`language=auto` uses Windows profile languages, falling back to an installed OCR language. Set a tag such as `en-US` to choose one explicitly, or select a language in the tray menu. Install additional OCR language features through Windows language settings. Each capture uses one recognition language.

## Privacy and performance

OCR runs locally. Screenshots and recognized text stay in process memory and are released when OCR mode ends. Copying uses the Windows clipboard; your Windows clipboard history and cloud-sync settings still apply.

The tray app blocks on Windows messages, and its worker sleeps while idle. The OCR engine is initialized once at startup. Each activation captures and recognizes the current screen afresh; there is no OCR result cache.

Recognition runs on a worker thread at native screen resolution. Large screens are processed in overlapping strips using grayscale OCR input. Selection repaints only changed areas, and fonts and text measurements are reused within the active session. Latency depends on screen size, text density, installed language, and Windows OCR.

The v1.2.4 executable is approximately **286 KiB**, with a statically linked C++ runtime. Runtime memory depends on display resolution: the screenshot and rendering buffer alone use about **63 MiB for one 4K desktop**, plus OCR and other app allocations.

## Limitations

- OCR can misread small, stylized, rotated, or low-contrast text. Character boundaries are estimated from word boxes and font measurements.
- Ordinary selection follows visual rows, top to bottom and left to right within each monitor. Use rectangular selection for columns or separate layout regions.
- Protected video, secure desktops, and some exclusive fullscreen applications cannot be captured normally.
- Recognition already in progress may finish after Esc; stale results are discarded. Changing the monitor layout exits OCR mode.

## Build from source

Install Visual Studio 2022 Build Tools or later with **Desktop development with C++**, plus a recent Windows SDK containing C++/WinRT headers and MakeAppx. No third-party libraries or package manager are required.

From PowerShell in the repository root:

```powershell
.\build.ps1
# Executable only:
.\build.ps1 -SkipPackage
```

Output goes to `dist\Glyph.exe` and `dist\Glyph.msix`, with installation scripts and documentation. Compiler intermediates go to `build\`. Both generated directories are excluded from Git.

| Path | Purpose |
|---|---|
| `src/main.cpp` | Tray app, capture, OCR worker, selection, and rendering |
| `src/app.rc`, `src/app.manifest` | Executable icon, version, and Windows manifest |
| `assets/` | Vector icon source and Windows icon/package assets |
| `packaging/AppxManifest.xml` | MSIX identity and capabilities |
| `build.ps1` | Native release build and MSIX packaging |
| `install.ps1`, `uninstall.ps1` | Local package installation and removal |

API references: [Windows OCR](https://learn.microsoft.com/en-us/uwp/api/windows.media.ocr), [OcrEngine](https://learn.microsoft.com/en-us/uwp/api/windows.media.ocr.ocrengine), [RegisterHotKey](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-registerhotkey).