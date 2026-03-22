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
      libtool \
      make \
      mingw64-binutils \
      mingw64-crt \
      mingw64-gcc \
      mingw64-gcc-c++ \
      mingw64-headers \
      mingw-nsis-base \
      mingw32-nsis \
      mingw64-nsis \
      mingw64-zlib \
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
      qt6-qtbase-devel \
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

RUN python3 -m pip install --no-cache-dir --upgrade pip setuptools wheel && \
    python3 -m pip install --no-cache-dir --upgrade aqtinstall

ARG QT_VERSION=6.10.2
ARG QT_ARCH=win64_mingw

RUN aqt install-qt -O /opt/Qt windows desktop "${QT_VERSION}" "${QT_ARCH}" -m qtmultimedia

ARG LUA_VERSION=5.4.7
ARG LUASOCKET_VERSION=3.1.0
ARG LPEG_VERSION=1.1.0

RUN cat > /usr/local/bin/build-qmud-windows-lua-deps.sh <<'EOF' && chmod +x /usr/local/bin/build-qmud-windows-lua-deps.sh
#!/usr/bin/env bash
set -euxo pipefail

LUA_VERSION="${LUA_VERSION:?LUA_VERSION not set}"
LUASOCKET_VERSION="${LUASOCKET_VERSION:?LUASOCKET_VERSION not set}"
LPEG_VERSION="${LPEG_VERSION:?LPEG_VERSION not set}"

LUA_CC="${LUA_CC:-x86_64-w64-mingw32-gcc}"
if ! command -v "${LUA_CC}" >/dev/null 2>&1; then
  echo "Error: could not find ${LUA_CC}" >&2
  exit 1
fi

mkdir -p /tmp/qmud-src
mkdir -p /opt/qmud/windows-deps/lua/include /opt/qmud/windows-deps/lua/lib /opt/qmud/windows-deps/lua/bin
mkdir -p /opt/qmud/windows-deps/lua-modules/socket /opt/qmud/windows-deps/lua-modules/mime /opt/qmud/windows-deps/lua-modules/json

curl -fsSL "https://www.lua.org/ftp/lua-${LUA_VERSION}.tar.gz" -o /tmp/qmud-src/lua.tar.gz
tar -xf /tmp/qmud-src/lua.tar.gz -C /tmp/qmud-src
LUA_SRC_DIR="$(find /tmp/qmud-src -maxdepth 1 -type d -name "lua-${LUA_VERSION}*" | head -n 1 || true)"
[[ -n "${LUA_SRC_DIR}" ]]
cd "${LUA_SRC_DIR}/src"
LUA_C_SOURCES="lapi.c lauxlib.c lbaselib.c lcode.c lcorolib.c lctype.c ldblib.c ldebug.c ldo.c ldump.c lfunc.c lgc.c linit.c liolib.c llex.c lmathlib.c lmem.c loadlib.c lobject.c lopcodes.c loslib.c lparser.c lstate.c lstring.c lstrlib.c ltable.c ltablib.c ltm.c lundump.c lutf8lib.c lvm.c lzio.c"
"${LUA_CC}" -O2 -DLUA_BUILD_AS_DLL -shared ${LUA_C_SOURCES} \
  -Wl,--out-implib,/opt/qmud/windows-deps/lua/lib/lua54.dll.a \
  -o /opt/qmud/windows-deps/lua/bin/lua54.dll \
  -lws2_32 -lm
cp lua.h lauxlib.h lualib.h luaconf.h /opt/qmud/windows-deps/lua/include/
cp /opt/qmud/windows-deps/lua/bin/lua54.dll /opt/qmud/windows-deps/lua/lib/lua54.dll
ln -sf lua54.dll /opt/qmud/windows-deps/lua/bin/lua.dll
ln -sf lua54.dll.a /opt/qmud/windows-deps/lua/lib/lua.dll.a

curl -fsSL "https://github.com/lunarmodules/luasocket/archive/refs/tags/v${LUASOCKET_VERSION}.tar.gz" -o /tmp/qmud-src/luasocket.tar.gz || \
curl -fsSL "https://github.com/lunarmodules/luasocket/archive/refs/tags/${LUASOCKET_VERSION}.tar.gz" -o /tmp/qmud-src/luasocket.tar.gz
tar -xf /tmp/qmud-src/luasocket.tar.gz -C /tmp/qmud-src
LUASOCKET_SRC="$(find /tmp/qmud-src -maxdepth 1 -type d -name "luasocket*${LUASOCKET_VERSION}*" | head -n 1 || true)"
[[ -n "${LUASOCKET_SRC}" ]]
LUASOCKET_SRC_C_DIR="${LUASOCKET_SRC}/src"
[[ -d "${LUASOCKET_SRC_C_DIR}" ]]
cd "${LUASOCKET_SRC_C_DIR}"
"${LUA_CC}" -O2 -shared -Wl,--export-all-symbols -I/opt/qmud/windows-deps/lua/include \
  auxiliar.c buffer.c compat.c except.c inet.c io.c luasocket.c options.c select.c tcp.c timeout.c udp.c wsocket.c \
  -o /opt/qmud/windows-deps/lua-modules/socket/core.dll \
  -L/opt/qmud/windows-deps/lua/lib -llua54 -lws2_32
