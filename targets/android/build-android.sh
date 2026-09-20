#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
ANDROID_DIR="$ROOT_DIR/targets/android"
# Packages come from this repo's own node_modules, the way node resolves them.
resolve_package() { node -e "process.stdout.write(require('fs').realpathSync('$ROOT_DIR/node_modules/$1'))" 2>/dev/null || true; }
GEA_CORE_DIR="${GEA_CORE_DIR:-$(resolve_package @geastack/core)}"
GEA_COMPILER_DIR="${GEA_COMPILER_DIR:-$(resolve_package @geastack/compiler)}"
GEA_CLI_BIN="${GEA_CLI_BIN:-$(resolve_package @geastack/cli)/bin/gea.mjs}"
for _pair in "@geastack/core:$GEA_CORE_DIR" "@geastack/compiler:$GEA_COMPILER_DIR" "@geastack/cli:$GEA_CLI_BIN"; do
  [ -e "${_pair#*:}" ] || { echo "Cannot resolve ${_pair%%:*} — run \`npm install\` in $ROOT_DIR" >&2; exit 1; }
done
ANDROID_SDK="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-$HOME/Library/Android/sdk}}"
if [ -n "${GEA_ANDROID_ADB:-}" ]; then
  ADB="$GEA_ANDROID_ADB"
elif [ -x "$ANDROID_SDK/platform-tools/adb" ]; then
  ADB="$ANDROID_SDK/platform-tools/adb"
else
  ADB="adb"
fi

APP_ID="${1:-tic-tac-toe}"
MODE="${2:-debug}"
DEBUG_VIEW_BOUNDS="${GEA_ANDROID_DEBUG_VIEW_BOUNDS:-${GEA_ANDROID_DEBUG_BOUNDS:-0}}"
shift $(( $# >= 1 ? 1 : 0 ))
shift $(( $# >= 1 ? 1 : 0 ))

ADB_SERIAL="${GEA_ANDROID_SERIAL:-}"
PASSTHROUGH=()
while [ "$#" -gt 0 ]; do
  case "$1" in
    --serial=*)
      ADB_SERIAL="${1#--serial=}"
      ;;
    --serial)
      ADB_SERIAL="${2:-}"
      shift
      ;;
    --debug-view-bounds|--show-view-bounds)
      DEBUG_VIEW_BOUNDS=1
      ;;
    *)
      PASSTHROUGH+=("$1")
      ;;
  esac
  shift
done

case "$MODE" in
  debug|build|device|install|launch|monitor)
    ;;
  *)
    echo "usage: targets/android/build-android.sh [app-id] [debug|device|install|launch|monitor] [--serial <adb-serial>]" >&2
    exit 2
    ;;
esac

require_file() {
  local file="$1"
  local label="$2"
  if [ ! -f "$file" ]; then
    echo "ERROR: Missing $label: $file" >&2
    exit 1
  fi
}

require_command() {
  local command="$1"
  local label="${2:-$1}"
  if ! command -v "$command" >/dev/null 2>&1; then
    echo "ERROR: Missing $label command: $command" >&2
    exit 1
  fi
}

