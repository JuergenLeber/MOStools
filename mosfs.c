/*
 * mosfs.c - read-only access to alphatronic P2 "MOS" floppy filesystems
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
#include "mosfs.h"
#include "mosimd.h"

#include <ctype.h>
#include <errno.h>
#include <fnmatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static const mos_format formats[] = {
	{ "mos160", "single sided, 40 tracks, 16 sectors, 256 bytes/sector",
	  40, 1, 16, 256, 4, 20, 0, 11, 13, 3, 12, 1 },
	{ "mos80",  "single sided, 40 tracks, 16 sectors, 128 bytes/sector",
	  40, 1, 16, 128, 4, 20, 0, 11, 13, 3, 12, 0 },
	{ NULL, NULL, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }
};

const mos_format *mos_format_at(int index)
{
	if (index < 0 || index >= (int)(sizeof formats / sizeof formats[0]) - 1)
		return NULL;
	return &formats[index];
}

const mos_format *mos_format_find(const char *name)
{
	int i;

	for (i = 0; formats[i].name != NULL; i++)
		if (strcmp(formats[i].name, name) == 0)
			return &formats[i];
	return NULL;
}

static long format_size(const mos_format *f)
{
	return (long)f->tracks * f->heads * f->sectors * f->sector_size;
}

const mos_format *mos_format_detect(long image_size)
{
	int i;

	for (i = 0; formats[i].name != NULL; i++)
		if (format_size(&formats[i]) == image_size)
			return &formats[i];
	return NULL;
}

/* ------------------------------------------------------------------ */

static long lsn_offset(const mos_image *img, long lsn)
{
	return lsn * img->fmt->sector_size;
}

static const uint8_t *dir_sector(const mos_image *img, int n)
{
	const mos_format *f = img->fmt;
	long lsn = (long)f->dir_track * f->sectors + f->dir_sector + n;

	return img->data + lsn_offset(img, lsn);
}

static const uint8_t *track_sector(const mos_image *img, int track, int sector)
{
	const mos_format *f = img->fmt;

	return img->data + lsn_offset(img, (long)track * f->sectors + sector);
}

static int read_file(const char *path, uint8_t **buf, long *len,
                     char *err, size_t errlen)
{
	FILE *fp;
	struct stat st;
	long n;

	if ((fp = fopen(path, "rb")) == NULL) {
		snprintf(err, errlen, "%s: %s", path, strerror(errno));
		return -1;
	}
	if (fstat(fileno(fp), &st) != 0 || !S_ISREG(st.st_mode)) {
		snprintf(err, errlen, "%s: not a regular file", path);
		fclose(fp);
		return -1;
	}
	*len = (long)st.st_size;
	if ((*buf = malloc((size_t)(*len > 0 ? *len : 1))) == NULL) {
		snprintf(err, errlen, "out of memory (%ld bytes)", *len);
		fclose(fp);
		return -1;
	}
	n = (long)fread(*buf, 1, (size_t)*len, fp);
	if (n != *len) {
		snprintf(err, errlen, "%s: short read (%ld of %ld bytes)", path, n, *len);
		free(*buf);
		*buf = NULL;
		fclose(fp);
		return -1;
	}
	fclose(fp);
	return 0;
}

/*
 * Load the image.  ImageDisk files are decoded into the same flat layout the
 * rest of the code works on, but keep their per sector read status.
 */
static int read_image(mos_image *img, const char *path, char *err, size_t errlen)
{
	uint8_t *raw;
	long rawlen;

	if (read_file(path, &raw, &rawlen, err, errlen) != 0)
		return -1;

	if (!mos_imd_detect(raw, rawlen)) {
		img->data = raw;
		img->size = rawlen;
		snprintf(img->source, sizeof img->source, "flat sector image");
		return 0;
	}

	{
		mos_imd imd;

		if (mos_imd_load(raw, rawlen, &imd, err, errlen) != 0) {
			free(raw);
			return -1;
		}
		free(raw);
		img->data = imd.data;
		img->size = imd.size;
		img->secstatus = imd.status;
		img->sec_error = imd.bad;
		img->sec_missing = imd.missing;
		img->sec_deleted = imd.deleted;
		snprintf(img->source, sizeof img->source,
		         "ImageDisk, %d tracks x %d sectors x %d bytes, head %d%s%s",
		         imd.tracks, imd.sectors, imd.sector_size, imd.head,
		         imd.doublestep ? ", double stepped" : "",
		         (imd.heads_seen & ~(1 << imd.head)) ? ", second head ignored"
		                                             : "");
		if (imd.outside > 0)
			snprintf(img->source + strlen(img->source),
			         sizeof img->source - strlen(img->source),
			         ", %d misnumbered sector(s) dropped", imd.outside);
		return 0;
	}
}

