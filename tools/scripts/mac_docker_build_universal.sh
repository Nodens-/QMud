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

LUA_VERSION=${LUA_VERSION:-5.4.7}
LUASOCKET_VERSION=${LUASOCKET_VERSION:-3.1.0}
LPEG_VERSION=${LPEG_VERSION:-1.1.0}
LUASEC_VERSION=${LUASEC_VERSION:-1.3.2}
OPENSSL_VERSION=${OPENSSL_VERSION:-3.3.2}

UNIVERSAL_DEPS_DIR="$BUILD_DIR/universal-deps"
QMUD_MAC_DOCKER_LUA_PREFIX="$UNIVERSAL_DEPS_DIR/lua"
QMUD_MAC_DOCKER_LUA_MODULES_PREFIX="$UNIVERSAL_DEPS_DIR/lua-modules"
QMUD_MAC_DOCKER_OPENSSL_PREFIX="$UNIVERSAL_DEPS_DIR/openssl"

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

find_osxcross_host() {
  local pattern=$1
  local host
  host=$(find -L /opt/osxcross/bin "${OSXCROSS_TARGET_DIR}/bin" -maxdepth 1 \( -type f -o -type l \) -name "${pattern}-apple-darwin*-clang" 2>/dev/null | sort | head -n 1 | awk -F/ '{print $NF}' | sed 's/-clang$//')
  printf '%s' "$host"
}

OSXCROSS_X86_HOST=$(find_osxcross_host x86_64)
OSXCROSS_ARM_HOST=$(find_osxcross_host aarch64)
if [ -z "$OSXCROSS_ARM_HOST" ]; then
  OSXCROSS_ARM_HOST=$(find_osxcross_host arm64)
fi
if [ -z "$OSXCROSS_X86_HOST" ] || [ -z "$OSXCROSS_ARM_HOST" ]; then
  echo "Error: universal macOS build requires both x86_64 and arm64 osxcross clang toolchains." >&2
  echo "Resolved x86_64 host: ${OSXCROSS_X86_HOST:-<missing>}" >&2
  echo "Resolved arm64 host: ${OSXCROSS_ARM_HOST:-<missing>}" >&2
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

