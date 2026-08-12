/*
 * mosfs.h - read-only access to alphatronic P2 "MOS" floppy filesystems
 *
 * Part of MOStools.  See README.md for a description of the on-disk format.
 *
 * Copyright (c) 2026 Jürgen Leber
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.  It is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General
 * Public License in the file LICENSE for more details.
 */
#ifndef MOSFS_H
#define MOSFS_H

#include <stddef.h>
#include <stdint.h>

#define MOS_VERSION "0.1.0"

#define MOS_NAME_LEN      9   /* directory name field, space padded      */
#define MOS_DIRENT_SIZE  16   /* bytes per directory entry               */
#define MOS_LABEL_LEN    32   /* disk label in the configuration sector  */
#define MOS_DATE_LEN      8   /* label date field                        */
#define MOS_AUTOSTART_LEN 176 /* autostart command string, 0x20 .. 0xcf  */

/* Offsets inside the configuration sector. */
#define MOS_CFG_DRIVES    0x03 /* boot answer: number of disk drives     */
#define MOS_CFG_FILES     0x04 /* boot answer: number of open files      */
#define MOS_CFG_AUTOSTART 0x20 /* NUL terminated MOS command line        */
#define MOS_CFG_LABEL     0xd0 /* 32 byte label, then 8 byte date        */

/* Directory entry attribute bits (byte 9).  0x00 is a MOS BASIC data file,
 * 0x80 a tokenised BASIC program (SAVE).  0x40 has been seen once, on a
 * deleted entry called "INTER ASM"; presumably machine code. */
#define MOS_ATTR_PROGRAM  0x80u
#define MOS_ATTR_BINARY   0x40u
#define MOS_ATTR_DATA     0x00u

/* Special FAT byte values.  Everything below the cluster count is a
 * "next cluster" pointer. */
#define MOS_FAT_LAST      0xc0u  /* 0xc0 + n: last cluster, n sectors used */
#define MOS_FAT_LAST_MASK 0xf8u
#define MOS_FAT_RESERVED  0xfeu  /* system area / directory track          */
#define MOS_FAT_FREE      0xffu  /* unallocated                            */

/*
 * Per sector state of the image.  A flat .img cannot say anything about read
 * failures, so everything is MOS_SEC_OK there; an .imd source fills these in.
 */
#define MOS_SEC_OK        0
#define MOS_SEC_ERROR     1   /* read back with a CRC or data error      */
#define MOS_SEC_MISSING   2   /* not present in the image at all         */
#define MOS_SEC_DELETED   3   /* carries a deleted data address mark     */

/* mos_open() flags */
#define MOS_SCAN_ALL      0x01   /* do not stop at the first 0xff entry    */
#define MOS_KEEP_DELETED  0x02   /* report deleted entries as well         */

typedef struct {
	const char *name;
	const char *desc;
	int tracks;            /* cylinders                                   */
	int heads;             /* sides (only 1 is supported so far)           */
	int sectors;           /* sectors per track                            */
	int sector_size;       /* bytes per sector                             */
	int cluster_sectors;   /* sectors per allocation unit                  */
	int dir_track;         /* track holding directory, FAT and config      */
	int dir_sector;        /* first directory sector within that track     */
	int dir_sectors;       /* number of directory sectors                  */
	int fat_sector;        /* first FAT copy within the directory track    */
	int fat_copies;        /* number of identical FAT copies               */
	int cfg_sector;        /* configuration sector (label, autostart)      */
	int verified;          /* 1 = confirmed against a real image           */
} mos_format;

typedef struct {
	char     name[MOS_NAME_LEN + 1]; /* trailing blanks removed            */
	uint8_t  raw[MOS_DIRENT_SIZE];
	uint8_t  attr;          /* byte 9: 0x80 seen on BASIC programs         */
	uint8_t  start;         /* byte 10: first cluster                      */
	int      slot;          /* directory index                             */
	int      deleted;       /* name field started with 0x00                */
	uint8_t *chain;         /* cluster chain, chain_len entries            */
	int      chain_len;
	int      last_sectors;  /* sectors used in the final cluster           */
	long     sectors;       /* total sectors occupied by file data         */
	long     size;          /* sectors * sector_size                       */
	const char *error;      /* NULL, or why the chain could not be walked  */
} mos_file;

