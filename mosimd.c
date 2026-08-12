/*
 * mosimd.c - decode ImageDisk (.imd) files into a flat sector image
 *
 * An IMD file keeps what a flat sector image cannot: the sector numbers as they
 * were read, and per sector whether the read succeeded.  Both matter here.  A
 * P2 system disk may carry a deliberately misnumbered sector on track 0 as copy
 * protection, and on a worn disk a sector that could not be read must not be
 * mistaken for a sector full of zeros.
 *
 * Part of MOStools.
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
#include "mosimd.h"
#include "mosfs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IMD_MAX_TRACKS   256
#define IMD_MAX_SECTORS   64

/* Sector data record types, as written by ImageDisk. */
enum {
	IMD_UNAVAILABLE       = 0,
	IMD_NORMAL            = 1,
	IMD_COMPRESSED        = 2,
	IMD_DELETED           = 3,
	IMD_DELETED_COMP      = 4,
	IMD_ERROR             = 5,
	IMD_ERROR_COMP        = 6,
	IMD_DELETED_ERROR     = 7,
	IMD_DELETED_ERROR_COMP = 8
};

/* one physical track as it appears in the file */
typedef struct {
	int cyl, head, nsec, sector_size;
	uint8_t smap[IMD_MAX_SECTORS];
	uint8_t type[IMD_MAX_SECTORS];
	const uint8_t *data[IMD_MAX_SECTORS];  /* NULL when compressed/absent */
	uint8_t fill[IMD_MAX_SECTORS];         /* fill byte when compressed   */
} imd_track;

int mos_imd_detect(const uint8_t *buf, long len)
{
	long i;

	if (len < 8 || memcmp(buf, "IMD", 3) != 0)
		return 0;
	for (i = 0; i < len && i < 1024; i++)   /* comment ends in 0x1a */
		if (buf[i] == 0x1a)
			return 1;
	return 0;
}

static int record_size(int type, int sector_size)
{
	switch (type) {
	case IMD_UNAVAILABLE:
		return 0;
	case IMD_COMPRESSED:
	case IMD_DELETED_COMP:
	case IMD_ERROR_COMP:
	case IMD_DELETED_ERROR_COMP:
		return 1;
	default:
		return sector_size;
	}
}

static int status_of(int type)
{
	switch (type) {
	case IMD_UNAVAILABLE:
		return MOS_SEC_MISSING;
	case IMD_ERROR:
	case IMD_ERROR_COMP:
		return MOS_SEC_ERROR;
	case IMD_DELETED:
	case IMD_DELETED_COMP:
		return MOS_SEC_DELETED;
	case IMD_DELETED_ERROR:
	case IMD_DELETED_ERROR_COMP:
		return MOS_SEC_ERROR;
	default:
		return MOS_SEC_OK;
	}
}

/* Read all track records.  Returns the number of tracks, or -1. */
static int parse_tracks(const uint8_t *buf, long len, imd_track *tracks,
                        int maxtracks, char *err, size_t errlen)
{
	long p = 0;
	int n = 0;

	while (p < len && buf[p] != 0x1a)       /* skip the ASCII header */
		p++;
	if (p >= len) {
		snprintf(err, errlen, "IMD: header is not terminated");
		return -1;
	}
	p++;

	while (p < len) {
		imd_track *t;
		int flags, code, i;

		if (p + 5 > len) {
			snprintf(err, errlen, "IMD: truncated track header at offset %ld",
			         p);
			return -1;
		}
		if (n >= maxtracks) {
			snprintf(err, errlen, "IMD: more than %d tracks", maxtracks);
			return -1;
		}
		t = &tracks[n];
		/* buf[p] is the recording mode, which does not affect the data */
		t->cyl = buf[p + 1];
		flags = buf[p + 2];
		t->head = flags & 0x0f;
		t->nsec = buf[p + 3];
		code = buf[p + 4];
		p += 5;

		if (code > 6) {
			snprintf(err, errlen, "IMD: bad sector size code %d on track %d",
			         code, t->cyl);
			return -1;
		}
		t->sector_size = 128 << code;
		if (t->nsec > IMD_MAX_SECTORS) {
			snprintf(err, errlen, "IMD: %d sectors on track %d, at most %d"
			         " supported", t->nsec, t->cyl, IMD_MAX_SECTORS);
			return -1;
		}
		if (p + t->nsec > len) {
			snprintf(err, errlen, "IMD: truncated sector map on track %d",
			         t->cyl);
			return -1;
		}
		memcpy(t->smap, buf + p, (size_t)t->nsec);
		p += t->nsec;
		if (flags & 0x80)                       /* per sector cylinder map */
			p += t->nsec;
		if (flags & 0x40)                       /* per sector head map     */
			p += t->nsec;
		/* An empty track record may legitimately end the file. */
		if (p > len || (t->nsec > 0 && p >= len)) {
			snprintf(err, errlen, "IMD: truncated track %d", t->cyl);
			return -1;
		}

		for (i = 0; i < t->nsec; i++) {
			int type, size;

			if (p >= len) {
				snprintf(err, errlen, "IMD: truncated track %d", t->cyl);
				return -1;
			}
			type = buf[p++];
			if (type > IMD_DELETED_ERROR_COMP) {
				snprintf(err, errlen, "IMD: unknown sector type %d on track %d",
				         type, t->cyl);
				return -1;
			}
			size = record_size(type, t->sector_size);
			if (p + size > len) {
				snprintf(err, errlen, "IMD: truncated track %d", t->cyl);
				return -1;
			}
			t->type[i] = (uint8_t)type;
			if (size == 1) {
				t->data[i] = NULL;
				t->fill[i] = buf[p];
			} else if (size > 0) {
				t->data[i] = buf + p;
				t->fill[i] = 0;
			} else {
				t->data[i] = NULL;
				t->fill[i] = 0;
			}
			p += size;
		}
		n++;
	}
	return n;
}

