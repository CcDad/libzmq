#!/usr/bin/env bash
#
#   Exit if any step fails
set -e

# Configuration
export NDK_VERSION="${NDK_VERSION:-android-ndk-r25}"
export ANDROID_NDK_ROOT="${ANDROID_NDK_ROOT:-/tmp/${NDK_VERSION}}"
export MIN_SDK_VERSION=${MIN_SDK_VERSION:-21}
export ANDROID_BUILD_DIR="${ANDROID_BUILD_DIR:-$(cd $(dirname "${BASH_SOURCE[0]}") ; pwd)}"

# Cleanup.
rm -rf /tmp/android_build/
rm -rf $(dirname "${BASH_SOURCE[0]}")/prefix
rm -rf /tmp/tmp-deps
mkdir -p /tmp/tmp-deps

case $(uname | tr '[:upper:]' '[:lower:]') in
  linux*)
    HOST_PLATFORM=linux
    ;;
  darwin*)
    HOST_PLATFORM=darwin
    ;;
  *)
    echo "Unsupported platform"
    exit 1
    ;;
esac

if [ ! -d "${ANDROID_NDK_ROOT}" ]; then
    export FILENAME=$NDK_VERSION-$HOST_PLATFORM.zip

    (cd '/tmp' \
        && wget http://dl.google.com/android/repository/$FILENAME -O $FILENAME &> /dev/null \
        && unzip -q $FILENAME) || exit 1
    unset FILENAME
fi

./build.sh "arm"
./build.sh "arm64"
./build.sh "x86"
./build.sh "x86_64"
