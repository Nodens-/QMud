#!/usr/bin/env bash
## @file mac_docker_build_universal.sh
## @brief Builds a universal macOS QMud.app inside the MacDocker builder image.

set -euo pipefail

PROJECT_DIR=${PROJECT_DIR:-/home/user/project}
BUILD_DIR=${BUILD_DIR:-/home/user/build}
QMUD_MAC_DOCKER_IMAGE=${QMUD_MAC_DOCKER_IMAGE:-qmud-macos-builder:qt6.11}
QMUD_MAC_DOCKER_BUILD_TYPE=${QMUD_MAC_DOCKER_BUILD_TYPE:-Release}
QMUD_MAC_DOCKER_OSX_DEPLOYMENT_TARGET=${QMUD_MAC_DOCKER_OSX_DEPLOYMENT_TARGET:-13.0.0}
QMUD_MAC_DOCKER_QT_PREFIX=${QMUD_MAC_DOCKER_QT_PREFIX:-/opt/Qt/latest/macos}
QMUD_MAC_DOCKER_QT_HOST_PREFIX=${QMUD_MAC_DOCKER_QT_HOST_PREFIX:-/usr/lib64/qt6}
QMUD_MAC_DOCKER_LUA_PREFIX=${QMUD_MAC_DOCKER_LUA_PREFIX:-${QMUD_MACOS_LUA_PREFIX:-/opt/qmud/macos-deps/lua}}
QMUD_MAC_DOCKER_LUA_MODULES_PREFIX=${QMUD_MAC_DOCKER_LUA_MODULES_PREFIX:-${QMUD_MACOS_LUA_MODULES:-/opt/qmud/macos-deps/lua-modules}}

X86_BUILD_DIR="$BUILD_DIR/x86_64"
ARM_BUILD_DIR="$BUILD_DIR/arm64"

rm -rf "$X86_BUILD_DIR" "$ARM_BUILD_DIR" "$BUILD_DIR/macapp-out"
export PATH="$QMUD_MAC_DOCKER_QT_PREFIX/bin:$PATH"

QT_HOST_PREFIX="$QMUD_MAC_DOCKER_QT_HOST_PREFIX"
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
if [ ! -x "$QT_HOST_MOC" ] || [ ! -x "$QT_HOST_UIC" ] || [ ! -x "$QT_HOST_RCC" ]; then
  echo "Error: runnable host Qt tools missing (moc/uic/rcc)." >&2
  echo "Install Qt host tools in ${QMUD_MAC_DOCKER_IMAGE} (for example: dnf install qt6-qtbase-devel)." >&2
  exit 1
fi

if command -v osxcross-conf >/dev/null 2>&1; then
  eval "$(osxcross-conf)"
fi
if [ -z "${OSXCROSS_TARGET_DIR:-}" ]; then
  OSXCROSS_TARGET_DIR=/opt/osxcross/target
fi
if [ -z "${OSXCROSS_SDK:-}" ]; then
  OSXCROSS_SDK=$(find "${OSXCROSS_TARGET_DIR}/SDK" -maxdepth 1 -type d -name 'MacOSX*.sdk' | sort | tail -n 1)
fi

find_osxcross_host() {
  local pattern=$1
  local host
  host=$(find -L /opt/osxcross/bin "${OSXCROSS_TARGET_DIR}/bin" -maxdepth 1 \( -type f -o -type l \) -name "${pattern}-apple-darwin*-clang" 2>/dev/null | sort | head -n 1 | awk -F/ '{print $NF}' | sed 's/-clang$//')
  printf '%s' "$host"
}

OSXCROSS_X86_HOST=$(find_osxcross_host x86_64)
if [ -z "$OSXCROSS_X86_HOST" ]; then
  echo "Error: universal macOS build requires the osxcross Clang toolchain." >&2
  echo "Resolved x86_64 host: ${OSXCROSS_X86_HOST:-<missing>}" >&2
  exit 1
fi

