# Save-format and edit-persistence contract

What the on-disk world guarantees, why each guarantee exists, and the
command that proves it. Everything here regenerates locally; the format
tests run in CI on every commit.

## The format (chunk_serialize.h, v3)

12-byte header, then RLE runs:

    magic "VCHK" | version u8 | flags u8 | reserved u16 | crc u32
    (u8 block_id, u16 run_length) ... until blocks sum to 16*256*16

- Flags bit 0 is the edited bit: this chunk holds player edits and must
  never be regenerated from terrain.
- The CRC (IEEE 802.3) covers header bytes 0..7 plus the payload, so a
  flipped version or edited bit fails decode the same way a flipped
  payload bit does. A hand-edited chunk cannot silently become
  "regenerable" through disk corruption.
- Structural checks reject unknown block ids, zero-length runs, volume
  mismatches, and trailing bytes; the CRC catches damage that still
  spells valid runs. Older versions are rejected, never migrated.
- Measured compression on the radius-12 world: 39.06 MB raw to 0.67 MB
  on disk (about 58x), from `--bench-io`.

## The write path

- Every file is written atomically: `.tmp` in the same directory, then
  rename. A crash mid-save leaves the previous file or an ignored
  `.tmp`, never a torn chunk. The stream is closed and checked before
  the rename, so a failed flush (disk full, I/O error) cannot promote
  an unverified `.tmp` over a good save. fsync is deliberately out of
  scope and documented as such (process crashes, not power loss).
- `world.manifest` records the terrain seed the world was generated
  from, one line: `seed <u32>`.

## The persistence contract

1. A player edit survives its chunk streaming out and back in: modified
   chunks are RLE-stashed on eviction instead of regenerated, and
   in-flight job identity (request stamps) guarantees a stale terrain
   job can never overwrite a restored edit.
2. On load with a manifest seed matching the active terrain, only
   chunks whose header carries the edited bit are preserved; everything
   else regenerates on demand, so the stash grows with edits, not with
   play time.
3. On load with a missing or mismatched manifest, nothing on disk is
   regenerable and every chunk is conservatively preserved.

## Proving it

    ./build/voxel_engine --verify-edit-persistence   # contract 1, one-shot
    scripts/verify_persistence.sh                    # contracts 2 + 3, end to end
    ./build/voxel_engine --bench-io                  # atomic save/load round trip

`verify_persistence.sh` output on this machine (radius 6, 169 chunks):

    PERSIST_VERIFY seed-match ok (EDIT_PERSIST ... stashed=1 restored=1 survived=1 ok)
    PERSIST_VERIFY seed-mismatch ok (EDIT_PERSIST ... stashed=169 restored=169 survived=1 ok)
    PERSIST_VERIFY all ok
