#!/bin/sh
set -eu
FSX=${FSX:-./build/fsx}
TMP=${TMPDIR:-/tmp}/fsx-integration.$$
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP"
./scripts/make-test-images.py "$TMP" >/dev/null

EXT="$TMP/ext4.img"
JNL="$TMP/stage.fsxj"

"$FSX" info "$EXT"
"$FSX" probe "$EXT"
"$FSX" capabilities "$EXT"
"$FSX" check "$EXT" --progress=none
"$FSX" check metadata "$EXT" --progress=none
"$FSX" check deep "$EXT" --progress=none
"$FSX" allocation "$EXT" --progress=none
"$FSX" owner-map "$EXT" 56MiB --progress=none
"$FSX" io-profile "$EXT"
"$FSX" plan resize "$EXT" 56MiB --progress=none
"$FSX" plan relocate "$EXT" 56MiB --progress=none
"$FSX" stage resize "$EXT" 56MiB --journal="$JNL" --write --progress=none
"$FSX" journal inspect "$JNL"
"$FSX" journal dump "$JNL" 8
"$FSX" journal verify "$JNL"
"$FSX" commit resize "$EXT" 56MiB --journal="$JNL" --write --progress=none
"$FSX" analyze shrink "$EXT" 56MiB --progress=none
"$FSX" finalize shrink "$EXT" 56MiB --journal="$JNL" --write --progress=none
"$FSX" check metadata "$EXT" --progress=none
"$FSX" grow "$EXT" 60MiB --write --progress=none
"$FSX" check deep "$EXT" --progress=none
"$FSX" tune label "$EXT" FSXINT --write
"$FSX" info "$EXT"

"$FSX" partition list "$TMP/gpt.img"
"$FSX" partition backup "$TMP/gpt.img" "$TMP/gpt.backup"
"$FSX" partition resize "$TMP/gpt.img" 1 2048 12287 --write --progress=none
"$FSX" partition list "$TMP/gpt.img"
"$FSX" partition restore "$TMP/gpt.img" "$TMP/gpt.backup" --write
"$FSX" partition list "$TMP/gpt.img"

"$FSX" mbr list "$TMP/mbr.img"
"$FSX" mbr resize "$TMP/mbr.img" 1 2048 4096 --backup-sector="$TMP/mbr.sector" --write
"$FSX" mbr restore "$TMP/mbr.img" --backup-sector="$TMP/mbr.sector" --write
"$FSX" mbr list "$TMP/mbr.img"

"$FSX" benchmark "$EXT" 8MiB --progress=none
"$FSX" scrub "$EXT" 8MiB --passes=2 --chunk=1MiB --progress=none
"$FSX" clone "$EXT" "$TMP/ext.clone" --create-file --write --chunk=1MiB --progress=none
cmp "$EXT" "$TMP/ext.clone"

"$FSX" image create "$EXT" "$TMP/ext.fsximg" --chunk=1MiB --progress=none
"$FSX" image info "$TMP/ext.fsximg"
"$FSX" image verify "$TMP/ext.fsximg" --progress=none
"$FSX" image restore "$TMP/ext.fsximg" "$TMP/ext.restore" --create-file --write --progress=none
cmp "$EXT" "$TMP/ext.restore"
"$FSX" probe "$TMP/ext.restore"

"$FSX" probe "$TMP/xfs.img"
"$FSX" info "$TMP/xfs.img"
"$FSX" check "$TMP/xfs.img" --progress=none
"$FSX" probe "$TMP/ntfs.img"
"$FSX" info "$TMP/ntfs.img"
"$FSX" check "$TMP/ntfs.img" --progress=none

"$FSX" probe "$TMP/btrfs.img"
"$FSX" info "$TMP/btrfs.img"
"$FSX" check "$TMP/btrfs.img" --progress=none
"$FSX" probe "$TMP/iso9660.img"
"$FSX" info "$TMP/iso9660.img"
"$FSX" check "$TMP/iso9660.img" --progress=none
"$FSX" probe "$TMP/ufs2.img"
"$FSX" info "$TMP/ufs2.img"
"$FSX" check "$TMP/ufs2.img" --progress=none
