FROM quay.io/fedora/fedora:44

ENV LANG=C.UTF-8
ENV LC_ALL=C.UTF-8

RUN dnf -y upgrade && \
    dnf -y install \
      autoconf \
      automake \
      bash \
      bc \
      bison \
      bzip2-devel \
      ca-certificates \
      clang \
      clang-tools-extra \
      cmake \
      curl \
      file \
      findutils \
      flex \
      git \
      glibc-langpack-en \
      hfsplus-tools \
      libarchive \
      libarchive-devel \
      libxml2-devel \
      libuuid-devel \
      libtool \
      make \
      ninja-build \
      openssl \
      openssl-devel \
      patch \
      perl \
      perl-Digest-SHA \
      perl-FindBin \
      perl-IPC-Cmd \
      perl-Thread-Queue \
      pkgconf-pkg-config \
      python3 \
      python3-pip \
      rsync \
      tar \
      unzip \
      wget \
      which \
      xz-devel \
      xz \
      zlib-devel \
      zip && \
    dnf clean all

RUN git clone --depth 1 https://github.com/mozilla/libdmg-hfsplus.git /tmp/libdmg-hfsplus && \
    cmake -S /tmp/libdmg-hfsplus -B /tmp/libdmg-hfsplus-build -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_FLAGS="-UHAVE_CRYPT" && \
    cmake --build /tmp/libdmg-hfsplus-build -j"$(nproc)" && \
    test -x /tmp/libdmg-hfsplus-build/dmg/dmg && \
    test -x /tmp/libdmg-hfsplus-build/hfs/hfsplus && \
    install -m 0755 /tmp/libdmg-hfsplus-build/dmg/dmg /usr/local/bin/dmg && \
    install -m 0755 /tmp/libdmg-hfsplus-build/hfs/hfsplus /usr/local/bin/hfsplus && \
    if [ -x /tmp/libdmg-hfsplus-build/hdutil/hdutil ]; then install -m 0755 /tmp/libdmg-hfsplus-build/hdutil/hdutil /usr/local/bin/hdutil; fi && \
    rm -rf /tmp/libdmg-hfsplus /tmp/libdmg-hfsplus-build

RUN if ! command -v xml2-config >/dev/null 2>&1 && pkg-config --exists libxml-2.0; then \
      printf '%s\n' \
        '#!/usr/bin/env bash' \
        'set -euo pipefail' \
        'case "${1:-}" in' \
        '  --cflags) pkg-config --cflags libxml-2.0 ;;' \
        '  --libs) pkg-config --libs libxml-2.0 ;;' \
        '  --version) pkg-config --modversion libxml-2.0 ;;' \
        '  *) echo "Usage: xml2-config [--cflags|--libs|--version]" >&2; exit 1 ;;' \
        'esac' \
        > /usr/local/bin/xml2-config; \
      chmod +x /usr/local/bin/xml2-config; \
    fi

RUN python3 -m pip install --no-cache-dir --upgrade pip setuptools wheel && \
    python3 -m pip install --no-cache-dir --upgrade aqtinstall

RUN mkdir -p /etc/aqt && \
    printf '%s\n' \
      '[aqt]' \
      'baseurl: https://download.qt.io' \
      '' \
      '[requests]' \
      'hash_algorithm: sha256' \
      'INSECURE_NOT_FOR_PRODUCTION_ignore_hash: False' \
      '' \
      '[mirrors]' \
      'trusted_mirrors:' \
      '    https://download.qt.io' \
      'blacklist:' \
      'fallbacks:' \
      '    https://download.qt.io' \
      > /etc/aqt/settings.ini

ENV AQT_CONFIG=/etc/aqt/settings.ini

# osxcross requires a macOS SDK archive.
# Default points to the requested 14.5 SDK release URL and can be overridden
# with --build-arg MACOS_SDK_URL=...
ARG OSXCROSS_REF=master
ARG MACOS_SDK_URL=https://github.com/joseluisq/macosx-sdks/releases/download/14.5/MacOSX14.5.sdk.tar.xz
ARG MACOS_SDK_ARCHIVE=MacOSX14.5.sdk.tar.xz