resolve_tool() {
  local tool_name=$1
  local candidate
  for candidate in \
    "/opt/osxcross/bin/${tool_name}" \
    "${OSXCROSS_TARGET_DIR}/bin/${tool_name}" \
    "/opt/osxcross/bin/${OSXCROSS_X86_HOST}-${tool_name}" \
    "${OSXCROSS_TARGET_DIR}/bin/${OSXCROSS_X86_HOST}-${tool_name}" \
    "/opt/osxcross/bin/x86_64-apple-darwin-${tool_name}" \
    "${OSXCROSS_TARGET_DIR}/bin/x86_64-apple-darwin-${tool_name}"; do
    if [ -x "$candidate" ]; then
      printf '%s' "$candidate"
      return 0
    fi
  done
  if command -v "$tool_name" >/dev/null 2>&1; then
    command -v "$tool_name"
    return 0
  fi
  return 1
}

LIPO=$(resolve_tool lipo || true)
if [ -z "$LIPO" ]; then
  LIPO=$(resolve_tool llvm-lipo || true)
fi
if [ -z "$LIPO" ]; then
  echo "Error: could not resolve lipo or llvm-lipo for universal macOS binaries." >&2
  exit 1
fi

export OSXCROSS_TARGET_DIR
export OSXCROSS_SDK
export QT_CHAINLOAD_TOOLCHAIN_FILE=/opt/osxcross/target/toolchain.cmake
export CCACHE_DISABLE=1
export SOURCE_DATE_EPOCH=${SOURCE_DATE_EPOCH:-$(date +%s)}
unset CCACHE_DIR CCACHE_TEMPDIR

QT_TOOLCHAIN="$QMUD_MAC_DOCKER_QT_PREFIX/lib/cmake/Qt6/qt.toolchain.cmake"
if [ ! -f "$QT_TOOLCHAIN" ]; then
  echo "Error: Qt toolchain file is missing at $QT_TOOLCHAIN" >&2
  exit 1
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

verify_universal() {
  local path=$1
  local info
  if [ ! -e "$path" ]; then
    echo "Error: expected universal binary is missing at $path." >&2
    exit 1
  fi
  info=$("$LIPO" -info "$path")
  case "$info" in
    *x86_64*arm64*|*arm64*x86_64*) ;;
    *)
      echo "Error: $path is not universal: $info" >&2
      exit 1
      ;;
  esac
}

configure_slice() {
  local arch=$1
  local build_dir=$2
  local output_dir=$3
  local cmake_exe="/opt/osxcross/bin/${OSXCROSS_X86_HOST}-cmake"

  if [ ! -x "$cmake_exe" ]; then
    echo "Error: osxcross CMake dispatcher is missing at $cmake_exe." >&2
    exit 1
  fi

  OSXCROSS_HOST="$OSXCROSS_X86_HOST" "$cmake_exe" -S "$PROJECT_DIR" -G Ninja -B "$build_dir" \
    -DCMAKE_TOOLCHAIN_FILE="$QT_TOOLCHAIN" \
    -DQT_CHAINLOAD_TOOLCHAIN_FILE=/opt/osxcross/target/toolchain.cmake \
    -DCMAKE_SYSTEM_NAME=Darwin \
    -DCMAKE_OSX_ARCHITECTURES="$arch" \
    -DCMAKE_OSX_DEPLOYMENT_TARGET="$QMUD_MAC_DOCKER_OSX_DEPLOYMENT_TARGET" \
    -DOSXCROSS_HOST="$OSXCROSS_X86_HOST" \
    -DOSXCROSS_TARGET_DIR="$OSXCROSS_TARGET_DIR" \
    -DOSXCROSS_SDK="$OSXCROSS_SDK" \
    -DCMAKE_BUILD_TYPE="$QMUD_MAC_DOCKER_BUILD_TYPE" \
    -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF \
    -DQMUD_PROBE_IPO=OFF \
    -DQMUD_MACAPP_FORCE_NO_MACDEPLOYQT=ON \
    -DQMUD_MACAPP_ALLOW_NO_MACDEPLOYQT=ON \
    -DQMUD_MACAPP_MACDEPLOYQT_NO_PLUGINS=ON \
    -DQMUD_MACAPP_OUTPUT_DIR="$output_dir" \
    -DQt6_DIR="$QMUD_MAC_DOCKER_QT_PREFIX/lib/cmake/Qt6" \
    -DQt6Multimedia_DIR="$QMUD_MAC_DOCKER_QT_PREFIX/lib/cmake/Qt6Multimedia" \
    -DQt6TextToSpeech_DIR="$QMUD_MAC_DOCKER_QT_PREFIX/lib/cmake/Qt6TextToSpeech" \
    -DQT_HOST_PATH="$QT_HOST_PATH_ROOT" \
    -DQT_HOST_PATH_CMAKE_DIR="$QT_HOST_CMAKE_DIR" \
    -DCMAKE_AUTOMOC_EXECUTABLE="$QT_HOST_MOC" \
    -DQMUD_SYSTEM_LUA_MODULES_PREFIX="$QMUD_MAC_DOCKER_LUA_MODULES_PREFIX" \
    -DLUA_INCLUDE_DIR="$QMUD_MAC_DOCKER_LUA_PREFIX/include" \
    -DLUA_LIBRARY="$QMUD_MAC_DOCKER_LUA_PREFIX/lib/liblua.dylib" \
    -DQMUD_LPEG_PROVIDER=SYSTEM \
    -DQMUD_BC_PROVIDER=BUNDLED \
    -DQMUD_LUASOCKET_PROVIDER=SYSTEM
}

