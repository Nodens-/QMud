#!/bin/sh
set -eu

PROJECT_DIR=${PROJECT_DIR:-/home/user/project}
BUILD_DIR=${BUILD_DIR:-/home/user/build}
QMUD_WINDOCKER_BUILD_TYPE=${QMUD_WINDOCKER_BUILD_TYPE:-Release}
QMUD_WINDOCKER_QT_PREFIX=${QMUD_WINDOCKER_QT_PREFIX:-/opt/Qt/latest/mingw_64}
QMUD_WINDOCKER_QT_HOST_PREFIX=${QMUD_WINDOCKER_QT_HOST_PREFIX:-/usr/lib64/qt6}
QMUD_WINDOCKER_MINGW_PREFIX=${QMUD_WINDOCKER_MINGW_PREFIX:-/usr/x86_64-w64-mingw32/sys-root/mingw}
QMUD_WINDOCKER_MINGW_TRIPLET=${QMUD_WINDOCKER_MINGW_TRIPLET:-x86_64-w64-mingw32}
QMUD_WINDOCKER_LUA_PREFIX=${QMUD_WINDOCKER_LUA_PREFIX:-/opt/qmud/windows-deps/lua}
QMUD_WINDOCKER_LUA_MODULES_PREFIX=${QMUD_WINDOCKER_LUA_MODULES_PREFIX:-/opt/qmud/windows-deps/lua-modules}
QMUD_WINDOCKER_NVDA_CONTROLLER_URL=${QMUD_WINDOCKER_NVDA_CONTROLLER_URL:-}

rm -f "$BUILD_DIR/CMakeCache.txt"
rm -rf "$BUILD_DIR/CMakeFiles"
export CCACHE_DISABLE=1
unset CCACHE_DIR CCACHE_TEMPDIR

QT_PREFIX="$QMUD_WINDOCKER_QT_PREFIX"
QT_HOST_PREFIX="$QMUD_WINDOCKER_QT_HOST_PREFIX"
MINGW_PREFIX="$QMUD_WINDOCKER_MINGW_PREFIX"
LUA_PREFIX="$QMUD_WINDOCKER_LUA_PREFIX"
LUA_MODULES_PREFIX="$QMUD_WINDOCKER_LUA_MODULES_PREFIX"
MINGW_TRIPLET="$QMUD_WINDOCKER_MINGW_TRIPLET"
MINGW_INCLUDE_DIR="$MINGW_PREFIX/include"
MINGW_LIB_DIR="$MINGW_PREFIX/lib"
PE_OBJDUMP="${MINGW_TRIPLET}-objdump"

if [ "$MINGW_TRIPLET" != "x86_64-w64-mingw32" ]; then
  echo "Error: unsupported MinGW triplet '$MINGW_TRIPLET'. Only x86_64-w64-mingw32 is supported." >&2
  exit 1
fi
if ! command -v "$PE_OBJDUMP" >/dev/null 2>&1; then
  echo "Error: ${PE_OBJDUMP} was not found; Windows dependency staging requires mingw64-binutils." >&2
  exit 1
fi

