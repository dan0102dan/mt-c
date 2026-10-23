#!/bin/sh
# Native test libraries from the OpenWrt 24.10.0 source revisions. Firmware
# builds always use the target SDK's own libraries, never these host objects.
set -eu
prefix=${1:?usage: openwrt-host-libs.sh ABSOLUTE_PREFIX}
case "$prefix" in /*) ;; *) echo 'prefix must be absolute' >&2; exit 1 ;; esac
mkdir -p "$prefix/src"
fetch() {
    name=$1 revision=$2
    if [ ! -d "$prefix/src/$name/.git" ]; then
        git init -q "$prefix/src/$name"
        git -C "$prefix/src/$name" remote add origin "https://github.com/openwrt/$name.git"
    fi
    git -C "$prefix/src/$name" fetch --depth=1 origin "$revision"
    git -C "$prefix/src/$name" checkout --detach FETCH_HEAD
}
fetch libubox eb9bcb64185ac155c02cc1a604692c4b00368324
fetch ubus afa57cce0aff82f4a7a0e509d4387ebc23dd3be7
for project in libubox ubus; do
    cmake -S "$prefix/src/$project" -B "$prefix/src/$project/build" \
        -DBUILD_LUA=OFF -DBUILD_EXAMPLES=OFF \
        -DCMAKE_PREFIX_PATH="$prefix" -DCMAKE_INSTALL_PREFIX="$prefix"
    cmake --build "$prefix/src/$project/build" --parallel 2
    cmake --install "$prefix/src/$project/build"
done