RUN git clone --depth 1 --branch "${OSXCROSS_REF}" https://github.com/tpoechtrager/osxcross.git /opt/osxcross && \
    mkdir -p /opt/osxcross/tarballs && \
    if [ -n "${MACOS_SDK_URL}" ]; then curl -fsSL "${MACOS_SDK_URL}" -o "/opt/osxcross/tarballs/${MACOS_SDK_ARCHIVE}"; fi && \
    test -s "/opt/osxcross/tarballs/${MACOS_SDK_ARCHIVE}" && \
    cd /opt/osxcross && \
    UNATTENDED=1 OSX_VERSION_MIN=12.0 ./build.sh && \
    if [ -d /opt/osxcross/target/bin ]; then ln -sfn /opt/osxcross/target/bin /opt/osxcross/bin; fi

ENV OSXCROSS_ROOT=/opt/osxcross
ENV OSXCROSS_BIN_DIR=/opt/osxcross/target/bin
ENV PATH=/opt/osxcross/target/bin:/opt/osxcross/bin:${PATH}

ARG QT_SERIES=6.11
ARG QT_ARCH=clang_64

RUN dnf -y install qt6-qtbase-devel && \
    dnf clean all && \
    set -eux; \
    QT_VERSION="$(aqt list-qt mac desktop --spec "${QT_SERIES}" --latest-version)"; \
    aqt install-qt -O /opt/Qt mac desktop "${QT_VERSION}" "${QT_ARCH}" -m qtmultimedia qtspeech && \
    ln -sfn "/opt/Qt/${QT_VERSION}" /opt/Qt/latest

ARG LUA_VERSION=5.4.7
ARG LUASOCKET_VERSION=3.1.0
ARG LPEG_VERSION=1.1.0
ARG LUASEC_VERSION=1.3.2
ARG OPENSSL_VERSION=3.3.2
ARG MACOS_DEPLOYMENT_TARGET=13.0.0

RUN cat > /usr/local/bin/build-qmud-macos-lua-deps.sh <<'EOF' && chmod +x /usr/local/bin/build-qmud-macos-lua-deps.sh
#!/usr/bin/env bash
set -euxo pipefail

LUA_VERSION="${LUA_VERSION:?LUA_VERSION not set}"
LUASOCKET_VERSION="${LUASOCKET_VERSION:?LUASOCKET_VERSION not set}"
LPEG_VERSION="${LPEG_VERSION:?LPEG_VERSION not set}"
LUASEC_VERSION="${LUASEC_VERSION:?LUASEC_VERSION not set}"
OPENSSL_VERSION="${OPENSSL_VERSION:?OPENSSL_VERSION not set}"
MACOS_DEPLOYMENT_TARGET="${MACOS_DEPLOYMENT_TARGET:?MACOS_DEPLOYMENT_TARGET not set}"

find_osxcross_host() {
  local pattern="$1"
  find -L /opt/osxcross/bin /opt/osxcross/target/bin -maxdepth 1 \( -type f -o -type l \) \
    -name "${pattern}-apple-darwin*-clang" 2>/dev/null | sort | head -n 1 | awk -F/ '{print $NF}' | sed 's/-clang$//'
}

resolve_tool() {
  local tool_name="$1"
  local candidate
  for candidate in \
    "/opt/osxcross/bin/${tool_name}" \
    "/opt/osxcross/target/bin/${tool_name}" \
    "/opt/osxcross/bin/${X86_HOST}-${tool_name}" \
    "/opt/osxcross/target/bin/${X86_HOST}-${tool_name}"; do
    if [[ -x "${candidate}" ]]; then
      printf '%s' "${candidate}"
      return 0
    fi
  done
  return 1
}

resolve_host_tool() {
  local host="$1"
  local tool_name="$2"
  local candidate
  for candidate in \
    "/opt/osxcross/bin/${host}-${tool_name}" \
    "/opt/osxcross/target/bin/${host}-${tool_name}" \
    "/opt/osxcross/bin/${tool_name}" \
    "/opt/osxcross/target/bin/${tool_name}"; do
    if [[ -x "${candidate}" ]]; then
      printf '%s' "${candidate}"
      return 0
    fi
  done
  return 1
}

