#!/usr/bin/env bash
# Copies the Linux-built BC250Kmd.sys/.pdb + INF + firmware to the Windows test disk (HIKSEMI).
# Does NOT install anything: on Windows run  scripts\sign-linux-build.ps1  then  scripts\install-kmd-safe.ps1
#
# Usage: scripts/linux/deploy.sh [Release|Debug]
set -euo pipefail
cd "$(dirname "$0")/../.."
CFG=${1:-Release}
SYS=build/$CFG/kmd/BC250Kmd.sys
[[ -f $SYS ]] || { echo "Build first: scripts/linux/build.sh $CFG kmd" >&2; exit 1; }

source scripts/linux/lib.sh
win_mount_rw

DST=$WIN_MNT/src/bc250/out/linux-$CFG/kmd
sudo mkdir -p "$DST"
sudo cp -f "$SYS" "build/$CFG/kmd/BC250Kmd.pdb" "$DST/"
sudo cp -f package/bc250kmd.inf "$DST/BC250Kmd.inf"
# Keep the Windows-side checkout's scripts/firmware in sync with this tree (sources stay Linux-owned).
sudo rsync -rt --no-perms --no-owner --no-group scripts/ "$WIN_MNT/src/bc250/scripts/"
sudo rsync -rt --no-perms --no-owner --no-group firmware/ "$WIN_MNT/src/bc250/firmware/"
sudo sync
echo "Deployed to C:\\src\\bc250\\out\\linux-$CFG\\kmd"
sudo ls -l "$DST"
win_umount
