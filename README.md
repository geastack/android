# Android target

Builds GeaStack apps into native Android APKs. geatsc compiles the app to C++,
the NDK builds it into a native library, and each component node is rendered as
a real Android view (`FrameLayout`, `TextView`, `ImageView`, `ScrollView`, ...)
through JNI. There is no WebView and no JavaScript engine on the device.

`gea build --target android` is not available (the CLI answers "The android
build is not driven by gea"). The entry point is the package's
`targets/android/build-android.sh`, run from the app project.

## Set up an app

`@geastack/android` has no dependencies of its own. The app installs the
packages the script needs next to it: the framework packages, the compiler
(a dependency of `@geastack/core`) and the CLI.

```sh
npm install --save-dev @geastack/android @geastack/core @geastack/host \
  @geastack/engine @geastack/elements @geastack/geaos @geastack/cli
```

From the app folder (the script resolves the packages and the app id from the
current directory):

```sh
bash node_modules/@geastack/android/targets/android/build-android.sh <app-id> build
bash node_modules/@geastack/android/targets/android/build-android.sh <app-id> device --serial <adb-serial>
```

`<app-id>` is the `gea.id` in the app's `package.json`.

## Modes and flags

`build-android.sh [app-id] [mode] [--serial <adb-serial>] [--debug-view-bounds]`

- `build` (or `debug`, the default): build and sign the APK.
- `install`: build, then `adb install -r` it.
- `launch`: build, then start the already-installed package's
  `MainActivity` (it does not install).
- `device`: build, install and launch.
- `monitor`: skip the build and stream `adb logcat` (unfiltered); extra
  arguments are passed to `logcat`.
- `--serial <adb-serial>` (or `--serial=<adb-serial>`): the device to use;
  same as `GEA_ANDROID_SERIAL`.
- `--debug-view-bounds`: outline every native view's bounds; same as
  `GEA_ANDROID_DEBUG_VIEW_BOUNDS=1`.

The signed APK is written to `dist/android/<app-id>/<app-id>-debug.apk` in the
app folder, with intermediate files in `dist/android/<app-id>/build/`.
`GEA_ANDROID_OUTPUT_DIR` moves the `dist/android` tree.

## Requirements

- Android SDK with at least one platform (`platforms/android-*`), build-tools,
  NDK and CMake (with its `ninja`) installed, found through `ANDROID_HOME` or
  `ANDROID_SDK_ROOT` (default `~/Library/Android/sdk` on macOS,
  `~/Android/Sdk` on Linux). The newest installed
  version of each is used.
- A JDK (`javac`, and `keytool` to create the debug keystore on first use).
- `zip`, Node.js 22 or later (for `@geastack/cli`), and `adb` for the install,
  launch, device and monitor modes.

The target uses the Android SDK command-line tools directly (`aapt2`, `javac`,
`d8`, `zipalign`, and `apksigner`) so it does not require Gradle.

Useful environment overrides:

- `GEA_ANDROID_SERIAL`: adb serial for the target board.
- `GEA_ANDROID_ADB`: adb binary to use when multiple Android SDKs are installed.
- `GEA_ANDROID_NDK` / `GEA_ANDROID_CMAKE`: NDK and CMake to use (default: the
  latest installed in the SDK).
- `GEA_ANDROID_ABI`: native ABI to build (default `arm64-v8a`).
- `GEA_ANDROID_MIN_SDK`: minimum SDK level (default `23`).
- `GEA_ANDROID_PACKAGE_NAME`: Java package name (default: derived from the app id).
- `GEA_ANDROID_SCREEN_ORIENTATION`: activity orientation (default `portrait`).
- `GEA_ANDROID_DEVICE_PIXEL_RATIO`: CSS pixel ratio (default `1.5`).
- `GEA_ANDROID_BUILD_JOBS`: parallel native compile jobs (default `4`).
- `GEA_ANDROID_KEYSTORE`, `GEA_ANDROID_KEYSTORE_PASS`, `GEA_ANDROID_KEY_ALIAS`,
  `GEA_ANDROID_KEY_PASS`: signing key (default: the Android debug keystore).
- `GEA_ANDROID_DEBUG_VIEW_BOUNDS=1`: outline every native view's bounds.
- `GEA_CORE_DIR`, `GEA_COMPILER_DIR`, `GEA_CLI_BIN`, `GEA_HOST_DIR`,
  `GEA_ENGINE_DIR`, `GEA_ELEMENTS_DIR`, `GEA_GEAOS_PACKAGE_DIR`: use these
  package copies instead of the ones installed in the app.

The build fails if the generated Java or native sources reference a WebView.

## License

Apache-2.0 (see `LICENSE`). You can ship closed-source products
built on it. The only GeaStack code under a different license is
the embedded board support (`targets` and `@geastack/chips`, GPL-3.0-only):
shipping closed-source firmware through those needs a commercial license.
Contact [contact@geastack.com](mailto:contact@geastack.com) for commercial terms, support and hosted builds.