X86_HOST="$(find_osxcross_host x86_64)"
ARM_HOST="$(find_osxcross_host aarch64)"
if [[ -z "${ARM_HOST}" ]]; then
  ARM_HOST="$(find_osxcross_host arm64)"
fi
if [[ -z "${X86_HOST}" || -z "${ARM_HOST}" ]]; then
  echo "Error: universal macOS dependencies require both x86_64 and arm64 osxcross clang toolchains." >&2
  exit 1
fi

X86_CC="/opt/osxcross/bin/${X86_HOST}-clang"
ARM_CC="/opt/osxcross/bin/${ARM_HOST}-clang"
X86_AR="$(resolve_host_tool "${X86_HOST}" ar)"
X86_RANLIB="$(resolve_host_tool "${X86_HOST}" ranlib)"
ARM_AR="$(resolve_host_tool "${ARM_HOST}" ar)"
ARM_RANLIB="$(resolve_host_tool "${ARM_HOST}" ranlib)"
LIPO="$(resolve_tool lipo || resolve_tool llvm-lipo)"
UNIVERSAL_CFLAGS=(-O2 -fPIC "-mmacosx-version-min=${MACOS_DEPLOYMENT_TARGET}" -arch x86_64 -arch arm64)

mkdir -p /tmp/qmud-src
mkdir -p /opt/qmud/macos-deps/lua/include /opt/qmud/macos-deps/lua/lib
mkdir -p /opt/qmud/macos-deps/lua-modules/socket /opt/qmud/macos-deps/lua-modules/mime /opt/qmud/macos-deps/lua-modules/json /opt/qmud/macos-deps/lua-modules/ssl
mkdir -p /opt/qmud/macos-deps/openssl/include /opt/qmud/macos-deps/openssl/lib /tmp/qmud-src/openssl-slices

curl -fsSL "https://www.lua.org/ftp/lua-${LUA_VERSION}.tar.gz" -o /tmp/qmud-src/lua.tar.gz
tar -xf /tmp/qmud-src/lua.tar.gz -C /tmp/qmud-src
LUA_SRC_DIR="$(find /tmp/qmud-src -maxdepth 1 -type d -name "lua-${LUA_VERSION}*" | head -n 1 || true)"
[[ -n "${LUA_SRC_DIR}" ]]
cd "${LUA_SRC_DIR}/src"
"${X86_CC}" "${UNIVERSAL_CFLAGS[@]}" -DLUA_USE_MACOSX -I. -dynamiclib \
  lapi.c lauxlib.c lbaselib.c lcode.c lcorolib.c lctype.c ldblib.c ldebug.c ldo.c ldump.c lfunc.c lgc.c \
  linit.c liolib.c llex.c lmathlib.c lmem.c loadlib.c lobject.c lopcodes.c loslib.c lparser.c lstate.c \
  lstring.c lstrlib.c ltable.c ltablib.c ltm.c lundump.c lutf8lib.c lvm.c lzio.c \
  -Wl,-install_name,@rpath/liblua.5.4.dylib -o /opt/qmud/macos-deps/lua/lib/liblua.5.4.dylib -lm
ln -sf liblua.5.4.dylib /opt/qmud/macos-deps/lua/lib/liblua.dylib
cp lua.h lauxlib.h lualib.h luaconf.h /opt/qmud/macos-deps/lua/include/

curl -fsSL "https://github.com/lunarmodules/luasocket/archive/refs/tags/v${LUASOCKET_VERSION}.tar.gz" -o /tmp/qmud-src/luasocket.tar.gz || \
curl -fsSL "https://github.com/lunarmodules/luasocket/archive/refs/tags/${LUASOCKET_VERSION}.tar.gz" -o /tmp/qmud-src/luasocket.tar.gz
tar -xf /tmp/qmud-src/luasocket.tar.gz -C /tmp/qmud-src
LUASOCKET_SRC="$(find /tmp/qmud-src -maxdepth 1 -type d -name "luasocket*${LUASOCKET_VERSION}*" | head -n 1 || true)"
[[ -n "${LUASOCKET_SRC}" ]]
LUASOCKET_SRC_C_DIR="${LUASOCKET_SRC}/src"
[[ -d "${LUASOCKET_SRC_C_DIR}" ]]
cd "${LUASOCKET_SRC_C_DIR}"
"${X86_CC}" "${UNIVERSAL_CFLAGS[@]}" -I/opt/qmud/macos-deps/lua/include -bundle -undefined dynamic_lookup \
  auxiliar.c buffer.c compat.c except.c inet.c io.c luasocket.c options.c select.c tcp.c timeout.c udp.c usocket.c \
  -o /opt/qmud/macos-deps/lua-modules/socket/core.so
