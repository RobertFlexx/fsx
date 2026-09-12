# FSX 1.0 write safety model

FSX treats storage mutation as a transaction and recovery problem, not merely an I/O problem.

The core invariants are:

```text
1. Never release the only authoritative copy before a verified replacement exists.
2. Never shorten a partition before the filesystem is already durably smaller and verified.
3. Never resume a journal against a different filesystem UUID, geometry, operation or target.
4. Never move an allocated range whose owner cannot be established safely.
5. Never treat fixed filesystem metadata as ordinary relocatable file data.
6. Never mutate a target known to be mounted, swap, held, or otherwise active.
7. Never advertise a mutated filesystem as clean before post-mutation verification succeeds.
8. Every irreversible transition must have an explicit recovery state.
9. Safety checks are part of the fast path; there is no unsafe-fast mode.
10. Ambiguous/unsupported metadata causes refusal, not best-effort mutation.
```

## Relocation ordering

For each supported extent relocation:

```text
relocation intent durable
reserve destination allocation
copy source -> destination
flush
reread destination
CRC32C verify destination
verified-copy record durable
switch exact owner metadata
verify owner now references destination
release source allocation
update checksum/counter metadata
checkpoint commit state
```

The old source is not released before the replacement exists, has been made durable, and has been reread and verified.

## Journal publication and recovery

A transaction journal contains two independent 4096-byte headers and CRC32C-protected records. Journal creation uses a unique temporary sibling. Both headers are initialized, the file is flushed, it is atomically renamed to the requested path, and the parent directory is fsynced.

Journal semantic validation checks transaction identity/geometry and state ordering. Recovery does not accept an impossible record sequence simply because individual CRCs happen to be valid.

Signals request cancellation. Long relocation operations stop accepting new work, finish a bounded active transaction batch, checkpoint, flush and leave a recoverable paused/staged state.

## Fault model exercised by the release tests

The block layer has dormant test-only fault hooks. The 1.0 release tests inject failures:

```text
before read
before write
after write
before flush
after flush
```

The journal matrix also forks child processes and terminates them with `_exit()` after selected durability points so C++ destructors do not run. The parent then reopens the journal and requires a structurally/semantically valid old or new state.

This models userspace/process loss and uncertain syscall completion. It is not a claim to emulate every disk firmware/cache failure possible under literal power removal. Hardware with volatile write caches still ultimately defines what its flush contract can guarantee.

## Filesystem size transitions

Supported ext shrink/grow mutation marks the filesystem dirty before changing size/allocation metadata. It stays dirty through verification and is only returned to clean state after the supported metadata checks pass.

The 1.0 shrink finalizer intentionally refuses a target that removes ext block groups and requires inode-number relocation. There is no override switch.

## Partition ordering

GPT mutation writes and flushes the backup side before replacing the primary side, then rereads and verifies both copies. GPT metadata backup sidecars are published with no-clobber semantics before they are considered usable, and restore rejects sidecars with unexpected trailing bytes.

Filesystem-to-partition synchronization requires the selected partition's current byte length to exactly match the filesystem block-device geometry. This rejects the dangerous partition-first truncation pattern.

MBR mutation requires an explicit sector-0 undo backup. The undo sector is durably published before sector 0 is modified, existing backup paths are never intentionally overwritten, restore requires exactly 512 bytes, and protective/hybrid GPT media are refused.

## Copy and imaging overlap

FSX refuses an image restore whose archive and destination are the same inode before any restore write. Raw clone refuses exact aliases and, on Linux, disk/partition ancestry and shared dm/md backing storage. Active block-device clone/image sources are refused by default; `--allow-live-source` is an explicit request to accept crash-consistency risk.

## Explicit write gates

Regular files are the preferred development/test targets. Real block-device mutation requires explicit `--write` and `--allow-block-device` where applicable.

FSX performs platform-specific inactivity checks. Linux checks mounts, swap, block holders, and active descendants. BSD-family systems use native mount inspection where implemented. If the current platform cannot establish a required safety invariant, destructive access fails closed.

## Known 1.0 mutation boundaries

The following are deliberate refusals, not hidden fallback behavior:

```text
ext block-group-removing shrink / inode-number relocation
ext meta_bg destructive mutation
ext bigalloc destructive mutation
incomplete legacy indirect-block owner mutation
ext grow requiring new block groups
FAT/exFAT mutation
UFS mutation
XFS mutation
NTFS mutation
Btrfs mutation
ISO9660 mutation
```

No storage software can promise impossibility of corruption in the face of failing media, broken firmware, kernel defects or violated hardware flush semantics. FSX's goal is to keep its own supported operations ordered, verified, recoverable and fail-closed.