int mos_imd_load(const uint8_t *buf, long len, mos_imd *out,
                 char *err, size_t errlen)
{
	imd_track *tracks;
	int ntracks, i, j;
	int maxsec = 0, ssize = 0, minsec = 255, maxcyl = 0;
	int odd = 0, head, nlogical;
	long total;

	memset(out, 0, sizeof *out);
	if ((tracks = calloc(IMD_MAX_TRACKS, sizeof *tracks)) == NULL) {
		snprintf(err, errlen, "out of memory");
		return -1;
	}
	ntracks = parse_tracks(buf, len, tracks, IMD_MAX_TRACKS, err, errlen);
	if (ntracks < 0) {
		free(tracks);
		return -1;
	}

	/* Which head carries the filesystem?  MOS disks are single sided. */
	head = -1;
	for (i = 0; i < ntracks; i++)
		if (tracks[i].nsec > 0) {
			out->heads_seen |= 1 << tracks[i].head;
			if (head < 0 || tracks[i].head < head)
				head = tracks[i].head;
		}
	if (head < 0) {
		snprintf(err, errlen, "IMD: no track holds any sector");
		free(tracks);
		return -1;
	}
	out->head = head;

	for (i = 0; i < ntracks; i++) {
		imd_track *t = &tracks[i];

		if (t->nsec == 0 || t->head != head)
			continue;
		if (ssize == 0)
			ssize = t->sector_size;
		else if (ssize != t->sector_size) {
			snprintf(err, errlen, "IMD: mixed sector sizes (%d and %d), not"
			         " supported", ssize, t->sector_size);
			free(tracks);
			return -1;
		}
		if (t->nsec > maxsec)
			maxsec = t->nsec;
		if (t->cyl > maxcyl)
			maxcyl = t->cyl;
		if (t->cyl & 1)
			odd = 1;
		for (j = 0; j < t->nsec; j++)
			if (t->smap[j] < minsec)
				minsec = t->smap[j];
	}

	/*
	 * A 40 track disk read in an 80 track drive without double stepping lands
	 * on the even cylinders only.  Collapse that so the filesystem sees the
	 * geometry it was written with.
	 */
	out->doublestep = (!odd && maxcyl > 43);
	out->sectors = maxsec;
	out->sector_size = ssize;
	out->first_sector = minsec;
	out->tracks = out->doublestep ? maxcyl / 2 + 1 : maxcyl + 1;
	nlogical = out->tracks;

	total = (long)nlogical * out->sectors * out->sector_size;
	if (total <= 0) {
		snprintf(err, errlen, "IMD: empty image");
		free(tracks);
		return -1;
	}
	if ((out->data = calloc(1, (size_t)total)) == NULL ||
	    (out->status = malloc((size_t)nlogical * (size_t)out->sectors)) == NULL) {
		snprintf(err, errlen, "out of memory");
		mos_imd_free(out);
		free(tracks);
		return -1;
	}
	out->size = total;
	memset(out->status, MOS_SEC_MISSING, (size_t)nlogical * (size_t)out->sectors);

	for (i = 0; i < ntracks; i++) {
		imd_track *t = &tracks[i];
		int lt;

		if (t->nsec == 0 || t->head != head)
			continue;
		lt = out->doublestep ? t->cyl / 2 : t->cyl;
		if (lt >= nlogical)
			continue;

		for (j = 0; j < t->nsec; j++) {
			int index = t->smap[j] - minsec;
			long lsn, off;

			if (index < 0 || index >= out->sectors) {
				out->outside++;         /* misnumbered, e.g. protection */
				continue;
			}
			lsn = (long)lt * out->sectors + index;
			off = lsn * out->sector_size;
			out->status[lsn] = (uint8_t)status_of(t->type[j]);
			if (t->data[j] != NULL)
				memcpy(out->data + off, t->data[j], (size_t)out->sector_size);
			else if (t->type[j] != IMD_UNAVAILABLE)
				memset(out->data + off, t->fill[j], (size_t)out->sector_size);
		}
	}

	for (i = 0; i < nlogical * out->sectors; i++)
		switch (out->status[i]) {
		case MOS_SEC_ERROR:   out->bad++; break;
		case MOS_SEC_DELETED: out->deleted++; break;
		case MOS_SEC_MISSING: out->missing++; break;
		default: break;
		}

	free(tracks);
	return 0;
}

void mos_imd_free(mos_imd *imd)
{
	free(imd->data);
	free(imd->status);
	imd->data = NULL;
	imd->status = NULL;
}