configure_slice x86_64 "$X86_BUILD_DIR" "$BUILD_DIR/macapp-out"
configure_slice arm64 "$ARM_BUILD_DIR" "$ARM_BUILD_DIR/macapp-out"

cmake --build "$X86_BUILD_DIR" --target MacApp -j 3 &
X86_BUILD_PID=$!
cmake --build "$ARM_BUILD_DIR" --target QMud -j 3 &
ARM_BUILD_PID=$!
BUILD_FAILED=0
wait "$X86_BUILD_PID" || BUILD_FAILED=1
wait "$ARM_BUILD_PID" || BUILD_FAILED=1
if [ "$BUILD_FAILED" -ne 0 ]; then
  echo "Error: one or more macOS architecture builds failed." >&2
  exit 1
fi

APP_STAGE_DIR="$BUILD_DIR/macapp-out/QMud.app"
APP_MACOS_DIR="$APP_STAGE_DIR/Contents/MacOS"
APP_FRAMEWORKS_DIR="$APP_STAGE_DIR/Contents/Frameworks"
APP_PLUGINS_DIR="$APP_STAGE_DIR/Contents/PlugIns"
APP_TLS_DIR="$APP_PLUGINS_DIR/tls"
X86_QMUD="$X86_BUILD_DIR/QMud.app/Contents/MacOS/QMud"
ARM_QMUD="$ARM_BUILD_DIR/QMud.app/Contents/MacOS/QMud"
UNIVERSAL_QMUD="$APP_MACOS_DIR/QMud.universal"

if [ ! -x "$X86_QMUD" ] || [ ! -x "$ARM_QMUD" ]; then
  echo "Error: one or more macOS architecture executables are missing." >&2
  echo "x86_64 executable: $X86_QMUD" >&2
  echo "arm64 executable: $ARM_QMUD" >&2
  exit 1
fi
"$LIPO" -create "$X86_QMUD" "$ARM_QMUD" -output "$UNIVERSAL_QMUD"
chmod 0755 "$UNIVERSAL_QMUD"
mv "$UNIVERSAL_QMUD" "$APP_MACOS_DIR/QMud"

copy_qt_framework() {
  local framework_name=$1
  local framework_source="$QMUD_MAC_DOCKER_QT_PREFIX/lib/${framework_name}.framework"
  local framework_target="$APP_FRAMEWORKS_DIR/${framework_name}.framework"

  if [ ! -d "$framework_source" ]; then
    echo "Error: required Qt framework is missing at $framework_source." >&2
    exit 1
  fi

  rm -rf "$framework_target"
  cp -a "$framework_source" "$APP_FRAMEWORKS_DIR/"
  rm -rf "$framework_target/Headers" "$framework_target/Sources"
  if [ -d "$framework_target/Versions" ]; then
    find "$framework_target/Versions" -mindepth 2 -maxdepth 2 \( -name Headers -o -name Sources \) -type d -prune -exec rm -rf {} +
  fi
  find "$framework_target" -type f -name '*.prl' -delete
}

mkdir -p "$APP_MACOS_DIR" "$APP_FRAMEWORKS_DIR" "$APP_PLUGINS_DIR/platforms" "$APP_PLUGINS_DIR/sqldrivers"
if [ -d "$QMUD_MAC_DOCKER_QT_PREFIX/plugins/multimedia" ]; then
  mkdir -p "$APP_PLUGINS_DIR/multimedia"
