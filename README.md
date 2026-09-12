# fsx

FSX is a storage toolkit for Unix-like systems. It is written in C++20 and ships as one command instead of a loose collection of filesystem, partition, copy, and recovery utilities.

I started it because disk work is easy to fuck up, and the older tools really suck. A normal resize can involve one tool for the filesystem, another for the partition table, a third for checking the result, and a lot of manual bookkeeping in between. FSX tries to make that whole job less fragile.

It is not a wrapper around the usual tools. For the operations it implements, FSX reads the on-disk formats itself and performs the work itself. It does not shell out to `e2fsck`, `resize2fs`, `tune2fs`, `blkid`, `fdisk`, `cfdisk`, `parted`, `dd`, or `rsync`, and the ext backend does not depend on `libext2fs`.

The basic rule is simple:

```text
If FSX cannot prove a write is safe, it should refuse the write.
```

That means there are cases where FSX will tell you "no" instead of trying something clever. For a storage tool, that is intentional.

## What it does

FSX covers four main jobs:

- inspect and check filesystems
- maintain supported ext2/3/4 filesystems
- inspect, repair, back up, and update GPT/MBR partition tables
- clone, scrub, image, verify, and restore block devices and files

The strongest write support is currently ext2/3/4 plus GPT and MBR. FAT, exFAT, XFS, NTFS, Btrfs, ISO9660, and UFS have read-only probing and validation support to different depths.

FSX also includes its own transaction journal, recovery logic, checksumming, sparse image format, block-device safety checks, and live progress reporting.

## A quick look

> replace the drive with what drive you wanna use it on.

Probe a filesystem:

```sh
fsx probe /dev/nvme0n1p1
```

Get a useful summary:

```sh
fsx info /dev/nvme0n1p1
```

Run a normal check:

```sh
fsx check /dev/nvme0n1p1
```

Run the deeper ext checker:

```sh
fsx check deep /dev/nvme0n1p1
```

See what a resize would actually have to move before changing anything:

```sh
fsx plan resize /dev/nvme0n1p1 790GiB
fsx plan relocate /dev/nvme0n1p1 790GiB
```

Inspect a disk's partition table:

```sh
fsx partition list /dev/nvme0n1
```

Scrub a device twice and make sure both passes agree:

```sh
fsx scrub /dev/nvme0n1p1 --passes=2
```

Create and verify an FSX image:

```sh
fsx image create /dev/nvme0n1p1 backup.fsximg
fsx image verify backup.fsximg
```

For the complete command list, use:

```sh
fsx --help
```

## Progress output

Long operations use a small live ASCII display when stdout is a terminal. It updates in place instead of dumping thousands of progress lines into scrollback.

A resize can look roughly like this:

```text
phase:        relocating data
progress:     11.84 / 18.27 GiB  64.8%
rate:         1.37 GiB/s
read:         12.21 GiB
written:      11.84 GiB
verified:     11.84 GiB
extents:      1842 / 2719
elapsed:      00:00:13
eta:          00:00:05
status:       running
```

Redirect output to a file or pipe and FSX automatically switches to normal line-oriented output.

Progress modes are:

```text
auto
live
plain
none
```

## Filesystem support

```text
Filesystem   Probe/info   Check                         Write/repair                  Resize
-----------  -----------  ----------------------------  ----------------------------  -----------------------
ext2/3/4     yes          normal + deep                 counters, label, mutation    conditional grow/shrink
FAT12/16/32  yes          allocation + FAT copies       no                            no
exFAT        yes          boot regions + allocation     no                            no
XFS          yes          geometry, AG copies, v5 CRC   no                            no
NTFS         yes          boot + MFT/MFTMirr fixups     no                            no
Btrfs        yes          mirrors + super checksums     no                            no
ISO9660      yes          descriptors + root geometry   no                            no
UFS1/UFS2    yes          superblock geometry           no                            no
```

You can ask the binary directly instead of relying on this table:

```sh
fsx capabilities /dev/nvme0n1p1
```

### ext resize limits in 1.0

FSX can relocate owned extent-format data, verify staged copies, update the supported ext metadata/checksums, and complete a shrink when the new end stays inside the existing final block group. It can also grow inside the existing final block group.

