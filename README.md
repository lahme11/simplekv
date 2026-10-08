# SimpleKV

A small persistent key–value database written in C, with a Redis-style command line. It uses only the C library
and POSIX calls: `pread`/`pwrite`, `fsync`, `ftruncate`, `flock` and `rename`. Every change is appended to a file
of checksummed records. The database survives restarts and crashes, compacts itself, and refuses to be opened by
two programs at once.

SimpleKV covers storage: a binary file format, checksums, durability, crash recovery, compaction, file locking and a resizable hash table.

It comes in two versions, plus a test suite:

- **Stage 1**: an in-memory hash table saved to a text log that is replayed at start-up.
- **Stage 2**: the full engine, with a CRC-32 checked binary log, an index of where each value lives, crash
  recovery, atomic compaction, file locking, expiring keys, counters and stats.
- **Stage 3**: unit and integration tests under AddressSanitizer and UBSan, a benchmark, and a demo script.

## Project structure

```
Stage1/
  src/
    simplekv.c     command loop, tokenising and output
    table.c        fixed-size hash table and the text log
    simplekv.h
  manual/readme.txt
  bin/
  makefile
Stage2/
  src/
    main.c         options, the command loop and the exit status
    command.c      the command table: argument checks and replies
    store.c        the database: open, recover, read, write, expire, compact
    index.c        a hash table from keys to value locations (grows at 3/4 full)
    record.c       CRC-32 and the record format
    parse.c        splitting command lines (quotes, escapes) and quoting output
    simplekv.h
  manual/readme.txt
  bin/
  makefile
Stage3/
  tests/test_units.c     124 checks on the codec, tokenizer, index and store
  tests/integration.sh   27 checks running both programs with scripts
  bench/bench.c          throughput for each sync mode
  demo.sh                scripted walkthrough
  makefile               make test, make bench
```

## Requirements

- Linux (or WSL) with `gcc` and `make`
- `bash`, `truncate` and `stat` for the integration tests

## Build

```bash
cd Stage2        # or Stage1
make
```

The binary is written to `bin/simplekv`. Both stages build with `gcc -Wall -Wextra` and no warnings.

## Usage

```bash
cd Stage2
./bin/simplekv [-f file] [-s always|batch|never] [-q] [-h]
./bin/simplekv -f shop.skv < commands.txt     # run a script
```

| Option | Description | Default |
| --- | --- | --- |
| `-f file` | Database file | `data.skv` |
| `-s mode` | When to `fsync`: `always` (after every write), `batch` (every 100 writes and on exit) or `never` | `always` |
| `-q` | No banner or prompt | |
| `-h` | Show usage | |

When the input is a script rather than a terminal, there is no prompt, and the exit status is 1 if any command
failed.

### Commands

| Command | Reply |
| --- | --- |
| `SET key value` | `OK` |
| `SETEX key seconds value` | `OK`; the key disappears after `seconds` |
| `GET key` | the value, or `(nil)` |
| `DEL key` | `(integer) 1`, or `0` if it wasn't there |
| `EXISTS key` | `(integer) 1` or `0` |
| `KEYS [prefix]` | `1) key` … in sorted order, or `(empty)` |
| `COUNT` | `(integer) n` |
| `EXPIRE key seconds` | `(integer) 1`, or `0` if there is no such key |
| `TTL key` | seconds left; `-1` never expires; `-2` no such key |
| `INCR key [by]` | the new number; a missing key counts as 0 |
| `COMPACT` | `OK (reclaimed N bytes)` |
| `STATS` | keys, records, file size, live and dead bytes, sync mode, last compaction |
| `HELP`, `QUIT` | |

- Command names can be any case.
- **Quoting:** wrap keys or values containing spaces in double quotes. Inside quotes you can use `\"`, `\\`,
  `\n`, `\t`, `\r` and `\xNN`. `GET` prints values with the same escapes, so its output can be pasted back into
  a `SET`.
- **Limits:** keys are 1–1,024 bytes and values up to 1 MiB.
- **Errors** start with `ERR`, for example `ERR wrong number of arguments for 'set'`.

### Example session

```
$ ./bin/simplekv -f demo.skv
simplekv: 0 key(s) in demo.skv. Type HELP for commands.
simplekv> SET "full name" "Aoife Byrne"
OK
simplekv> GET "full name"
"Aoife Byrne"
simplekv> INCR visits 41
(integer) 41
simplekv> SETEX session 60 abc123
OK
simplekv> TTL session
(integer) 60
simplekv> KEYS
1) "full name"
2) session
3) visits
simplekv> QUIT

$ truncate -s -4 demo.skv            # simulate a crash in the middle of a write
$ echo KEYS | ./bin/simplekv -f demo.skv
simplekv: recovered: removed 30 damaged bytes at offset 78
1) "full name"
2) visits

$ (sleep 5; echo QUIT) | ./bin/simplekv -f demo.skv &
$ echo GET visits | ./bin/simplekv -f demo.skv
simplekv: database is locked by another process
```

