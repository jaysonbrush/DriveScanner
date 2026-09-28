# DriveScanner

A small, portable disk usage viewer for Windows.

- One `.exe` of about 110 KB. There's no installer, no registry entries, and no settings files.
- It scans `C:\` on launch. Pick any other drive from the dropdown.
- **Rings view** (default): each ring outward is one folder level deeper, and a slice's angle is its share of the parent folder. Hover a slice for details, click a folder to zoom into it, and click the center (or press Backspace) to go up.
- **Free space** (checkbox, on by default): at the drive level, the inner ring includes a gray "Free" slice so used and free space are shown to scale. The center shows the drive's used and total size.
- **List view** (checkbox): a tree sorted by size, with a "% of parent" bar on every row.
- The breadcrumb at the top jumps back to any parent folder.
- Right-click any item for **Open in Explorer** or **Copy path**.
- F5 rescans.
- The ⓘ button (top right) shows the version, copyright, and license.

## Scanning

| Mode | When | Notes |
|---|---|---|
| Fast (file table) | Run as administrator on an NTFS drive | Reads the NTFS Master File Table directly. It's the fastest and most complete mode, including folders a normal user can't open. |
| Standard | Otherwise (not admin, FAT/exFAT, network drives) | Parallel folder scan. Folders you don't have access to are skipped, and the status bar reports how many. |

Sizes are **size on disk** (allocated space). OneDrive cloud-only files count as zero, and a file with several hard links counts once. This is why the numbers match the drive's used space more closely than Explorer's "Size" column does.

## Building

The build needs [Zig](https://ziglang.org/download/), which is a single portable zip with no installer. The build script expects it at `C:\Tools\zig\zig.exe`; set `$env:ZIG` to use a different location.

```powershell
.\build.ps1
```

This produces `DriveScanner.exe` in the project root. The icon (`src\DriveScanner.ico`) is created by `tools\make-icon.ps1` if it's missing.

## Layout

```
src\main.cpp                the whole app: scanners, Direct2D views, window
src\DriveScanner.rc         icon, manifest, version info
src\DriveScanner.manifest   per-monitor DPI, visual styles, long paths
tools\make-icon.ps1         generates the icon
build.ps1                   build script
```

## License

MIT. Copyright (c) 2026 Jayson Brush. See [LICENSE](LICENSE).
