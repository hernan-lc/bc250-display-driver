# shared helpers for scripts/linux/*.sh   (source this file)
# The test Windows lives on the HIKSEMI S1000 USB SSD. Devices are resolved by-id so they survive sda/sdb renaming.
WIN_ID=${WIN_ID:-usb-HIKSEMI_S1000_AA000000000000000878-0:0}
WIN_DEV=/dev/disk/by-id/${WIN_ID}-part3     # WINDOWS (NTFS)
ESP_DEV=/dev/disk/by-id/${WIN_ID}-part1     # SYSTEM  (EFI)
WIN_MNT=${WIN_MNT:-/mnt/hik}

win_mount_rw() {
  [[ -e $WIN_DEV ]] || { echo "HIKSEMI disk not connected ($WIN_DEV)" >&2; return 1; }
  sudo mkdir -p "$WIN_MNT"
  mountpoint -q "$WIN_MNT" && sudo umount "$WIN_MNT"
  # A Windows crash leaves the NTFS 'dirty'; this only clears the flag (no hibernation => safe).
  sudo ntfs-3g "$WIN_DEV" "$WIN_MNT" -o rw 2>/dev/null \
    || { sudo ntfsfix -d "$WIN_DEV" && sudo ntfs-3g "$WIN_DEV" "$WIN_MNT" -o rw; }
}
win_mount_ro() { sudo mkdir -p "$WIN_MNT"; mountpoint -q "$WIN_MNT" && sudo umount "$WIN_MNT"; sudo mount -t ntfs-3g -o ro "$WIN_DEV" "$WIN_MNT"; }
win_umount()   { sudo sync; mountpoint -q "$WIN_MNT" && sudo umount "$WIN_MNT"; echo "unmounted $WIN_MNT"; }
