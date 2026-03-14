# QMud

QMud is a Qt 6 port and continuation of the
original [MUSHclient](https://www.mushclient.com/mushclient/mushclient.htm) (by Nick Gammon),
designed and written by Panagiotis Kalogiratos (Nodens) of [CthulhuMUD](https://www.cthulhumud.com).
It is compatible with existing files and plugins, but it will migrate them to
its own format in order to maintain separation. As more features are implemented,
things were bound to diverge, especially in data persistence, so, as a conscious
choice, QMud diverges from the get-go.
The active implementation in this repository is C++20 + Qt 6.10.

## Project status

The porting has been completed. Behavior aims for high compatibility
with original persistence and Lua workflows while using a modern Qt
implementation. There are several improvements and new features implemented
already. The possibility of bugs is currently high as not everything has been
tested to the ground. Please use the issue tracker, with the appropriate
template to report issues, request features, etc.

## Contact / Support

For support, testing feedback, and development discussion, join:

- [CthulhuMUD Discord](https://discord.gg/secxwnTJCq)

Do **NOT** use the issue tracker for general support requests.

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

### Cross-build Windows/macOS on Linux (Docker)

Build the cross-build images first:

```bash
docker build -t qmud-macos-builder:qt6.10 -f tools/docker/macos-qt610/Dockerfile tools/docker/macos-qt610
docker build -t qmud-windows-builder:qt6.10 -f tools/docker/windows-qt610/Dockerfile tools/docker/windows-qt610
```

Configure once (Docker targets are Linux-host only):

```bash
cmake -S . -B cmake-build-release \
  -DCMAKE_BUILD_TYPE=Release \
  -DQMUD_ENABLE_MAC_DOCKER=ON \
  -DQMUD_ENABLE_WINDOCKER=ON \
  -DQMUD_DOCKER_EXECUTABLE=docker
```

Build cross targets:

```bash
cmake --build cmake-build-release --target MacDocker
cmake --build cmake-build-release --target WinDocker
```

Artifacts are written to:

- macOS: `cmake-build-release/mac-docker-out`
- Windows: `cmake-build-release/windows-docker-out`

## Data directory resolution (`QMUD_HOME`)

QMud resolves its startup/data directory in this order:

1. `QMUD_HOME` environment variable (all platforms).
2. If env var is missing, `QMUD_HOME` from system config file:
    - Linux: `/etc/QMud/config`
    - macOS: `/Library/Application Support/QMud/config`
    - Windows: `%LOCALAPPDATA%/QMud/config`

System config lines support both:

- `QMUD_HOME=/path/to/dir`
- `export QMUD_HOME=/path/to/dir`

Quoted values are accepted, and leading `~` is expanded.

If nothing is configured, defaults are:

- AppImage: `$HOME/QMud`
- macOS: `~/Library/Application Support/QMud`
- Windows and non-AppImage Linux: executable directory

## Environment flags and CLI switches

### Environment flags

- `QMUD_HOME`: Overrides startup/data directory resolution (see section above).
- `QMUD_ALLOW_MULTI_INSTANCE`: When set to `1`, `y`, `yes`, or `true`, bypasses single-instance enforcement.

### CLI switches

- `--multi-instance` (alias: `--allow-multi-instance`): Bypass single-instance enforcement for that process. (Not safe
  with same datadir)
- `--dump-lua-api <output-dir>`: Export Lua API inventory to the given directory and exit.

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
