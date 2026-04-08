FROM quay.io/fedora/fedora:43

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

ARG QT_VERSION=6.10.2
ARG QT_ARCH=clang_64

RUN dnf -y install qt6-qtbase-devel && \
    dnf clean all && \
    aqt install-qt -O /opt/Qt mac desktop "${QT_VERSION}" "${QT_ARCH}" -m qtmultimedia qtspeech

ARG LUA_VERSION=5.4.7
ARG LUASOCKET_VERSION=3.1.0
ARG LPEG_VERSION=1.1.0
ARG LUASEC_VERSION=1.3.2
ARG OPENSSL_VERSION=3.3.2

RUN cat > /usr/local/bin/build-qmud-macos-lua-deps.sh <<'EOF' && chmod +x /usr/local/bin/build-qmud-macos-lua-deps.sh
#!/usr/bin/env bash
set -euxo pipefail

LUA_VERSION="${LUA_VERSION:?LUA_VERSION not set}"
LUASOCKET_VERSION="${LUASOCKET_VERSION:?LUASOCKET_VERSION not set}"
LPEG_VERSION="${LPEG_VERSION:?LPEG_VERSION not set}"
LUASEC_VERSION="${LUASEC_VERSION:?LUASEC_VERSION not set}"
OPENSSL_VERSION="${OPENSSL_VERSION:?OPENSSL_VERSION not set}"

LUA_CC=""
for CANDIDATE_DIR in "${OSXCROSS_BIN_DIR:-/opt/osxcross/target/bin}" /opt/osxcross/bin /usr/local/osxcross/bin; do
  if [[ -d "${CANDIDATE_DIR}" ]]; then
    LUA_CC="$(find "${CANDIDATE_DIR}" -maxdepth 1 -type f -name 'x86_64-apple-darwin*-clang' | sort | head -n 1 || true)"
    [[ -n "${LUA_CC}" ]] && break
  fi
done
if [[ -z "${LUA_CC}" ]] && command -v o64-clang >/dev/null 2>&1; then
  LUA_CC="$(command -v o64-clang)"
fi
if [[ -z "${LUA_CC}" ]]; then
  echo "Error: could not locate macOS cross-clang compiler in expected osxcross bin directories" >&2
  ls -la "${OSXCROSS_BIN_DIR:-/opt/osxcross/target/bin}" >&2 || true
  ls -la /opt/osxcross/bin >&2 || true
  ls -la /usr/local/osxcross/bin >&2 || true
  exit 1
fi

mkdir -p /tmp/qmud-src
mkdir -p /opt/qmud/macos-deps/lua/include /opt/qmud/macos-deps/lua/lib
mkdir -p /opt/qmud/macos-deps/lua-modules/socket /opt/qmud/macos-deps/lua-modules/mime /opt/qmud/macos-deps/lua-modules/json /opt/qmud/macos-deps/lua-modules/ssl

curl -fsSL "https://www.lua.org/ftp/lua-${LUA_VERSION}.tar.gz" -o /tmp/qmud-src/lua.tar.gz
tar -xf /tmp/qmud-src/lua.tar.gz -C /tmp/qmud-src
LUA_SRC_DIR="$(find /tmp/qmud-src -maxdepth 1 -type d -name "lua-${LUA_VERSION}*" | head -n 1 || true)"
[[ -n "${LUA_SRC_DIR}" ]]
cd "${LUA_SRC_DIR}/src"
"${LUA_CC}" -O2 -fPIC -mmacosx-version-min=12.0 -DLUA_USE_MACOSX -I. -dynamiclib \
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
"${LUA_CC}" -O2 -fPIC -mmacosx-version-min=12.0 -I/opt/qmud/macos-deps/lua/include -bundle -undefined dynamic_lookup \
  auxiliar.c buffer.c compat.c except.c inet.c io.c luasocket.c options.c select.c tcp.c timeout.c udp.c usocket.c \
  -o /opt/qmud/macos-deps/lua-modules/socket/core.so
"${LUA_CC}" -O2 -fPIC -mmacosx-version-min=12.0 -I/opt/qmud/macos-deps/lua/include -bundle -undefined dynamic_lookup \
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
cd "${OPENSSL_SRC}"
OPENSSL_TARGET="darwin64-x86_64-cc"
if [[ "${LUA_CC}" == *"aarch64-apple-darwin"* ]] || [[ "${LUA_CC}" == *"arm64-apple-darwin"* ]]; then
  OPENSSL_TARGET="darwin64-arm64-cc"
fi
CC="${LUA_CC}" ./Configure "${OPENSSL_TARGET}" no-shared no-tests no-apps no-module no-legacy \
  --prefix=/opt/qmud/macos-deps/openssl \
  --openssldir=/opt/qmud/macos-deps/openssl
make -j"$(nproc)"
make install_sw

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
"${LUA_CC}" -O2 -fPIC -mmacosx-version-min=12.0 \
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
"${LUA_CC}" -O2 -fPIC -mmacosx-version-min=12.0 -I/opt/qmud/macos-deps/lua/include -bundle -undefined dynamic_lookup \
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

RUN LUA_VERSION="${LUA_VERSION}" LUASOCKET_VERSION="${LUASOCKET_VERSION}" LPEG_VERSION="${LPEG_VERSION}" LUASEC_VERSION="${LUASEC_VERSION}" OPENSSL_VERSION="${OPENSSL_VERSION}" \
    /usr/local/bin/build-qmud-macos-lua-deps.sh

ENV QMUD_MACOS_QT_PREFIX=/opt/Qt/${QT_VERSION}/macos
ENV QMUD_MACOS_QT_HOST_PREFIX=/usr/lib64/qt6
ENV QMUD_MACOS_LUA_PREFIX=/opt/qmud/macos-deps/lua
ENV QMUD_MACOS_LUA_MODULES=/opt/qmud/macos-deps/lua-modules

WORKDIR /work
