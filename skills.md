# Development Guide

This guide provides instructions for building and deploying project.

## 1. Prerequisites

- **Android Studio** (or Android SDK & NDK)
- **Gradle**
- **ADB** (Android Debug Bridge)

## 2. Build Instructions

### Native Code & APK

Before build, you must clean build using:

**gradlew **clean** build**

To build the debug APK from the root directory:

```powershell
$env:JAVA_HOME="C:\Program Files\Android\Android Studio\jbr"
.\gradlew assembleDebug
```

The APK will be located at: `app\build\outputs\apk\debug\app-debug.apk`

## 3. Deployment & ADB Commands

### Install APK

```powershell
.\platform-tools\adb.exe -s 127.0.0.1:16384 install -r "app\build\outputs\apk\debug\app-debug.apk"
```
