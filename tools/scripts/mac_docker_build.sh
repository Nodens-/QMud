#!/bin/sh
set -eu

PROJECT_DIR=${PROJECT_DIR:-/home/user/project}
BUILD_DIR=${BUILD_DIR:-/home/user/build}
BUILD_ARCH=${BUILD_ARCH:-x86_64}
QMUD_MAC_DOCKER_IMAGE=${QMUD_MAC_DOCKER_IMAGE:-qmud-macos-builder:qt6.10}
QMUD_MAC_DOCKER_BUILD_TYPE=${QMUD_MAC_DOCKER_BUILD_TYPE:-Release}
QMUD_MAC_DOCKER_OSX_DEPLOYMENT_TARGET=${QMUD_MAC_DOCKER_OSX_DEPLOYMENT_TARGET:-13.0.0}
QMUD_MAC_DOCKER_QT_PREFIX=${QMUD_MAC_DOCKER_QT_PREFIX:-/opt/Qt/latest/macos}
QMUD_MAC_DOCKER_QT_HOST_PREFIX=${QMUD_MAC_DOCKER_QT_HOST_PREFIX:-/usr/lib64/qt6}
QMUD_MAC_DOCKER_LUA_PREFIX=${QMUD_MAC_DOCKER_LUA_PREFIX:-/opt/qmud/macos-deps/lua}
QMUD_MAC_DOCKER_LUA_MODULES_PREFIX=${QMUD_MAC_DOCKER_LUA_MODULES_PREFIX:-/opt/qmud/macos-deps/lua-modules}

rm -f "$BUILD_DIR/CMakeCache.txt"
rm -rf "$BUILD_DIR/CMakeFiles"
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
if [ "$BUILD_ARCH" = "aarch64" ]; then
  OSXCROSS_HOST=$(find -L /opt/osxcross/bin -maxdepth 1 \( -type f -o -type l \) -name 'aarch64-apple-darwin*-clang' | sort | head -n 1 | awk -F/ '{print $NF}' | sed 's/-clang$//')
  if [ -z "$OSXCROSS_HOST" ]; then
    OSXCROSS_HOST=$(find -L /opt/osxcross/bin -maxdepth 1 \( -type f -o -type l \) -name 'arm64-apple-darwin*-clang' | sort | head -n 1 | awk -F/ '{print $NF}' | sed 's/-clang$//')
  fi
else
  OSXCROSS_HOST=$(find -L /opt/osxcross/bin -maxdepth 1 \( -type f -o -type l \) -name 'x86_64-apple-darwin*-clang' | sort | head -n 1 | awk -F/ '{print $NF}' | sed 's/-clang$//')
fi
if [ -z "$OSXCROSS_HOST" ]; then
  echo "Error: could not resolve OSXCROSS_HOST for BUILD_ARCH=${BUILD_ARCH}" >&2
  exit 1
fi
export OSXCROSS_HOST
export OSXCROSS_TARGET_DIR
export OSXCROSS_SDK
export QT_CHAINLOAD_TOOLCHAIN_FILE=/opt/osxcross/target/toolchain.cmake
export CCACHE_DISABLE=1
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
if [ "$BUILD_ARCH" = "aarch64" ]; then
  CMAKE_OSX_ARCH=arm64
else
  CMAKE_OSX_ARCH=x86_64
fi

CMAKE_EXE=cmake
if [ -x /opt/Qt/Tools/CMake/bin/cmake ]; then
  CMAKE_EXE=/opt/Qt/Tools/CMake/bin/cmake
fi

"$CMAKE_EXE" -S "$PROJECT_DIR" -G Ninja -B "$BUILD_DIR" \
  -DCMAKE_TOOLCHAIN_FILE="$QT_TOOLCHAIN" \
  -DQT_CHAINLOAD_TOOLCHAIN_FILE=/opt/osxcross/target/toolchain.cmake \
  -DCMAKE_SYSTEM_NAME=Darwin \
  -DCMAKE_C_COMPILER="/opt/osxcross/bin/${OSXCROSS_HOST}-clang" \
  -DCMAKE_CXX_COMPILER="/opt/osxcross/bin/${OSXCROSS_HOST}-clang++" \
  -DCMAKE_OSX_ARCHITECTURES="$CMAKE_OSX_ARCH" \
  -DCMAKE_OSX_DEPLOYMENT_TARGET="$QMUD_MAC_DOCKER_OSX_DEPLOYMENT_TARGET" \
  -DOSXCROSS_HOST="$OSXCROSS_HOST" \
  -DOSXCROSS_TARGET_DIR="$OSXCROSS_TARGET_DIR" \
  -DOSXCROSS_SDK="$OSXCROSS_SDK" \
  -DCMAKE_BUILD_TYPE="$QMUD_MAC_DOCKER_BUILD_TYPE" \
  -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF \
  -DQMUD_PROBE_IPO=OFF \
  -DQMUD_MACAPP_FORCE_NO_MACDEPLOYQT=ON \
  -DQMUD_MACAPP_ALLOW_NO_MACDEPLOYQT=ON \
  -DQMUD_MACAPP_MACDEPLOYQT_NO_PLUGINS=ON \
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

cmake --build "$BUILD_DIR" --target MacApp -j 6

APP_STAGE_DIR="$BUILD_DIR/macapp-out/QMud.app"
APP_MACOS_DIR="$APP_STAGE_DIR/Contents/MacOS"
APP_FRAMEWORKS_DIR="$APP_STAGE_DIR/Contents/Frameworks"
APP_PLUGINS_DIR="$APP_STAGE_DIR/Contents/PlugIns"
APP_TLS_DIR="$APP_PLUGINS_DIR/tls"

mkdir -p "$APP_MACOS_DIR" "$APP_FRAMEWORKS_DIR" "$APP_PLUGINS_DIR/platforms" "$APP_PLUGINS_DIR/sqldrivers"
if [ -d "$QMUD_MAC_DOCKER_QT_PREFIX/plugins/multimedia" ]; then
  mkdir -p "$APP_PLUGINS_DIR/multimedia"
fi
if [ -d "$QMUD_MAC_DOCKER_QT_PREFIX/plugins/texttospeech" ]; then
  mkdir -p "$APP_PLUGINS_DIR/texttospeech"
fi

cp -R "$QMUD_MAC_DOCKER_QT_PREFIX"/lib/Qt*.framework "$APP_FRAMEWORKS_DIR/"
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
cp "$QMUD_MAC_DOCKER_LUA_PREFIX/lib/liblua.dylib" "$APP_FRAMEWORKS_DIR/"

mkdir -p "$APP_MACOS_DIR/socket" "$APP_MACOS_DIR/mime" "$APP_MACOS_DIR/ssl" "$APP_MACOS_DIR/lua/json" "$APP_MACOS_DIR/lua/ssl"
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
