# Porting Walshard Studio to Linux and Windows

**Status (2026-11-24): macOS is the only platform that has ever been
built.** Linux and Windows have scaffolding in the tree (entry points,
resources, platform guards, CI jobs) but neither has been compiled.
Contributions are very welcome — this document is the map.

If you pick one up, please open an issue saying so (to avoid duplicate
work), then a PR. Partial progress is fine: "configures but doesn't
link" is a useful PR.

`PORTING_WINDOWS.md` is an older, more detailed brief for the Windows
work. Where it disagrees with this file, **this file wins** (several
items it lists as TODO are already done — see the checklist below).

---

## 1. How the build is put together

- axmol (pinned `v2.9.1`, see `AXMOL_REF` in
  `.github/workflows/build.yml`) supplies the engine, GLFW, ImGui and
  the CMake templates.
- Our `CMakeLists.txt` includes axmol's template modules.
  `AXGameSourceSetup.cmake` (in axmol) decides the platform entry point:

  | Platform | Entry point (must exist at this path) |
  |---|---|
  | macOS   | `proj.ios_mac/mac/main.cpp` (+ `Info.plist`, `Icon.icns`) |
  | Linux   | `proj.linux/main.cpp` |
  | Windows | `proj.win32/main.cpp`, `main.h`, `resource.h`, `game.rc` (+ `res/game.ico`), and `run.bat.in` at repo root |

- All three entry points are one-liners into `walshardMain()` in
  `app/AppMain.cpp`, which owns argument parsing (`--panel=`,
  `--size=`, `.csd` paths). Don't add per-platform argv logic to the
  `proj.*` files.
- Everything under `app/*.cpp` is globbed into the host app; `.mm` only
  on Apple. Platform-specific code lives behind `#if defined(_WIN32)` /
  `__APPLE__` / `__linux__` in shared files — no per-platform source
  lists in CMake.

## 2. Prerequisites

```bash
git clone --depth 1 --branch v2.9.1 https://github.com/axmolengine/axmol ~/axmol
pwsh ~/axmol/setup.ps1   # downloads axslcc (shader compiler) + toolchain bits
```

`setup.ps1` is required on every platform — without it configure fails
with *"Please run setup.ps1 again to download axslcc"*. It needs
PowerShell 7 (`pwsh`), which axmol's `setup.ps1` bootstraps on Linux.

### Linux (Ubuntu 22.04 reference — same list CI installs)

```bash
sudo apt-get install -y cmake ninja-build pkg-config python3 \
  libgl1-mesa-dev libglu1-mesa-dev libglew-dev \
  libxi-dev libxinerama-dev libxrandr-dev libxcursor-dev \
  libgtk-3-dev libssl-dev libuv1-dev libpng-dev libjpeg-dev \
  librsvg2-bin libfontconfig1-dev libvlc-dev libwebkit2gtk-4.0-dev \
  libxxf86vm-dev libvlccore-dev
cmake -S . -B build -G Ninja -DAX_ROOT=$HOME/axmol -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

### Windows (Visual Studio 2022, "Desktop development with C++")

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DAX_ROOT=C:/axmol
cmake --build build --config Release
```

## 3. Checklist

Legend: ✅ done (but unverified on that OS unless stated) · ⬜ todo ·
🔍 needs investigation

### Shared / both platforms

- ✅ Platform-independent entry point `app/AppMain.cpp`
  (verified on macOS).
- ✅ Include paths use the on-disk case `ImGui/imgui.h` (the old
  `imgui/imgui.h` only worked on case-insensitive filesystems).
- ✅ `PlatformCompat.h` shims `getpid` / `popen` / `pclose` for MSVC.
- ✅ Process spawning: `app/PlatformSpawn.cpp` has POSIX
  (`posix_spawn`, `/proc/self/exe` on Linux) and Win32
  (`CreateProcessA`) paths.
- ✅ Inspect server sockets: Winsock path in `InspectServer.cpp`
  (`WSAStartup`, `closesocket`, `ws2_32` linked in CMake).
- ✅ IPC dir: `%TEMP%\walshard` on Windows, `/tmp/walshard` elsewhere
  (`IpcBridge.cpp`).
- ✅ App data dir (recents, `inspect.token`): `%APPDATA%`, XDG, or
  `~/Library/Application Support` (`Editor::recentsFile`).
- ✅ Native menu/dialog stubs for non-Apple (`app/NativeMenu.cpp`);
  ImGui's in-window menu bar carries File actions.
- ⬜ **Native file dialogs.** `showOpenImageDialog` /
  `showChooseDirectoryDialog` return empty on Linux/Windows, so those
  buttons do nothing. Suggest [portable-file-dialogs] (header-only) or
  [nativefiledialog-extended] instead of per-OS code.