resolve_host_tool() {
  local host=$1
  local tool_name=$2
  local candidate
  for candidate in \
    "/opt/osxcross/bin/${host}-${tool_name}" \
    "${OSXCROSS_TARGET_DIR}/bin/${host}-${tool_name}" \
    "/opt/osxcross/bin/${tool_name}" \
    "${OSXCROSS_TARGET_DIR}/bin/${tool_name}"; do
    if [ -x "$candidate" ]; then
      printf '%s' "$candidate"
      return 0
    fi
  done
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
export OSXCROSS_HOST="$OSXCROSS_X86_HOST"
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

X86_CC="/opt/osxcross/bin/${OSXCROSS_X86_HOST}-clang"
X86_CXX="/opt/osxcross/bin/${OSXCROSS_X86_HOST}-clang++"
ARM_CC="/opt/osxcross/bin/${OSXCROSS_ARM_HOST}-clang"
X86_AR=$(resolve_host_tool "$OSXCROSS_X86_HOST" ar || true)
X86_RANLIB=$(resolve_host_tool "$OSXCROSS_X86_HOST" ranlib || true)
ARM_AR=$(resolve_host_tool "$OSXCROSS_ARM_HOST" ar || true)
ARM_RANLIB=$(resolve_host_tool "$OSXCROSS_ARM_HOST" ranlib || true)
if [ -z "$X86_AR" ] || [ -z "$X86_RANLIB" ] || [ -z "$ARM_AR" ] || [ -z "$ARM_RANLIB" ]; then
  echo "Error: could not resolve osxcross ar/ranlib for both universal architectures." >&2
  echo "x86_64 ar: ${X86_AR:-<missing>}" >&2
  echo "x86_64 ranlib: ${X86_RANLIB:-<missing>}" >&2
  echo "arm64 ar: ${ARM_AR:-<missing>}" >&2
  echo "arm64 ranlib: ${ARM_RANLIB:-<missing>}" >&2
  exit 1
fi
UNIVERSAL_CFLAGS=(-O2 -fPIC -mmacosx-version-min="$QMUD_MAC_DOCKER_OSX_DEPLOYMENT_TARGET" -arch x86_64 -arch arm64)

download_sources() {
  local src_root=$1
  mkdir -p "$src_root"

  curl -fsSL "https://www.lua.org/ftp/lua-${LUA_VERSION}.tar.gz" -o "$src_root/lua.tar.gz"
  tar -xf "$src_root/lua.tar.gz" -C "$src_root"

  curl -fsSL "https://github.com/lunarmodules/luasocket/archive/refs/tags/v${LUASOCKET_VERSION}.tar.gz" -o "$src_root/luasocket.tar.gz" || \
  curl -fsSL "https://github.com/lunarmodules/luasocket/archive/refs/tags/${LUASOCKET_VERSION}.tar.gz" -o "$src_root/luasocket.tar.gz"
  tar -xf "$src_root/luasocket.tar.gz" -C "$src_root"

  curl -fsSL "https://www.openssl.org/source/openssl-${OPENSSL_VERSION}.tar.gz" -o "$src_root/openssl.tar.gz" || \
  curl -fsSL "https://www.openssl.org/source/old/3.3/openssl-${OPENSSL_VERSION}.tar.gz" -o "$src_root/openssl.tar.gz"
  tar -xf "$src_root/openssl.tar.gz" -C "$src_root"

  curl -fsSL "https://github.com/brunoos/luasec/archive/refs/tags/v${LUASEC_VERSION}.tar.gz" -o "$src_root/luasec.tar.gz" || \
  curl -fsSL "https://github.com/brunoos/luasec/archive/refs/tags/${LUASEC_VERSION}.tar.gz" -o "$src_root/luasec.tar.gz"
  tar -xf "$src_root/luasec.tar.gz" -C "$src_root"

  curl -fsSL "https://www.inf.puc-rio.br/~roberto/lpeg/lpeg-${LPEG_VERSION}.tar.gz" -o "$src_root/lpeg.tar.gz"
  tar -xf "$src_root/lpeg.tar.gz" -C "$src_root"
}

copy_lua_file() {
  local src_dir=$1
  local c_dir=$2
  local name=$3
  if [[ -f "${c_dir}/${name}" ]]; then
    cp "${c_dir}/${name}" "${QMUD_MAC_DOCKER_LUA_MODULES_PREFIX}/${name}"
    return 0
  fi
  if [[ -f "${src_dir}/${name}" ]]; then
    cp "${src_dir}/${name}" "${QMUD_MAC_DOCKER_LUA_MODULES_PREFIX}/${name}"
    return 0
  fi
  return 1
}

copy_socket_lua_file() {
  local src_dir=$1
  local c_dir=$2
  local name=$3
  if [[ -f "${c_dir}/${name}.lua" ]]; then
    cp "${c_dir}/${name}.lua" "${QMUD_MAC_DOCKER_LUA_MODULES_PREFIX}/socket/${name}.lua"
    return 0
  fi
  if [[ -f "${src_dir}/${name}.lua" ]]; then
    cp "${src_dir}/${name}.lua" "${QMUD_MAC_DOCKER_LUA_MODULES_PREFIX}/socket/${name}.lua"
    return 0
  fi
  return 1
}

build_openssl_slice() {
  local source_dir=$1
  local build_root=$2
  local arch_name=$3
  local cc=$4
  local ar=$5
  local ranlib=$6
  local openssl_target=$7
  local prefix=$8

  rm -rf "$build_root/openssl-${arch_name}"
  cp -R "$source_dir" "$build_root/openssl-${arch_name}"
  cd "$build_root/openssl-${arch_name}"
  CC="$cc" AR="$ar" RANLIB="$ranlib" ./Configure "$openssl_target" no-shared no-tests no-apps no-module no-legacy \
    --prefix="$prefix" \
    --openssldir="$prefix"
  make -j"$(nproc)" AR="$ar" RANLIB="$ranlib"
  make install_sw AR="$ar" RANLIB="$ranlib"
}

verify_arch() {
  local path=$1
  local arch=$2
  local info
  if [ ! -e "$path" ]; then
    echo "Error: expected binary is missing at $path." >&2
    exit 1
  fi
  info=$("$LIPO" -info "$path")
  case "$info" in
    *"$arch"*) ;;
    *)
      echo "Error: $path does not contain ${arch}: $info" >&2
      exit 1
      ;;
  esac
}

