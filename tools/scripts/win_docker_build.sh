#!/bin/sh
set -eu

PROJECT_DIR=${PROJECT_DIR:-/home/user/project}
BUILD_DIR=${BUILD_DIR:-/home/user/build}
QMUD_WINDOCKER_BUILD_TYPE=${QMUD_WINDOCKER_BUILD_TYPE:-Release}
QMUD_WINDOCKER_QT_PREFIX=${QMUD_WINDOCKER_QT_PREFIX:-/opt/Qt/6.10.2/mingw_64}
QMUD_WINDOCKER_QT_HOST_PREFIX=${QMUD_WINDOCKER_QT_HOST_PREFIX:-/usr/lib64/qt6}
QMUD_WINDOCKER_MINGW_PREFIX=${QMUD_WINDOCKER_MINGW_PREFIX:-/usr/x86_64-w64-mingw32/sys-root/mingw}
QMUD_WINDOCKER_MINGW_TRIPLET=${QMUD_WINDOCKER_MINGW_TRIPLET:-x86_64-w64-mingw32}
QMUD_WINDOCKER_LUA_PREFIX=${QMUD_WINDOCKER_LUA_PREFIX:-/opt/qmud/windows-deps/lua}
QMUD_WINDOCKER_LUA_MODULES_PREFIX=${QMUD_WINDOCKER_LUA_MODULES_PREFIX:-/opt/qmud/windows-deps/lua-modules}

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

export PKG_CONFIG_SYSROOT_DIR="$MINGW_PREFIX"
export PKG_CONFIG_LIBDIR="$MINGW_LIB_DIR/pkgconfig:$MINGW_LIB_DIR/share/pkgconfig"
export PKG_CONFIG_PATH=

if [ ! -x "$QT_HOST_PREFIX/libexec/moc" ]; then
  for QT_HOST_CANDIDATE in /usr/lib64/qt6 /usr/lib/qt6 /opt/Qt/6.10.2/gcc_64 /opt/Qt/6.10.2/linux_gcc_64; do
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
  -DCMAKE_C_COMPILER=${MINGW_TRIPLET}-gcc \
  -DCMAKE_CXX_COMPILER=${MINGW_TRIPLET}-g++ \
  -DCMAKE_RC_COMPILER=${MINGW_TRIPLET}-windres \
  -DCMAKE_C_FLAGS="-UUNICODE -U_UNICODE" \
  -DCMAKE_CXX_FLAGS="-UUNICODE -U_UNICODE" \
  -DCMAKE_FIND_ROOT_PATH="${MINGW_PREFIX};${QT_PREFIX}" \
  -DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER \
  -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY \
  -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY \
  -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=BOTH \
  -DCMAKE_BUILD_TYPE="$QMUD_WINDOCKER_BUILD_TYPE" \
  -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF \
  -DQMUD_PROBE_IPO=OFF \
  -DVulkan_INCLUDE_DIR="$MINGW_INCLUDE_DIR" \
  -DQt6_DIR="$QT_PREFIX/lib/cmake/Qt6" \
  -DQt6Multimedia_DIR="$QT_PREFIX/lib/cmake/Qt6Multimedia" \
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

if [ -d "$QT_PREFIX/bin" ]; then
  cp "$QT_PREFIX/bin/"*.dll "$STAGE_DIR/lib/" || true
fi
if [ -d "$QT_PREFIX/plugins" ]; then
  cp -R "$QT_PREFIX/plugins" "$STAGE_DIR/qtplugins"
fi
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
if [ -d "$MINGW_PREFIX/bin" ]; then
  cp "$MINGW_PREFIX/bin/"*.dll "$STAGE_DIR/lib/" || true
fi
if [ -d "$LUA_PREFIX/bin" ]; then
  cp "$LUA_PREFIX/bin/"*.dll "$STAGE_DIR/lib/" || true
fi

STARTUP_DLLS='Qt6Core.dll Qt6Gui.dll Qt6Multimedia.dll Qt6Network.dll Qt6PrintSupport.dll Qt6Sql.dll Qt6Widgets.dll lua54.dll libgcc_s_seh-1.dll libstdc++-6.dll zlib1.dll libwinpthread-1.dll'
for dll in $STARTUP_DLLS; do
  src="$(find "$STAGE_DIR/lib" -maxdepth 1 -type f -iname "$dll" | head -n 1)"
  if [ -n "$src" ]; then
    mv -f "$src" "$STAGE_DIR/$dll"
  fi
done

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

for name in socket.lua ltn12.lua mime.lua; do
  if [ ! -f "$BUILD_DIR/lua/$name" ]; then
    echo "Error: expected generated LuaSocket Lua file at $BUILD_DIR/lua/$name, but it was not found." >&2
    exit 1
  fi
  cp "$BUILD_DIR/lua/$name" "$STAGE_DIR/lua/$name"
done

for name in ftp.lua http.lua smtp.lua tp.lua url.lua; do
  if [ ! -f "$BUILD_DIR/socket/$name" ]; then
    echo "Error: expected generated LuaSocket Lua file at $BUILD_DIR/socket/$name, but it was not found." >&2
    exit 1
  fi
  cp "$BUILD_DIR/socket/$name" "$STAGE_DIR/socket/$name"
done

if [ -f "$LUA_MODULES_PREFIX/lpeg.dll" ]; then
  cp "$LUA_MODULES_PREFIX/lpeg.dll" "$STAGE_DIR/lua/lpeg.dll"
fi
for name in json.lua re.lua; do
  if [ -f "$LUA_MODULES_PREFIX/$name" ]; then
    cp "$LUA_MODULES_PREFIX/$name" "$STAGE_DIR/lua/$name"
  fi
done
if [ -d "$LUA_MODULES_PREFIX/json" ]; then
  mkdir -p "$STAGE_DIR/lua/json"
  cp -R "$LUA_MODULES_PREFIX/json/." "$STAGE_DIR/lua/json/"
fi

cmake -E rm -f "$STAGE_ROOT/$PACKAGE_NAME.zip"
cmake -E chdir "$STAGE_ROOT" cmake -E tar cf "$PACKAGE_NAME.zip" --format=zip "$PACKAGE_NAME"
