#!/bin/sh
set -eu

if [ "$#" -lt 2 ]; then
  echo "Usage: $0 <AppDir> <binary-or-library> [more...]" >&2
  exit 2
fi

APPDIR="$1"
shift

BUNDLE_GLIBC="${QMUD_APPIMAGE_BUNDLE_GLIBC:-0}"

if [ -d "$APPDIR/usr/lib" ]; then
  APP_LIB_DIR="$APPDIR/usr/lib"
elif [ -d "$APPDIR/usr/lib64" ]; then
  APP_LIB_DIR="$APPDIR/usr/lib64"
else
  APP_LIB_DIR="$APPDIR/usr/lib"
fi

APP_GLIBC_DIR="$APPDIR/usr/glibc"
APP_PLUGIN_DIR="$APPDIR/usr/plugins"
mkdir -p "$APP_LIB_DIR" "$APP_GLIBC_DIR" "$APP_PLUGIN_DIR"

is_glibc_runtime_lib() {
  lib_name="$1"
  case "$lib_name" in
    ld-linux*.so*|ld-musl*.so*|libc.so*|libm.so*|libdl.so*|libpthread.so*|librt.so*|libresolv.so*|libnss_*|\
    libgcc_s.so*)
      return 0
      ;;
  esac
  return 1
}

copy_lib() {
  lib="$1"
  [ -f "$lib" ] || return 0
  name="$(basename "$lib")"
  dst_dir="$APP_LIB_DIR"
  if is_glibc_runtime_lib "$name"; then
    if [ "$BUNDLE_GLIBC" != "1" ]; then
      return 0
    fi
    dst_dir="$APP_GLIBC_DIR"
  fi
  dst="$dst_dir/$name"
  if [ -e "$dst" ]; then
    src_real="$(readlink -f "$lib" 2>/dev/null || printf '%s' "$lib")"
    dst_real="$(readlink -f "$dst" 2>/dev/null || printf '%s' "$dst")"
    if [ "$src_real" = "$dst_real" ]; then
      return 0
    fi
  fi
  case "$name" in
    linux-vdso.so*)
      return 0
      ;;
  esac
  case "$name" in
    ld-musl*.so*|\
    libwayland-*.so*|libxkbcommon*.so*|libxcb*.so*|libX11*.so*|libXau.so*|libXdmcp.so*|libXext.so*|libXrender.so*|libSM.so*|libICE.so*|\
    libEGL.so*|libGLX.so*|libOpenGL.so*|libGLdispatch.so*|libdrm.so*|libgbm.so*|\
    libdbus-1.so*|libglib-2.0.so*|libgobject-2.0.so*|libgmodule-2.0.so*|libgio-2.0.so*|\
    libsystemd.so*|libselinux.so*|libmount.so*|libblkid.so*|libcap.so*)
      return 0
      ;;
  esac
  cp -L "$lib" "$dst"
}

has_command() {
  command -v "$1" >/dev/null 2>&1
}

is_elf_binary() {
  target="$1"
  [ -f "$target" ] || return 1
  if ! has_command patchelf; then
    return 1
  fi
  patchelf --print-rpath "$target" >/dev/null 2>&1
}