build_universal_lua_deps() {
  local src_root="$BUILD_DIR/universal-src"
  local openssl_slices="$UNIVERSAL_DEPS_DIR/openssl-slices"
  rm -rf "$src_root" "$UNIVERSAL_DEPS_DIR"
  mkdir -p "$QMUD_MAC_DOCKER_LUA_PREFIX/include" "$QMUD_MAC_DOCKER_LUA_PREFIX/lib"
  mkdir -p "$QMUD_MAC_DOCKER_LUA_MODULES_PREFIX/socket" "$QMUD_MAC_DOCKER_LUA_MODULES_PREFIX/mime" "$QMUD_MAC_DOCKER_LUA_MODULES_PREFIX/json" "$QMUD_MAC_DOCKER_LUA_MODULES_PREFIX/ssl"
  mkdir -p "$openssl_slices" "$QMUD_MAC_DOCKER_OPENSSL_PREFIX/include" "$QMUD_MAC_DOCKER_OPENSSL_PREFIX/lib"

  download_sources "$src_root"

  local lua_src
  lua_src="$(find "$src_root" -maxdepth 1 -type d -name "lua-${LUA_VERSION}*" | head -n 1 || true)"
  [[ -n "$lua_src" ]]
  cd "$lua_src/src"
  "$X86_CC" "${UNIVERSAL_CFLAGS[@]}" -DLUA_USE_MACOSX -I. -dynamiclib \
    lapi.c lauxlib.c lbaselib.c lcode.c lcorolib.c lctype.c ldblib.c ldebug.c ldo.c ldump.c lfunc.c lgc.c \
    linit.c liolib.c llex.c lmathlib.c lmem.c loadlib.c lobject.c lopcodes.c loslib.c lparser.c lstate.c \
    lstring.c lstrlib.c ltable.c ltablib.c ltm.c lundump.c lutf8lib.c lvm.c lzio.c \
    -Wl,-install_name,@rpath/liblua.5.4.dylib -o "$QMUD_MAC_DOCKER_LUA_PREFIX/lib/liblua.5.4.dylib" -lm
  ln -sf liblua.5.4.dylib "$QMUD_MAC_DOCKER_LUA_PREFIX/lib/liblua.dylib"
  cp lua.h lauxlib.h lualib.h luaconf.h "$QMUD_MAC_DOCKER_LUA_PREFIX/include/"

  local luasocket_src luasocket_c_dir
  luasocket_src="$(find "$src_root" -maxdepth 1 -type d -name "luasocket*${LUASOCKET_VERSION}*" | head -n 1 || true)"
  [[ -n "$luasocket_src" ]]
  luasocket_c_dir="$luasocket_src/src"
  [[ -d "$luasocket_c_dir" ]]
  cd "$luasocket_c_dir"
  "$X86_CC" "${UNIVERSAL_CFLAGS[@]}" -I"$QMUD_MAC_DOCKER_LUA_PREFIX/include" -bundle -undefined dynamic_lookup \
    auxiliar.c buffer.c compat.c except.c inet.c io.c luasocket.c options.c select.c tcp.c timeout.c udp.c usocket.c \
    -o "$QMUD_MAC_DOCKER_LUA_MODULES_PREFIX/socket/core.so"
  "$X86_CC" "${UNIVERSAL_CFLAGS[@]}" -I"$QMUD_MAC_DOCKER_LUA_PREFIX/include" -bundle -undefined dynamic_lookup \
    mime.c compat.c -o "$QMUD_MAC_DOCKER_LUA_MODULES_PREFIX/mime/core.so"

  copy_lua_file "$luasocket_src" "$luasocket_c_dir" socket.lua
  copy_lua_file "$luasocket_src" "$luasocket_c_dir" mime.lua
  copy_lua_file "$luasocket_src" "$luasocket_c_dir" ltn12.lua
  cp "$QMUD_MAC_DOCKER_LUA_MODULES_PREFIX/socket.lua" "$QMUD_MAC_DOCKER_LUA_MODULES_PREFIX/socket/socket.lua"
  cp "$QMUD_MAC_DOCKER_LUA_MODULES_PREFIX/mime.lua" "$QMUD_MAC_DOCKER_LUA_MODULES_PREFIX/socket/mime.lua"
  cp "$QMUD_MAC_DOCKER_LUA_MODULES_PREFIX/ltn12.lua" "$QMUD_MAC_DOCKER_LUA_MODULES_PREFIX/socket/ltn12.lua"
  for f in ftp headers http smtp tp url; do
    copy_socket_lua_file "$luasocket_src" "$luasocket_c_dir" "$f"
  done
  copy_socket_lua_file "$luasocket_src" "$luasocket_c_dir" socket || true
  copy_socket_lua_file "$luasocket_src" "$luasocket_c_dir" mbox || true

  local openssl_src
  openssl_src="$(find "$src_root" -maxdepth 1 -type d -name "openssl-${OPENSSL_VERSION}*" | head -n 1 || true)"
  [[ -n "$openssl_src" ]]
  build_openssl_slice "$openssl_src" "$openssl_slices" x86_64 "$X86_CC" "$X86_AR" "$X86_RANLIB" darwin64-x86_64-cc "$openssl_slices/x86_64"
  build_openssl_slice "$openssl_src" "$openssl_slices" arm64 "$ARM_CC" "$ARM_AR" "$ARM_RANLIB" darwin64-arm64-cc "$openssl_slices/arm64"
  verify_arch "$openssl_slices/x86_64/lib/libssl.a" x86_64
  verify_arch "$openssl_slices/x86_64/lib/libcrypto.a" x86_64
  verify_arch "$openssl_slices/arm64/lib/libssl.a" arm64
  verify_arch "$openssl_slices/arm64/lib/libcrypto.a" arm64
  cp -R "$openssl_slices/x86_64/include/." "$QMUD_MAC_DOCKER_OPENSSL_PREFIX/include/"
  "$LIPO" -create "$openssl_slices/x86_64/lib/libssl.a" "$openssl_slices/arm64/lib/libssl.a" -output "$QMUD_MAC_DOCKER_OPENSSL_PREFIX/lib/libssl.a"
  "$LIPO" -create "$openssl_slices/x86_64/lib/libcrypto.a" "$openssl_slices/arm64/lib/libcrypto.a" -output "$QMUD_MAC_DOCKER_OPENSSL_PREFIX/lib/libcrypto.a"
  verify_arch "$QMUD_MAC_DOCKER_OPENSSL_PREFIX/lib/libssl.a" x86_64
  verify_arch "$QMUD_MAC_DOCKER_OPENSSL_PREFIX/lib/libssl.a" arm64
  verify_arch "$QMUD_MAC_DOCKER_OPENSSL_PREFIX/lib/libcrypto.a" x86_64
  verify_arch "$QMUD_MAC_DOCKER_OPENSSL_PREFIX/lib/libcrypto.a" arm64

  local luasec_src luasec_c_dir
  local -a luasec_c_sources=()
  local -a luasec_socket_sources=()
  luasec_src="$(find "$src_root" -maxdepth 1 -type d -name "luasec*${LUASEC_VERSION}*" | head -n 1 || true)"
  [[ -n "$luasec_src" ]]
  luasec_c_dir="$luasec_src/src"
  [[ -d "$luasec_c_dir" ]]
  cd "$luasec_c_dir"
  for src in config.c context.c ec.c options.c ssl.c x509.c; do
    if [[ -f "$src" ]]; then
      luasec_c_sources+=("$src")
    fi
  done
  if [[ "${#luasec_c_sources[@]}" -eq 0 ]]; then
    echo "Error: could not resolve LuaSec C sources in ${luasec_c_dir}" >&2
    exit 1
  fi
  for src in luasocket/buffer.c luasocket/io.c luasocket/timeout.c; do
    if [[ -f "$src" ]]; then
      luasec_socket_sources+=("$src")
    fi
  done
  if [[ -f "luasocket/usocket.c" ]]; then
    luasec_socket_sources+=("luasocket/usocket.c")
  elif [[ -f "luasocket/wsocket.c" ]]; then
    luasec_socket_sources+=("luasocket/wsocket.c")
  fi
  if [[ "${#luasec_socket_sources[@]}" -eq 0 ]]; then
    echo "Error: could not resolve bundled LuaSocket sources in ${luasec_c_dir}/luasocket" >&2
    exit 1
  fi
  "$X86_CC" "${UNIVERSAL_CFLAGS[@]}" \
    -I"$QMUD_MAC_DOCKER_LUA_PREFIX/include" \
    -I"$QMUD_MAC_DOCKER_OPENSSL_PREFIX/include" \
    -I"$luasec_c_dir" \
    -bundle -undefined dynamic_lookup \
    "${luasec_c_sources[@]}" "${luasec_socket_sources[@]}" \
    "$QMUD_MAC_DOCKER_OPENSSL_PREFIX/lib/libssl.a" \
    "$QMUD_MAC_DOCKER_OPENSSL_PREFIX/lib/libcrypto.a" \
    -lz \
    -o "$QMUD_MAC_DOCKER_LUA_MODULES_PREFIX/ssl/core.so"

  if [[ -f "$luasec_src/ssl.lua" ]]; then
    cp "$luasec_src/ssl.lua" "$QMUD_MAC_DOCKER_LUA_MODULES_PREFIX/ssl.lua"
  elif [[ -f "$luasec_c_dir/ssl.lua" ]]; then
    cp "$luasec_c_dir/ssl.lua" "$QMUD_MAC_DOCKER_LUA_MODULES_PREFIX/ssl.lua"
  fi
  for name in config.lua https.lua options.lua; do
    if [[ -f "$luasec_c_dir/$name" ]]; then
      cp "$luasec_c_dir/$name" "$QMUD_MAC_DOCKER_LUA_MODULES_PREFIX/ssl/$name"
    fi
  done
  if [[ -d "$luasec_c_dir/ssl" ]]; then
    cp -R "$luasec_c_dir/ssl/." "$QMUD_MAC_DOCKER_LUA_MODULES_PREFIX/ssl/"
  elif [[ -d "$luasec_src/ssl" ]]; then
    cp -R "$luasec_src/ssl/." "$QMUD_MAC_DOCKER_LUA_MODULES_PREFIX/ssl/"
  fi

  local lpeg_src lpeg_c_dir
  lpeg_src="$(find "$src_root" -maxdepth 1 -type d -name "lpeg*${LPEG_VERSION}*" | head -n 1 || true)"
  [[ -n "$lpeg_src" ]]
  if [[ -f "$lpeg_src/lpcap.c" ]]; then
    lpeg_c_dir="$lpeg_src"
  else
    lpeg_c_dir="$lpeg_src/src"
  fi
  [[ -f "$lpeg_c_dir/lpcap.c" ]]
  cd "$lpeg_c_dir"
  "$X86_CC" "${UNIVERSAL_CFLAGS[@]}" -I"$QMUD_MAC_DOCKER_LUA_PREFIX/include" -bundle -undefined dynamic_lookup \
    lpcap.c lpcode.c lptree.c lpvm.c -o "$QMUD_MAC_DOCKER_LUA_MODULES_PREFIX/lpeg.so"
  if [[ -f "$lpeg_src/re.lua" ]]; then
    cp "$lpeg_src/re.lua" "$QMUD_MAC_DOCKER_LUA_MODULES_PREFIX/re.lua"
  elif [[ -f "$lpeg_c_dir/re.lua" ]]; then
    cp "$lpeg_c_dir/re.lua" "$QMUD_MAC_DOCKER_LUA_MODULES_PREFIX/re.lua"
  fi

  curl -fsSL "https://raw.githubusercontent.com/rxi/json.lua/master/json.lua" -o "$QMUD_MAC_DOCKER_LUA_MODULES_PREFIX/json.lua"
  cp "$QMUD_MAC_DOCKER_LUA_MODULES_PREFIX/json.lua" "$QMUD_MAC_DOCKER_LUA_MODULES_PREFIX/json/init.lua"

  test -f "$QMUD_MAC_DOCKER_LUA_PREFIX/lib/liblua.5.4.dylib"
  test -f "$QMUD_MAC_DOCKER_LUA_MODULES_PREFIX/socket/core.so"
  test -f "$QMUD_MAC_DOCKER_LUA_MODULES_PREFIX/mime/core.so"
  test -f "$QMUD_MAC_DOCKER_LUA_MODULES_PREFIX/ssl/core.so"
  test -f "$QMUD_MAC_DOCKER_LUA_MODULES_PREFIX/ssl.lua"
  test -f "$QMUD_MAC_DOCKER_OPENSSL_PREFIX/include/openssl/ssl.h"
  test -f "$QMUD_MAC_DOCKER_OPENSSL_PREFIX/lib/libssl.a"
  test -f "$QMUD_MAC_DOCKER_OPENSSL_PREFIX/lib/libcrypto.a"
}

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

build_universal_lua_deps

CMAKE_EXE=cmake
if [ -x /opt/Qt/Tools/CMake/bin/cmake ]; then
  CMAKE_EXE=/opt/Qt/Tools/CMake/bin/cmake
fi

"$CMAKE_EXE" -S "$PROJECT_DIR" -G Ninja -B "$BUILD_DIR" \
  -DCMAKE_TOOLCHAIN_FILE="$QT_TOOLCHAIN" \
  -DQT_CHAINLOAD_TOOLCHAIN_FILE=/opt/osxcross/target/toolchain.cmake \
  -DCMAKE_SYSTEM_NAME=Darwin \
  -DCMAKE_C_COMPILER="$X86_CC" \
  -DCMAKE_CXX_COMPILER="$X86_CXX" \
  -DCMAKE_OSX_ARCHITECTURES="x86_64;arm64" \
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

bash "$PROJECT_DIR/tools/scripts/mac_validate_qt_frameworks.sh" "$APP_STAGE_DIR"
bash "$PROJECT_DIR/tools/scripts/package_mac_dmg.sh" "$APP_STAGE_DIR" "$BUILD_DIR/macapp-out/QMud.dmg"