- ⬜ **Shell-outs to `/usr/bin/curl`** in `AppDelegate.cpp` (NLP prompt
  + sprite search, both opt-in via `OCS_NLP_URL` /
  `OCS_SPRITE_SEARCH_URL`). These also build a shell string from the
  URL (injection risk) and use `tmpnam`. Replace with axmol's
  `network::HttpClient`, or the argv-based `runCapture` used in
  `EditorInspect.cpp`. Fixes Windows *and* the security issue.
- ⬜ **`runCapture` on Windows** (`EditorInspect.cpp`) returns -1 → the
  `download`, `svg-rasterize`, `svg-bake` RPCs fail. Implement with
  `CreateProcessW` + pipes, or move HTTP to `HttpClient` and SVG to
  lunasvg (below).
- ⬜ **SVG rasterisation** shells out to `rsvg-convert`
  (`SvgImageNode.cpp`, `findSvgConverter`). Linux: `apt install
  librsvg2-bin` works as-is. Windows: no binary. Long-term fix for
  both: link [lunasvg] (small, MIT) and drop the external tool.
- ⬜ **TexturePacker** (`Editor::packFolder`, `popen`) — resolve
  `TexturePacker.exe` / `/usr/bin/TexturePacker` in
  `findTexturePacker`; quoting in `shellQuote` is POSIX-style and
  wrong for `cmd.exe`.
- 🔍 **Non-ASCII paths on Windows.** Paths flow through `std::string`
  → `std::filesystem::path`, which uses the ANSI code page on MSVC.
  Either build with `/utf-8` + UTF-8 active code page manifest, or
  convert via `std::u8string`. Noted in `AppMain.cpp`.

### Linux

- ✅ `proj.linux/main.cpp`.
- ✅ CI job (`ubuntu-22.04`) installs deps, runs `setup.ps1`.
- ⬜ First successful configure + build. Expect warnings-as-errors
  differences vs clang/AppleClang; fix in shared code, no
  `#ifdef __linux__` unless it's genuinely platform behaviour.
- 🔍 `DragLabel.cpp` (tab tear-off ghost window) uses GL 2.1
  fixed-function in its own GLFW window. Should work under Mesa;
  verify, and check Wayland (GLFW may need `GLFW_PLATFORM_X11`
  hint for window positioning — Wayland forbids client-positioned
  windows, which tab tear-off relies on).
- 🔍 Window frame coordinates published to `/tmp/walshard/<pid>.window`
  for cross-window tab drag: same Wayland caveat.
- ⬜ Packaging: CI tars `build/bin/Walshard`. An AppImage would be
  nicer (`packaging/` has the macOS bits only).

### Windows

- ✅ `proj.win32/main.cpp` (UTF-16 command line → UTF-8 → `walshardMain`),
  `main.h`, `resource.h`, `game.rc` (icon + version info),
  `res/game.ico`, `run.bat.in`.
- ✅ `DragLabel.cpp` includes `<windows.h>` before `<GL/gl.h>`.
- ✅ CI job (`windows-2022`): clone path quoted, `setup.ps1` step.
- ⬜ First successful configure + build. Likely hits: remaining
  POSIX-isms in files not yet compiled on MSVC (`ssize_t`,
  `strcasecmp`, `<dirent.h>`…); MSVC `min`/`max` macro clashes
  (`NOMINMAX`); C++ conformance flags (`/permissive-`, `/Zc:__cplusplus`).
- ⬜ `PlatformSpawn.cpp` uses `CreateProcessA` — switch to
  `CreateProcessW` for non-ASCII exe/scene paths.
- ⬜ `IpcBridge`'s `processAlive` / signal handling — verify the
  Win32 branch (`OpenProcess` + `GetExitCodeProcess`) and that
  `atexit` cleanup runs (no `SIGTERM` on Windows).
- ⬜ Inspect token file permissions: `%APPDATA%` is per-user, so no
  ACL work planned — confirm that's acceptable.
- ⬜ Packaging: CI zips `build/bin/Walshard/Release`; axmol's
  `ax_sync_target_dlls` must have copied the DLLs next to the exe.

## 4. Verifying a port

1. `cmake --build` succeeds.
2. `Walshard <some>.csd` opens the editor and renders the scene.
3. Inspect server: launch with `OCS_INSPECT_PORT=9876`, then
   `describe` / `tree` over TCP return data, and `auth <token>` using
   the `inspect.token` file unlocks `undo` (see README → Security model).
4. Tear a tab off into a second window and drag it back.
5. Save, re-open, compare the `.csd` (no diff for an untouched scene).
6. macOS still builds — CI's `macos-arm64` job must stay green.

## 5. CI

`.github/workflows/build.yml` runs macOS / Linux / Windows on pushes
to `main` and on PRs. Linux and Windows are **expected to fail** until
the first port lands; `fail-fast: false` keeps them from cancelling
the macOS job. When a platform goes green, keep it green.

[portable-file-dialogs]: https://github.com/samhocevar/portable-file-dialogs
[nativefiledialog-extended]: https://github.com/btzy/nativefiledialog-extended
[lunasvg]: https://github.com/sammycage/lunasvg
