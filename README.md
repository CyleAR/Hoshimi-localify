# Hoshimi Localify

Android Xposed module and translation manager for IDOLY PRIDE.

## Builds

Run from this repository with a configured JDK and Android SDK. In PowerShell:

| Build | Command | Bundled translation data |
| --- | --- | --- |
| Full | `.\gradlew clean assembleRelease` | Included |
| Lite | `.\gradlew clean assembleRelease -Plite=true` | Omitted |

Both write to `app/build/outputs/apk/release/app-release.apk`. Save Full elsewhere before building Lite. UI, settings and package name are shared; switching builds replaces the installed management app.

## Releases

- `Build APK` runs on pushes to `main` or manual dispatch.
- Filenames use `versionName` from `app/build.gradle`, including its `v` prefix:
  - `HoshimiLocalify_{versionName}_full.apk`
  - `HoshimiLocalify_{versionName}_lite.apk`
- This repository publishes both suffixed builds. `ipr-patcher` embeds Full, then distributes Lite as `HoshimiLocalify_{versionName}.apk` alongside the patched games.
- Publish the Full/Lite pair here before running the patcher. Both inputs must have the same version and use the `_full` / `_lite` suffixes; only the patcher's distributed management APK omits the suffix.
- Re-running the same version updates its tag and replaces release assets.

## Credits

- [Chinosk6 / gakuen-imas-localify](https://github.com/chinosk6/gakuen-imas-localify) — original code.
- Vilebbit: [HoshimiToolkit](https://github.com/Vilebbit/HoshimiToolkit), [SolisClient](https://github.com/Vilebbit/SolisClient).