/*
 * Is v a value the filesystem could legally have put into the FAT?
 *
 * Only the first n_clusters bytes of the FAT sector belong to the FAT; the rest
 * of the sector is whatever the sector buffer happened to hold.  On some disks
 * the tail of the FAT area itself was never initialised, so entries have to be
 * checked instead of trusted.
 */
static int fat_value_ok(const mos_image *img, uint8_t v)
{
	if (v < img->nclusters)
		return 1;                       /* pointer to the next cluster   */
	if ((v & MOS_FAT_LAST_MASK) == MOS_FAT_LAST)
		return (v - MOS_FAT_LAST) <= (unsigned)img->fmt->cluster_sectors;
	return v == MOS_FAT_RESERVED || v == MOS_FAT_FREE;
}

/*
 * The FAT is stored several times in a row.  Take a per-byte majority vote so
 * that a single damaged copy does not spoil the result.
 */
static void read_fat(mos_image *img)
{
	const mos_format *f = img->fmt;
	int i, c, best, bestcount;

	img->fat_first_invalid = -1;

	for (i = 0; i < img->nclusters; i++) {
		int count[256];

		memset(count, 0, sizeof count);
		for (c = 0; c < f->fat_copies; c++)
			count[track_sector(img, f->dir_track, f->fat_sector + c)[i]]++;
		best = 0;
		bestcount = -1;
		for (c = 0; c < 256; c++)
			if (count[c] > bestcount) {
				bestcount = count[c];
				best = c;
			}
		if (bestcount != f->fat_copies)
			img->fat_mismatch = 1;
		img->fat[i] = (uint8_t)best;
	}
	/*
	 * Locate the end of the FAT area.  On a disk whose FAT tail was never
	 * initialised the first illegal value marks it; since a formatted disk
	 * always holds whole tracks, round down to a track boundary.
	 */
	img->fat_len = img->nclusters;
	for (i = 0; i < img->nclusters; i++)
		if (!fat_value_ok(img, img->fat[i])) {
			int per_track = f->sectors / f->cluster_sectors;

			img->fat_first_invalid = i;
			img->fat_len = i - (i % per_track);
			break;
		}
	for (i = 0; i < 256; i++)
		img->fat_ok[i] = (uint8_t)(i < img->fat_len);
}

static void read_config(mos_image *img)
{
	const mos_format *f = img->fmt;
	const uint8_t *cfg;
	int i;

	if (f->cfg_sector < 0 || f->sector_size < 256)
		return;
	cfg = track_sector(img, f->dir_track, f->cfg_sector);
	img->cfg_drives = cfg[MOS_CFG_DRIVES];
	img->cfg_files = cfg[MOS_CFG_FILES];

	for (i = 0; i < MOS_AUTOSTART_LEN && cfg[MOS_CFG_AUTOSTART + i] != 0; i++)
		img->autostart[i] = (char)cfg[MOS_CFG_AUTOSTART + i];
	img->autostart[i] = '\0';

	memcpy(img->label, cfg + MOS_CFG_LABEL, MOS_LABEL_LEN);
	img->label[MOS_LABEL_LEN] = '\0';
	memcpy(img->date, cfg + MOS_CFG_LABEL + MOS_LABEL_LEN, MOS_DATE_LEN);
	img->date[MOS_DATE_LEN] = '\0';
	for (i = MOS_LABEL_LEN - 1; i >= 0 && img->label[i] == ' '; i--)
		img->label[i] = '\0';
	for (i = MOS_DATE_LEN - 1; i >= 0 && img->date[i] == ' '; i--)
		img->date[i] = '\0';
}

