# Android target

Builds GeaStack apps into native Android APKs. geatsc compiles the app to C++,
the NDK builds it into a native library, and each component node is rendered as
a real Android view (`FrameLayout`, `TextView`, `ImageView`, `ScrollView`, ...)
through JNI. There is no WebView and no JavaScript engine on the device.

## Set up an app

Declare `gea.targets.android` in the app's `package.json` and install the target alongside
the framework packages and CLI.

```sh
npm install --save-dev @geastack/android @geastack/core @geastack/host \
  @geastack/engine @geastack/elements @geastack/cli
gea build --target android
gea run --target android
```

Run these commands from the app folder. `build` signs an APK and `run` also installs
and launches it. Pass `--serial <adb-serial>` to select a device when several
are connected, or set `GEA_ANDROID_ROOT` to use a target source checkout.

The CLI invokes `targets/android/build-android.sh` with the app ID from `gea.id`.

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
  version of each is used. Use NDK r30 or newer for compiler runtimes that require
  C++20 `std::atomic_ref`.
- JDK 11 or newer (`javac`, and `keytool` to create the debug keystore on first use).
- `zip`, `curl` (fetches the Java transport libraries from Maven Central on the first build), Node.js 22 or later (for `@geastack/cli`), and `adb` for the install,
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
  `GEA_ENGINE_DIR`, `GEA_ELEMENTS_DIR`: use these
  package copies instead of the ones installed in the app.

The build fails if the generated Java or native sources reference a WebView.

## Host services

Audio-only `navigator.mediaDevices.getUserMedia` uses Android's runtime microphone
permission and captures mono PCM with `AudioRecord`. Capture pauses while the
activity is in the background. The WebSocket client supports text frames and
the native host can queue binary audio frames using OkHttp. The HTTP server
adapts requests from NanoHTTPD to the app's Gea HTTP handler.

Android-specific C and C++ sources can be listed in `gea.targets.android.nativeSources`.
The target also loads an optional `android/geatsc-native-plugin.mjs` and compiles
optional `android/*.java` sources, replacing `@PACKAGE@` with the generated
Java package name. Set `gea.targets.android.screenOrientation` for apps that
require a specific activity orientation. Set `gea.targets.android.devicePixelRatio`
to choose how CSS pixels map to display pixels.

## License

Apache-2.0 (see `LICENSE`). You can ship closed-source products
built on it. The only GeaStack code under a different license is
the embedded board support (`targets` and `@geastack/chips`, GPL-3.0-only):
shipping closed-source firmware through those needs a commercial license.
Contact [contact@geastack.com](mailto:contact@geastack.com) for commercial terms, support and hosted builds.
