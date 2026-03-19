#!/bin/sh
set -eu

PROJECT_DIR=${PROJECT_DIR:-/home/user/project}
BUILD_DIR=${BUILD_DIR:-/home/user/build}
QMUD_APPIMAGE_DOCKER_BUILD_TYPE=${QMUD_APPIMAGE_DOCKER_BUILD_TYPE:-Release}
QMUD_APPIMAGE_DOCKER_QT_PREFIX=${QMUD_APPIMAGE_DOCKER_QT_PREFIX:-/opt/Qt/6.10.2/gcc_64}
QMUD_APPIMAGE_VERSION=${QMUD_APPIMAGE_VERSION:-dev}
QMUD_APPIMAGE_DOCKER_LINUXDEPLOY_PLUGIN_APPIMAGE_URL=${QMUD_APPIMAGE_DOCKER_LINUXDEPLOY_PLUGIN_APPIMAGE_URL:-https://github.com/linuxdeploy/linuxdeploy-plugin-appimage/releases/download/1-alpha-20250213-1/linuxdeploy-plugin-appimage-x86_64.AppImage}

rm -f "$BUILD_DIR/CMakeCache.txt"
rm -rf "$BUILD_DIR/CMakeFiles"
export CCACHE_DISABLE=1
unset CCACHE_DIR CCACHE_TEMPDIR
export PATH="$QMUD_APPIMAGE_DOCKER_QT_PREFIX/bin:$PATH"

LUA_BIN=""
if command -v lua5.4 >/dev/null 2>&1; then
  LUA_BIN="lua5.4"
elif command -v lua >/dev/null 2>&1; then
  LUA_BIN="lua"
fi

lua_find_module() {
  module="$1"
  if [ -z "$LUA_BIN" ]; then
    return 0
  fi
  "$LUA_BIN" -e "local p = package.searchpath('${module}', package.cpath); if p then io.write(p) end" 2>/dev/null || true
}

find_first() {
  pattern="$1"
  shift
  for root in "$@"; do
    [ -n "$root" ] || continue
    [ -d "$root" ] || continue
    found="$(find "$root" -type f -path "$pattern" | sort | head -n 1)"
    if [ -n "$found" ]; then
      printf '%s\n' "$found"
      return 0
    fi
  done
  return 1
}

LPEG_SO="$(lua_find_module 'lpeg')"
SOCKET_CORE_SO="$(lua_find_module 'socket.core')"
MIME_CORE_SO="$(lua_find_module 'mime.core')"

if [ -z "$LPEG_SO" ]; then
  LPEG_SO="$(find_first '*/lua/*/lpeg.so' /usr/lib64 /usr/lib /usr/local/lib64 /usr/local/lib || true)"
fi
if [ -z "$SOCKET_CORE_SO" ]; then
  SOCKET_CORE_SO="$(find_first '*/lua/*/socket/core.so' /usr/lib64 /usr/lib /usr/local/lib64 /usr/local/lib || true)"
fi
if [ -z "$MIME_CORE_SO" ]; then
  MIME_CORE_SO="$(find_first '*/lua/*/mime/core.so' /usr/lib64 /usr/lib /usr/local/lib64 /usr/local/lib || true)"
fi

if [ -z "$LPEG_SO" ] || [ -z "$SOCKET_CORE_SO" ] || [ -z "$MIME_CORE_SO" ]; then
  echo "Failed to resolve required Lua module shared objects for AppImage build." >&2
  echo "lpeg.so=$LPEG_SO" >&2
  echo "socket/core.so=$SOCKET_CORE_SO" >&2
  echo "mime/core.so=$MIME_CORE_SO" >&2
  exit 1
fi

QMUD_MODULE_ROOT="$(printf '%s\n' "$LPEG_SO" | sed -E 's#/lua/[^/]+/lpeg\.so$##')"
if [ -z "$QMUD_MODULE_ROOT" ] || [ ! -d "$QMUD_MODULE_ROOT" ]; then
  echo "Failed to derive QMUD_SYSTEM_LUA_MODULES_PREFIX from $LPEG_SO" >&2
  exit 1
fi

CMAKE_EXE=cmake
if [ -x /opt/Qt/Tools/CMake/bin/cmake ]; then
  CMAKE_EXE=/opt/Qt/Tools/CMake/bin/cmake
fi

LINUXDEPLOY_PLUGIN_DIR="$BUILD_DIR/tools"
LINUXDEPLOY_PLUGIN_APPIMAGE="$LINUXDEPLOY_PLUGIN_DIR/linuxdeploy-plugin-appimage-x86_64.AppImage"
if [ ! -s "$LINUXDEPLOY_PLUGIN_APPIMAGE" ]; then
  mkdir -p "$LINUXDEPLOY_PLUGIN_DIR"
  TMP_PLUGIN_PATH="$LINUXDEPLOY_PLUGIN_APPIMAGE.tmp"
  rm -f "$TMP_PLUGIN_PATH"
  curl -fL "$QMUD_APPIMAGE_DOCKER_LINUXDEPLOY_PLUGIN_APPIMAGE_URL" -o "$TMP_PLUGIN_PATH"
  mv -f "$TMP_PLUGIN_PATH" "$LINUXDEPLOY_PLUGIN_APPIMAGE"
fi
if [ ! -s "$LINUXDEPLOY_PLUGIN_APPIMAGE" ]; then
  echo "Failed to obtain linuxdeploy-plugin-appimage in $LINUXDEPLOY_PLUGIN_DIR." >&2
  exit 1
fi
chmod 0755 "$LINUXDEPLOY_PLUGIN_APPIMAGE"

"$CMAKE_EXE" -S "$PROJECT_DIR" -G Ninja -B "$BUILD_DIR" \
  -DCMAKE_BUILD_TYPE="$QMUD_APPIMAGE_DOCKER_BUILD_TYPE" \
  -DCMAKE_PREFIX_PATH="$QMUD_APPIMAGE_DOCKER_QT_PREFIX" \
  -DQt6_DIR="$QMUD_APPIMAGE_DOCKER_QT_PREFIX/lib/cmake/Qt6" \
  -DQt6Multimedia_DIR="$QMUD_APPIMAGE_DOCKER_QT_PREFIX/lib/cmake/Qt6Multimedia" \
  -DQMUD_ENABLE_APPIMAGE=ON \
  -DQMUD_ENABLE_MAC_DOCKER=OFF \
  -DQMUD_ENABLE_WINDOCKER=OFF \
  -DQMUD_LINUXDEPLOY_PLUGIN_APPIMAGE_EXECUTABLE="$LINUXDEPLOY_PLUGIN_APPIMAGE" \
  -DQMUD_APPIMAGE_VERSION="$QMUD_APPIMAGE_VERSION" \
  -DQMUD_SYSTEM_LUA_MODULES_PREFIX="$QMUD_MODULE_ROOT" \
  -DQMUD_SYSTEM_LPEG_SO="$LPEG_SO" \
  -DQMUD_SYSTEM_SOCKET_CORE_SO="$SOCKET_CORE_SO" \
  -DQMUD_SYSTEM_MIME_CORE_SO="$MIME_CORE_SO"

CCACHE_DISABLE=1 cmake --build "$BUILD_DIR" --target AppImage -j 6