"${X86_CC}" "${UNIVERSAL_CFLAGS[@]}" -I/opt/qmud/macos-deps/lua/include -bundle -undefined dynamic_lookup \
  mime.c compat.c -o /opt/qmud/macos-deps/lua-modules/mime/core.so

copy_lua_file() {
  local name="$1"
  if [[ -f "${LUASOCKET_SRC_C_DIR}/${name}" ]]; then
    cp "${LUASOCKET_SRC_C_DIR}/${name}" "/opt/qmud/macos-deps/lua-modules/${name}"
    return 0
  fi
  if [[ -f "${LUASOCKET_SRC}/${name}" ]]; then
    cp "${LUASOCKET_SRC}/${name}" "/opt/qmud/macos-deps/lua-modules/${name}"
    return 0
  fi
  return 1
}

copy_socket_lua_file() {
  local name="$1"
  if [[ -f "${LUASOCKET_SRC_C_DIR}/${name}.lua" ]]; then
    cp "${LUASOCKET_SRC_C_DIR}/${name}.lua" "/opt/qmud/macos-deps/lua-modules/socket/${name}.lua"
    return 0
  fi
  if [[ -f "${LUASOCKET_SRC}/${name}.lua" ]]; then
    cp "${LUASOCKET_SRC}/${name}.lua" "/opt/qmud/macos-deps/lua-modules/socket/${name}.lua"
    return 0
  fi
  return 1
}

copy_lua_file socket.lua
copy_lua_file mime.lua
copy_lua_file ltn12.lua
cp /opt/qmud/macos-deps/lua-modules/socket.lua /opt/qmud/macos-deps/lua-modules/socket/socket.lua
cp /opt/qmud/macos-deps/lua-modules/mime.lua /opt/qmud/macos-deps/lua-modules/socket/mime.lua
cp /opt/qmud/macos-deps/lua-modules/ltn12.lua /opt/qmud/macos-deps/lua-modules/socket/ltn12.lua
for f in ftp headers http smtp tp url; do
  copy_socket_lua_file "${f}"
done
copy_socket_lua_file socket || true
copy_socket_lua_file mbox || true

curl -fsSL "https://www.openssl.org/source/openssl-${OPENSSL_VERSION}.tar.gz" -o /tmp/qmud-src/openssl.tar.gz || \
curl -fsSL "https://www.openssl.org/source/old/3.3/openssl-${OPENSSL_VERSION}.tar.gz" -o /tmp/qmud-src/openssl.tar.gz
tar -xf /tmp/qmud-src/openssl.tar.gz -C /tmp/qmud-src
OPENSSL_SRC="$(find /tmp/qmud-src -maxdepth 1 -type d -name "openssl-${OPENSSL_VERSION}*" | head -n 1 || true)"
[[ -n "${OPENSSL_SRC}" ]]

build_openssl_slice() {
  local arch_name="$1"
  local cc="$2"
  local ar="$3"
  local ranlib="$4"
  local openssl_target="$5"
  local prefix="/tmp/qmud-src/openssl-slices/${arch_name}"
  local build_dir="/tmp/qmud-src/openssl-${arch_name}"

  cp -R "${OPENSSL_SRC}" "${build_dir}"
  cd "${build_dir}"
  CC="${cc}" AR="${ar}" RANLIB="${ranlib}" ./Configure "${openssl_target}" no-shared no-tests no-apps no-module no-legacy \
    --prefix="${prefix}" \
    --openssldir="${prefix}"
  make -j"$(nproc)" AR="${ar}" RANLIB="${ranlib}"
  make install_sw AR="${ar}" RANLIB="${ranlib}"
}