case "$QT_PREFIX" in
  */mingw_64|*/mingw_64/*)
    ;;
  *)
    echo "Error: unsupported Qt target prefix '$QT_PREFIX'. Expected a 64-bit mingw_64 Qt SDK path." >&2
    exit 1
    ;;
esac

export PKG_CONFIG_SYSROOT_DIR="$MINGW_PREFIX"
export PKG_CONFIG_LIBDIR="$MINGW_LIB_DIR/pkgconfig:$MINGW_LIB_DIR/share/pkgconfig"
export PKG_CONFIG_PATH=

if [ ! -x "$QT_HOST_PREFIX/libexec/moc" ]; then
  for QT_HOST_CANDIDATE in /usr/lib64/qt6 /usr/lib/qt6 /opt/Qt/latest/gcc_64 /opt/Qt/latest/linux_gcc_64; do
    if [ -x "$QT_HOST_CANDIDATE/libexec/moc" ]; then
      QT_HOST_PREFIX="$QT_HOST_CANDIDATE"
      break
    fi
  done
fi
if [ -d "$QT_HOST_PREFIX/bin" ]; then
  export PATH="$QT_HOST_PREFIX/bin:$PATH"
fi
QT_HOST_MOC="$QT_HOST_PREFIX/libexec/moc"
QT_HOST_UIC="$QT_HOST_PREFIX/libexec/uic"
QT_HOST_RCC="$QT_HOST_PREFIX/libexec/rcc"
if [ ! -x "$QT_HOST_MOC" ] && command -v moc >/dev/null 2>&1; then
  QT_HOST_MOC="$(command -v moc)"
fi
if [ ! -x "$QT_HOST_UIC" ] && command -v uic >/dev/null 2>&1; then
  QT_HOST_UIC="$(command -v uic)"
fi
if [ ! -x "$QT_HOST_RCC" ] && command -v rcc >/dev/null 2>&1; then
  QT_HOST_RCC="$(command -v rcc)"
fi
QT_HOST_PATH_ROOT="$QT_HOST_PREFIX"
QT_HOST_CMAKE_DIR="$QT_HOST_PREFIX/lib/cmake"
if [ "$QT_HOST_PREFIX" = "/usr/lib64/qt6" ] || [ "$QT_HOST_PREFIX" = "/usr/lib/qt6" ]; then
  QT_HOST_PATH_ROOT=/usr
fi
if [ -d "$QT_HOST_PREFIX/lib64/cmake/Qt6" ]; then
  QT_HOST_CMAKE_DIR="$QT_HOST_PREFIX/lib64/cmake"
elif [ -d "$QT_HOST_PREFIX/lib/cmake/Qt6" ]; then
  QT_HOST_CMAKE_DIR="$QT_HOST_PREFIX/lib/cmake"
elif [ -d /usr/lib64/cmake/Qt6 ]; then
  QT_HOST_CMAKE_DIR=/usr/lib64/cmake
elif [ -d /usr/lib/cmake/Qt6 ]; then
  QT_HOST_CMAKE_DIR=/usr/lib/cmake
fi

CMAKE_EXE=cmake
if [ -x /opt/Qt/Tools/CMake/bin/cmake ]; then
  CMAKE_EXE=/opt/Qt/Tools/CMake/bin/cmake
fi

"$CMAKE_EXE" -S "$PROJECT_DIR" -G Ninja -B "$BUILD_DIR" \
  -DCMAKE_SYSTEM_NAME=Windows \
  -DCMAKE_C_COMPILER="${MINGW_TRIPLET}-gcc" \
  -DCMAKE_CXX_COMPILER="${MINGW_TRIPLET}-g++" \
  -DCMAKE_RC_COMPILER="${MINGW_TRIPLET}-windres" \
  -DCMAKE_C_FLAGS="-UUNICODE -U_UNICODE" \
  -DCMAKE_CXX_FLAGS="-UUNICODE -U_UNICODE" \
  -DCMAKE_FIND_ROOT_PATH="${MINGW_PREFIX};${QT_PREFIX}" \
  -DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER \
  -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY \
  -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY \
  -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=BOTH \
  -DCMAKE_BUILD_TYPE="$QMUD_WINDOCKER_BUILD_TYPE" \
  -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF \
  -DQMUD_PROBE_IPO=ON \
  -DVulkan_INCLUDE_DIR="$MINGW_INCLUDE_DIR" \
  -DQt6_DIR="$QT_PREFIX/lib/cmake/Qt6" \
  -DQt6Multimedia_DIR="$QT_PREFIX/lib/cmake/Qt6Multimedia" \
  -DQt6TextToSpeech_DIR="$QT_PREFIX/lib/cmake/Qt6TextToSpeech" \
  -DQT_HOST_PATH="$QT_HOST_PATH_ROOT" \
  -DQT_HOST_PATH_CMAKE_DIR="$QT_HOST_CMAKE_DIR" \
  -DQT_NO_QTPATHS_DEPLOYMENT_WARNING=ON \
  -DCMAKE_AUTOMOC_EXECUTABLE="$QT_HOST_MOC" \
  -DQMUD_SYSTEM_LUA_MODULES_PREFIX="$LUA_MODULES_PREFIX" \
  -DLUA_INCLUDE_DIR="$LUA_PREFIX/include" \
  -DLUA_LIBRARY="$LUA_PREFIX/lib/lua54.dll.a" \
  -DQMUD_LPEG_PROVIDER=SYSTEM \
  -DQMUD_BC_PROVIDER=BUNDLED \
  -DQMUD_LUASOCKET_PROVIDER=SYSTEM

cmake --build "$BUILD_DIR" --target QMud -j 6

QMUD_EXE="$(find "$BUILD_DIR" -maxdepth 4 -type f -name 'QMud.exe' | sort | head -n 1)"
if [ -z "$QMUD_EXE" ]; then
  echo "Error: could not find QMud.exe after build." >&2
  exit 1
fi

PACKAGE_NAME=QMud
STAGE_ROOT="$BUILD_DIR/windocker-out"
STAGE_DIR="$STAGE_ROOT/$PACKAGE_NAME"
rm -rf "$STAGE_ROOT"
mkdir -p "$STAGE_DIR"
cp "$QMUD_EXE" "$STAGE_DIR/QMud.exe"
mkdir -p "$STAGE_DIR/lib"

lower_name() {
  printf '%s' "$1" | tr '[:upper:]' '[:lower:]'
}

is_packaged_dependency_dll() {
  dll_lower="$(lower_name "$1")"
  case "$dll_lower" in
    qt6*.dll | lua54.dll | lua.dll | libgcc_s_seh-1.dll | libstdc++-6.dll | libwinpthread-1.dll | \
      zlib1.dll | libatomic-1.dll | libssp-0.dll | libssl-*.dll | libcrypto-*.dll | avcodec-*.dll | \
      avformat-*.dll | avutil-*.dll | swresample-*.dll | swscale-*.dll | d3dcompiler_47.dll | opengl32sw.dll)
      return 0
      ;;
  esac
  return 1
}

find_dependency_dll() {
  dll="$1"
  dll_lower="$(lower_name "$dll")"

  case "$dll_lower" in
    qt6*.dll | avcodec-*.dll | avformat-*.dll | avutil-*.dll | swresample-*.dll | swscale-*.dll | \
      d3dcompiler_47.dll | opengl32sw.dll)
      search_dirs="$STAGE_DIR
$STAGE_DIR/lib
$QT_PREFIX/bin
$MINGW_PREFIX/bin
$LUA_PREFIX/bin
$LUA_PREFIX/lib"
      ;;
    lua54.dll | lua.dll)
      search_dirs="$STAGE_DIR
$STAGE_DIR/lib
$LUA_PREFIX/bin
$LUA_PREFIX/lib
$MINGW_PREFIX/bin
$QT_PREFIX/bin"
      ;;
    libgcc_s_seh-1.dll | libstdc++-6.dll | libwinpthread-1.dll | zlib1.dll | libatomic-1.dll | \
      libssp-0.dll | libssl-*.dll | libcrypto-*.dll)
      search_dirs="$STAGE_DIR
$STAGE_DIR/lib
$MINGW_PREFIX/bin
$QT_PREFIX/bin
$LUA_PREFIX/bin
$LUA_PREFIX/lib"
      ;;
    *)
      search_dirs="$STAGE_DIR
$STAGE_DIR/lib
$QT_PREFIX/bin
$MINGW_PREFIX/bin
$LUA_PREFIX/bin
$LUA_PREFIX/lib"
      ;;
  esac

  printf '%s\n' "$search_dirs" | while IFS= read -r search_dir; do
    [ -d "$search_dir" ] || continue
    found="$(find "$search_dir" -maxdepth 1 -type f -iname "$dll" | sort | head -n 1 || true)"
    if [ -n "$found" ]; then
      printf '%s\n' "$found"
      return 0
    fi
  done
}

dependency_is_staged() {
  dll="$1"
  find "$STAGE_DIR" "$STAGE_DIR/lib" -maxdepth 1 -type f -iname "$dll" | grep -q .
}

collect_dll_closure() {
  destination="$1"
  shift
  mkdir -p "$destination"

  queue_file="$BUILD_DIR/windocker-deps-queue.$$"
  seen_file="$BUILD_DIR/windocker-deps-seen.$$"
  : >"$queue_file"
  : >"$seen_file"
  for binary in "$@"; do
    [ -f "$binary" ] || continue
    printf '%s\n' "$binary" >>"$queue_file"
  done

  while [ -s "$queue_file" ]; do
    binary="$(sed -n '1p' "$queue_file")"
    sed '1d' "$queue_file" >"$queue_file.next"
    mv "$queue_file.next" "$queue_file"
    [ -f "$binary" ] || continue

    "$PE_OBJDUMP" -p "$binary" 2>/dev/null | sed -n 's/^[[:space:]]*DLL Name: //p' | while IFS= read -r dependency; do
      [ -n "$dependency" ] || continue
      dependency_lower="$(lower_name "$dependency")"
      if grep -Fxq "$dependency_lower" "$seen_file"; then
        continue
      fi
      printf '%s\n' "$dependency_lower" >>"$seen_file"

      if ! is_packaged_dependency_dll "$dependency"; then
        continue
      fi
      if dependency_is_staged "$dependency"; then
        continue
      fi

      source_path="$(find_dependency_dll "$dependency" || true)"
      if [ -z "$source_path" ]; then
        echo "Error: unresolved packaged Windows dependency '$dependency' required by $binary." >&2
        exit 1
      fi

      target_path="$destination/$(basename "$source_path")"
      cp -f "$source_path" "$target_path"
      printf '%s\n' "$target_path" >>"$queue_file"
    done
  done

  rm -f "$queue_file" "$seen_file"
}

download_url() {
  url="$1"
  output="$2"
  if command -v curl >/dev/null 2>&1; then
    curl -fsSL "$url" -o "$output"
    return
  fi
  if command -v wget >/dev/null 2>&1; then
    wget -q -O "$output" "$url"
    return
  fi
  echo "Error: curl or wget is required to download $url." >&2
  exit 1
}

stage_nvda_controller_client() {
  destination="$STAGE_DIR/lua/native/windows-x86_64"
  mkdir -p "$destination"

  controller_url="$QMUD_WINDOCKER_NVDA_CONTROLLER_URL"
  if [ -z "$controller_url" ]; then
    index_file="$BUILD_DIR/nvda-controller-stable-index.html"
    download_url "https://download.nvaccess.org/releases/stable/" "$index_file"
    controller_href="$(
      sed -n 's/.*href="\([^"]*controllerClient\.zip\)".*/\1/p' "$index_file" |
        sort -V |
        tail -n 1
    )"
    if [ -z "$controller_href" ]; then
      echo "Error: could not find an NVDA controllerClient zip in the stable release index." >&2
      exit 1
    fi
    case "$controller_href" in
      https://* | http://*)
        controller_url="$controller_href"
        ;;
      *)
        controller_url="https://download.nvaccess.org/releases/stable/$controller_href"
        ;;
    esac
  fi

  archive="$BUILD_DIR/nvda-controllerClient.zip"
  extract_dir="$BUILD_DIR/nvda-controllerClient"
  rm -rf "$extract_dir"
  mkdir -p "$extract_dir"
  download_url "$controller_url" "$archive"
  cmake -E chdir "$extract_dir" cmake -E tar xf "$archive"

  dll_path="$(
    find "$extract_dir" -type f \( \
      -path '*/x64/nvdaControllerClient.dll' -o \
      -iname 'nvdaControllerClient64.dll' \
    \) | sort | head -n 1
  )"
  if [ -z "$dll_path" ]; then
    echo "Error: NVDA controllerClient archive does not contain an x64 controller DLL." >&2
    exit 1
  fi
  cp -f "$dll_path" "$destination/$(basename "$dll_path")"

  license_path="$(find "$extract_dir" -type f -iname 'license.txt' | sort | head -n 1)"
  if [ -n "$license_path" ]; then
    mkdir -p "$STAGE_DIR/docs/licenses"
    cp -f "$license_path" "$STAGE_DIR/docs/licenses/NVDA_client_LICENSE.txt"
  fi
}