fi
if [ -d "$QMUD_MAC_DOCKER_QT_PREFIX/plugins/texttospeech" ]; then
  mkdir -p "$APP_PLUGINS_DIR/texttospeech"
fi

for QT_FRAMEWORK in \
  QtCore \
  QtGui \
  QtWidgets \
  QtNetwork \
  QtSql \
  QtPrintSupport \
  QtConcurrent \
  QtDBus \
  QtMultimedia \
  QtTextToSpeech
do
  copy_qt_framework "$QT_FRAMEWORK"
done
cp "$QMUD_MAC_DOCKER_QT_PREFIX/plugins/platforms/libqcocoa.dylib" "$APP_PLUGINS_DIR/platforms/"
cp "$QMUD_MAC_DOCKER_QT_PREFIX/plugins/sqldrivers/libqsqlite.dylib" "$APP_PLUGINS_DIR/sqldrivers/"
if [ -d "$QMUD_MAC_DOCKER_QT_PREFIX/plugins/multimedia" ]; then
  cp -R "$QMUD_MAC_DOCKER_QT_PREFIX/plugins/multimedia/." "$APP_PLUGINS_DIR/multimedia/"
fi
if [ ! -d "$QMUD_MAC_DOCKER_QT_PREFIX/plugins/texttospeech" ]; then
  echo "Error: Qt TextToSpeech plugins directory is missing at $QMUD_MAC_DOCKER_QT_PREFIX/plugins/texttospeech." >&2
  exit 1
fi
cp -R "$QMUD_MAC_DOCKER_QT_PREFIX/plugins/texttospeech/." "$APP_PLUGINS_DIR/texttospeech/"
if ! find "$APP_PLUGINS_DIR/texttospeech" -maxdepth 1 -type f -name '*.dylib' | grep -q .; then
  echo "Error: macOS package is missing Qt TextToSpeech engine plugins." >&2
  echo "Expected at least one plugin dylib in $APP_PLUGINS_DIR/texttospeech." >&2
  exit 1
fi
if [ ! -d "$QMUD_MAC_DOCKER_QT_PREFIX/plugins/tls" ]; then
  echo "Error: Qt TLS plugins directory is missing at $QMUD_MAC_DOCKER_QT_PREFIX/plugins/tls." >&2
  exit 1
fi
mkdir -p "$APP_TLS_DIR"
cp -R "$QMUD_MAC_DOCKER_QT_PREFIX/plugins/tls/." "$APP_TLS_DIR/"
if ! find "$APP_TLS_DIR" -maxdepth 1 -type f \( -name 'libqsecuretransportbackend*.dylib' -o -name 'libqopensslbackend*.dylib' \) | grep -q .; then
  echo "Error: macOS package is missing a functional Qt TLS backend plugin." >&2
  echo "Expected libqsecuretransportbackend*.dylib or libqopensslbackend*.dylib in $APP_TLS_DIR." >&2
  exit 1
fi

cp "$QMUD_MAC_DOCKER_LUA_PREFIX/lib/liblua.5.4.dylib" "$APP_FRAMEWORKS_DIR/"
ln -sf liblua.5.4.dylib "$APP_FRAMEWORKS_DIR/liblua.dylib"

mkdir -p "$APP_MACOS_DIR/socket" "$APP_MACOS_DIR/mime" "$APP_MACOS_DIR/ssl" "$APP_MACOS_DIR/lua/json" "$APP_MACOS_DIR/lua/ssl"
mkdir -p \
  "$APP_MACOS_DIR/lua/native/linux-x86_64" \
  "$APP_MACOS_DIR/lua/native/macos-universal" \
  "$APP_MACOS_DIR/lua/native/macos-arm64" \
  "$APP_MACOS_DIR/lua/native/macos-x86_64" \
  "$APP_MACOS_DIR/lua/native/windows-x86_64"
cp "$QMUD_MAC_DOCKER_LUA_MODULES_PREFIX/socket/core.so" "$APP_MACOS_DIR/socket/core.so"
cp "$QMUD_MAC_DOCKER_LUA_MODULES_PREFIX/mime/core.so" "$APP_MACOS_DIR/mime/core.so"
if [ ! -f "$QMUD_MAC_DOCKER_LUA_MODULES_PREFIX/ssl/core.so" ]; then
  echo "Error: expected LuaSec core module at $QMUD_MAC_DOCKER_LUA_MODULES_PREFIX/ssl/core.so, but it was not found." >&2
  exit 1