/*
 * Follow the cluster chain of one file.  A chain ends in 0xc0 + n, where n is
 * the number of sectors actually used in the last cluster (n may be 0 for a
 * file that was created but never written).
 */
static void walk_chain(const mos_image *img, mos_file *file)
{
	const mos_format *f = img->fmt;
	uint8_t *seen;
	int cap = img->nclusters + 1;
	int cur = file->start;
	uint8_t v;

	file->chain = NULL;
	file->chain_len = 0;
	file->last_sectors = 0;
	file->sectors = 0;
	file->size = 0;

	if (cur >= img->nclusters) {
		file->error = "start cluster outside disk";
		return;
	}
	if ((seen = calloc((size_t)img->nclusters, 1)) == NULL ||
	    (file->chain = malloc((size_t)cap)) == NULL) {
		free(seen);
		file->error = "out of memory";
		return;
	}

	for (;;) {
		if (seen[cur]) {
			file->error = "cyclic cluster chain";
			break;
		}
		seen[cur] = 1;
		file->chain[file->chain_len++] = (uint8_t)cur;

		v = img->fat[cur];
		if (!img->fat_ok[cur]) {
			file->error = "illegal FAT entry in chain";
			break;
		}
		if (v < img->nclusters) {
			cur = v;
			continue;
		}
		if ((v & MOS_FAT_LAST_MASK) == MOS_FAT_LAST) {
			file->last_sectors = v - MOS_FAT_LAST;
			break;
		}
		if (v == MOS_FAT_FREE)
			file->error = "chain runs into free cluster";
		else
			file->error = "chain runs into reserved cluster";
		break;
	}
	free(seen);

	if (file->error != NULL)
		return;
	file->sectors = (long)(file->chain_len - 1) * f->cluster_sectors +
	                file->last_sectors;
	file->size = file->sectors * f->sector_size;
}

static void read_dir(mos_image *img, unsigned flags)
{
	const mos_format *f = img->fmt;
	int per_sector = f->sector_size / MOS_DIRENT_SIZE;
	int max = f->dir_sectors * per_sector;
	int s, e;

	img->files = calloc((size_t)max, sizeof *img->files);
	if (img->files == NULL)
		return;

	for (s = 0; s < f->dir_sectors; s++) {
		const uint8_t *sec = dir_sector(img, s);

		for (e = 0; e < per_sector; e++) {
			const uint8_t *raw = sec + e * MOS_DIRENT_SIZE;
			mos_file *file;
			int i;

			if (raw[0] == 0xff) {          /* never used */
				if (!(flags & MOS_SCAN_ALL)) {
					img->dir_truncated = 1;
					return;
				}
				continue;
			}
			if (raw[0] == 0x00 && !(flags & MOS_KEEP_DELETED))
				continue;

			file = &img->files[img->nfiles];
			memcpy(file->raw, raw, MOS_DIRENT_SIZE);
			file->slot = s * per_sector + e;
			file->deleted = (raw[0] == 0x00);
			file->attr = raw[9];
			file->start = raw[10];

			for (i = 0; i < MOS_NAME_LEN; i++) {
				uint8_t c = raw[i];

				file->name[i] = (i == 0 && file->deleted) ? '?' :
				                (c < 0x20 ? '.' : (char)c);
			}
			file->name[MOS_NAME_LEN] = '\0';
			for (i = MOS_NAME_LEN - 1; i >= 0 && file->name[i] == ' '; i--)
				file->name[i] = '\0';

			walk_chain(img, file);
			img->nfiles++;
		}
	}
}

int mos_open(mos_image *img, const char *path, const mos_format *fmt,
             unsigned flags, char *err, size_t errlen)
{
	memset(img, 0, sizeof *img);

	if (read_image(img, path, err, errlen) != 0) {
		mos_close(img);
		return -1;
	}
	if (fmt == NULL && (fmt = mos_format_detect(img->size)) == NULL) {
		snprintf(err, errlen,
		         "%s: cannot guess format from size %ld bytes, use -f",
		         path, img->size);
		mos_close(img);
		return -1;
	}
	if (format_size(fmt) != img->size) {
		snprintf(err, errlen,
		         "%s: size %ld bytes does not match format %s (%ld bytes)",
		         path, img->size, fmt->name, format_size(fmt));
		mos_close(img);
		return -1;
	}
	img->fmt = fmt;
	img->nclusters = (int)(((long)fmt->tracks * fmt->heads * fmt->sectors) /
	                       fmt->cluster_sectors);
	if (img->nclusters > 256) {
		snprintf(err, errlen, "format %s needs more than 256 clusters",
		         fmt->name);
		mos_close(img);
		return -1;
	}
	if ((img->path = strdup(path)) == NULL) {
		snprintf(err, errlen, "out of memory");
		mos_close(img);
		return -1;
	}

	read_fat(img);
	read_config(img);
	read_dir(img, flags);
	if (img->files == NULL) {
		snprintf(err, errlen, "out of memory reading directory");
		mos_close(img);
		return -1;
	}
	return 0;
}

