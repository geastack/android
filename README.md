# Android target

Builds GeaStack web-rendered apps into a small native Android WebView APK.

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
- `GEA_ANDROID_DISABLE_WEBVIEW_MULTIPROCESS=0`: skip the default
  `cmd webviewupdate disable-multiprocess` device workaround.
- `GEA_ANDROID_WEBVIEW_LAYER_TYPE=software`: force a software WebView layer.
- `GEA_ANDROID_DEBUG_LOGS=1`: emit WebView lifecycle and DOM probes to logcat.

The packager inlines the Vite output into the APK and loads it as base64 HTML.
That keeps Gea apps independent of Android asset URL quirks on watch-style
boards such as the Lokmat APPLLP Max.

## License

Apache-2.0 (see `LICENSE`). You can ship closed-source products
built on it. The only GeaStack code under a different license is
the embedded board support (`targets` and `@geastack/chips`, GPL-3.0-only):
shipping closed-source firmware through those needs a commercial license.
Contact [contact@geastack.com](mailto:contact@geastack.com) for commercial terms, support and hosted builds.