FSX 1.0 refuses a shrink that removes whole ext block groups. Doing that correctly can require inode-number reassignment and updates to directory entries, hard links, xattrs, and other feature-dependent references. FSX does not guess its way through that job.

It also refuses destructive layouts it does not fully understand, including unsupported `meta_bg` cases, `bigalloc` mutation, incomplete legacy indirect-block ownership, and grow operations that require entirely new block groups.

## Safety model

FSX treats storage mutation as a recovery problem first and an I/O problem second.

For a supported data relocation, the rough order is:

```text
record intent
reserve destination
copy data
flush
reread destination
verify CRC32C
record verified copy
switch the exact owner metadata
verify the switch
release the old allocation
update counters and checksums
checkpoint recovery state
```

The old copy is not intentionally released until the replacement has been written, flushed, reread, and verified.

The transaction journal has redundant headers and checksummed records. Journal publication uses no-clobber semantics so an existing journal is not silently replaced. Cancellation is bounded: signals ask the operation to stop at a safe checkpoint instead of killing the write path in the middle of a transaction.

There is no `--unsafe-fast` mode.

### Real block devices require explicit intent

Destructive commands on real block devices normally require both:

```text
--write
--allow-block-device
```

Before writing, FSX tries to prove that the target is inactive. On Linux this includes mounted filesystems, swap, block holders, and active child partitions where relevant. Platform adapters use the native checks available on other Unix systems.

If an essential safety check cannot be made on the current platform, the write path fails closed.

### Filesystem first, partition second

FSX will not intentionally shorten the partition around a still-larger filesystem.

The integrated sequence is:

```text
filesystem work
flush
filesystem verification
clean-state transition
partition intent
partition-table update
partition-table reread and verification
journal completion
```

This is also why `fsx partition sync` checks the filesystem and partition geometry before it will make a containing GPT partition smaller.

## GPT and MBR

GPT support validates the primary and backup headers, header CRCs, entry-array CRCs, protective MBR, geometry, and agreement between both GPT copies.

Writes are backup-first:

```text
write backup entry array
write backup header
flush
write primary entry array
write primary header
flush
reread and verify both copies
```

FSX can repair one valid GPT copy from the other and can create a checksummed metadata backup containing the protective MBR, both GPT headers, and both entry arrays.

MBR support is intentionally simpler and stricter. A write requires an explicit sector-0 backup, that backup is made durable before sector 0 changes, and protective or hybrid GPT disks are refused by the MBR write path.

Common partition commands:

```sh
fsx partition list DISK
fsx partition backup DISK FILE
fsx partition restore DISK FILE --write --allow-block-device
fsx partition repair DISK --write --allow-block-device
fsx partition resize DISK INDEX FIRST_LBA LAST_LBA --write --allow-block-device

fsx mbr list DISK
fsx mbr resize DISK INDEX FIRST_LBA SECTORS --backup-sector=FILE --write --allow-block-device
fsx mbr restore DISK --backup-sector=FILE --write --allow-block-device
```

## Clone, scrub, and imaging

### scrub

`fsx scrub` reads the requested range with bounded parallel I/O and calculates a deterministic CRC32C. With more than one pass, it also checks that the data remains stable between passes.

```sh
fsx scrub DEVICE --passes=2
```

### clone

`fsx clone` copies with bounded parallel positional I/O, flushes the destination, then performs a full read-back verification by default.

```sh
fsx clone SOURCE TARGET --write --progress=live
```

FSX checks source/destination overlap before starting. On Linux that includes ordinary disk/partition ancestry and shared dm/md backing storage. Active block-device sources are refused unless you explicitly accept crash-consistency risk with `--allow-live-source`.

### FSXIMG1

FSXIMG1 is the project's sparse rescue and migration image format. It uses:

```text
4096-byte checksummed header
ordered checksummed chunk records
CRC32C for stored payloads
zero-chunk elision
checksummed completion footer
strict contiguous chunk accounting
```

Create an image:

```sh
fsx image create SOURCE backup.fsximg
```

Inspect or verify it:

```sh
fsx image info backup.fsximg
fsx image verify backup.fsximg
```