read_geatsc_sources() {
  local out_dir="$1"
  local source_list="$out_dir/geatsc-sources.txt"
  if [ ! -f "$source_list" ]; then
    echo "ERROR: Missing geatsc source list: $source_list" >&2
    return 1
  fi
  local source
  while IFS= read -r source; do
    [ -n "$source" ] || continue
    case "$source" in
      /*) ;;
      *) source="$out_dir/$source" ;;
    esac
    if [ ! -f "$source" ]; then
      echo "ERROR: geatsc source listed but missing: $source" >&2
      return 1
    fi
    printf '%s\n' "$source"
  done < "$source_list"
}

adb_cmd() {
  ADB_MDNS_AUTO_CONNECT="${ADB_MDNS_AUTO_CONNECT:-0}" \
    ADB_MDNS_OPENSCREEN="${ADB_MDNS_OPENSCREEN:-0}" \
    "$ADB" "$@"
}

ensure_adb_server() {
  local attempt
  for attempt in 1 2 3; do
    if adb_cmd start-server >/dev/null 2>&1; then
      return 0
    fi
    adb_cmd kill-server >/dev/null 2>&1 || true
    sleep "$attempt"
  done
  adb_cmd start-server >/dev/null
}

adb_target_cmd() {
  if [ -n "$ADB_SERIAL" ]; then
    adb_cmd -s "$ADB_SERIAL" "$@"
  else
    adb_cmd "$@"
  fi
}

json_string() {
  node -e "process.stdout.write(JSON.stringify(process.argv[1] || ''))" "$1"
}

xml_escape() {
  node -e "const s = process.argv[1] || ''; process.stdout.write(s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/\"/g, '&quot;'))" "$1"
}

android_launcher_icon_source() {
  node - "$1" "$2" <<'NODE'
const fs = require('fs')
const path = require('path')

const app = JSON.parse(process.argv[2] || '{}')
const appDir = process.argv[3] || process.cwd()
const icons = app.icons && typeof app.icons === 'object' ? app.icons : {}
const numericSizes = Object.keys(icons)
  .map((key) => Number(key))
  .filter((size) => Number.isFinite(size) && size > 0)
  .sort((a, b) => b - a)
const sizes = [...new Set([512, 256, 128, 64, 32, ...numericSizes])]

for (const size of sizes) {
  const icon = icons[String(size)]
  if (typeof icon !== 'string' || icon.length === 0) continue
  const resolved = path.resolve(appDir, icon)
  if (fs.existsSync(resolved)) {
    process.stdout.write(resolved)
    process.exit(0)
  }
}
NODE
}

# Generate the launcher icon resources for every density. The app artwork is
# used as the full-bleed adaptive background; the foreground stays transparent
# (adaptive-icon requires a foreground layer, but the artwork is the background).
android_generate_launcher_icons() {
  node - "$1" "$2" "$GEA_CORE_DIR" <<'NODE'
const fs = require('fs')
const path = require('path')

const source = process.argv[2]
const resDir = process.argv[3]
const coreDir = process.argv[4]

;(async () => {
let sharp
try {
  sharp = require('sharp')
} catch {
  sharp = require(path.join(coreDir, 'node_modules', 'sharp'))
}

const densities = [
  ['mdpi', 1],
  ['hdpi', 1.5],
  ['xhdpi', 2],
  ['xxhdpi', 3],
  ['xxxhdpi', 4]
]

for (const [qualifier, scale] of densities) {
  const dir = path.join(resDir, `mipmap-${qualifier}`)
  fs.mkdirSync(dir, { recursive: true })
  const adaptiveSize = Math.round(108 * scale)
  const legacySize = Math.round(48 * scale)
  const legacy = await sharp(source)
    .resize(legacySize, legacySize, { fit: 'cover' })
    .ensureAlpha()
    .png()
    .toBuffer()
  const adaptiveBackground = await sharp(source)
    .resize(adaptiveSize, adaptiveSize, { fit: 'cover' })
    .ensureAlpha()
    .png()
    .toBuffer()
  const transparentForeground = await sharp({
    create: {
      width: adaptiveSize,
      height: adaptiveSize,
      channels: 4,
      background: { r: 0, g: 0, b: 0, alpha: 0 }
    }
  })
    .png()
    .toBuffer()

  fs.writeFileSync(path.join(dir, 'gea_launcher_icon.png'), legacy)
  fs.writeFileSync(path.join(dir, 'gea_launcher_icon_background.png'), adaptiveBackground)
  fs.writeFileSync(path.join(dir, 'gea_launcher_icon_foreground.png'), transparentForeground)
}
})().catch((error) => {
  console.error(error && error.stack ? error.stack : String(error))
  process.exit(1)
})
NODE
}

java_package_from_app_id() {
  node -e "
    const raw = process.argv[1] || 'app';
    const parts = raw.toLowerCase().split(/[^a-z0-9]+/).filter(Boolean);
    const tail = (parts.join('') || 'app').replace(/^[0-9]+/, '') || 'app';
    process.stdout.write('com.geastack.apps.' + tail);
  " "$1"
}

jni_prefix_from_package() {
  node -e "process.stdout.write((process.argv[1] || '').replace(/_/g, '_1').replace(/[.]/g, '_'))" "$1"
}

latest_android_platform() {
  local best_api=0
  local best_dir=""
  local dir api
  for dir in "$ANDROID_SDK"/platforms/android-*; do
    [ -d "$dir" ] || continue
    api="${dir##*-}"
    [[ "$api" =~ ^[0-9]+$ ]] || continue
    if (( api > best_api )); then
      best_api="$api"
      best_dir="$dir"
    fi
  done
  printf '%s\n' "$best_dir"
}

latest_build_tools() {
  local best_score=0
  local best_dir=""
  local dir name major minor patch score
  for dir in "$ANDROID_SDK"/build-tools/*; do
    [ -d "$dir" ] || continue
    name="$(basename "$dir")"
    IFS=. read -r major minor patch _ <<< "$name"
    [[ "${major:-}" =~ ^[0-9]+$ ]] || continue
    [[ "${minor:-0}" =~ ^[0-9]+$ ]] || minor=0
    [[ "${patch:-0}" =~ ^[0-9]+$ ]] || patch=0
    score=$(( major * 1000000 + minor * 1000 + patch ))
    if (( score > best_score )); then
      best_score="$score"
      best_dir="$dir"
    fi
  done
  printf '%s\n' "$best_dir"
}

latest_ndk() {
  local best_score=0
  local best_dir=""
  local dir name major minor patch score
  for dir in "$ANDROID_SDK"/ndk/*; do
    [ -d "$dir" ] || continue
    name="$(basename "$dir")"
    IFS=. read -r major minor patch _ <<< "$name"
    [[ "${major:-}" =~ ^[0-9]+$ ]] || continue
    [[ "${minor:-0}" =~ ^[0-9]+$ ]] || minor=0
    [[ "${patch:-0}" =~ ^[0-9]+$ ]] || patch=0
    score=$(( major * 1000000 + minor * 1000 + patch ))
    if (( score > best_score )); then
      best_score="$score"
      best_dir="$dir"
    fi
  done
  printf '%s\n' "$best_dir"
}

latest_cmake_dir() {
  local best_score=0
  local best_dir=""
  local dir name major minor patch score
  for dir in "$ANDROID_SDK"/cmake/*; do
    [ -d "$dir" ] || continue
    name="$(basename "$dir")"
    IFS=. read -r major minor patch _ <<< "$name"
    [[ "${major:-}" =~ ^[0-9]+$ ]] || continue
    [[ "${minor:-0}" =~ ^[0-9]+$ ]] || minor=0
    [[ "${patch:-0}" =~ ^[0-9]+$ ]] || patch=0
    score=$(( major * 1000000 + minor * 1000 + patch ))
    if (( score > best_score )); then
      best_score="$score"
      best_dir="$dir"
    fi
  done
  printf '%s\n' "$best_dir"
}

if [ "$MODE" = "monitor" ]; then
  require_command "$ADB" "Android Debug Bridge"
  ensure_adb_server
  if [ -n "$ADB_SERIAL" ]; then
    exec env \
      ADB_MDNS_AUTO_CONNECT="${ADB_MDNS_AUTO_CONNECT:-0}" \
      ADB_MDNS_OPENSCREEN="${ADB_MDNS_OPENSCREEN:-0}" \
      "$ADB" -s "$ADB_SERIAL" logcat ${PASSTHROUGH[@]+"${PASSTHROUGH[@]}"}
  fi
  exec env \
    ADB_MDNS_AUTO_CONNECT="${ADB_MDNS_AUTO_CONNECT:-0}" \
    ADB_MDNS_OPENSCREEN="${ADB_MDNS_OPENSCREEN:-0}" \
    "$ADB" logcat ${PASSTHROUGH[@]+"${PASSTHROUGH[@]}"}
fi

require_file "$GEA_CLI_BIN" "Gea CLI"
require_file "$GEA_CORE_DIR/package.json" "@geastack/core package"
require_file "$GEA_COMPILER_DIR/package.json" "@geastack/compiler package"
require_file "$GEA_CORE_DIR/gea_sources.sh" "Gea source manifest"
require_command javac "Java compiler"
require_command zip "zip"

# The project is the directory this script runs in -- run it from the app
# project you want to compile.
APP_JSON="$(node "$GEA_CLI_BIN" inspect "$APP_ID" --json)"
# `apps inspect` reports an absolute directory; nothing to join it to.
APP_DIR="$(node -e "const app = JSON.parse(process.argv[1]); process.stdout.write(app.root)" "$APP_JSON")"
APP_ENTRY="$(node -e "const app = JSON.parse(process.argv[1]); process.stdout.write(app.entry)" "$APP_JSON")"
APP_RUNTIME="$(node -e "const app = JSON.parse(process.argv[1]); process.stdout.write(app.runtime)" "$APP_JSON")"
APP_NAME="$(node -e "const app = JSON.parse(process.argv[1]); process.stdout.write(app.name || app.id)" "$APP_JSON")"

if [ "$APP_RUNTIME" != "gea" ]; then
  echo "ERROR: Android target only supports runtime=gea apps for now: $APP_ID is runtime=$APP_RUNTIME" >&2
  exit 1
fi

ANDROID_PLATFORM_DIR="$(latest_android_platform)"
BUILD_TOOLS_DIR="$(latest_build_tools)"
NDK_DIR="${GEA_ANDROID_NDK:-$(latest_ndk)}"
CMAKE_DIR="${GEA_ANDROID_CMAKE:-$(latest_cmake_dir)}"
if [ -z "$ANDROID_PLATFORM_DIR" ] || [ -z "$BUILD_TOOLS_DIR" ] || [ -z "$NDK_DIR" ] || [ -z "$CMAKE_DIR" ]; then
  echo "ERROR: Android SDK platforms, build-tools, cmake, and ndk are required. Set ANDROID_HOME or ANDROID_SDK_ROOT." >&2
  exit 1
fi

ANDROID_JAR="$ANDROID_PLATFORM_DIR/android.jar"
AAPT2="$BUILD_TOOLS_DIR/aapt2"
D8="$BUILD_TOOLS_DIR/d8"
ZIPALIGN="$BUILD_TOOLS_DIR/zipalign"
APKSIGNER="$BUILD_TOOLS_DIR/apksigner"
CMAKE_BIN="$CMAKE_DIR/bin/cmake"
NINJA_BIN="$CMAKE_DIR/bin/ninja"
require_file "$ANDROID_JAR" "Android platform jar"
require_file "$AAPT2" "aapt2"
require_file "$D8" "d8"
require_file "$ZIPALIGN" "zipalign"
require_file "$APKSIGNER" "apksigner"
require_file "$CMAKE_BIN" "cmake"
require_file "$NINJA_BIN" "ninja"
require_file "$NDK_DIR/build/cmake/android.toolchain.cmake" "Android NDK toolchain"

PACKAGE_NAME="${GEA_ANDROID_PACKAGE_NAME:-$(java_package_from_app_id "$APP_ID")}"
JNI_PREFIX="$(jni_prefix_from_package "$PACKAGE_NAME")"
MIN_SDK="${GEA_ANDROID_MIN_SDK:-23}"
TARGET_SDK="${ANDROID_PLATFORM_DIR##*-}"
ABI="${GEA_ANDROID_ABI:-arm64-v8a}"
ORIENTATION="${GEA_ANDROID_SCREEN_ORIENTATION:-portrait}"
DEVICE_PIXEL_RATIO="${GEA_ANDROID_DEVICE_PIXEL_RATIO:-1.5}"
BUILD_DIR="$ANDROID_DIR/build/$APP_ID"
DIST_DIR="$ANDROID_DIR/dist/$APP_ID"
GENERATED_DIR="$BUILD_DIR/generated"
ANDROID_BUILD_DIR="$BUILD_DIR/android"
NATIVE_BUILD_DIR="$BUILD_DIR/native-build"
NATIVE_TEMPLATE_DIR="$ANDROID_DIR/native"

if ! [[ "$DEVICE_PIXEL_RATIO" =~ ^[0-9]+([.][0-9]+)?$ ]]; then
  echo "ERROR: GEA_ANDROID_DEVICE_PIXEL_RATIO must be a non-negative number, or 0 to use Android display density." >&2
  exit 1
fi

case "$DEBUG_VIEW_BOUNDS" in
  1|true|TRUE|yes|YES|on|ON)
    DEBUG_VIEW_BOUNDS=true
    ;;
  *)
    DEBUG_VIEW_BOUNDS=false
    ;;
esac

rm -rf "$BUILD_DIR"
mkdir -p "$GENERATED_DIR" "$DIST_DIR" "$ANDROID_BUILD_DIR" "$NATIVE_BUILD_DIR"

echo "Generating native Gea C++ for $APP_ID..."
node "$GEA_CORE_DIR/scripts/build-gea-vite-geatsc.mjs" \
  --app-dir "$APP_DIR" \
  --entry "$APP_ENTRY" \
  --out-dir "$GENERATED_DIR" \
  --font-device-pixel-ratio "${GEA_ANDROID_FONT_DEVICE_PIXEL_RATIOS:-1,1.5,2,3}" \
  --font-viewport-width "${GEA_ANDROID_FONT_VIEWPORT_WIDTHS:-480}" \
  --font-viewport-height "${GEA_ANDROID_FONT_VIEWPORT_HEIGHTS:-640}" \
  --cxx-standard c++20
if [ ! -f "$GENERATED_DIR/gea_runtime.cpp" ]; then
  printf '#include "%s"\n' "$GEA_COMPILER_DIR/dist/targets/cpp/runtime/runtime.cpp" > "$GENERATED_DIR/gea_runtime.cpp"
fi
GEATSC_GENERATED_SOURCES=()
while IFS= read -r source_file; do
  GEATSC_GENERATED_SOURCES+=("$source_file")
done < <(read_geatsc_sources "$GENERATED_DIR")
if [ "${#GEATSC_GENERATED_SOURCES[@]}" -eq 0 ]; then
  echo "ERROR: geatsc source list is empty: $GENERATED_DIR/geatsc-sources.txt" >&2
  exit 1
fi

export GEA_CORE="$GEA_CORE_DIR"
# gea_sources.mjs resolves every framework package from these (see the
# manifest's header); default them to the collection layout like build-macos.sh.
GEA_HOST_DIR="${GEA_HOST_DIR:-$GEA_CORE_DIR/../host}"
GEA_ENGINE_DIR="${GEA_ENGINE_DIR:-$GEA_CORE_DIR/../engine}"
GEA_ELEMENTS_DIR="${GEA_ELEMENTS_DIR:-$GEA_CORE_DIR/../elements}"
GEA_GEAOS_PACKAGE_DIR="${GEA_GEAOS_PACKAGE_DIR:-$GEA_CORE_DIR/../geaos}"
export GEA_HOST_DIR GEA_ENGINE_DIR GEA_ELEMENTS_DIR GEA_GEAOS_PACKAGE_DIR
# shellcheck source=/dev/null
source "$GEA_CORE_DIR/gea_sources.sh"
GEA_FW_C_SOURCES=()
while IFS= read -r source_file; do
  GEA_FW_C_SOURCES+=("$source_file")
done < <(gea_fw_c_sources)

GEA_FW_CXX_SOURCES=()
while IFS= read -r source_file; do
  GEA_FW_CXX_SOURCES+=("$source_file")
done < <(gea_fw_cxx_sources | grep -vE "/(runtime|services/[a-z_]+)\.cpp$")

GEA_FW_INCLUDES=()
while IFS= read -r include_dir; do
  GEA_FW_INCLUDES+=("$include_dir")
done < <(gea_fw_include_flags | sed 's/^-I//')
for runtime_include in "$GEA_COMPILER_DIR/dist/targets/cpp/runtime" "$GEA_COMPILER_DIR/src/targets/cpp/runtime"; do
  [ -d "$runtime_include" ] && GEA_FW_INCLUDES+=("$runtime_include")
done

APP_NATIVE_C_SOURCES=()
APP_NATIVE_CXX_SOURCES=()
APP_NATIVE_INCLUDE_DIRS=()
APP_NATIVE_HEADER_INCLUDES=()
while IFS= read -r native_source; do
  [ -n "$native_source" ] || continue
  case "$native_source" in
    /*) resolved_source="$native_source" ;;
    *) resolved_source="$APP_DIR/$native_source" ;;
  esac
  require_file "$resolved_source" "app native source"
  source_dir="$(dirname "$resolved_source")"
  APP_NATIVE_INCLUDE_DIRS+=("$source_dir")
  case "$resolved_source" in
    *.c)
      APP_NATIVE_C_SOURCES+=("$resolved_source")
      ;;
    *.cc|*.cpp|*.cxx)
      APP_NATIVE_CXX_SOURCES+=("$resolved_source")
      source_base="${resolved_source%.*}"
      for header_ext in h hh hpp hxx; do
        header_file="$source_base.$header_ext"
        if [ -f "$header_file" ]; then
          APP_NATIVE_HEADER_INCLUDES+=("$header_file")
          break
        fi
      done
      ;;
    *.h|*.hh|*.hpp|*.hxx)
      APP_NATIVE_HEADER_INCLUDES+=("$resolved_source")
      ;;
  esac
done < <(node - "$APP_DIR/package.json" <<'NODE'
const fs = require('fs')
const manifestPath = process.argv[2]
const manifest = JSON.parse(fs.readFileSync(manifestPath, 'utf8'))
for (const source of manifest.gea?.nativeSources ?? []) {
  if (typeof source === 'string' && source.length > 0) console.log(source)
}
NODE
)

PACKAGE_PATH="$(printf '%s' "$PACKAGE_NAME" | tr . /)"
JAVA_SRC_DIR="$ANDROID_BUILD_DIR/java"
MANIFEST_DIR="$ANDROID_BUILD_DIR/manifest"
RES_DIR="$ANDROID_BUILD_DIR/res"
GEN_DIR="$ANDROID_BUILD_DIR/generated"
CLASSES_DIR="$ANDROID_BUILD_DIR/classes"
DEX_DIR="$ANDROID_BUILD_DIR/dex"
LIBS_DIR="$ANDROID_BUILD_DIR/apk/lib"
ASSETS_DIR="$ANDROID_BUILD_DIR/apk/assets"
FONT_ASSETS_DIR="$ASSETS_DIR/fonts"
mkdir -p "$JAVA_SRC_DIR/$PACKAGE_PATH" "$MANIFEST_DIR" "$RES_DIR/values" "$GEN_DIR" "$CLASSES_DIR" "$DEX_DIR" "$LIBS_DIR/$ABI" "$FONT_ASSETS_DIR"

ANDROID_LAUNCHER_ICON="$(android_launcher_icon_source "$APP_JSON" "$APP_DIR")"
ANDROID_ICON_ATTRIBUTES=()
if [ -n "$ANDROID_LAUNCHER_ICON" ]; then
  mkdir -p "$RES_DIR/mipmap-anydpi-v26"
  android_generate_launcher_icons "$ANDROID_LAUNCHER_ICON" "$RES_DIR"
  cat > "$RES_DIR/mipmap-anydpi-v26/gea_launcher_icon.xml" <<'XML'
<adaptive-icon xmlns:android="http://schemas.android.com/apk/res/android">
  <background android:drawable="@mipmap/gea_launcher_icon_background" />
  <foreground android:drawable="@mipmap/gea_launcher_icon_foreground" />
</adaptive-icon>
XML
  ANDROID_ICON_ATTRIBUTES+=('      android:icon="@mipmap/gea_launcher_icon"')
  ANDROID_ICON_ATTRIBUTES+=('      android:roundIcon="@mipmap/gea_launcher_icon"')
fi

for template in MainActivity.java GeaNativeBridge.java GeaNativeView.java GeaNativeAudio.java; do
  sed \
    -e "s/@PACKAGE@/$PACKAGE_NAME/g" \
    -e "s/@DEVICE_PIXEL_RATIO@/$DEVICE_PIXEL_RATIO/g" \
    -e "s/@DEBUG_VIEW_BOUNDS@/$DEBUG_VIEW_BOUNDS/g" \
    "$NATIVE_TEMPLATE_DIR/$template.in" > "$JAVA_SRC_DIR/$PACKAGE_PATH/$template"
done
node - "$APP_DIR" "$(pwd -P)" "$FONT_ASSETS_DIR" "$JAVA_SRC_DIR/$PACKAGE_PATH/GeaFontAssets.java" "$PACKAGE_NAME" <<'NODE'
const fs = require('fs')
const path = require('path')
const crypto = require('crypto')

const [appDir, projectDir, fontAssetsDir, javaOut, packageName] = process.argv.slice(2)

function findCssFiles(dir) {
  const files = []
  const stack = [dir]
  while (stack.length > 0) {
    const current = stack.pop()
    if (!current || !fs.existsSync(current)) continue
    for (const entry of fs.readdirSync(current, { withFileTypes: true })) {
      const full = path.join(current, entry.name)
      if (entry.isDirectory()) stack.push(full)
      else if (entry.isFile() && entry.name.endsWith('.css')) files.push(full)
    }
  }
  return files.sort()
}

function stripCssComments(css) {
  return css.replace(/\/\*[\s\S]*?\*\//g, '')
}

function unquoteCss(value) {
  const text = value.trim()
  if (text.length >= 2 && ((text[0] === '"' && text[text.length - 1] === '"') || (text[0] === "'" && text[text.length - 1] === "'"))) {
    return text.slice(1, -1)
  }
  return text
}

function parseFontFaces(css, cssFile) {
  const faces = []
  const cssDir = path.dirname(cssFile)
  const re = /@font-face\s*\{([^}]+)\}/gi
  let match
  while ((match = re.exec(css)) !== null) {
    const block = match[1]
    const familyMatch = block.match(/font-family\s*:\s*(?:"([^"]+)"|'([^']+)'|([^;]+))/i)
    const srcMatch = block.match(/src\s*:\s*url\(\s*(?:"([^"]+)"|'([^']+)'|([^)"']+))\s*\)/i)
    const family = familyMatch ? unquoteCss((familyMatch[1] ?? familyMatch[2] ?? familyMatch[3]).trim()) : null
    const rawSrc = srcMatch ? (srcMatch[1] ?? srcMatch[2] ?? srcMatch[3]).trim() : null
    if (!family || !rawSrc || /^https?:\/\//i.test(rawSrc) || rawSrc.startsWith('data:')) continue
    const cleanSrc = rawSrc.split(/[?#]/, 1)[0]
    const resolved = path.resolve(cssDir, cleanSrc)
    if (fs.existsSync(resolved)) faces.push({ family, src: resolved })
  }
  return faces
}

function sanitizeAssetName(value) {
  const name = value.toLowerCase().replace(/[^a-z0-9]+/g, '_').replace(/^_+|_+$/g, '')
  return name || 'font'
}

// The app, then whatever the project shares between its apps.
const sourceDirs = [
  appDir,
  path.join(projectDir, 'shared'),
  path.join(projectDir, 'components'),
]
const faces = new Map()
for (const dir of sourceDirs) {
  for (const file of findCssFiles(dir)) {
    const css = stripCssComments(fs.readFileSync(file, 'utf8'))
    for (const face of parseFontFaces(css, file)) {
      if (!faces.has(face.family)) faces.set(face.family, face.src)
    }
  }
}

const entries = []
for (const [family, src] of [...faces.entries()].sort((a, b) => a[0].localeCompare(b[0]))) {
  const bytes = fs.readFileSync(src)
  const hash = crypto.createHash('sha1').update(bytes).digest('hex').slice(0, 10)
  const ext = path.extname(src).toLowerCase() || '.ttf'
  const assetName = `${sanitizeAssetName(family)}-${hash}${ext}`
  const assetPath = `fonts/${assetName}`
  fs.copyFileSync(src, path.join(fontAssetsDir, assetName))
  entries.push({ family, assetPath })
}

function javaString(value) {
  return JSON.stringify(value).replace(/\u2028/g, '\\u2028').replace(/\u2029/g, '\\u2029')
}

const lines = [
  `package ${packageName};`,
  '',
  'final class GeaFontAssets {',
  '  private static final String[] FAMILIES = {',
]
for (const entry of entries) lines.push(`    ${javaString(entry.family)},`)
lines.push('  };')
lines.push('  private static final String[] PATHS = {')
for (const entry of entries) lines.push(`    ${javaString(entry.assetPath)},`)
lines.push('  };')
lines.push('')
lines.push('  private GeaFontAssets() {}')
lines.push('')
lines.push('  static String pathFor(String family) {')
lines.push('    if (family == null) return null;')
lines.push('    for (int i = 0; i < FAMILIES.length; i++) {')
lines.push('      if (family.equals(FAMILIES[i])) return PATHS[i];')
lines.push('    }')
lines.push('    return null;')
lines.push('  }')
lines.push('}')
lines.push('')
fs.writeFileSync(javaOut, lines.join('\n'))
console.log(`Packaged Android font assets: ${entries.map((entry) => entry.family).join(', ') || 'none'}`)
NODE
sed \
  -e "s/@JNI_PREFIX@/$JNI_PREFIX/g" \
  -e "s|@PACKAGE_PATH@|$PACKAGE_PATH|g" \
  "$NATIVE_TEMPLATE_DIR/android_jni.cpp.in" > "$NATIVE_BUILD_DIR/android_jni.cpp"

APP_NATIVE_HEADER_FILE="$NATIVE_BUILD_DIR/gea_app_native_headers.h"
{
  echo '#pragma once'
  for header in ${APP_NATIVE_HEADER_INCLUDES[@]+"${APP_NATIVE_HEADER_INCLUDES[@]}"}; do
    printf '#include "%s"\n' "$header"
  done
} > "$APP_NATIVE_HEADER_FILE"

cmake_q() {
  node -e "process.stdout.write(JSON.stringify(process.argv[1] || ''))" -- "$1"
}

{
  echo 'cmake_minimum_required(VERSION 3.22)'
  echo 'project(gea_android_native LANGUAGES C CXX)'
  echo 'set(CMAKE_CXX_STANDARD 20)'
  echo 'set(CMAKE_CXX_STANDARD_REQUIRED ON)'
  echo 'set(CMAKE_CXX_EXTENSIONS OFF)'
  echo 'add_library(gea_android SHARED'
  printf '  %s\n' "$(cmake_q "$NATIVE_BUILD_DIR/android_jni.cpp")"
  printf '  %s\n' "$(cmake_q "$NATIVE_TEMPLATE_DIR/android_display.cpp")"
  printf '  %s\n' "$(cmake_q "$NATIVE_TEMPLATE_DIR/android_timers.cpp")"
  printf '  %s\n' "$(cmake_q "$NATIVE_TEMPLATE_DIR/android_memory.cpp")"
  printf '  %s\n' "$(cmake_q "$NATIVE_TEMPLATE_DIR/android_audio.cpp")"
  printf '  %s\n' "$(cmake_q "$NATIVE_TEMPLATE_DIR/android_network.cpp")"
  printf '  %s\n' "$(cmake_q "$NATIVE_TEMPLATE_DIR/android_platform_stubs.cpp")"
  printf '  %s\n' "$(cmake_q "$NATIVE_TEMPLATE_DIR/android_apps.c")"
  printf '  %s\n' "$(cmake_q "$GEA_CORE_DIR/gea_app_entry.cpp")"
  [ -f "$GEA_GEAOS_PACKAGE_DIR/resident_apps.cpp" ] && printf '  %s\n' "$(cmake_q "$GEA_GEAOS_PACKAGE_DIR/resident_apps.cpp")"
  for src in "${GEATSC_GENERATED_SOURCES[@]}"; do printf '  %s\n' "$(cmake_q "$src")"; done
  printf '  %s\n' "$(cmake_q "$GENERATED_DIR/gea_embedded_font_generated.cpp")"
  printf '  %s\n' "$(cmake_q "$GENERATED_DIR/gea_embedded_assets_generated.cpp")"
  for src in ${APP_NATIVE_C_SOURCES[@]+"${APP_NATIVE_C_SOURCES[@]}"}; do printf '  %s\n' "$(cmake_q "$src")"; done
  for src in ${APP_NATIVE_CXX_SOURCES[@]+"${APP_NATIVE_CXX_SOURCES[@]}"}; do printf '  %s\n' "$(cmake_q "$src")"; done
  for src in "${GEA_FW_C_SOURCES[@]}"; do printf '  %s\n' "$(cmake_q "$src")"; done
  for src in "${GEA_FW_CXX_SOURCES[@]}"; do printf '  %s\n' "$(cmake_q "$src")"; done
  echo ')'
  echo 'target_compile_definitions(gea_android PRIVATE'
  echo '  GEA_EMBEDDED_PIXEL_FORMAT=GEA_PIXEL_ARGB8888'
  echo '  GEA_EMBEDDED_DISPLAY_WIDTH=480'
  echo '  GEA_EMBEDDED_DISPLAY_HEIGHT=640'
  echo '  GEA_EMBEDDED_DISPLAY_NATIVE_WIDTH=480'
  echo '  GEA_EMBEDDED_DISPLAY_NATIVE_HEIGHT=640'
  echo '  GEA_CPP_USE_FROM_CHARS_DOUBLE=0'
  echo '  GEA_EMBEDDED_GIF_C_API=1'
  echo ')'
  echo 'target_compile_options(gea_android PRIVATE -fexceptions -frtti -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)'
  echo 'set_source_files_properties('
  for src in "${GEATSC_GENERATED_SOURCES[@]}"; do printf '  %s\n' "$(cmake_q "$src")"; done
  printf '  PROPERTIES COMPILE_OPTIONS %s\n' "$(cmake_q "-include;$APP_NATIVE_HEADER_FILE")"
  echo ')'
  echo 'target_link_libraries(gea_android PRIVATE log android)'
  echo 'target_include_directories(gea_android PRIVATE'
  printf '  %s\n' "$(cmake_q "$GENERATED_DIR")"
  for inc in ${APP_NATIVE_INCLUDE_DIRS[@]+"${APP_NATIVE_INCLUDE_DIRS[@]}"}; do printf '  %s\n' "$(cmake_q "$inc")"; done
  for inc in "${GEA_FW_INCLUDES[@]}"; do printf '  %s\n' "$(cmake_q "$inc")"; done
  echo ')'
} > "$NATIVE_BUILD_DIR/CMakeLists.txt"

echo "Building native Android library ($ABI)..."
"$CMAKE_BIN" \
  -S "$NATIVE_BUILD_DIR" \
  -B "$NATIVE_BUILD_DIR/cmake" \
  -G Ninja \
  -DCMAKE_MAKE_PROGRAM="$NINJA_BIN" \
  -DCMAKE_TOOLCHAIN_FILE="$NDK_DIR/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI="$ABI" \
  -DANDROID_PLATFORM="android-$MIN_SDK" \
  -DCMAKE_BUILD_TYPE=Release
"$CMAKE_BIN" --build "$NATIVE_BUILD_DIR/cmake" --target gea_android --parallel "${GEA_ANDROID_BUILD_JOBS:-4}"
cp "$NATIVE_BUILD_DIR/cmake/libgea_android.so" "$LIBS_DIR/$ABI/libgea_android.so"

WEBKIT_PATTERN='android\.web''kit\.'
BROWSER_VIEW_PATTERN='Web''View'
if rg -n "${WEBKIT_PATTERN}${BROWSER_VIEW_PATTERN}|${BROWSER_VIEW_PATTERN}" "$JAVA_SRC_DIR" "$NATIVE_TEMPLATE_DIR" >/dev/null; then
  echo "ERROR: Android native target must not reference browser-backed views." >&2
  exit 1
fi

APP_LABEL="$(xml_escape "$APP_NAME")"
{
  echo "<manifest xmlns:android=\"http://schemas.android.com/apk/res/android\" package=\"$PACKAGE_NAME\">"
  echo "  <uses-permission android:name=\"android.permission.INTERNET\" />"
  echo "  <uses-permission android:name=\"android.permission.ACCESS_NETWORK_STATE\" />"
  echo "  <application"
  echo "      android:theme=\"@style/GeaAndroidTheme\""
  echo "      android:label=\"$APP_LABEL\""
  for attr in "${ANDROID_ICON_ATTRIBUTES[@]}"; do
    echo "$attr"
  done
  echo "      android:hardwareAccelerated=\"true\""
  echo "      android:extractNativeLibs=\"true\">"
  echo "    <activity"
  echo "        android:name=\".MainActivity\""
  echo "        android:screenOrientation=\"$ORIENTATION\""
  echo "        android:configChanges=\"keyboard|keyboardHidden|orientation|screenSize|smallestScreenSize|uiMode\""
  echo "        android:exported=\"true\">"
  echo "      <intent-filter>"
  echo "        <action android:name=\"android.intent.action.MAIN\" />"
  echo "        <category android:name=\"android.intent.category.LAUNCHER\" />"
  echo "      </intent-filter>"
  echo "    </activity>"
  echo "  </application>"
  echo "</manifest>"
} > "$MANIFEST_DIR/AndroidManifest.xml"

cat > "$RES_DIR/values/styles.xml" <<'XML'
<resources>
  <style name="GeaAndroidTheme" parent="@android:style/Theme.Material.NoActionBar">
    <item name="android:windowNoTitle">true</item>
    <item name="android:windowActionBar">false</item>
    <item name="android:windowFullscreen">true</item>
    <item name="android:windowDrawsSystemBarBackgrounds">true</item>
    <item name="android:navigationBarColor">#000000</item>
    <item name="android:statusBarColor">#000000</item>
    <item name="android:windowLightStatusBar">false</item>
  </style>
</resources>
XML

COMPILED_RES="$ANDROID_BUILD_DIR/resources.zip"
UNSIGNED_APK="$ANDROID_BUILD_DIR/$APP_ID-unsigned.apk"
DEX_APK="$ANDROID_BUILD_DIR/$APP_ID-dex.apk"
ALIGNED_APK="$ANDROID_BUILD_DIR/$APP_ID-aligned.apk"
SIGNED_APK="$DIST_DIR/$APP_ID-debug.apk"

"$AAPT2" compile --dir "$RES_DIR" -o "$COMPILED_RES"
"$AAPT2" link \
  -o "$UNSIGNED_APK" \
  -I "$ANDROID_JAR" \
  --manifest "$MANIFEST_DIR/AndroidManifest.xml" \
  --java "$GEN_DIR" \
  --min-sdk-version "$MIN_SDK" \
  --target-sdk-version "$TARGET_SDK" \
  --rename-manifest-package "$PACKAGE_NAME" \
  -R "$COMPILED_RES" \
  --auto-add-overlay

find "$JAVA_SRC_DIR" "$GEN_DIR" -name '*.java' -print0 \
  | xargs -0 javac -source 1.8 -target 1.8 -bootclasspath "$ANDROID_JAR" -classpath "$ANDROID_JAR" -d "$CLASSES_DIR"
find "$CLASSES_DIR" -name '*.class' -print0 \
  | xargs -0 "$D8" --min-api "$MIN_SDK" --lib "$ANDROID_JAR" --output "$DEX_DIR"

cp "$UNSIGNED_APK" "$DEX_APK"
zip -q -j "$DEX_APK" "$DEX_DIR/classes.dex"
(cd "$ANDROID_BUILD_DIR/apk" && zip -q -r "$DEX_APK" lib)
if [ -d "$ASSETS_DIR" ]; then
  (cd "$ANDROID_BUILD_DIR/apk" && zip -q -r "$DEX_APK" assets)
fi

KEYSTORE="${GEA_ANDROID_KEYSTORE:-$HOME/.android/debug.keystore}"
KEYSTORE_PASS="${GEA_ANDROID_KEYSTORE_PASS:-android}"
KEY_ALIAS="${GEA_ANDROID_KEY_ALIAS:-androiddebugkey}"
KEY_PASS="${GEA_ANDROID_KEY_PASS:-android}"
if [ ! -f "$KEYSTORE" ]; then
  keytool -genkeypair \
    -keystore "$KEYSTORE" \
    -storepass "$KEYSTORE_PASS" \
    -alias "$KEY_ALIAS" \
    -keypass "$KEY_PASS" \
    -keyalg RSA \
    -keysize 2048 \
    -validity 10000 \
    -dname "CN=Android Debug,O=GeaStack,C=US" >/dev/null
fi

"$ZIPALIGN" -f 4 "$DEX_APK" "$ALIGNED_APK"
"$APKSIGNER" sign \
  --ks "$KEYSTORE" \
  --ks-pass "pass:$KEYSTORE_PASS" \
  --ks-key-alias "$KEY_ALIAS" \
  --key-pass "pass:$KEY_PASS" \
  --out "$SIGNED_APK" \
  "$ALIGNED_APK"

"$APKSIGNER" verify "$SIGNED_APK" >/dev/null
echo "Built native Android APK: $SIGNED_APK"

case "$MODE" in
  debug|build)
    exit 0
    ;;
  device|install|launch)
    require_command "$ADB" "Android Debug Bridge"
    ensure_adb_server
    if [ "$MODE" = "device" ] || [ "$MODE" = "install" ]; then
      adb_target_cmd install -r "$SIGNED_APK"
    fi
    if [ "$MODE" = "device" ] || [ "$MODE" = "launch" ]; then
      adb_target_cmd shell am start -S \
        -a android.intent.action.MAIN \
        -c android.intent.category.LAUNCHER \
        -n "$PACKAGE_NAME/.MainActivity" >/dev/null
      echo "Launched native Android package: $PACKAGE_NAME"
    fi
    ;;
esac
