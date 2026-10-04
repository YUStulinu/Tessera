# Tessera file formats

A database is two files: `name.tdb`, the database itself, and `name.tdb-wal`,
the write-ahead log. All integers are little-endian. Pages are 4096 bytes.

## Pages

Every page, in both files, ends with a 4-byte CRC32C of its first 4092 bytes.
A page whose checksum does not match is reported as corruption (`TS_CORRUPT`)
the first time it is read; nothing built on top of it is trusted.

```
0                                              4092      4096
+----------------------------------------------+---------+
| content (type-specific, 4092 bytes)          | CRC32C  |
+----------------------------------------------+---------+
```

Page numbers are 32-bit; page `n` lives at byte offset `n * 4096` of the
database file. Page 0 is the header; the B+tree root can be any page.

## Page 0: the header

| Offset | Size | Field                                             |
|-------:|-----:|---------------------------------------------------|
| 0      | 8    | magic `"TESSERA\0"`                                |
| 8      | 4    | format version (1)                                |
| 12     | 4    | page size (4096)                                  |
| 16     | 4    | page count: pages in use, including page 0       |
| 20     | 4    | root page of the B+tree                           |
| 24     | 4    | tree height (1 = the root is a leaf)              |
| 28     | 4    | first free-list trunk page (0 = empty free list)  |
| 32     | 4    | number of free pages                              |
| 40     | 8    | id of the last committed transaction              |
| 48     | 8    | number of keys                                    |

The header is rewritten by every commit (the transaction id always changes),
and its frame is always the last frame of a transaction in the log - so the
commit frame is the one that carries the new header.

## B+tree nodes (types 1 and 2)

```
 0  u8   type            1 = leaf, 2 = internal
 2  u16  cell count
 4  u16  content start   cells are packed from the end of the page downwards
 6  u16  fragmented      bytes of holes between cells (reclaimed by compaction)
 8  u32  link            leaf: next leaf (0 = last); internal: rightmost child
16  u16  slots[count]    offsets of the cells, sorted by key
 .. free space ..
    cells
4092 CRC32C
```

Leaf cell: `u16 key length, u32 value length, key bytes, value`. If the cell
would be larger than a quarter of a page, the value is replaced by a `u32`
pointer to its first overflow page.

Internal cell: `u32 child, u16 key length, key bytes`. Child `i` holds keys
`< key[i]`; the rightmost child (the `link` field) holds the rest. Separator
keys are suffix-truncated: the shortest prefix of the right node's first key
that is still greater than the left node's last key.

A node underflows below a quarter of its space; it then merges with, or
borrows from, a sibling.

## Overflow pages (type 3)

```
 0  u8   type = 3
 4  u32  next overflow page (0 = last)
 8  u32  bytes used on this page
12  ...  up to 4080 bytes of the value
```

## Free-list trunk pages (type 4)

```
 0  u8   type = 4
 4  u32  next trunk page (0 = last)
 8  u32  number of entries
12  u32  free page numbers, up to 1020
```

Freed pages go into the head trunk; when it is full, the freed page itself
becomes the new head trunk. Allocation takes the last entry, or the trunk page
itself when it has no entries left. The integrity check (`CHECK`) verifies
that every page below the page count is owned by exactly one of: the header,
a node, an overflow chain, a trunk, or the free list.

## The write-ahead log

```
header (32 bytes)
 0  u32  magic 0x57535354
 4  u32  version (1)
 8  u32  page size
12  u32  checkpoint sequence
16  u32  salt 1
20  u32  salt 2
28  u32  CRC32C of bytes 0..27

frame (24 + 4096 bytes), repeated
 0  u32  page number
 4  u32  commit marker: 0, or the database page count on a transaction's last frame
 8  u32  salt 1     \ must match the header: frames left over from an older
12  u32  salt 2     / generation of the log are never mistaken for new ones
16  u32  reserved (0)
20  u32  chained CRC32C: crc(previous frame's chain value, frame header 0..19, page)
24       page image
```

The chain starts from the header's CRC. Recovery reads frames in order and
stops at the first frame with the wrong salts or the wrong chained checksum;
only frames up to the last commit marker count. Everything after it - a torn
write, a transaction that never committed - is cut off.

### Checkpoint

1. fsync the log (so no page reaches the database file before the commit that
   produced it is durable);
2. copy the newest version of each page in the log into the database file, in
   ascending page order;
3. fsync the database file;
4. start a new log generation: write a header with new salts, truncate the log
   to the header, fsync.

A crash anywhere in 1-3 leaves the log intact: recovery replays it and copies
the same pages again. A crash in 4 leaves either the old log (replayed again,
harmlessly) or a new header whose salts invalidate every old frame.