copy_qt_plugin() {
  relative_path="$1"
  required="$2"
  source_path="$QT_PREFIX/plugins/$relative_path"
  target_path="$STAGE_DIR/qtplugins/$relative_path"
  if [ ! -f "$source_path" ]; then
    if [ "$required" = "required" ]; then
      echo "Error: required Qt plugin is missing: $source_path" >&2
      exit 1
    fi
    return 0
  fi
  mkdir -p "$(dirname "$target_path")"
  cp -f "$source_path" "$target_path"
}

copy_qt_plugin platforms/qwindows.dll required
copy_qt_plugin sqldrivers/qsqlite.dll required
copy_qt_plugin tls/qcertonlybackend.dll optional
copy_qt_plugin tls/qopensslbackend.dll optional
copy_qt_plugin tls/qschannelbackend.dll optional
copy_qt_plugin texttospeech/qtexttospeech_sapi.dll required
copy_qt_plugin iconengines/qsvgicon.dll required
copy_qt_plugin imageformats/qgif.dll required
copy_qt_plugin imageformats/qico.dll required
copy_qt_plugin imageformats/qjpeg.dll required
copy_qt_plugin imageformats/qsvg.dll required
copy_qt_plugin styles/qmodernwindowsstyle.dll optional
copy_qt_plugin networkinformation/qnetworklistmanager.dll optional