build_openssl_slice x86_64 "${X86_CC}" "${X86_AR}" "${X86_RANLIB}" darwin64-x86_64-cc
build_openssl_slice arm64 "${ARM_CC}" "${ARM_AR}" "${ARM_RANLIB}" darwin64-arm64-cc
cp -R /tmp/qmud-src/openssl-slices/x86_64/include/. /opt/qmud/macos-deps/openssl/include/
"${LIPO}" -create \
  /tmp/qmud-src/openssl-slices/x86_64/lib/libssl.a \
  /tmp/qmud-src/openssl-slices/arm64/lib/libssl.a \
  -output /opt/qmud/macos-deps/openssl/lib/libssl.a
"${LIPO}" -create \
  /tmp/qmud-src/openssl-slices/x86_64/lib/libcrypto.a \
  /tmp/qmud-src/openssl-slices/arm64/lib/libcrypto.a \
  -output /opt/qmud/macos-deps/openssl/lib/libcrypto.a

curl -fsSL "https://github.com/brunoos/luasec/archive/refs/tags/v${LUASEC_VERSION}.tar.gz" -o /tmp/qmud-src/luasec.tar.gz || \
curl -fsSL "https://github.com/brunoos/luasec/archive/refs/tags/${LUASEC_VERSION}.tar.gz" -o /tmp/qmud-src/luasec.tar.gz
tar -xf /tmp/qmud-src/luasec.tar.gz -C /tmp/qmud-src
LUASEC_SRC="$(find /tmp/qmud-src -maxdepth 1 -type d -name "luasec*${LUASEC_VERSION}*" | head -n 1 || true)"
[[ -n "${LUASEC_SRC}" ]]
LUASEC_SRC_C_DIR="${LUASEC_SRC}/src"
[[ -d "${LUASEC_SRC_C_DIR}" ]]
cd "${LUASEC_SRC_C_DIR}"
LUASEC_C_SOURCES=""
for src in config.c context.c ec.c options.c ssl.c x509.c; do
  if [[ -f "${src}" ]]; then
    LUASEC_C_SOURCES="${LUASEC_C_SOURCES} ${src}"
  fi
done
if [[ -z "${LUASEC_C_SOURCES}" ]]; then
  echo "Error: could not resolve LuaSec C sources in ${LUASEC_SRC_C_DIR}" >&2
  exit 1
fi
LUASEC_SOCKET_SOURCES=""
for src in luasocket/buffer.c luasocket/io.c luasocket/timeout.c; do
  if [[ -f "${src}" ]]; then
    LUASEC_SOCKET_SOURCES="${LUASEC_SOCKET_SOURCES} ${src}"
  fi
done
if [[ -f "luasocket/usocket.c" ]]; then
  LUASEC_SOCKET_SOURCES="${LUASEC_SOCKET_SOURCES} luasocket/usocket.c"
elif [[ -f "luasocket/wsocket.c" ]]; then
  LUASEC_SOCKET_SOURCES="${LUASEC_SOCKET_SOURCES} luasocket/wsocket.c"
fi
if [[ -z "${LUASEC_SOCKET_SOURCES}" ]]; then
  echo "Error: could not resolve bundled LuaSocket sources in ${LUASEC_SRC_C_DIR}/luasocket" >&2
  exit 1
fi
"${X86_CC}" "${UNIVERSAL_CFLAGS[@]}" \
  -I/opt/qmud/macos-deps/lua/include \
  -I/opt/qmud/macos-deps/openssl/include \
  -I"${LUASEC_SRC_C_DIR}" \
  -bundle -undefined dynamic_lookup \
  ${LUASEC_C_SOURCES} ${LUASEC_SOCKET_SOURCES} \
  /opt/qmud/macos-deps/openssl/lib/libssl.a \
  /opt/qmud/macos-deps/openssl/lib/libcrypto.a \
  -lz \
  -o /opt/qmud/macos-deps/lua-modules/ssl/core.so

if [[ -f "${LUASEC_SRC}/ssl.lua" ]]; then
  cp "${LUASEC_SRC}/ssl.lua" /opt/qmud/macos-deps/lua-modules/ssl.lua
elif [[ -f "${LUASEC_SRC_C_DIR}/ssl.lua" ]]; then
  cp "${LUASEC_SRC_C_DIR}/ssl.lua" /opt/qmud/macos-deps/lua-modules/ssl.lua
