# QMud

QMud is a Qt 6 port and continuation of the
original [MUSHclient](https://www.mushclient.com/mushclient/mushclient.htm) (by Nick Gammon),
designed and written by Panagiotis Kalogiratos (Nodens) of [CthulhuMUD](https://www.cthulhumud.com).
It is client program for connecting to MUD (Multi-User Dungeon) games.
It is compatible with existing MUSHclient files and plugins, but it will migrate them to
its own format in order to maintain separation. As more features are implemented,
things were bound to diverge, especially in data persistence, so, as a conscious
choice, QMud diverges from the get-go.
The active implementation in this repository is C++20 + Qt 6.10.

## Project status

The porting has been completed. Behavior aims for high compatibility
with original persistence and Lua workflows while using a modern Qt
implementation. There are several improvements and new features implemented
already. Please use the issue tracker, with the appropriate template to report
issues, request features, etc.

## Features

- Cross-platform (Linux, Windows, macOS).
- Unicode, NAWS, Terminal Type, CHARSET, EOR, ECHO, MXP, MSP, MCCP, MMCP, OSC8, xterm256 color, Truecolor.
- Lua scripting.
- Copyover-style in-place reload on Linux/macOS (`File -> Reload QMud`).
- Split-pane scrollback buffer, persistent scrollback buffer/command history.
- Autosave, autobackup, log rotation, log compression.
- Autoupdates.

## Contact / Support

For support, testing feedback, and development discussion, join:

- [CthulhuMUD Discord](https://discord.gg/secxwnTJCq)

Do **NOT** use the issue tracker for general support requests.

## Contributions

- Bug fix PRs are welcome.
- Feature PRs have to be discussed first either on Issue tracker or Discord.
- You can also contribute with documentation or translations (once work on both is started; but you can apply to help
  before that)
- You can also contribute with funding as funds are needed for code signing certificate, Apple registration etc.
  in order to bring QMud to the Apple Store/avoid issues with Windows SmartScreen/WDAC.

[![Support on Ko-fi](https://img.shields.io/badge/Ko--fi-Support%20this%20project-ff5e5b?logo=ko-fi&logoColor=white)](https://ko-fi.com/nodens)

## Supported platforms

- Linux (primary development platform)
- Windows (source/build support and packaging workflow)
- macOS (source/build support and packaging workflow)

## Migration from MUSHclient data

QMud can migrate an existing MUSHclient data tree on first run. Migration is
copy-based, so source files are preserved and moved under a `migrated` marker
path after successful import to avoid reprocessing.

What is migrated:

- World list/database entries (including plugin list metadata)
- World files under the worlds tree (`.MCL` and related world XML data)
- Preference/state data required for normal startup

Path handling during migration normalizes legacy Windows-style paths (for
example `C:\...`) so migrated worlds resolve correctly on the active platform.

### Migration Tips

If you want to keep an as clean datadir as possible when migrating MUSHclient data,
Move to QMud home dir only your worlds dir, scripts dir (if you have
any custom scripts), mushclient_prefs.sqlite, mushclient.ini and any lua modules
you may have manually placed in the lua dir. Nothing else is required.
Always keep a backup of your original MUSHclient dir. Extensive testing has been
done but better safe than sorry.

## Data directory resolution (`QMUD_HOME`)

QMud resolves its startup/data directory in this order:

1. `QMUD_HOME` environment variable (all platforms).
2. If env var is missing, `QMUD_HOME` from config file fallback:
    - Linux: `~/.config/QMud/config`, then `/etc/QMud/config`
    - macOS: `~/Library/Application Support/QMud/config`, then `/Library/Application Support/QMud/config`
    - Windows: `%LOCALAPPDATA%/QMud/config`

When multi-instance mode is enabled (`QMUD_ALLOW_MULTI_INSTANCE` env var or `--multi-instance`/
`--allow-multi-instance`), config fallback is disabled and `QMUD_HOME` must be set explicitly in the process environment
in order to avoid second instances writing to the same datadir.

System config lines support both:

- `QMUD_HOME=/path/to/dir`
- `export QMUD_HOME=/path/to/dir`

Quoted values are accepted, and leading `~` is expanded.
The same config fallback files can also define any `QMUD_*` environment flag, and those values are used when the real
process environment does not override them.

If nothing is configured, defaults are:

- AppImage: `$HOME/QMud`
- macOS: `~/Library/Application Support/QMud`
- Windows and non-AppImage Linux: executable directory

## Environment flags and CLI switches

### Environment flags

Flags below can be provided either as process environment variables or in the OS config fallback files (`QMUD_*`
entries, used as fallback when not set in the process environment).

- `QMUD_HOME`: Overrides startup/data directory resolution (see section above).
- `QMUD_ALLOW_MULTI_INSTANCE`: When set to `1`, `y`, `yes`, or `true`, bypasses single-instance enforcement. (Not safe
  with same datadir). In this mode, `QMUD_HOME` must be explicitly set in process environment.
- `QMUD_DISABLE_UPDATE`:  When set to `1`, `y`, `yes`, or `true`, disables the automatic updates functionality (for
  distro packaging).
- `QMUD_RELOAD_VERBOSE`: When set to `1`, `y`, `yes`, or `true`, enables verbose per-world reload diagnostics in logs.

### CLI switches

- `--multi-instance` (alias: `--allow-multi-instance`): Bypass single-instance enforcement for that process. (Not safe
  with same datadir). In this mode, `QMUD_HOME` must be explicitly set in process environment.
- `--dump-lua-api <output-dir>`: Export Lua API inventory to the given directory and exit.

## Reload QMud (Copyover-style)

`File -> Reload QMud` is available on Linux and macOS. It performs a reload keeping worlds connected when possible.

Current behavior/limitations:

- MCCPv1/2 enabled worlds that do not honor IAC DONT COMPRESS/2, on timeout/failure to end compression stream, downgrade
  to reconnect on reload.

## Build requirements

- CMake 3.21+
- C++20 compiler
- Qt 6 modules: `Widgets`, `Network`, `Sql`, `PrintSupport`
- Optional Qt 6 module: `Multimedia` (sound; disabled at runtime if missing)
- zlib
- Lua 5.4 when `QMUD_ENABLE_LUA_SCRIPTING=ON` (default)
- lua-socket
- lua-json
- lua-lpeg
-

## Build instructions

### Linux (Ninja/Makefiles)

```bash
cmake -S . -B cmake-build-release -DCMAKE_BUILD_TYPE=Release
cmake --build cmake-build-release --target QMud -j"$(nproc)"
```

### Linux AppImage package

```bash
cmake -S . -B cmake-build-release \
  -DCMAKE_BUILD_TYPE=Release \
  -DQMUD_ENABLE_APPIMAGE=ON
cmake --build cmake-build-release --target AppImage -j"$(nproc)"
```

The packaged AppImage is generated under `cmake-build-release/appimage/`.

### Windows (Visual Studio 2022)

```powershell
cmake -S . -B cmake-build-release -G "Visual Studio 17 2022" -A x64
cmake --build cmake-build-release --config Release --target QMud
```

### macOS (Ninja/Makefiles)

```bash
cmake -S . -B cmake-build-release -DCMAKE_BUILD_TYPE=Release
cmake --build cmake-build-release --target QMud -j"$(sysctl -n hw.ncpu)"
```

### Cross/Docker-build AppImage/Windows/macOS on Linux

Build the cross-build images first:

```bash
docker build -t qmud-appimage-builder:qt6.10 -f tools/docker/appimage-qt610/Dockerfile tools/docker/appimage-qt610
docker build -t qmud-macos-builder:qt6.10 -f tools/docker/macos-qt610/Dockerfile tools/docker/macos-qt610
docker build -t qmud-windows-builder:qt6.10 -f tools/docker/windows-qt610/Dockerfile tools/docker/windows-qt610
```

Configure once (Docker targets are Linux-host only):

```bash
cmake -S . -B cmake-build-release \
  -DCMAKE_BUILD_TYPE=Release \
  -DQMUD_ENABLE_APPIMAGE=OFF \
  -DQMUD_ENABLE_APPIMAGE_DOCKER=ON \
  -DQMUD_ENABLE_MAC_DOCKER=ON \
  -DQMUD_ENABLE_WINDOCKER=ON \
  -DQMUD_DOCKER_EXECUTABLE=docker
```

Build cross targets:

```bash
cmake --build cmake-build-release --target AppImageDocker
cmake --build cmake-build-release --target MacDocker
cmake --build cmake-build-release --target WinDocker
```

Artifacts are written to:

- AppImage: `cmake-build-release/appimage-docker-out`
- macOS: `cmake-build-release/mac-docker-out`
- Windows: `cmake-build-release/windows-docker-out`

## Running tests

Configure with tests enabled:

```bash
cmake -S . -B cmake-build-release \
  -DCMAKE_BUILD_TYPE=Release \
  -DQMUD_ENABLE_TESTING=ON \
  -DQMUD_ENABLE_GUI_TESTS=ON
```

Build and run all registered tests:

```bash
cmake --build cmake-build-release -j"$(nproc)"
ctest --test-dir cmake-build-release --output-on-failure
```

Run the default quick suite used for pull requests:

```bash
ctest --test-dir cmake-build-release --output-on-failure --label-exclude slow
```

CI policy:

- `.github/workflows/pipelines.yml` is the authoritative CI workflow.
- Pull requests should require the `Pipelines / PR/Push quick suite (exclude slow)` and package build jobs to pass
  before merge.

## Purposeful deviations from MUSHclient

These are intentional design choices in QMud:

- Qt-native UI/runtime stack instead of MFC.
- Regex engine uses `QRegularExpression` (PCRE2 behavior).
- XML parsing/serialization uses Qt XML APIs (`QXmlStreamReader`).
- Lua `sqlite3` integration is implemented on top of Qt SQL (`QSqlDatabase`/`QSqlQuery`) via the in-tree Lua binding
  layer.
- Windows Script Host integrations have not been ported; Lua is the ONLY supported scripting engine.
- PNG has been depracated and handled with native Qt.
- Legacy SHS code was deprecated; hashing paths use Qt (`QCryptographicHash`).
- Newly written XML metadata uses `qmud` elements; legacy `muclient` are still read for compatibility.

## Contributors

- Abigail Brady ([Cryosphere](https://cryosphere.org/))