if [ ! -d "$QT_PREFIX/plugins/multimedia" ]; then
  echo "Error: Qt Multimedia plugins directory is missing: $QT_PREFIX/plugins/multimedia" >&2
  exit 1
fi
mkdir -p "$STAGE_DIR/qtplugins/multimedia"
find "$QT_PREFIX/plugins/multimedia" -maxdepth 1 -type f -iname '*.dll' -exec cp -f {} "$STAGE_DIR/qtplugins/multimedia/" \;

TLS_PLUGIN_DIR="$STAGE_DIR/qtplugins/tls"
if [ ! -d "$TLS_PLUGIN_DIR" ]; then
  echo "Error: Qt TLS plugins directory is missing from staged package: $TLS_PLUGIN_DIR" >&2
  exit 1
fi
if ! find "$TLS_PLUGIN_DIR" -maxdepth 1 -type f \( -iname 'qschannelbackend.dll' -o -iname 'qopensslbackend.dll' \) | grep -q .; then
  echo "Error: Windows package is missing a functional Qt TLS backend plugin." >&2
  echo "Expected qschannelbackend.dll or qopensslbackend.dll in $TLS_PLUGIN_DIR." >&2
  exit 1
fi
TTS_PLUGIN_DIR="$STAGE_DIR/qtplugins/texttospeech"
if [ ! -d "$TTS_PLUGIN_DIR" ]; then
  echo "Error: Qt TextToSpeech plugins directory is missing from staged package: $TTS_PLUGIN_DIR" >&2
  exit 1
