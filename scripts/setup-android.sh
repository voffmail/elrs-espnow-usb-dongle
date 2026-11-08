#!/usr/bin/env bash
set -euo pipefail

if command -v java >/dev/null 2>&1 && [ -n "${ANDROID_HOME:-${ANDROID_SDK_ROOT:-}}" ]; then
    echo "Android toolchain already available:"
    java -version 2>&1 | head -1
    echo "ANDROID_HOME=${ANDROID_HOME:-${ANDROID_SDK_ROOT:-}}"
    exit 0
fi

cat <<'MSG'
JDK 21 and Android SDK 35 are required.
Set JAVA_HOME and ANDROID_HOME (or ANDROID_SDK_ROOT), then run:
  cd android-stub
  ./gradlew :app:assembleDebug
MSG
exit 1
