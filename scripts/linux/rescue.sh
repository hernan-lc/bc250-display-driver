#!/usr/bin/env bash
# Recovery for the test Windows when BC250Kmd hangs the boot (spinning circle, PC heats up).
#
#   rescue.sh disable    move every installed BC250Kmd.sys out of the DriverStore (fast, reversible)
#   rescue.sh enable     move them back
#   rescue.sh status     show which BC250Kmd.sys / services are in place
#   rescue.sh log        show the driver's step log (where did bring-up stop?) + guard state
#   rescue.sh guard-reset  delete 'disable'/'attempt' so the driver may try again
#   rescue.sh guard-off  create 'disable' (driver refuses to start next boot; no binary moved)
#   rescue.sh image      back up the Windows partition (ntfsclone, used blocks only, zstd) to ~/dev/images
#   rescue.sh restore F  restore an image made by 'image'  (DESTRUCTIVE: overwrites the Windows partition)
#   rescue.sh registry   run tools/repair-windows-boot.py (needs python-registry; see docs/BOOT-RECOVERY.md)
set -euo pipefail
cd "$(dirname "$0")/../.."
source scripts/linux/lib.sh
FR=$WIN_MNT/Windows/System32/DriverStore/FileRepository
BK=$WIN_MNT/BC250Kmd_backup
IMG=${IMG_DIR:-$HOME/dev/images}

case ${1:-} in
  disable)
    win_mount_rw; sudo mkdir -p "$BK"; n=0
    for d in "$FR"/bc250kmd.inf_amd64_*/; do
      [[ -f $d/BC250Kmd.sys ]] || continue
      p=$(basename "$d"); sudo mkdir -p "$BK/$p"; sudo mv "$d/BC250Kmd.sys" "$BK/$p/"; echo "disabled $p"; n=$((n+1))
    done
    echo "$n driver binaries moved to C:\\BC250Kmd_backup"; win_umount ;;
  enable)
    win_mount_rw
    for d in "$BK"/*/; do p=$(basename "$d"); [[ -d $FR/$p ]] && sudo mv "$d/BC250Kmd.sys" "$FR/$p/" && echo "restored $p"; done
    win_umount ;;
  status)
    win_mount_ro
    echo "--- installed:"; sudo find "$FR" -iname 'BC250Kmd.sys' -printf '%TY-%Tm-%Td %TH:%TM %s %p\n'
    echo "--- backed up:"; sudo find "$BK" -iname 'BC250Kmd.sys' -printf '%TY-%Tm-%Td %TH:%TM %s %p\n' 2>/dev/null || true
    echo "--- crash evidence:"; sudo ls -la "$WIN_MNT/Windows/LiveKernelReports/WATCHDOG" "$WIN_MNT/Windows/Minidump" "$WIN_MNT/Windows/MEMORY.DMP" "$WIN_MNT/bc250" 2>/dev/null | head -20 || true
    win_umount ;;
  log)
    win_mount_ro; G=$WIN_MNT/Windows/System32/drivers/bc250
    echo "--- guard files:"; sudo ls -la "$G" 2>/dev/null | grep -E -i "disable|attempt|bc250.log" || echo "(none)"
    [[ -f $G/disable ]] && { echo "--- disable says:"; sudo cat "$G/disable"; }
    echo "--- last 40 lines of bc250.log:"; sudo tail -n 40 "$G/bc250.log" 2>/dev/null || echo "(no log yet)"
    win_umount ;;
  guard-reset)
    win_mount_rw; G=$WIN_MNT/Windows/System32/drivers/bc250; sudo rm -f "$G/disable" "$G/attempt"; echo "guard reset"; win_umount ;;
  guard-off)
    win_mount_rw; G=$WIN_MNT/Windows/System32/drivers/bc250; sudo mkdir -p "$G"; echo "manual" | sudo tee "$G/disable" >/dev/null; echo "driver will refuse to start"; win_umount ;;
  image)
    command -v zstd >/dev/null && command -v ntfsclone >/dev/null || { echo "need zstd + ntfs-3g (ntfsclone)" >&2; exit 1; }
    mkdir -p "$IMG"; mountpoint -q "$WIN_MNT" && sudo umount "$WIN_MNT"
    out=$IMG/windows-$(date +%Y%m%d-%H%M%S).ntfsclone.zst
    sudo ntfsclone --save-image --output - "$WIN_DEV" | zstd -T0 -3 -o "$out"; ls -lh "$out" ;;
  restore)
    f=${2:?image file}; echo "This OVERWRITES $WIN_DEV with $f. Type YES:"; read -r a; [[ $a == YES ]] || exit 1
    mountpoint -q "$WIN_MNT" && sudo umount "$WIN_MNT"
    zstd -dc "$f" | sudo ntfsclone --restore-image --overwrite "$WIN_DEV" - ;;
  registry)
    win_mount_rw; python3 tools/repair-windows-boot.py "${@:2}"; win_umount ;;
  *) sed -n 2,10p "$0"; exit 2 ;;
esac