fi
if ! find "$TTS_PLUGIN_DIR" -maxdepth 1 -type f -iname '*.dll' | grep -q .; then
  echo "Error: Windows package is missing Qt TextToSpeech engine plugins." >&2
  echo "Expected at least one plugin DLL in $TTS_PLUGIN_DIR." >&2
  exit 1
fi

collect_dll_closure "$STAGE_DIR" "$STAGE_DIR/QMud.exe"

printf '%s\n' '[Paths]' 'Plugins = qtplugins' > "$STAGE_DIR/qt.conf"

cp -R "$PROJECT_DIR/skeleton/." "$STAGE_DIR/"
if [ -d "$BUILD_DIR/lua" ]; then
  rm -rf "$STAGE_DIR/lua"
  cp -R "$BUILD_DIR/lua" "$STAGE_DIR/lua"
else
  echo "Error: expected generated Lua directory at $BUILD_DIR/lua, but it was not found." >&2
  exit 1
fi

mkdir -p "$STAGE_DIR/lua" "$STAGE_DIR/socket" "$STAGE_DIR/mime"
mkdir -p \
  "$STAGE_DIR/lua/native/linux-x86_64" \
  "$STAGE_DIR/lua/native/macos-universal" \
  "$STAGE_DIR/lua/native/macos-arm64" \
  "$STAGE_DIR/lua/native/macos-x86_64" \
  "$STAGE_DIR/lua/native/windows-x86_64"