Restore it:

```sh
fsx image restore backup.fsximg TARGET --write --allow-block-device
```

Image creation uses a temporary sibling and only publishes the finished file after it has been completed and flushed. Restore verifies the image before each write, flushes the destination, then rereads the complete restored range and verifies it again.

## Performance

FSX is meant to make the safe path fast. It does not skip flushes or verification to make a benchmark look better.

A few of the things the engine does to avoid unnecessary work:

```text
reuse allocation and owner scans across planning stages
keep ext block-group scanning bounded instead of queuing the whole filesystem
store allocated and free space as compressed extent runs
prefer large contiguous relocation targets
use bounded worker queues and large copy buffers
run independent positional I/O in parallel
adapt I/O settings for HDDs, SSDs, and NVMe devices
batch transaction records where durability ordering allows it
use SSE4.2 CRC32C on supported x86/x86-64 CPUs
use a portable slicing-by-8 CRC path elsewhere
stream FAT validation instead of loading huge FATs into memory
use LTO/IPO in supported release builds
```

For shrink operations, the useful number is not just "old size minus new size". What matters is how much allocated data and metadata actually live past the requested new end. FSX plans around that workload and reports it before mutation.

`fsx benchmark` is included so performance claims can be measured on the machine and device that actually matter:

```sh
fsx benchmark /dev/nvme0n1p1 4GiB
```

## Building

FSX needs a C++20 compiler and CMake.

A normal release build:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
FSX=./build/fsx ./scripts/integration.sh
```

There is also a small Makefile wrapper:

```sh
make
make test
```

The release archive includes Linux x86-64 dynamic and fully static binaries in `bin/`.

### Sanitizer build

```sh
cmake -S . -B build-asan \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DFSX_ENABLE_ASAN=ON \
  -DFSX_ENABLE_LTO=OFF

cmake --build build-asan -j
ctest --test-dir build-asan --output-on-failure
FSX=./build-asan/fsx ./scripts/integration.sh
```

### Source formatting

The repository includes `.clang-format` plus two small helper scripts:

```sh
./scripts/format.sh
./scripts/check-style.py
```

The committed C/C++ tree is kept ASCII-only, tab-free, free of trailing whitespace, and within the project's line-length limit.

## Release gate

`make release-gate` runs the local release matrix. It covers style, warning-as-error GCC and Clang builds, sanitizers, ThreadSanitizer, `_GLIBCXX_ASSERTIONS`, the static build, tests, and integration.

The 1.0 validation work also includes fault injection around block I/O and flushes, process-death tests around transaction durability boundaries, malformed filesystem/image inputs, partition restore corruption tests, and end-to-end mutation fixtures.

For the exact release record, see:

```text
docs/VALIDATION.md
docs/SAFETY.md
docs/RELEASE_NOTES.md
```

## Portability

The filesystem engine is deliberately kept away from Linux-only APIs. Platform-specific device and safety checks live behind a small platform layer.

There are adapters for:

```text
Linux
FreeBSD
DragonFly BSD
OpenBSD
NetBSD
macOS
```

The generic I/O path is POSIX-style, which also keeps illumos support practical.

The Linux x86-64 build is the platform covered by the full 1.0 release matrix. The other Unix adapters are present in the source, but they should not be described as production-verified until they have native CI and hardware testing of their own.

## Project layout

```text
src/        implementation
include/    public/internal headers
tests/      unit, regression, corruption, and fault tests
scripts/    formatting, integration, and release helpers
docs/       safety, validation, format, portability, and release notes
bin/        release binaries
```

## A note on scope

the current version of fsx is not pretending that thirty years of filesystem tooling disappeared overnight.
the point of this project is to see how safe and how fast file system utilities can be. dont trust this as a replacement for other tools, as it is currently experimental. its not always guaranteed to break, just treat it with caution.

There is plenty left to do, especially deeper writable support for additional filesystems and the harder ext shrink cases. Those can be added without weakening the rules that make the current write paths trustworthy.

## License

See `LICENSE`.

### Notice

> the scripts are agentically created, under my supervision. they act as a sort of test utilities. (before you scream "ai slop".)