typedef struct {
	const mos_format *fmt;
	char     *path;
	uint8_t  *data;
	long      size;
	char      source[128];   /* how the image was read in, for reporting  */
	uint8_t  *secstatus;     /* MOS_SEC_* per sector, NULL when all fine  */
	int       sec_error;     /* sectors read with an error                */
	int       sec_missing;   /* sectors absent from the image             */
	int       sec_deleted;   /* sectors with a deleted address mark       */
	int       nclusters;
	uint8_t   fat[256];
	uint8_t   fat_ok[256];   /* per entry: inside the usable FAT area      */
	int       fat_len;       /* trustworthy FAT entries, <= nclusters      */
	int       fat_first_invalid; /* -1, or first entry holding junk        */
	int       fat_mismatch;  /* FAT copies disagreed (majority vote used)  */
	mos_file *files;
	int       nfiles;
	int       dir_truncated; /* stopped at end-of-directory marker         */
	char      label[MOS_LABEL_LEN + 1];
	char      date[MOS_DATE_LEN + 1];
	char      autostart[MOS_AUTOSTART_LEN + 1];
	uint8_t   cfg_drives;
	uint8_t   cfg_files;
} mos_image;

/* Format profiles. */
const mos_format *mos_format_find(const char *name);
const mos_format *mos_format_detect(long image_size);
const mos_format *mos_format_at(int index);   /* NULL terminated list */

/* Open/close.  On failure -1 is returned and err holds a message. */
int  mos_open(mos_image *img, const char *path, const mos_format *fmt,
              unsigned flags, char *err, size_t errlen);
void mos_close(mos_image *img);

/* Read the data of one file.  *buf is malloc()ed and must be free()d. */
int  mos_read_file(const mos_image *img, const mos_file *file,
                   uint8_t **buf, long *len, char *err, size_t errlen);

/*
 * Sectors of this file that could not be read properly.  Always 0 for a flat
 * image, which has no way of knowing.  Use it before trusting file contents.
 */
int  mos_file_bad_sectors(const mos_image *img, const mos_file *file);

/* Raw cluster access, for tools that cannot trust the FAT. */
const uint8_t *mos_cluster(const mos_image *img, int cluster);
long mos_cluster_size(const mos_image *img);
int  mos_cluster_is_meta(const mos_image *img, int cluster);

/* Cluster/space accounting.  "lost" counts allocated clusters that no file
 * claims, "unknown" the clusters behind the end of the usable FAT area. */
void mos_space(const mos_image *img, int *used, int *freecl, int *reserved,
               int *lost, int *unknown);

/*
 * Number of clusters that more than one file claims.  Anything but zero means
 * the FAT does not describe the directory any more - typically a disk that was
 * pulled before MOS wrote the FAT back - and affected files extract as
 * garbage from the point where the wrong chain is followed.
 */
int  mos_overlaps(const mos_image *img);

/* Filesystem name -> safe local filename (trailing blanks and '/' removed). */
void mos_local_name(const mos_file *file, char *buf, size_t buflen);

/* Shell pattern match against a MOS filename, optionally ignoring case.
 * Disks do exist that hold two names differing only in case. */
int  mos_match(const char *pattern, const char *name, int casefold);

/* Text helpers.
 *
 * mos_text_len()  length up to the CP/M style 0x1a end-of-file mark.
 * mos_crlf_to_lf() in-place CR LF -> LF, returns new length.
 * mos_de_to_utf8() DIN 66003 (German 7 bit) -> UTF-8, returns malloc()ed
 *                  buffer, or NULL when out of memory.
 */
/* UTF-8 for one DIN 66003 character, or NULL when it is plain ASCII. */
const char *mos_de_char(uint8_t c);

long  mos_text_len(const uint8_t *buf, long len);
long  mos_crlf_to_lf(uint8_t *buf, long len);
char *mos_de_to_utf8(const uint8_t *buf, long len, long *outlen);

#endif /* MOSFS_H */
