# MOStools

Tools for reading floppy disk images of the **Triumph-Adler alphatronic P2**
running **MOS** (Micro Operating System) — the native MOS filesystem, *not*
CP/M. Think `cpmls`/`cpmcp` from [cpmtools](http://www.moria.de/~michael/cpmtools/),
but for MOS disks:

| tool         | purpose                                                 |
|--------------|---------------------------------------------------------|
| `mosls`      | list the directory of a MOS disk image                  |
| `moscp`      | copy files out of a MOS disk image                      |
| `mosbasic`   | list tokenised MOS BASIC programs as readable text      |
| `mosrecover` | rebuild file chains from the data when the FAT is stale |

All four are read-only; they never modify the image.

## Build

```sh
make
make install                # PREFIX=/usr/local by default
```

No dependencies beyond a C99 compiler and POSIX `fnmatch()`.

Disk images are deliberately not part of this repository (see `.gitignore`).
The examples below use `alphatronic-system30.img`, a 160 KB MOS 3.0 system
disk; `make check` runs `mosls` against it.

## Input formats

All three tools take either a **flat sector image** (`.img`, 163840 bytes for a
160 KB disk) or an **ImageDisk file** (`.imd`, as written by ImageDisk, HxC and
similar), detected by content rather than by file name. Prefer `.imd` when you
have it, because a flat image cannot carry two things that matter:

* **Whether each sector actually read.** IMD records CRC/data errors, deleted
  address marks and unreadable sectors per sector. A flat image turns those into
  zeros, which `mosrecover` would then cheerfully treat as file data. With an
  IMD, the tools name the affected files instead, and `moscp` exits non-zero
  rather than pretending the copy is complete.
* **The sector numbers as read.** Some P2 system disks carry a deliberately
  misnumbered sector on track 0 as copy protection — sector 19 where 16 belongs,
  which the drive reports as a CRC error. `mosls -v` says so and marks the
  missing logical sector, instead of depending on what a converter decided.

Odd captures are handled too: a 40-track disk read in an 80-track drive lands on
even cylinders only, and side 2 of a single-sided disk is often captured as
well. Both are collapsed automatically, and `mosls -v` reports what was used:

```
Source:     ImageDisk, 40 tracks x 16 sectors x 256 bytes, head 0, double stepped, second head ignored
```

Head 0 is always used, since MOS disks are single sided; there is no switch for
picking side 2 because no two-sided MOS disk exists here to test it against.

## Usage

```sh
$ mosls -v -l alphatronic-system30.img
Image:      alphatronic-system30.img (163840 bytes)
Source:     flat sector image
Format:     mos160 - single sided, 40 tracks, 16 sectors, 256 bytes/sector
Geometry:   40 tracks x 16 sectors x 256 bytes, 4 sectors/cluster, 160 clusters
Directory:  track 20, sectors 0-10
Boot cfg:   2 drive(s), 4 file(s)
Space:      7 clusters used, 89 free (91136 bytes), 44 reserved
Warning:    3 allocated cluster(s) not owned by any file
Warning:    FAT area ends at cluster 8c (junk from 8d on), 20 cluster(s) unaccounted for

attr start clst  sec     size  name
  80    4e     4   16     4096  AUTOLOAD
1 file(s), 4096 bytes
```

A data disk with a label and an autostart command, and with more than one file
on it, looks like this:

```
Label:      ARBEITSDISKETTE ABLAUF 1  04.07.83
Autostart:  MOUNT 1:RUN"ABLAUF"

attr start clst  sec     size  name
  00    46     1    2      512  NOTIZ1
  00    59     1    2      512  MEMO
  80    4f     9   36     9216  ABLAUF
  00    4a     2    5     1280  BRIEF 91
  00    54     1    2      512  LISTE1
```

```sh
mosls  [-l] [-c] [-d] [-a] [-v] [-S] [-f format] image [pattern ...]
moscp  [-t] [-u] [-o] [-S] [-d] [-f format] image pattern ... destdir
moscp  [-t] [-u] [-o] [-S] [-d] [-f format] image file destfile
```

`mosls`

* `-l` long listing: attribute byte, start cluster, cluster and sector count, size
* `-c` print the cluster chain of each file
* `-d` include deleted entries (their first name character is lost, shown as `?`)
* `-a` scan the whole directory area instead of stopping at the end marker
* `-v` disk label, autostart command, space usage and consistency warnings

`moscp`

* `-t` text mode: cut at the `0x1a` end-of-file mark, convert `CR LF` to `LF`
* `-u` translate the German 7-bit character set (DIN 66003) to UTF-8
* `-o` overwrite existing local files (refused by default)

Both take `-S` to match patterns case sensitively. That is needed on disks that
carry two names differing only in case — a games disk here has both `SPIEL` and
`spiel`, which the default case-insensitive matching cannot tell apart.

Copy everything, converted to plain UTF-8 text:

```sh
mkdir out && moscp -t -u alphatronic-system30.img '*' out/
```

Copy a single file, or dump one to stdout:

```sh
moscp -t -u datadisk.img 'BRIEF 91' brief.txt
moscp alphatronic-system30.img AUTOLOAD - | xxd | less
```

Patterns are shell wildcards, matched case-insensitively. MOS names may contain
spaces (`BRIEF 91`), so quote them.

Without `-t` you get the file exactly as it sits on the disk: whole 256-byte
sectors, the logical end marked by `0x1a`, and the rest of the last sector
zero-filled. That padding is on the disk, not an extraction fault — `-t` cuts
there, and `-u` fixes the umlauts.

### Listing BASIC programs

A file with attribute `80` is a tokenised BASIC program — binary, and unreadable
with `moscp`. `mosbasic` turns it back into a listing:

```sh
$ mosbasic -u alphatronic-system30.img
AUTOLOAD:
10 ON ERROR GOTO 450
15 DEFINT I:FIELD#0,128 AS X$,128 AS Y$
20 CE$=CHR$(27)+CHR$(19):CA$=CHR$(27)+CHR$(18)
30 E$=R$+"  Diskette nicht formatiert oder schreibgeschützt oder nicht im Laufwerk  !   "+N$
60 PRINT C$;H$;CA$;R$"           A  U  T  O  L  O  A  D         V 1.1         Copyright by TA         "N$
...
```

```sh
mosbasic [-u] [-T sysimage] [-x] [-S] [-f format] image [pattern ...]
mosbasic [-u] [-T sysimage] [-x] -r file ...
```

* `-u` translate the German 7-bit characters to UTF-8, in strings and comments
  alike; bytes above 0x7f (the alphatronic graphics characters) are shown as
  `{xx}` so the output stays valid UTF-8
* `-r` the arguments are program files already extracted with `moscp`
* `-T sysimage` read the token table out of the BASIC interpreter on that system
  disk instead of using the built-in one
* `-x` append `{xx}` after every token, to inspect the encoding

**The token table is not guesswork.** MOS BASIC keeps its keywords inside the
interpreter, which lives in the reserved system area of a system disk, so the
table was read out of a real one — the built-in table is BASIC Rev. 3.00 from an
`AUTOLOAD` disk, 144 keywords and 10 operators. Passing `-T` re-reads it at run
time and produces byte-identical output, which is how the built-in copy was
checked; use it if you meet a disk written by a different BASIC revision.

Getting at it needs one trick worth recording: the system area is **not** a flat
code image. Every sector there starts with a marker byte (`0xf6` on sector 0 of
a track, `0xff` elsewhere), so the code has to be stitched together from 255-byte
pieces before the table can be read. Ignore that and keywords spanning a sector
boundary come out corrupt — `REMOVE` reads as `REMOV`, `FRE` gets the wrong
token.

In the table itself each entry is the keyword minus its first letter, which is
implied by the group it sits in, with bit 7 set on the last character and the
token following. A token below 0x80 means a function, written in programs as
`0xff` plus `0x80 + token`; after the Z group come the operators as
(character | 0x80, token) pairs, which is where `+`, `=`, `<` and the `'`
comment shorthand come from.

Numbers are stored binary and are decoded back: line numbers, one-byte and
two-byte integers, `&H` and `&O` constants, and Microsoft binary format single
and double precision floats. A comment written with an apostrophe is stored as
`:REM'` and is printed as `'` again, the way the machine lists it.

Checked against every program on three disks: no unknown tokens, and every
`GOTO`/`GOSUB`/`THEN` target resolves to a line that exists (except on the disk
with the stale FAT, where the programs themselves are truncated).

### Recovering a disk with a stale FAT

MOS keeps the allocation table in RAM and writes it back when the disk is
dismounted. A disk pulled out before that keeps a current directory and current
data but a FAT from an earlier state, so `moscp` hands you files that turn to
garbage at their first cluster boundary. `mosls -v` flags such a disk (clusters
claimed by two files at once).

`mosrecover` ignores the chains and follows the files themselves:

```sh
$ mosrecover games.img
kind   status      bytes  fat   name
basic  complete     3379  same  REAKTION
basic  complete*    3052  diff  LOTTO
basic  partial       247  diff  WAIT
...
23 complete, 5 partial, 0 not recognised; 25 differ from the FAT
17 recovery/recoveries marked * had more than one possible continuation
recovered chains overlap each other in 0 cluster(s) - globally consistent
```

```sh
mosrecover [-x dir] [-t] [-u] [-o] [-c] [-p] [-d] [-S] [-f format] image [pattern ...]
```

Without `-x` nothing is written — the report alone tells you which files the FAT
still describes correctly (`fat: same`) and which it does not (`diff`, `err`, or
`short` when the FAT allocates less than the data needs). `-x dir` writes the
recovered files, `-c` prints both chains side by side, `-p` also writes partial
recoveries.

How it decides:

* A tokenised BASIC program is a chain of lines, each beginning with the address
  of the next, so only a cluster whose first bytes continue that chain can
  follow. A data file is a chain of printable records ending in `0x1a`, which
  constrains it the same way. The load address is not stored in the file, so it
  is derived by trying every possible length of the first line.
* Where the FAT's cluster still fits, it wins. A stale FAT is wrong in places,
  not everywhere, and on a disk holding several older copies of the same program
  more than one cluster may continue a line chain plausibly.
* A cluster belongs to one file, and no file continues into the first cluster of
  another. Chains that come out forced take their clusters off the table for
  everyone else; then the most constrained of the remaining chains is settled,
  and so on. That is why the report can state that the result is globally
  consistent — no two recovered files claim the same cluster.

Read the status column honestly:

| status      | meaning                                                      |
|-------------|--------------------------------------------------------------|
| `complete`  | ends on a proper terminator and every step was forced or was the FAT's |
| `complete*` | ends properly, but at some step we chose between alternatives |
| `partial`   | the file parses up to the byte count shown and then stops - no cluster on the disk continues it, so the rest is overwritten or gone |
| `no parse`  | neither parser locks on: binary or a random-access data file |

Feed it an `.imd` where you have one: a cluster whose sectors did not read
cleanly is reported per file, so a recovery is not silently built on zeros.

On healthy disks it doubles as a verifier: `mosrecover` on both intact sample
disks reproduces every FAT chain exactly, with no `*` and no overlaps.

A `partial` result on a file the FAT calls empty (0 sectors) is not content —
that is the old data still sitting in an allocated but never written cluster.

### Text files, programs and the attribute byte

MOS BASIC data files (attribute `00`, written with `PRINT#`) are plain text:
records are quoted, separated by `CR LF`, and the file ends with `0x1a` —
exactly what `-t` expects. Characters `[ \ ] { | } ~` carry `Ä Ö Ü ä ö ü ß`,
which `-u` converts.

Anything with a non-zero attribute is copied verbatim even with `-t`/`-u` (and
`moscp` says so on stderr), because converting it would corrupt it:

| attr | meaning                                                            |
|------|--------------------------------------------------------------------|
| `00` | BASIC data file, text as described above                            |
| `80` | tokenised BASIC program (`SAVE`)                                    |
| `40` | seen once, on a deleted entry named `INTER ASM`; presumably binary  |

Programs use the standard Microsoft BASIC layout: per line a 16-bit link
address, a 16-bit line number, tokens, and a `00` terminator; a `0000` link
ends the program.

## The MOS disk format

Reverse-engineered from two 160 KB disks — a MOS 3.0 system disk and a data
disk — and verified by walking the BASIC line links of the programs on both
across every cluster of their chains. Parts that are still unknown are marked
as such; corrections welcome.

The data disk is private, so its image name, its file names and its label appear
here as stand-ins. Everything technical about it — sizes, cluster numbers,
attributes, chains — is unchanged.

### Geometry

The only geometry confirmed so far (`mos160`) is 40 tracks × 16 sectors ×
256 bytes = 163840 bytes, single sided, sectors in linear order in the image
file (`LSN = track * 16 + sector`) with no interleave — the sector maps in five
IMD captures are all `1…16` in ascending order, so the flat layout is faithful.
The format is picked by image size; `-f` forces one.

A `mos80` profile (same layout with 128-byte sectors, 81920 bytes) is included
because the MOS floppy driver supported 80 KB media, but it is **unverified** —
no such image was available for testing.

### Allocation units

Four consecutive sectors form one **cluster** of 1024 bytes, giving 160
clusters (`0x00`–`0x9f`) on a 160 KB disk. Everything is allocated in whole
clusters.

### The system track

Track 20 — the middle of the disk — holds all metadata:

| sector  | content                                    |
|---------|--------------------------------------------|
| 0 – 10  | directory, 16 entries per sector           |
| 11      | unused (stale data on one of the samples)  |
| 12      | configuration sector                       |
| 13 – 15 | three identical copies of the FAT          |

MOS itself lives at the start of the disk. How much of it is reserved varies:
7 tracks on the data disk, 10 tracks on the system disk. Both that area and the
system track are marked reserved in the FAT, so the FAT is the authority — not
a fixed track count.

### Directory entry (16 bytes)

| offset | size | content                                                    |
|--------|------|------------------------------------------------------------|
| 0      | 9    | file name, padded with spaces                              |
| 9      | 1    | attribute (see above)                                      |
| 10     | 1    | first cluster                                              |
| 11     | 5    | unused, `0xff`                                             |

Byte 0 of the name field doubles as the slot state: `0x00` means the entry was
deleted (the original first character is gone), `0xff` means the slot was never
used and marks the end of the directory. New entries are appended rather than
filling deleted slots.

Deletion clears the FAT chain but leaves the entry and the data behind, so
`mosls -d` shows what used to be there while `moscp -d` will usually fail with
*chain runs into free cluster* — the cluster order is simply no longer
recorded. It only succeeds when the chain happens to be intact, e.g. when a
program was saved again under the same name.

### FAT (160 bytes, three copies)

One byte per cluster, indexed by cluster number:

| value           | meaning                                                   |
|-----------------|-----------------------------------------------------------|
| `0x00`–`0x9f`   | number of the next cluster of this file                    |
| `0xc0` + *n*    | last cluster; *n* = 0…4 sectors of it are used             |
| `0xfe`          | reserved (MOS system area, system track)                  |
| `0xff`          | free                                                      |

So a file's size is `(clusters - 1) * 1024 + n * 256` bytes, i.e. exact to the
sector; the byte-exact end of a text file is its `0x1a` mark. `0xc0` means zero
sectors used: a file that was created but never written. Chains are not
monotonic — MOS reuses whatever is free, so a chain may run backwards over the
disk.

`mosls` takes a per-byte majority vote over the three copies and warns when
they disagree.

Only the FAT itself is meaningful in that sector; the bytes behind it are
whatever the sector buffer held (MOS code, on both sample disks). On the system
disk that junk starts at cluster `0x8d`, i.e. before the nominal 160 entries,
so `mosls` locates the end of the FAT area at the last track boundary before
the first illegal value and reports the clusters behind it as unaccounted for
instead of inventing allocations.

### Configuration sector (track 20, sector 12)

| offset | size | content                                                   |
|--------|------|-----------------------------------------------------------|
| 0x00   | 3    | zero on all samples                                       |
| 0x03   | 1    | number of disk drives (1 or 2)                            |
| 0x04   | 1    | number of open files (2, 4, 5 seen)                       |
| 0x05   | 27   | zero                                                      |
| 0x20   | 176  | autostart command, NUL-terminated and NUL-padded          |
| 0xd0   | 32   | disk label, optional                                      |
| 0xf0   | 8    | date, `DD.MM.YY`, optional                                |
| 0xf8   | 8    | spaces                                                    |

Bytes 3 and 4 line up with the two questions MOS asks when it boots without a
configured disk (*How many disk drives* / *How many files(0-15)*), which is
where the interpretation above comes from.

Everything in this sector is optional. Of four sample disks only two — both
holding the same commercial program suite — carry a label; a games disk has an
autostart command but no label, and the MOS 3.0 system disk has an all-zero
configuration sector — no label, no command, so nothing to boot on its own.
`mosls -v` prints only the fields that are actually filled in.

That the two labelled disks are the ones with a bought program on them, and that
the disk service program never asks for a disk name when formatting, suggests
the label is written by an application or a duplication run rather than by MOS
itself.

The autostart string is a full MOS command line, not just a file name, and it
can use most of the 176 bytes:

```
PRINT"*****  S p i e l s y s t e m  *****":MOUNT 1:RUN"SPIEL"
```

### Consistency notes

`mosls -v` reports allocated clusters that no directory entry claims: three on
the system disk, six on the data disk. Those are lost chains left over from
earlier use — harmless, but MOS will not reuse them either.

It also reports clusters claimed by **two** files at once, which means the FAT
no longer describes the directory. One of the sample disks (a games collection)
is in that state: 32 clusters are double-claimed, and files whose chain is
affected extract as garbage past their first cluster — the correct continuation
is a different cluster, verified by following the BASIC line links. No better
FAT exists anywhere on that disk (all 640 sectors were searched), two
independent flux reads of it decode identically and the IMD reports no read
errors, so the on-disk table really is out of date; the likely cause is a disk
pulled or a machine switched off before MOS wrote the FAT back. Use
`mosrecover` on such a disk — `mosls`/`moscp` deliberately stay faithful to the
FAT and will not guess.

## Status and ideas

* Read-only. Writing (`mosput`, delete, format) is not implemented.
* A BASIC detokeniser (`mosbasic`) to turn saved programs into readable
  listings would be the obvious next tool; the token table still has to be
  extracted from the MOS BASIC ROM. It would also sharpen `mosrecover`: a
  detokeniser rejects a wrong continuation that merely has valid line links.
* Only `mos160` is verified. If you have an 80 KB image, or one from a P3/P4
  with a different geometry, please open an issue with a sample.

## License

GNU General Public License, version 3 or later — see [LICENSE](LICENSE).

Note that the GPL does not restrict commercial use; what it requires is that
anyone distributing this code or a derivative of it does so under the same
license and makes the source available.

## References

* [MOS documentation and floppy driver description](https://www.classic-computing.de/hellwie/)
  — archive of the former head of development at sks, who wrote MOS
* [Floppy disks used by the alphatronic](https://adangel.org/2020/05/03/floppy-disks-alphatronic/)
* [cpmtools](http://www.moria.de/~michael/cpmtools/) — for the CP/M disks of the same machine