fi
cp "$QMUD_MAC_DOCKER_LUA_MODULES_PREFIX/ssl/core.so" "$APP_MACOS_DIR/ssl/core.so"
cp "$QMUD_MAC_DOCKER_LUA_MODULES_PREFIX/ssl/core.so" "$APP_MACOS_DIR/lua/ssl/core.so"

if [ -f "$QMUD_MAC_DOCKER_LUA_MODULES_PREFIX/lpeg.so" ]; then
  cp "$QMUD_MAC_DOCKER_LUA_MODULES_PREFIX/lpeg.so" "$APP_MACOS_DIR/lua/lpeg.so"
fi
if [ -f "$QMUD_MAC_DOCKER_LUA_MODULES_PREFIX/re.lua" ]; then
  cp "$QMUD_MAC_DOCKER_LUA_MODULES_PREFIX/re.lua" "$APP_MACOS_DIR/lua/re.lua"
fi
if [ -f "$QMUD_MAC_DOCKER_LUA_MODULES_PREFIX/socket.lua" ]; then
  cp "$QMUD_MAC_DOCKER_LUA_MODULES_PREFIX/socket.lua" "$APP_MACOS_DIR/lua/socket.lua"
fi
if [ -f "$QMUD_MAC_DOCKER_LUA_MODULES_PREFIX/mime.lua" ]; then
  cp "$QMUD_MAC_DOCKER_LUA_MODULES_PREFIX/mime.lua" "$APP_MACOS_DIR/lua/mime.lua"
fi
if [ -f "$QMUD_MAC_DOCKER_LUA_MODULES_PREFIX/ltn12.lua" ]; then
  cp "$QMUD_MAC_DOCKER_LUA_MODULES_PREFIX/ltn12.lua" "$APP_MACOS_DIR/lua/ltn12.lua"
fi
if [ -f "$QMUD_MAC_DOCKER_LUA_MODULES_PREFIX/ssl.lua" ]; then
  cp "$QMUD_MAC_DOCKER_LUA_MODULES_PREFIX/ssl.lua" "$APP_MACOS_DIR/lua/ssl.lua"
else
  echo "Error: expected LuaSec top-level module ssl.lua at $QMUD_MAC_DOCKER_LUA_MODULES_PREFIX/ssl.lua, but it was not found." >&2
  exit 1
fi
if [ -f "$QMUD_MAC_DOCKER_LUA_MODULES_PREFIX/json.lua" ]; then
  cp "$QMUD_MAC_DOCKER_LUA_MODULES_PREFIX/json.lua" "$APP_MACOS_DIR/lua/json.lua"
fi
if [ -d "$QMUD_MAC_DOCKER_LUA_MODULES_PREFIX/json" ]; then
  cp -R "$QMUD_MAC_DOCKER_LUA_MODULES_PREFIX/json/." "$APP_MACOS_DIR/lua/json/"
fi
if [ -d "$QMUD_MAC_DOCKER_LUA_MODULES_PREFIX/ssl" ]; then
  cp -R "$QMUD_MAC_DOCKER_LUA_MODULES_PREFIX/ssl/." "$APP_MACOS_DIR/ssl/"
  cp -R "$QMUD_MAC_DOCKER_LUA_MODULES_PREFIX/ssl/." "$APP_MACOS_DIR/lua/ssl/"
fi

verify_universal "$APP_MACOS_DIR/QMud"
verify_universal "$APP_FRAMEWORKS_DIR/liblua.5.4.dylib"
verify_universal "$APP_MACOS_DIR/socket/core.so"
verify_universal "$APP_MACOS_DIR/mime/core.so"
verify_universal "$APP_MACOS_DIR/ssl/core.so"
if [ -f "$APP_MACOS_DIR/lua/lpeg.so" ]; then
  verify_universal "$APP_MACOS_DIR/lua/lpeg.so"
fi

OSXCROSS_HOST="$OSXCROSS_X86_HOST" bash "$PROJECT_DIR/tools/scripts/mac_validate_qt_frameworks.sh" "$APP_STAGE_DIR"
bash "$PROJECT_DIR/tools/scripts/package_mac_dmg.sh" "$APP_STAGE_DIR" "$BUILD_DIR/macapp-out/QMud.dmg"
