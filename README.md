# Android target

Builds GeaStack apps into native Android APKs. geatsc compiles the app to C++,
the NDK builds it into a native library, and each component node is rendered as
a real Android view (`FrameLayout`, `TextView`, `ImageView`, `ScrollView`, ...)
through JNI. There is no WebView and no JavaScript engine on the device.

```sh
targets/android/build-android.sh css-3d-cube debug
targets/android/build-android.sh css-3d-cube device
```

`debug` builds and signs the APK under `targets/android/dist/<app-id>/`.
`device` also installs and launches it through `adb`.

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

The build fails if the generated Java or native sources reference a WebView.

## License

Apache-2.0 (see `LICENSE`). You can ship closed-source products
built on it. The only GeaStack code under a different license is
the embedded board support (`targets` and `@geastack/chips`, GPL-3.0-only):
shipping closed-source firmware through those needs a commercial license.
Contact [contact@geastack.com](mailto:contact@geastack.com) for commercial terms, support and hosted builds.
