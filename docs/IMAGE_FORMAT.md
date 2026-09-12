# FSXIMG1 image format

FSXIMG1 is the FSX 1.0 rescue/migration container. It is intentionally simple, streaming, bounded-memory, and independently checkable.

All integer fields are little-endian. UI and diagnostics remain ASCII-only.

## Header

The image begins with a 4096-byte header containing the magic `FSXIMG1`, format version, original byte size, original logical sector size, chunk size, chunk count, completion flags, and CRC32C of the full header with the checksum field zeroed.

A header without the completion bit is never accepted by restore or verify.

## Chunks

Each logical source chunk is preceded by a fixed 48-byte `FSXCHN1` record. The record contains its index, exact source offset, logical length, stored length, flags, payload CRC32C, and record CRC32C.

Normal chunks store their bytes verbatim. A chunk that is entirely zero may use the zero flag and store no payload. No compression library is required.

Chunk indices and source offsets must be exactly contiguous. Unknown chunk flags, inconsistent lengths, out-of-order records, or checksum mismatches are hard errors.

## Footer

A 4096-byte `FSXEND1` footer records the final chunk count, data/zero chunk counts, stored payload total, and its own CRC32C. Verify rejects accounting mismatches and trailing bytes.

## Durability

Creation writes to a unique temporary sibling, writes an incomplete header, streams all records and data, writes the footer, rewrites the completed header, fsyncs the file, renames it to the requested final path, and fsyncs the parent directory.

Restore verifies each stored payload before writing. After all writes, it flushes the target and rereads the complete restored range, checking every chunk again. A successful restore therefore means the target was verified after the durability flush, not merely that write() returned success.