void mos_close(mos_image *img)
{
	int i;

	for (i = 0; i < img->nfiles; i++)
		free(img->files[i].chain);
	free(img->files);
	free(img->data);
	free(img->secstatus);
	free(img->path);
	memset(img, 0, sizeof *img);
}

int mos_read_file(const mos_image *img, const mos_file *file,
                  uint8_t **buf, long *len, char *err, size_t errlen)
{
	const mos_format *f = img->fmt;
	uint8_t *out;
	long pos = 0;
	int i;

	if (file->error != NULL) {
		snprintf(err, errlen, "%s: %s", file->name, file->error);
		return -1;
	}
	*len = file->size;
	if ((out = malloc((size_t)(file->size > 0 ? file->size : 1))) == NULL) {
		snprintf(err, errlen, "out of memory");
		return -1;
	}
	for (i = 0; i < file->chain_len; i++) {
		int nsec = (i == file->chain_len - 1) ? file->last_sectors
		                                     : f->cluster_sectors;
		long lsn = (long)file->chain[i] * f->cluster_sectors;
		long bytes = (long)nsec * f->sector_size;

		if (lsn_offset(img, lsn) + bytes > img->size) {
			snprintf(err, errlen, "%s: cluster %d beyond end of image",
			         file->name, file->chain[i]);
			free(out);
			return -1;
		}
		memcpy(out + pos, img->data + lsn_offset(img, lsn), (size_t)bytes);
		pos += bytes;
	}
	*buf = out;
	return 0;
}

long mos_cluster_size(const mos_image *img)
{
	return (long)img->fmt->cluster_sectors * img->fmt->sector_size;
}

const uint8_t *mos_cluster(const mos_image *img, int cluster)
{
	long off;

	if (cluster < 0 || cluster >= img->nclusters)
		return NULL;
	off = (long)cluster * mos_cluster_size(img);
	if (off + mos_cluster_size(img) > img->size)
		return NULL;
	return img->data + off;
}

/* True when the cluster overlaps the track holding directory, FAT and config. */
int mos_cluster_is_meta(const mos_image *img, int cluster)
{
	const mos_format *f = img->fmt;
	long first = (long)cluster * f->cluster_sectors;
	long last = first + f->cluster_sectors - 1;
	long dir_first = (long)f->dir_track * f->sectors;

	return last >= dir_first && first <= dir_first + f->sectors - 1;
}

int mos_file_bad_sectors(const mos_image *img, const mos_file *file)
{
	const mos_format *f = img->fmt;
	int i, j, bad = 0;

	if (img->secstatus == NULL || file->error != NULL)
		return 0;
	for (i = 0; i < file->chain_len; i++) {
		int nsec = (i == file->chain_len - 1) ? file->last_sectors
		                                     : f->cluster_sectors;
		long lsn = (long)file->chain[i] * f->cluster_sectors;

		for (j = 0; j < nsec; j++)
			if (img->secstatus[lsn + j] != MOS_SEC_OK)
				bad++;
	}
	return bad;
}