"${LUA_CC}" -O2 -shared -Wl,--export-all-symbols -I/opt/qmud/windows-deps/lua/include \
  mime.c compat.c \
  -o /opt/qmud/windows-deps/lua-modules/mime/core.dll \
  -L/opt/qmud/windows-deps/lua/lib -llua54

copy_lua_file() {
  local name="$1"
  if [[ -f "${LUASOCKET_SRC_C_DIR}/${name}" ]]; then
    cp "${LUASOCKET_SRC_C_DIR}/${name}" "/opt/qmud/windows-deps/lua-modules/${name}"
    return 0
  fi
  if [[ -f "${LUASOCKET_SRC}/${name}" ]]; then
    cp "${LUASOCKET_SRC}/${name}" "/opt/qmud/windows-deps/lua-modules/${name}"
    return 0
  fi
  return 1
}

copy_socket_lua_file() {
  local name="$1"
  if [[ -f "${LUASOCKET_SRC_C_DIR}/${name}.lua" ]]; then
    cp "${LUASOCKET_SRC_C_DIR}/${name}.lua" "/opt/qmud/windows-deps/lua-modules/socket/${name}.lua"
    return 0
  fi
  if [[ -f "${LUASOCKET_SRC}/${name}.lua" ]]; then
    cp "${LUASOCKET_SRC}/${name}.lua" "/opt/qmud/windows-deps/lua-modules/socket/${name}.lua"
    return 0
  fi
  return 1
}

copy_lua_file socket.lua
copy_lua_file mime.lua
copy_lua_file ltn12.lua
cp /opt/qmud/windows-deps/lua-modules/socket.lua /opt/qmud/windows-deps/lua-modules/socket/socket.lua
cp /opt/qmud/windows-deps/lua-modules/mime.lua /opt/qmud/windows-deps/lua-modules/socket/mime.lua
cp /opt/qmud/windows-deps/lua-modules/ltn12.lua /opt/qmud/windows-deps/lua-modules/socket/ltn12.lua
for f in ftp headers http smtp tp url; do
  copy_socket_lua_file "${f}"
done
copy_socket_lua_file socket || true
copy_socket_lua_file mbox || true

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
"${LUA_CC}" -O2 -shared -Wl,--export-all-symbols -I/opt/qmud/windows-deps/lua/include \
  lpcap.c lpcset.c lpcode.c lptree.c lpvm.c \
  -o /opt/qmud/windows-deps/lua-modules/lpeg.dll \
  -L/opt/qmud/windows-deps/lua/lib -llua54
if [[ -f "${LPEG_SRC}/re.lua" ]]; then
  cp "${LPEG_SRC}/re.lua" /opt/qmud/windows-deps/lua-modules/re.lua
elif [[ -f "${LPEG_C_DIR}/re.lua" ]]; then
  cp "${LPEG_C_DIR}/re.lua" /opt/qmud/windows-deps/lua-modules/re.lua
fi

curl -fsSL "https://raw.githubusercontent.com/rxi/json.lua/master/json.lua" -o /opt/qmud/windows-deps/lua-modules/json.lua
cp /opt/qmud/windows-deps/lua-modules/json.lua /opt/qmud/windows-deps/lua-modules/json/init.lua

test -f /opt/qmud/windows-deps/lua/bin/lua54.dll
test -f /opt/qmud/windows-deps/lua-modules/socket/core.dll
test -f /opt/qmud/windows-deps/lua-modules/mime/core.dll
test -f /opt/qmud/windows-deps/lua-modules/lpeg.dll
rm -rf /tmp/qmud-src
EOF

RUN LUA_VERSION="${LUA_VERSION}" LUASOCKET_VERSION="${LUASOCKET_VERSION}" LPEG_VERSION="${LPEG_VERSION}" \
    /usr/local/bin/build-qmud-windows-lua-deps.sh

ENV QMUD_WINDOWS_QT_PREFIX=/opt/Qt/${QT_VERSION}/mingw_64
ENV QMUD_WINDOWS_QT_HOST_PREFIX=/usr/lib64/qt6
ENV QMUD_WINDOWS_MINGW_PREFIX=/usr/x86_64-w64-mingw32/sys-root/mingw
ENV QMUD_WINDOWS_LUA_PREFIX=/opt/qmud/windows-deps/lua
ENV QMUD_WINDOWS_LUA_MODULES=/opt/qmud/windows-deps/lua-modules

WORKDIR /work
