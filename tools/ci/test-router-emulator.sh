#!/usr/bin/env bash
# End-to-end check of the actual OpenWrt .ipk built by the SDK job.
# Runs real OpenWrt 24.10.4 on a virtual MT7981-based Netis NX31.
set -euo pipefail

if [ "$#" -ne 1 ]; then
  echo "Usage: $0 /path/to/mt-c_openwrt-24.10.4_aarch64_cortex-a53.ipk" >&2
  exit 2
fi

repo_root=$(cd "$(dirname "$0")/../.." && pwd)
package=$(realpath "$1")
if [ ! -s "$package" ] || [[ "$package" != *.ipk ]]; then
  echo "Missing OpenWrt .ipk package: $package" >&2
  exit 2
fi

# v3.7.1 is presets-only: the newest x86_64 binary is still v3.7.
# These digests come from the published GitHub Release assets.
emulator_version=3.7
presets_version=3.7.1
emulator_sha256=32367c4a1e5aabffd20152d3336a949075be16af6a7d2b0a0ee98cae02a7b053
presets_sha256=de4a4bc24addf3ed3256f55200342b70eaedf016ea6329052b1b79f25650a497
openwrt_version=24.10.4

logs="$repo_root/.build/router-emulator-test/logs"
mkdir -p "$logs"
scratch=$(mktemp -d)
http_pid=
cleanup() {
  if [ -n "$http_pid" ]; then
    kill "$http_pid" 2>/dev/null || true
    wait "$http_pid" 2>/dev/null || true
  fi
  rm -rf "$scratch"
}
trap cleanup EXIT

fetch_asset() {
  local version=$1 filename=$2 digest=$3
  local url="https://github.com/stavultras/router-emulator/releases/download/v$version/$filename"
  curl --fail --location --show-error --silent --retry 4 --retry-all-errors \
    --retry-delay 3 --connect-timeout 30 --max-time 180 "$url" -o "$scratch/$filename"
  printf '%s  %s\n' "$digest" "$scratch/$filename" | sha256sum --check --status
  echo "Verified: $filename"
}

binary_archive="Router-Emulator-$emulator_version-linux-x86_64.tar.gz"
presets_archive="Router-Emulator-$presets_version-presets.zip"
fetch_asset "$emulator_version" "$binary_archive" "$emulator_sha256"
fetch_asset "$presets_version" "$presets_archive" "$presets_sha256"
tar -xzf "$scratch/$binary_archive" -C "$scratch"
emulator="$scratch/Router-Emulator"
unzip -q "$scratch/$presets_archive" -d "$scratch"

# Netis NX31 (unlike Cudy WR3000P) has official OpenWrt 24.10.4 images.
# --no-bdinfo matches the preset flash layout and FIP offset.
(
  cd "$emulator"
  bash tools/prepare-nand.sh --no-bdinfo netis_nx31 "$openwrt_version"
) 2>&1 | tee "$logs/prepare-nand.log"

mkdir -p "$scratch/http"
cp "$package" "$scratch/http/mt-c.ipk"
# QEMU user networking exposes the host loopback as 10.0.2.2 to the guest.
python3 -m http.server 18080 --bind 127.0.0.1 --directory "$scratch/http" \
  > "$logs/http-server.log" 2>&1 &
http_pid=$!
sleep 1
kill -0 "$http_pid"

# quick.py keeps a serial transcript, gates each step on its *actual* output,
# and always terminates QEMU. Markers must occur at the start of a line:
# matching the echoed shell command would otherwise produce false positives.
cd "$emulator"
mkdir -p work
python3 tests/quick.py --limit 1800 --log "$logs/serial.log" \
  -P netis-nx31 --qemu='-netdev user,id=wan' \
  'BL2: @120' 'U-Boot 20@240' 'Please press Enter@600' \
  '>' 'root@OpenWrt@90' \
  ">wget -q -O /tmp/mt-c.ipk http://10.0.2.2:18080/mt-c.ipk && printf '\nMT_C_DOWNLOAD_OK\n'" \
  '(?m)^MT_C_DOWNLOAD_OK\r?$@120' \
  ">opkg update && printf '\nMT_C_FEEDS_OK\n'" \
  '(?m)^MT_C_FEEDS_OK\r?$@300' \
  ">opkg install /tmp/mt-c.ipk && opkg status mt-c | grep -q 'Status: install ok installed' && test -x /usr/bin/magitrickled && printf '\nMT_C_INSTALLED_OK\n'" \
  '(?m)^MT_C_INSTALLED_OK\r?$@300' \
  ">sed -i 's/- br0/- br-lan/' /etc/magitrickle/state/config.yaml && /etc/init.d/magitrickle restart && sleep 6 && pidof magitrickled && printf '\nMT_C_RUNNING_OK\n'" \
  '(?m)^MT_C_RUNNING_OK\r?$@120' \
  ">wget -q -O /tmp/mt-web.html http://127.0.0.1:8080/ && grep -qi '<html' /tmp/mt-web.html && wget -q -O /tmp/mt-api.json http://127.0.0.1:8080/api/v1/groups && grep -q groups /tmp/mt-api.json && printf '\nMT_C_HTTP_OK\n'" \
  '(?m)^MT_C_HTTP_OK\r?$@120' \
  ">/etc/init.d/magitrickle stop && sleep 3 && ! pidof magitrickled && printf '\nMT_C_STOPPED_OK\n'" \
  '(?m)^MT_C_STOPPED_OK\r?$@120' \
  ">opkg remove mt-c && test ! -e /usr/bin/magitrickled && printf '\nMT_C_REMOVED_OK\n'" \
  '(?m)^MT_C_REMOVED_OK\r?$@120' \
  2>&1 | tee "$logs/test-steps.log"