void mos_space(const mos_image *img, int *used, int *freecl, int *reserved,
               int *lost, int *unknown)
{
	uint8_t *owned;
	int i, j;

	*used = *freecl = *reserved = *lost = 0;
	*unknown = img->nclusters - img->fat_len;
	owned = calloc((size_t)img->nclusters, 1);

	for (i = 0; i < img->nfiles; i++) {
		const mos_file *file = &img->files[i];

		if (file->deleted)
			continue;
		for (j = 0; j < file->chain_len; j++)
			if (owned != NULL && file->chain[j] < img->nclusters)
				owned[file->chain[j]] = 1;
	}
	for (i = 0; i < img->fat_len; i++) {
		if (img->fat[i] == MOS_FAT_FREE)
			(*freecl)++;
		else if (img->fat[i] == MOS_FAT_RESERVED)
			(*reserved)++;
		else {
			(*used)++;
			if (owned != NULL && !owned[i])
				(*lost)++;
		}
	}
	free(owned);
}

int mos_overlaps(const mos_image *img)
{
	int *owner;
	int i, j, count = 0;

	if ((owner = calloc((size_t)img->nclusters, sizeof *owner)) == NULL)
		return 0;
	for (i = 0; i < img->nfiles; i++) {
		const mos_file *file = &img->files[i];

		if (file->deleted)
			continue;
		for (j = 0; j < file->chain_len; j++) {
			int c = file->chain[j];

			if (c >= img->nclusters)
				continue;
			if (owner[c] == 0)
				owner[c] = i + 1;
			else if (owner[c] != i + 1)
				count++;
		}
	}
	free(owner);
	return count;
}

void mos_local_name(const mos_file *file, char *buf, size_t buflen)
{
	size_t i, n = 0;

	for (i = 0; file->name[i] != '\0' && n + 1 < buflen; i++) {
		char c = file->name[i];

		buf[n++] = (c == '/' || c == '\\') ? '_' : c;
	}
	buf[n] = '\0';
	if (n == 0)
		snprintf(buf, buflen, "_noname%02d", file->slot);
}

int mos_match(const char *pattern, const char *name, int casefold)
{
	char p[256], n[MOS_NAME_LEN + 1];
	size_t i;

	if (!casefold)
		return fnmatch(pattern, name, 0) == 0;

	for (i = 0; i + 1 < sizeof p && pattern[i] != '\0'; i++)
		p[i] = (char)toupper((unsigned char)pattern[i]);
	p[i] = '\0';
	for (i = 0; i + 1 < sizeof n && name[i] != '\0'; i++)
		n[i] = (char)toupper((unsigned char)name[i]);
	n[i] = '\0';

	return fnmatch(p, n, 0) == 0;
}

/* ------------------------------------------------------------------ */

long mos_text_len(const uint8_t *buf, long len)
{
	long i;

	for (i = 0; i < len; i++)
		if (buf[i] == 0x1a)
			return i;
	return len;
}

long mos_crlf_to_lf(uint8_t *buf, long len)
{
	long i, n = 0;

	for (i = 0; i < len; i++) {
		if (buf[i] == '\r' && i + 1 < len && buf[i + 1] == '\n')
			continue;
		buf[n++] = buf[i];
	}
	return n;
}

/*
 * DIN 66003 / ISO 646-DE: the alphatronic uses the German 7 bit variant, so
 * these seven positions carry umlauts instead of the ASCII punctuation.
 */
char *mos_de_to_utf8(const uint8_t *buf, long len, long *outlen)
{
	static const struct { uint8_t c; const char *utf8; } map[] = {
		{ 0x5b, "\xc3\x84" },  /* AE */
		{ 0x5c, "\xc3\x96" },  /* OE */
		{ 0x5d, "\xc3\x9c" },  /* UE */
		{ 0x7b, "\xc3\xa4" },  /* ae */
		{ 0x7c, "\xc3\xb6" },  /* oe */
		{ 0x7d, "\xc3\xbc" },  /* ue */
		{ 0x7e, "\xc3\x9f" }   /* sz */
	};
	char *out;
	long i, n = 0;
	size_t k;

	if ((out = malloc((size_t)len * 2 + 1)) == NULL)
		return NULL;
	for (i = 0; i < len; i++) {
		const char *rep = NULL;

		for (k = 0; k < sizeof map / sizeof map[0]; k++)
			if (buf[i] == map[k].c) {
				rep = map[k].utf8;
				break;
			}
		if (rep != NULL) {
			out[n++] = rep[0];
			out[n++] = rep[1];
		} else {
			out[n++] = (char)buf[i];
		}
	}
	out[n] = '\0';
	*outlen = n;
	return out;
}
