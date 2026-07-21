#!/bin/sh
## @file mac_validate_qt_frameworks.sh
## @brief Verifies that staged macOS app bundles contain all referenced Qt frameworks.

set -eu

if [ "$#" -ne 1 ]; then
  echo "Usage: $0 <QMud.app>" >&2
  exit 2
fi

APP_STAGE_DIR=$1
APP_CONTENTS_DIR="$APP_STAGE_DIR/Contents"
APP_MACOS_DIR="$APP_CONTENTS_DIR/MacOS"
APP_FRAMEWORKS_DIR="$APP_CONTENTS_DIR/Frameworks"
APP_PLUGINS_DIR="$APP_CONTENTS_DIR/PlugIns"

if [ ! -d "$APP_STAGE_DIR" ]; then
  echo "Error: macOS app bundle is missing at $APP_STAGE_DIR." >&2
  exit 1
fi

resolve_macho_dependency_tool() {
  for candidate in \
    "/opt/osxcross/bin/${OSXCROSS_HOST:-}-otool" \
    "${OSXCROSS_TARGET_DIR:-/opt/osxcross/target}/bin/${OSXCROSS_HOST:-}-otool" \
    /opt/osxcross/bin/otool \
    "${OSXCROSS_TARGET_DIR:-/opt/osxcross/target}/bin/otool"; do
    if [ -x "$candidate" ]; then
      printf 'otool:%s\n' "$candidate"
      return 0
    fi
  done

  if command -v otool >/dev/null 2>&1; then
    printf 'otool:%s\n' "$(command -v otool)"
    return 0
  fi
  if command -v llvm-otool >/dev/null 2>&1; then
    printf 'otool:%s\n' "$(command -v llvm-otool)"
    return 0
  fi
  if command -v llvm-objdump >/dev/null 2>&1; then
    printf 'llvm-objdump:%s\n' "$(command -v llvm-objdump)"
    return 0
  fi

  return 1
}

TOOL_SPEC=$(resolve_macho_dependency_tool || true)
if [ -z "$TOOL_SPEC" ]; then
  echo "Error: could not resolve otool, llvm-otool, or llvm-objdump for macOS dependency validation." >&2
  exit 1
fi
MACHO_DEPS_MODE=${TOOL_SPEC%%:*}
MACHO_DEPS_TOOL=${TOOL_SPEC#*:}

list_macho_dependencies() {
  target_file=$1
  case "$MACHO_DEPS_MODE" in
    otool)
      "$MACHO_DEPS_TOOL" -L "$target_file" 2>/dev/null | awk 'NR > 1 { print $1 }'
      ;;
    llvm-objdump)
      "$MACHO_DEPS_TOOL" --macho --dylibs-used "$target_file" 2>/dev/null | awk 'NR > 1 { print $1 }'
      ;;
    *)
      return 1
      ;;
  esac
}

qt_framework_name_from_dependency() {
  dependency=$1
  printf '%s\n' "$dependency" | sed -n 's#^.*\(Qt[^/]*\)\.framework/.*#\1#p'
}

qt_framework_binary_exists() {
  framework_name=$1
  framework_dir="$APP_FRAMEWORKS_DIR/${framework_name}.framework"

  [ -d "$framework_dir" ] || return 1
  [ -e "$framework_dir/Versions/A/$framework_name" ] && return 0
  [ -e "$framework_dir/Versions/Current/$framework_name" ] && return 0
  [ -e "$framework_dir/$framework_name" ] && return 0
  return 1
}

APP_PARENT_DIR=$(dirname "$APP_STAGE_DIR")
MISSING_REPORT="$APP_PARENT_DIR/qmud-missing-qt-frameworks.$$"
: > "$MISSING_REPORT"
trap 'rm -f "$MISSING_REPORT"' EXIT HUP INT TERM

for search_dir in "$APP_MACOS_DIR" "$APP_FRAMEWORKS_DIR" "$APP_PLUGINS_DIR"; do
  if [ ! -d "$search_dir" ]; then
    continue
  fi

  find "$search_dir" -type f | while IFS= read -r binary_file; do
    dependencies=$(list_macho_dependencies "$binary_file") || continue
    for dependency in $dependencies; do
      case "$dependency" in
        *Qt*.framework/*) ;;
        *) continue ;;
      esac

      framework_name=$(qt_framework_name_from_dependency "$dependency")
      if [ -z "$framework_name" ]; then
        continue
      fi
      if ! qt_framework_binary_exists "$framework_name"; then
        display_path=${binary_file#"$APP_STAGE_DIR/"}
        printf '%s requires %s, but %s.framework is not staged in Contents/Frameworks.\n' \
          "$display_path" "$dependency" "$framework_name" >> "$MISSING_REPORT"
      fi
    done
  done
done

if [ -s "$MISSING_REPORT" ]; then
  echo "Error: macOS package is missing required Qt framework dependencies:" >&2
  sort -u "$MISSING_REPORT" >&2
  exit 1
fi