stage_nvda_controller_client

if [ ! -f "$BUILD_DIR/socket/core.dll" ]; then
  echo "Error: expected generated LuaSocket core module at $BUILD_DIR/socket/core.dll, but it was not found." >&2
  exit 1
fi
cp "$BUILD_DIR/socket/core.dll" "$STAGE_DIR/socket/core.dll"

if [ ! -f "$BUILD_DIR/mime/core.dll" ]; then
  echo "Error: expected generated LuaSocket mime module at $BUILD_DIR/mime/core.dll, but it was not found." >&2
  exit 1
fi
cp "$BUILD_DIR/mime/core.dll" "$STAGE_DIR/mime/core.dll"

mkdir -p "$STAGE_DIR/ssl" "$STAGE_DIR/lua/ssl"

for name in socket.lua ltn12.lua mime.lua; do
  if [ ! -f "$BUILD_DIR/lua/$name" ]; then
    echo "Error: expected generated LuaSocket Lua file at $BUILD_DIR/lua/$name, but it was not found." >&2
    exit 1
  fi
  cp "$BUILD_DIR/lua/$name" "$STAGE_DIR/lua/$name"
done

set -- "$BUILD_DIR"/socket/*.lua
if [ ! -f "$1" ]; then
  echo "Error: expected generated LuaSocket Lua files at $BUILD_DIR/socket/*.lua, but none were found." >&2
  exit 1
fi
for socket_lua in "$BUILD_DIR"/socket/*.lua; do
  cp "$socket_lua" "$STAGE_DIR/socket/$(basename "$socket_lua")"
done
if [ ! -f "$STAGE_DIR/socket/headers.lua" ]; then
  echo "Error: staged LuaSocket modules are missing headers.lua in $STAGE_DIR/socket." >&2
  exit 1
fi

if [ -f "$LUA_MODULES_PREFIX/lpeg.dll" ]; then
  cp "$LUA_MODULES_PREFIX/lpeg.dll" "$STAGE_DIR/lua/lpeg.dll"
fi
for name in json.lua re.lua ssl.lua; do
  if [ -f "$LUA_MODULES_PREFIX/$name" ]; then
    cp "$LUA_MODULES_PREFIX/$name" "$STAGE_DIR/lua/$name"
  fi
done
if [ -d "$LUA_MODULES_PREFIX/json" ]; then
  mkdir -p "$STAGE_DIR/lua/json"
  cp -R "$LUA_MODULES_PREFIX/json/." "$STAGE_DIR/lua/json/"
fi
if [ ! -f "$LUA_MODULES_PREFIX/ssl/core.dll" ]; then
  echo "Error: expected LuaSec core module at $LUA_MODULES_PREFIX/ssl/core.dll, but it was not found." >&2
  exit 1
fi
cp "$LUA_MODULES_PREFIX/ssl/core.dll" "$STAGE_DIR/ssl/core.dll"
cp "$LUA_MODULES_PREFIX/ssl/core.dll" "$STAGE_DIR/lua/ssl/core.dll"
if [ -d "$LUA_MODULES_PREFIX/ssl" ]; then
  cp -R "$LUA_MODULES_PREFIX/ssl/." "$STAGE_DIR/ssl/"
  cp -R "$LUA_MODULES_PREFIX/ssl/." "$STAGE_DIR/lua/ssl/"
fi
if [ ! -f "$STAGE_DIR/lua/ssl.lua" ]; then
  echo "Error: expected LuaSec top-level module ssl.lua at $LUA_MODULES_PREFIX/ssl.lua, but it was not found." >&2
  exit 1
fi

PLUGIN_DLLS="$(find "$STAGE_DIR/qtplugins" -type f -iname '*.dll' | sort)"
MODULE_DLLS="$(find "$STAGE_DIR/lua" "$STAGE_DIR/socket" "$STAGE_DIR/mime" "$STAGE_DIR/ssl" -type f -iname '*.dll' | sort)"
# shellcheck disable=SC2086
collect_dll_closure "$STAGE_DIR/lib" $PLUGIN_DLLS $MODULE_DLLS

cmake -E rm -f "$STAGE_ROOT/$PACKAGE_NAME.zip"
cmake -E chdir "$STAGE_ROOT" cmake -E tar cf "$PACKAGE_NAME.zip" --format=zip "$PACKAGE_NAME"

WININSTALL_SCRIPT="$PROJECT_DIR/tools/scripts/wininstall.nsi"
if [ ! -f "$WININSTALL_SCRIPT" ]; then
  echo "Error: missing NSIS script: $WININSTALL_SCRIPT" >&2
  exit 1
fi
if ! command -v makensis >/dev/null 2>&1; then
  echo "Error: makensis was not found in PATH. Install NSIS in the Windows build environment." >&2
  exit 1
fi
MAKENSIS_BIN="$(command -v makensis)"
NSISDIR="/usr/share/nsis"
if [ ! -f "$NSISDIR/Stubs/zlib-amd64-unicode" ]; then
  echo "Error: 64-bit NSIS stub zlib-amd64-unicode was not found at $NSISDIR/Stubs." >&2
  echo "Install mingw64-nsis in the Fedora Windows builder image." >&2
  exit 1
fi

QMUD_VERSION="$(sed -n 's/.*kVersionString\[\] = \"\([^\"]*\)\".*/\1/p' "$PROJECT_DIR/src/Version.h" | head -n 1)"
if [ -z "$QMUD_VERSION" ]; then
  QMUD_VERSION=dev
fi
INSTALLER_ICON="$PROJECT_DIR/resources/windows/QMud.ico"
if [ ! -f "$INSTALLER_ICON" ]; then
  echo "Error: missing Windows installer icon: $INSTALLER_ICON" >&2
  exit 1
fi

INSTALLER_OUT="$STAGE_ROOT/QMud-setup.exe"
rm -f "$INSTALLER_OUT"
NSISDIR="$NSISDIR" "$MAKENSIS_BIN" -V2 \
  -DQMUD_SOURCE_DIR="$STAGE_DIR" \
  -DQMUD_OUTPUT_FILE="$INSTALLER_OUT" \
  -DQMUD_VERSION="$QMUD_VERSION" \
  -DQMUD_INSTALLER_ICON="$INSTALLER_ICON" \
  "$WININSTALL_SCRIPT"

if [ ! -f "$INSTALLER_OUT" ]; then
  echo "Error: NSIS installer was not produced: $INSTALLER_OUT" >&2
  exit 1
fi