## How it works

### The file format

```
file header (8 bytes)   "SKV2" | version 1 | 0 0 0
record                  crc32 (4) | type (1) | key_len (4) | value_len (4) | expires_at (8) | key | value
record ...
```

- **Integers** are little-endian, written byte by byte, so the file doesn't depend on the CPU.
- **The CRC-32** (IEEE, table-driven) covers every byte of the record after the CRC field.
- **Record types:** a put (type 1) stores a value, and a delete (type 2) has no value. `expires_at` is Unix
  seconds, or 0 for never.

### Reading and writing

1. **Opening:** `open` with `O_CREAT`, then `flock(LOCK_EX | LOCK_NB)`. If another process holds the lock, the
   open fails straight away. A new file gets the header; a file with the wrong header is refused.
2. **Loading:** the records are read in order. For each one, the hash table (`index.c`) stores where the key's
   latest value is: its offset, length and expiry. Values stay on disk. A delete removes the key from the index.
3. **Writing:** a record is encoded in memory and written with `pwrite` at the end of the file. Then `fsync` runs
   according to the sync mode, and only after that is the index updated. If the write fails, the file is cut
   back to where it was.
4. **Reading:** `GET` looks up the key and reads the value with a single `pread`. Keys whose expiry has passed
   are treated as missing and dropped from the index.

### Crash recovery

While loading, a record is accepted only if:
- its header fits in the file;
- its lengths are within the limits;
- its type is known;
- the whole record fits in the file;
- its CRC matches.

At the first record that fails, the file is `ftruncate`d to the end of the last good record and synced, and
`recovered: removed N damaged bytes at offset X` is reported. A write that was cut off by a crash or power
failure is therefore removed, and everything before it is kept. A damaged record in the middle of the file also
drops the records after it, because the log is only trusted up to the first bad record.

### Compaction

The log only grows: every overwrite and delete leaves a dead record behind. `COMPACT`:

1. writes the header and every live, unexpired record to `<file>.compact` and `fsync`s it;
2. `rename`s it over the database. A rename within one directory is atomic, so there is never a moment where
   the database is missing or half-written;
3. `fsync`s the directory, so the rename itself survives a crash;
4. switches to the new file (which it already holds the lock on), closes the old one, and updates the index
   with the new offsets.

If anything fails before the rename, the temporary file is removed and the original is untouched. Compaction also
runs automatically after a write when the file is over 1 MiB and more than half of it is dead records.

### The index

`index.c` is a hash table with separate chaining and 64-bit FNV-1a hashing. It starts with 64 buckets and doubles
(rehashing every entry) when it is more than 3/4 full, so lookups stay constant-time as the database grows. Keys
are compared by length and bytes, so they may contain any byte.

## Performance

`make -C Stage3 bench` on a laptop running WSL2 (ext4), with 100-byte values:

| Sync mode | SET | GET | DEL |
| --- | --- | --- | --- |
| `never` | ~840,000 ops/s | ~2,500,000 ops/s | ~360,000 ops/s |
| `batch` | ~80,000 ops/s | ~2,200,000 ops/s | ~75,000 ops/s |
| `always` | ~1,000 ops/s | ~2,700,000 ops/s | ~1,000 ops/s |

Reads are one `pread` from the page cache. Writes in `always` mode are limited by `fsync`, which waits for the
disk. That is the price of not losing any acknowledged write, and the reason `batch` exists.

## Tests

```bash
cd Stage3
make test        # unit tests, then integration tests
make bench       # benchmark every sync mode
./demo.sh        # walkthrough (DEMO_NOPAUSE=1 to run straight through)
```

- **Unit tests (124 checks):**
  - **Format and parsing:** CRC-32 known answers, the record codec, rejection of damaged records, the tokenizer
    and output quoting.
  - **Index:** growth past 10,000 keys.
  - **Store:** persistence across reopen; recovery from a cut-off file and from a flipped byte; refusal of
    foreign files; the lock; TTL with a fake clock; INCR overflow; sorted prefix listing; compaction; and a
    compaction that fails safely when its temporary file can't be created.
- **Integration tests (27 checks):** every command's output; error replies and the exit status; a 1.2 MB input
  line; recovery after `truncate`; a second process locked out; auto-compaction after 3,000 overwrites; the
  sync modes; and Stage 1's log replay.

Everything is built with `-fsanitize=address,undefined -fno-sanitize-recover=all`, so any memory error or
undefined behaviour fails the run. I also checked the locking and auto-compaction tests by disabling each
feature: the tests failed as they should.

## Limitations

- The whole index is kept in memory, so the number of keys is limited by RAM (values stay on disk).
- One process at a time; there is no network server, so use [SimpleHTTPd](../simplehttpd) for that kind of
  project.
- No transactions or multi-key atomic operations; each command is one record.
- `KEYS` lists by prefix only; there are no range scans or patterns.
- Expired keys are removed lazily (when touched, listed or compacted), not by a background timer.
- A damaged record in the middle of the file loses the records after it.
