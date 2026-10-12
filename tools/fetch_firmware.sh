#!/bin/sh
# Fetches the Xbox Wireless Adapter firmware from Microsoft and copies it to
# the console over FTP.
#
# The firmware is Microsoft's and may not be redistributed, so it is never part
# of this repository: each user downloads it from Windows Update for their own
# adapter. Sources and checksums are the ones dlundqvist/xone's
# install/firmware.sh uses.
#
#   tools/fetch_firmware.sh <adapter-pid> <console-ip> [ftp-port]
#   tools/fetch_firmware.sh 02e6 192.168.0.20 2121
#
# Needs curl, bsdtar (built into macOS as tar) and shasum.
set -eu

USAGE="usage: $0 <02e6|02fe|02f9|091e> <console-ip> [ftp-port]"
PID=$(echo "${1:?$USAGE}" | tr 'A-Z' 'a-z')
HOST=${2:?$USAGE}
PORT=${3:-2121}

BASE=https://catalog.s.download.windowsupdate.com
case "$PID" in
  02e6) URL=$BASE/d/msdownload/update/driver/drvs/2017/03/2ea9591b-f751-442c-80ce-8f4692cdc67b_6b555a3a288153cf04aec6e03cba360afe2fce34.cab
        FILE=FW_ACC_00U.bin
        SHA=080ce4091e53a4ef3e5fe29939f51fd91f46d6a88be6d67eb6e99a5723b3a223 ;;
  02fe) URL=$BASE/c/msdownload/update/driver/drvs/2017/07/1cd6a87c-623f-4407-a52d-c31be49e925c_e19f60808bdcbfbd3c3df6be3e71ffc52e43261e.cab
        FILE=FW_ACC_00U.bin
        SHA=48084d9fa53b9bb04358f3bb127b7495dc8f7bb0b3ca1437bd24ef2b6eabdf66 ;;
  02f9) URL=$BASE/c/msdownload/update/driver/drvs/2017/06/1dbd7cb4-53bc-4857-a5b0-5955c8acaf71_9081931e7d664429a93ffda0db41b7545b7ac257.cab
        FILE=FW_ACC_CL.bin
        SHA=0023a7bae02974834500c665a281e25b1ba52c9226c84989f9084fa5ce591d9b ;;
  091e) URL=$BASE/d/msdownload/update/driver/drvs/2017/08/aeff215c-3bc4-4d36-a3ea-e14bfa8fa9d2_e58550c4f74a27e51e5cb6868b10ff633fa77164.cab
        FILE=FW_ACC_BR.bin
        SHA=e2710daf81e7b36d35985348f68a81d18bc537a2b0c508ffdfde6ac3eae1bad7 ;;
  *) echo "unknown adapter $PID (expected 02e6, 02fe, 02f9 or 091e)" >&2; exit 1 ;;
esac

# The download is untrusted until its checksum matches, so it goes in a fresh
# directory of its own and only the one expected file is extracted.
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

echo "Downloading the driver package for adapter $PID..."
curl -fsSL -o "$WORK/driver.cab" "$URL"

tar -xf "$WORK/driver.cab" -C "$WORK" "$FILE"

GOT=$(shasum -a 256 "$WORK/$FILE" | cut -d' ' -f1)
if [ "$GOT" != "$SHA" ]; then
  echo "checksum mismatch: got $GOT, expected $SHA -- not uploading" >&2
  exit 1
fi
echo "Checksum OK ($(wc -c < "$WORK/$FILE" | tr -d ' ') bytes)."

DEST=/data/fgg-xsense/firmware/dongle_$PID.bin
curl -fsS --ftp-create-dirs -T "$WORK/$FILE" "ftp://$HOST:$PORT$DEST"
echo "Uploaded to $DEST on $HOST."