ensure_runpath() {
  target="$1"
  runpath="$2"
  case "$target" in
    "$APP_GLIBC_DIR"/*)
      return 0
      ;;
  esac
  if ! is_elf_binary "$target"; then
    return 0
  fi
  current="$(patchelf --print-rpath "$target" 2>/dev/null || true)"
  if [ "$current" = "$runpath" ]; then
    return 0
  fi
  patchelf --set-rpath "$runpath" "$target"
}

collect_deps_for() {
  item="$1"
  [ -e "$item" ] || return 0
  ldd "$item" 2>/dev/null | awk '{for(i=1;i<=NF;i++) if($i ~ /^\//) print $i}' | while IFS= read -r dep; do
    [ -n "$dep" ] || continue
    copy_lib "$dep"
  done
}

for entry in "$@"; do
  collect_deps_for "$entry"
done

if [ "$BUNDLE_GLIBC" = "1" ]; then
  copy_first_system_lib() {
    lib_name="$1"
    for lib_dir in /lib64 /lib /usr/lib64 /usr/lib; do
      if [ -f "$lib_dir/$lib_name" ]; then
        copy_lib "$lib_dir/$lib_name"
        return 0
      fi
    done
    return 0
  }

  for nss_lib in libnss_files.so.2 libnss_dns.so.2 libnss_compat.so.2; do
    copy_first_system_lib "$nss_lib"
  done
fi

if [ -n "${QMUD_QT_PLUGIN_DIR:-}" ] && [ -d "${QMUD_QT_PLUGIN_DIR}" ]; then
  copy_plugin() {
    src="$1"
    sub="$2"
    [ -f "$src" ] || return 0
    mkdir -p "$APP_PLUGIN_DIR/$sub"
    cp -L "$src" "$APP_PLUGIN_DIR/$sub/"
  }

  # Keep plugin set minimal and Qt-native. Avoid platformtheme extras that can
  # drag in large host/KDE stacks and unstable portal behavior.
  copy_plugin "${QMUD_QT_PLUGIN_DIR}/platforms/libqxcb.so" platforms
  copy_plugin "${QMUD_QT_PLUGIN_DIR}/platforms/libqwayland.so" platforms
  copy_plugin "${QMUD_QT_PLUGIN_DIR}/platforms/libqwayland-egl.so" platforms
  copy_plugin "${QMUD_QT_PLUGIN_DIR}/platforms/libqwayland-generic.so" platforms
  copy_plugin "${QMUD_QT_PLUGIN_DIR}/sqldrivers/libqsqlite.so" sqldrivers
  copy_plugin "${QMUD_QT_PLUGIN_DIR}/tls/libqopensslbackend.so" tls
  copy_plugin "${QMUD_QT_PLUGIN_DIR}/iconengines/libqsvgicon.so" iconengines
  copy_plugin "${QMUD_QT_PLUGIN_DIR}/styles/libqfusionstyle.so" styles
  copy_plugin "${QMUD_QT_PLUGIN_DIR}/xcbglintegrations/libqxcb-glx-integration.so" xcbglintegrations
  copy_plugin "${QMUD_QT_PLUGIN_DIR}/xcbglintegrations/libqxcb-egl-integration.so" xcbglintegrations

  for sub in wayland-decoration-client wayland-graphics-integration-client wayland-shell-integration; do
    if [ -d "${QMUD_QT_PLUGIN_DIR}/${sub}" ]; then
      mkdir -p "$APP_PLUGIN_DIR/$sub"
      find "${QMUD_QT_PLUGIN_DIR}/${sub}" -maxdepth 1 -type f -name '*.so*' -exec cp -L {} "$APP_PLUGIN_DIR/$sub/" \;
    fi
  done

  if [ -d "${QMUD_QT_PLUGIN_DIR}/imageformats" ]; then
    mkdir -p "$APP_PLUGIN_DIR/imageformats"
    find "${QMUD_QT_PLUGIN_DIR}/imageformats" -maxdepth 1 -type f -name 'libq*.so*' -exec cp -L {} "$APP_PLUGIN_DIR/imageformats/" \;
  fi
fi

for plugin_dir in "$APPDIR/usr/plugins" "$APPDIR/usr/lib/qt6/plugins" "$APPDIR/usr/lib64/qt6/plugins"; do
  if [ -d "$plugin_dir" ]; then
    find "$plugin_dir" -type f -name '*.so*' | while IFS= read -r plugin; do
      collect_deps_for "$plugin"
    done
  fi
done

if has_command patchelf; then
  APP_LIB_RUNPATH="\$ORIGIN:\$ORIGIN/../lib:\$ORIGIN/../lib64"
  APP_BIN_RUNPATH="\$ORIGIN/../lib:\$ORIGIN/../lib64:\$ORIGIN/../plugins"

  for entry in "$@"; do
    case "$entry" in
      "$APPDIR/usr/bin/"*)
        ensure_runpath "$entry" "$APP_BIN_RUNPATH"
        ;;
    esac
  done

  find "$APP_LIB_DIR" -maxdepth 1 -type f -name '*.so*' | while IFS= read -r lib_path; do
    ensure_runpath "$lib_path" "$APP_LIB_RUNPATH"
  done
else
  echo "error: patchelf is required for AppImage RUNPATH patching" >&2
  exit 1
fi
