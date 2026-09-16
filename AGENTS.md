# Development Guide

This guide provides instructions for building and deploying the project.

## 1. Prerequisites

- **Android Studio** (or Android SDK & NDK)
- **Gradle**
- **ADB** (Android Debug Bridge)

## 2. Build Instructions

### Native Code & APK

Before building, use a clean release build:

```powershell
chcp 65001 > $null
$env:JAVA_HOME="C:\Program Files\Android\Android Studio\jbr"
.\gradlew clean assembleRelease
```

For local debug-only work, use:
```powershell
chcp 65001 > $null
$env:JAVA_HOME="C:\Program Files\Android\Android Studio\jbr"
.\gradlew assembleDebug
```

The release APK will be located at: `app\build\outputs\apk\release\app-release.apk`

The debug APK will be located at: `app\build\outputs\apk\debug\app-debug.apk`

## 3. GitHub Actions Release Build

The GitHub Actions workflow is manual-only. Run `Build APK` from the GitHub Actions tab.

The workflow:

- Builds the release APK with `./gradlew clean assembleRelease`
- Reads `versionName` from `app/build.gradle`
- Renames the APK to `HoshimiLocalify_{versionName}.apk`
- Creates a git tag using `versionName`
- Creates or updates the GitHub Release for that tag
- Uploads the APK as a Release asset

If the tag already exists on a different commit, the workflow fails instead of overwriting it.

## 4. Deployment & ADB Commands

### Install APK

```powershell
chcp 65001 > $null
.\platform-tools\adb.exe -s 127.0.0.1:16384 install -r "app\build\outputs\apk\release\app-release.apk"
```
or
```powershell
chcp 65001 > $null
.\platform-tools\adb.exe -s emulator-5556 install -r "app\build\outputs\apk\release\app-release.apk"
```

## Repository layout

This repository contains the Android implementation. iOS is maintained separately at `../hoshimi-localify-ios`. Use UTF-8 for PowerShell and text files. Do not modify the sibling iOS repository unless requested.