fi
for name in config.lua https.lua options.lua; do
  if [[ -f "${LUASEC_SRC_C_DIR}/${name}" ]]; then
    cp "${LUASEC_SRC_C_DIR}/${name}" "/opt/qmud/macos-deps/lua-modules/ssl/${name}"
  fi
done
if [[ -d "${LUASEC_SRC_C_DIR}/ssl" ]]; then
  cp -R "${LUASEC_SRC_C_DIR}/ssl/." /opt/qmud/macos-deps/lua-modules/ssl/
elif [[ -d "${LUASEC_SRC}/ssl" ]]; then
  cp -R "${LUASEC_SRC}/ssl/." /opt/qmud/macos-deps/lua-modules/ssl/
fi

curl -fsSL "https://www.inf.puc-rio.br/~roberto/lpeg/lpeg-${LPEG_VERSION}.tar.gz" -o /tmp/qmud-src/lpeg.tar.gz
tar -xf /tmp/qmud-src/lpeg.tar.gz -C /tmp/qmud-src
LPEG_SRC="$(find /tmp/qmud-src -maxdepth 1 -type d -name "lpeg*${LPEG_VERSION}*" | head -n 1 || true)"
[[ -n "${LPEG_SRC}" ]]
if [[ -f "${LPEG_SRC}/lpcap.c" ]]; then
  LPEG_C_DIR="${LPEG_SRC}"
else
  LPEG_C_DIR="${LPEG_SRC}/src"
fi
[[ -f "${LPEG_C_DIR}/lpcap.c" ]]
cd "${LPEG_C_DIR}"
"${X86_CC}" "${UNIVERSAL_CFLAGS[@]}" -I/opt/qmud/macos-deps/lua/include -bundle -undefined dynamic_lookup \
  lpcap.c lpcode.c lptree.c lpvm.c -o /opt/qmud/macos-deps/lua-modules/lpeg.so
if [[ -f "${LPEG_SRC}/re.lua" ]]; then
  cp "${LPEG_SRC}/re.lua" /opt/qmud/macos-deps/lua-modules/re.lua
elif [[ -f "${LPEG_C_DIR}/re.lua" ]]; then
  cp "${LPEG_C_DIR}/re.lua" /opt/qmud/macos-deps/lua-modules/re.lua
fi

curl -fsSL "https://raw.githubusercontent.com/rxi/json.lua/master/json.lua" -o /opt/qmud/macos-deps/lua-modules/json.lua
cp /opt/qmud/macos-deps/lua-modules/json.lua /opt/qmud/macos-deps/lua-modules/json/init.lua

test -f /opt/qmud/macos-deps/lua/lib/liblua.5.4.dylib
test -f /opt/qmud/macos-deps/lua-modules/socket/core.so
test -f /opt/qmud/macos-deps/lua-modules/mime/core.so
test -f /opt/qmud/macos-deps/lua-modules/ssl/core.so
test -f /opt/qmud/macos-deps/lua-modules/ssl.lua
test -f /opt/qmud/macos-deps/openssl/include/openssl/ssl.h
test -f /opt/qmud/macos-deps/openssl/lib/libssl.a
test -f /opt/qmud/macos-deps/openssl/lib/libcrypto.a
rm -rf /tmp/qmud-src
EOF

RUN LUA_VERSION="${LUA_VERSION}" LUASOCKET_VERSION="${LUASOCKET_VERSION}" LPEG_VERSION="${LPEG_VERSION}" LUASEC_VERSION="${LUASEC_VERSION}" OPENSSL_VERSION="${OPENSSL_VERSION}" MACOS_DEPLOYMENT_TARGET="${MACOS_DEPLOYMENT_TARGET}" \
    /usr/local/bin/build-qmud-macos-lua-deps.sh

ENV QMUD_MACOS_QT_PREFIX=/opt/Qt/latest/macos
ENV QMUD_MACOS_QT_HOST_PREFIX=/usr/lib64/qt6
ENV QMUD_MACOS_LUA_PREFIX=/opt/qmud/macos-deps/lua
ENV QMUD_MACOS_LUA_MODULES=/opt/qmud/macos-deps/lua-modules
ENV QMUD_MACOS_OPENSSL_PREFIX=/opt/qmud/macos-deps/openssl

WORKDIR /work
