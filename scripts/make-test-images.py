#!/usr/bin/env python3
import binascii
import hashlib
import os
import struct
import sys

out = sys.argv[1] if len(sys.argv) > 1 else "/tmp/fsx-test"
os.makedirs(out, exist_ok=True)

def crc32c_raw(data, seed=0xffffffff):
    poly=0x82f63b78
    c=seed
    for x in data:
        c ^= x
        for _ in range(8):
            c = (c >> 1) ^ (poly if (c & 1) else 0)
    return c & 0xffffffff

# Minimal ext4-shaped image sufficient for fsx's standalone structural scanner.
ext = os.path.join(out, "ext4.img")
size = 64 * 1024 * 1024
bs = 4096
blocks = size // bs
buf = bytearray(size)
sb = memoryview(buf)[1024:2048]
def p16(off,v): struct.pack_into('<H', sb, off, v)
def p32(off,v): struct.pack_into('<I', sb, off, v)
p32(0x00, 4096)            # inodes
p32(0x04, blocks)          # blocks lo
p32(0x08, 256)             # reserved blocks
p32(0x0c, blocks - 360)    # exact free blocks: 260 metadata + 100 data
p32(0x10, 4095)
p32(0x14, 0)               # first data block for 4K
p32(0x18, 2)               # 1024 << 2 = 4096
p32(0x20, 32768)           # blocks/group
p32(0x28, 4096)            # inodes/group
p16(0x38, 0xEF53)
p16(0x3a, 1)               # clean
p16(0x3c, 1)
p32(0x4c, 1)
p32(0x54, 11)
p16(0x58, 256)
p32(0x5c, 0)
p32(0x60, 0x40)            # extents incompat feature
p32(0x64, 0)
sb[0x68:0x78] = bytes.fromhex('00112233445566778899aabbccddeeff')
sb[0x78:0x78+8] = b'FSXTEST\x00'
p16(0xfe, 32)
# Group descriptor table at block 1.
gd = memoryview(buf)[bs:bs+32]
struct.pack_into('<I', gd, 0, 2) # block bitmap
struct.pack_into('<I', gd, 4, 3) # inode bitmap
struct.pack_into('<I', gd, 8, 4) # inode table
struct.pack_into('<H', gd, 12, blocks-360)
struct.pack_into('<H', gd, 14, 4095)
struct.pack_into('<H', gd, 16, 1)
# Block bitmap at block 2. Mark metadata/low data and one tail region allocated.
bm = memoryview(buf)[2*bs:3*bs]
def setbit(n): bm[n//8] = bytes([bm[n//8] | (1 << (n%8))])[0]
for i in range(0, 260): setbit(i)
for i in range(14500, 14600): setbit(i)

# One allocated extent-format inode whose data lives in the tail region.
ib = memoryview(buf)[3*bs:4*bs]
ib[0] = 0x02  # inode 2 (bit 1)
inode2_off = 4*bs + 256  # inode index 1, inode size 256
inode2 = memoryview(buf)[inode2_off:inode2_off+256]
struct.pack_into('<H', inode2, 0, 0x81A4)  # regular file 0644
struct.pack_into('<I', inode2, 4, 100*bs)
struct.pack_into('<H', inode2, 26, 1)
struct.pack_into('<I', inode2, 28, 100*8)
struct.pack_into('<I', inode2, 32, 0x00080000)  # EXT4_EXTENTS_FL
struct.pack_into('<I', inode2, 100, 0x12345678)
# extent header in i_block
struct.pack_into('<H', inode2, 40, 0xF30A)
struct.pack_into('<H', inode2, 42, 1)
struct.pack_into('<H', inode2, 44, 4)
struct.pack_into('<H', inode2, 46, 0)
# extent: logical 0, len 100, physical block 14500
struct.pack_into('<I', inode2, 52, 0)
struct.pack_into('<H', inode2, 56, 100)
struct.pack_into('<H', inode2, 58, 0)
struct.pack_into('<I', inode2, 60, 14500)
for block in range(14500,14600):
    start = block*bs
    buf[start:start+bs] = bytes([(block ^ 0x5a) & 0xff]) * bs
with open(ext,'wb') as f: f.write(buf)

# Small GPT image with one partition and valid CRCs.
gpt = os.path.join(out, "gpt.img")
sector = 512
sectors = 32768
raw = bytearray(sector * sectors)
# protective MBR
raw[446+4] = 0xEE
struct.pack_into('<I', raw, 446+8, 1)
struct.pack_into('<I', raw, 446+12, min(sectors-1, 0xffffffff))
raw[510:512] = b'\x55\xaa'
entries = bytearray(128 * 128)
# Linux filesystem type GUID 0fc63daf-8483-4772-8e79-3d69d8477de4 in GPT byte encoding
entries[0:16] = bytes.fromhex('af3dc60f838472478e793d69d8477de4')
entries[16:32] = bytes.fromhex('78563412341278569abc001122334455')
struct.pack_into('<Q', entries, 32, 2048)
struct.pack_into('<Q', entries, 40, 16383)
name = 'FSX test'.encode('utf-16le')
entries[56:56+len(name)] = name
entries_crc = binascii.crc32(entries) & 0xffffffff
raw[2*sector:2*sector+len(entries)] = entries

def make_header(current, backup, entries_lba):
    h = bytearray(sector)
    h[0:8] = b'EFI PART'
    struct.pack_into('<I', h, 8, 0x00010000)
    struct.pack_into('<I', h, 12, 92)
    struct.pack_into('<Q', h, 24, current)
    struct.pack_into('<Q', h, 32, backup)
    struct.pack_into('<Q', h, 40, 34)
    struct.pack_into('<Q', h, 48, sectors-34)
    h[56:72] = bytes.fromhex('00112233445566778899aabbccddeeff')
    struct.pack_into('<Q', h, 72, entries_lba)
    struct.pack_into('<I', h, 80, 128)
    struct.pack_into('<I', h, 84, 128)
    struct.pack_into('<I', h, 88, entries_crc)
    temp = h[:92]
    struct.pack_into('<I', temp, 16, 0)
    struct.pack_into('<I', h, 16, binascii.crc32(temp) & 0xffffffff)
    return h
raw[sector:2*sector] = make_header(1,sectors-1,2)
# Backup GPT entries/header, enough for realism even though reader currently checks primary.
backup_entries_lba = sectors-33
raw[backup_entries_lba*sector:backup_entries_lba*sector+len(entries)] = entries
raw[(sectors-1)*sector:sectors*sector] = make_header(sectors-1,1,backup_entries_lba)
with open(gpt,'wb') as f: f.write(raw)
# Simple MBR image for mutation/restore integration coverage.
mbr = os.path.join(out, "mbr.img")
mraw = bytearray(16 * 1024 * 1024)
mraw[446] = 0x80
mraw[446+4] = 0x83
struct.pack_into('<I', mraw, 446+8, 2048)
struct.pack_into('<I', mraw, 446+12, 8192)
struct.pack_into('<I', mraw, 440, 0x12345678)
mraw[510:512] = b'\x55\xaa'
with open(mbr,'wb') as f: f.write(mraw)

print(ext)
print(gpt)
print(mbr)

# Minimal XFS image with redundant allocation-group superblocks.
xfs = os.path.join(out, "xfs.img")
xraw = bytearray(16 * 1024 * 1024)
def write_xfs_sb(off):
    xraw[off:off+4] = b'XFSB'
    struct.pack_into('>I', xraw, off+4, 4096)
    struct.pack_into('>Q', xraw, off+8, len(xraw)//4096)
    xraw[off+32:off+48] = bytes(range(1,17))
    struct.pack_into('>I', xraw, off+84, 1024)
    struct.pack_into('>I', xraw, off+88, 4)
    struct.pack_into('>I', xraw, off+96, 64)
    struct.pack_into('>H', xraw, off+100, 5)
    struct.pack_into('>H', xraw, off+102, 512)
    struct.pack_into('>H', xraw, off+104, 512)
    struct.pack_into('>H', xraw, off+106, 8)
    xraw[off+108:off+115] = b'FSX-XFS'
    struct.pack_into('<I', xraw, off+224, 0)
    crc = (~crc32c_raw(xraw[off:off+512])) & 0xffffffff
    struct.pack_into('<I', xraw, off+224, crc)
for ag in range(4): write_xfs_sb(ag * 1024 * 4096)
with open(xfs,'wb') as f: f.write(xraw)

# Minimal NTFS image with backup boot sector and valid MFT/MFTMirr USA records.
ntfs = os.path.join(out, "ntfs.img")
nraw = bytearray(16 * 1024 * 1024)
boot = bytearray(512)
boot[0:3] = b'\xeb\x52\x90'
boot[3:11] = b'NTFS    '
struct.pack_into('<H', boot, 11, 512)
boot[13] = 8
struct.pack_into('<Q', boot, 40, len(nraw)//512)
struct.pack_into('<Q', boot, 48, 4)
struct.pack_into('<Q', boot, 56, 8)
boot[64] = 0xf6
struct.pack_into('<Q', boot, 72, 0x123456789abcdef0)
boot[510:512] = b'\x55\xaa'
nraw[0:512] = boot
nraw[-512:] = boot
rec = bytearray(1024)
rec[0:4] = b'FILE'
struct.pack_into('<H', rec, 4, 48)
struct.pack_into('<H', rec, 6, 3)
struct.pack_into('<H', rec, 20, 56)
struct.pack_into('<I', rec, 24, 64)
struct.pack_into('<I', rec, 28, 1024)
struct.pack_into('<H', rec, 48, 0xa55a)
struct.pack_into('<H', rec, 50, 0x1111)
struct.pack_into('<H', rec, 52, 0x2222)
struct.pack_into('<H', rec, 510, 0xa55a)
struct.pack_into('<H', rec, 1022, 0xa55a)
nraw[4*4096:4*4096+1024] = rec
nraw[8*4096:8*4096+1024] = rec
with open(ntfs,'wb') as f: f.write(nraw)

# Btrfs with two SHA-256 protected superblock mirrors.
btrfs = os.path.join(out, 'btrfs.img')
bsize = 66 * 1024 * 1024
braw = bytearray(bsize)
def write_btrfs_sb(off):
    sb = bytearray(4096)
    sb[32:48] = bytes(range(1,17))
    struct.pack_into('<Q', sb, 48, off)
    struct.pack_into('<Q', sb, 64, 0x4D5F53665248425F)
    struct.pack_into('<Q', sb, 72, 7)
    struct.pack_into('<Q', sb, 80, 1 << 20)
    struct.pack_into('<Q', sb, 88, 2 << 20)
    struct.pack_into('<Q', sb, 112, bsize)
    struct.pack_into('<Q', sb, 120, 4 << 20)
    struct.pack_into('<Q', sb, 136, 1)
    struct.pack_into('<I', sb, 144, 4096)
    struct.pack_into('<I', sb, 148, 16384)
    struct.pack_into('<I', sb, 156, 4096)
    struct.pack_into('<H', sb, 196, 2) # SHA-256
    sb[198] = sb[199] = 1
    struct.pack_into('<Q', sb, 201, 1)
    struct.pack_into('<Q', sb, 209, len(braw))
    struct.pack_into('<Q', sb, 217, 4*1024*1024)
    sb[299:308] = b'FSX-BTRFS'
    sb[0:32] = hashlib.sha256(sb[32:]).digest()
    braw[off:off+4096] = sb
write_btrfs_sb(64*1024)
write_btrfs_sb(64*1024*1024)
with open(btrfs,'wb') as f: f.write(braw)

# Small ISO9660 image with PVD + terminator.
iso = os.path.join(out, 'iso9660.img')
iraw = bytearray(64 * 2048)
def both16(buf,off,v):
    struct.pack_into('<H',buf,off,v);struct.pack_into('>H',buf,off+2,v)
def both32(buf,off,v):
    struct.pack_into('<I',buf,off,v);struct.pack_into('>I',buf,off+4,v)
pvd = bytearray(2048);pvd[0]=1;pvd[1:6]=b'CD001';pvd[6]=1;pvd[40:47]=b'FSX ISO'
both32(pvd,80,64);both16(pvd,128,2048);pvd[156]=34;both32(pvd,158,20);both32(pvd,166,2048);pvd[181]=2;pvd[188]=1
term=bytearray(2048);term[0]=255;term[1:6]=b'CD001';term[6]=1
iraw[16*2048:17*2048]=pvd;iraw[17*2048:18*2048]=term
with open(iso,'wb') as f:f.write(iraw)

# BSD UFS2/FFS2-shaped superblock using the stable fixed-width prefix.
ufs = os.path.join(out, 'ufs2.img')
uraw = bytearray(32 * 1024 * 1024)
sb = bytearray(8192)
def u32(off,v): struct.pack_into('<I',sb,off,v)
u32(8,16);u32(12,24);u32(16,32);u32(20,64);u32(44,8);u32(48,8192);u32(52,1024);u32(56,8);u32(104,2048);u32(120,32);u32(184,2048);u32(188,4096);sb[209]=1;u32(1372,0x19540119)
uraw[65536:65536+8192]=sb
with open(ufs,'wb') as f:f.write(uraw)

print(xfs)
print(ntfs)
print(btrfs)
print(iso)
print(ufs)

