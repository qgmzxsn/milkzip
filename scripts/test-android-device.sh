#!/bin/sh
# Run on the Linux CI emulator. Retain diagnostics before Gradle/emulator cleanup.
set -u
root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
artifacts="$root/android-artifacts"
mkdir -p "$artifacts"
timeout 15 adb logcat -c || true
adb logcat -v threadtime TestRunner:I AndroidRuntime:E '*:S' > "$artifacts/test-events.txt" &
logger=$!
trap 'kill "$logger" 2>/dev/null || true' EXIT
cd "$root/apps/android" || exit 1
# The AOSP Quickstep launcher can ANR during first-boot task transitions on API 35/36.
# Use a minimal home from the separate test APK, only on an emulator. App failures stay visible.
if [ "$(adb shell getprop ro.kernel.qemu | tr -d '\r')" = "1" ]; then
  timeout 30 adb install -r -t app/build/outputs/apk/debug/app-debug.apk || exit 1
  timeout 30 adb install -r -t app/build/outputs/apk/androidTest/debug/app-debug-androidTest.apk || exit 1
  timeout 15 adb shell cmd package set-home-activity app.localarchiver.test/app.localarchiver.TestHomeActivity || exit 1
  timeout 15 adb shell am force-stop com.android.launcher3 || exit 1
  timeout 15 adb shell am start -n app.localarchiver.test/app.localarchiver.TestHomeActivity || exit 1
fi
timeout --kill-after=15s 300 ./gradlew connectedDebugAndroidTest --stacktrace
result=$?
timeout 15 adb pull /sdcard/Download/MilkZipScreenshots "$artifacts/" || true
timeout 15 adb logcat -d > "$artifacts/logcat.txt" || true
if [ "$result" != 0 ]; then
  timeout 15 adb shell dumpsys window > "$artifacts/window.txt" || true
  timeout 15 adb exec-out screencap -p > "$artifacts/failure.png" || true
fi
exit "$result"
