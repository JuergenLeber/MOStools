/*
 * mosdsk.c - decode CPCEMU .dsk images into a flat sector image
 *
 * The .dsk container comes from the Amstrad CPC world but is what HxC and the
 * usual flux tools write for any FDC-format disk, so TA-PC 8 disks tend to
 * arrive in it.  Like IMD it keeps the sector numbers as they were read and
 * the FDC result status per sector, and unlike IMD it carries both sides of a
 * disk as a matter of course - which the double sided PC 8 media need.
 *
 * Two variants exist.  The original ("MV - CPCEMU...") gives every track the
 * same length; the extended one ("EXTENDED CPC DSK File") has a per track
 * length table and a real data length per sector, which is what makes
 * unformatted tracks and weak sectors expressible.  Both are read here.
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
#include "mosdsk.h"
#include "mosfs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DSK_MAX_TRACKS   256     /* cylinders x sides, as the header counts  */
#define DSK_MAX_SECTORS   64

#define DSK_HDR_SIZE     256     /* disk information block                   */
#define DSK_TRK_SIZE     256     /* track information block                  */

/* Offsets in the disk information block. */
#define DSK_CREATOR     0x22
#define DSK_TRACKS      0x30
#define DSK_SIDES       0x31
#define DSK_TRACKSIZE   0x32     /* original variant: one size for all       */
#define DSK_SIZETABLE   0x34     /* extended variant: one high byte each     */

/* Offsets in the track information block. */
#define DSK_TRK_CYL     0x10
#define DSK_TRK_HEAD    0x11
#define DSK_TRK_N       0x14     /* sector size code                         */
#define DSK_TRK_NSEC    0x15
#define DSK_TRK_LIST    0x18     /* sector information list, 8 bytes each    */
#define DSK_SEC_ENTRY      8

/*
 * FDC result bits worth acting on.  ST1 bit 5 and ST2 bit 5 are the CRC
 * errors, ST2 bit 6 is the deleted data address mark, and a missing address
 * mark or "no data" means the sector was never handed over at all.
 */
#define DSK_ST1_MISSING  0x01
#define DSK_ST1_NODATA   0x04
#define DSK_ST1_CRC      0x20
#define DSK_ST2_MISSING  0x01
#define DSK_ST2_CRC      0x20
#define DSK_ST2_DELETED  0x40

/* one physical track as it appears in the file */
typedef struct {
	int cyl, head, nsec, sector_size;
	uint8_t id[DSK_MAX_SECTORS];
	uint8_t st1[DSK_MAX_SECTORS];
	uint8_t st2[DSK_MAX_SECTORS];
	const uint8_t *data[DSK_MAX_SECTORS];  /* NULL when nothing was stored */
	int len[DSK_MAX_SECTORS];              /* bytes stored for this sector */
} dsk_track;

int mos_dsk_detect(const uint8_t *buf, long len)
{
	if (len < DSK_HDR_SIZE)
		return 0;
	if (memcmp(buf + 0x17, "Disk-Info", 9) != 0)
		return 0;
	return memcmp(buf, "MV - CPC", 8) == 0 || memcmp(buf, "EXTENDED", 8) == 0;
}

static int status_of(uint8_t st1, uint8_t st2, int stored)
{
	if (stored == 0 || (st1 & (DSK_ST1_MISSING | DSK_ST1_NODATA)) != 0 ||
	    (st2 & DSK_ST2_MISSING) != 0)
		return MOS_SEC_MISSING;
	if ((st1 & DSK_ST1_CRC) != 0 || (st2 & DSK_ST2_CRC) != 0)
		return MOS_SEC_ERROR;    /* a deleted mark plus CRC counts as error */
	if ((st2 & DSK_ST2_DELETED) != 0)
		return MOS_SEC_DELETED;
	return MOS_SEC_OK;
}

/* Read all track information blocks.  Returns the number of tracks, or -1. */
static int parse_tracks(const uint8_t *buf, long len, int extended,
                        dsk_track *tracks, int maxtracks, char *err,
                        size_t errlen)
{
	long p = DSK_HDR_SIZE;
	int nentries = buf[DSK_TRACKS] * buf[DSK_SIDES];
	int fixed = buf[DSK_TRACKSIZE] | (buf[DSK_TRACKSIZE + 1] << 8);
	int e, n = 0;

	/* The extended track size table has to fit in the information block. */
	if (extended && nentries > DSK_HDR_SIZE - DSK_SIZETABLE) {
		snprintf(err, errlen, "DSK: %d track entries, more than the size table"
		         " can hold", nentries);
		return -1;
	}
	if (nentries > maxtracks) {
		snprintf(err, errlen, "DSK: %d track entries, at most %d supported",
		         nentries, maxtracks);
		return -1;
	}

	for (e = 0; e < nentries; e++) {
		dsk_track *t = &tracks[n];
		long tsize = extended ? (long)buf[DSK_SIZETABLE + e] * 256 : fixed;
		const uint8_t *th;
		long q;
		int i;

		if (tsize == 0)
			continue;               /* extended: unformatted track      */
		if (tsize < DSK_TRK_SIZE || p + tsize > len) {
			snprintf(err, errlen, "DSK: truncated track %d at offset %ld", e, p);
			return -1;
		}
		th = buf + p;
		p += tsize;
		if (memcmp(th, "Track-Info", 10) != 0) {
			/*
			 * The original variant has no size table, so an unformatted
			 * track still takes its slot; writers that do not bother with
			 * a header for it leave the slot blank.  Anything else is a
			 * file we have lost our place in.
			 */
			for (i = 0; i < DSK_TRK_SIZE && th[i] == 0; i++)
				;
			if (i == DSK_TRK_SIZE)
				continue;
			snprintf(err, errlen, "DSK: track %d has no Track-Info block", e);
			return -1;
		}
		t->cyl = th[DSK_TRK_CYL];
		t->head = th[DSK_TRK_HEAD];
		t->nsec = th[DSK_TRK_NSEC];
		if (th[DSK_TRK_N] > 6) {
			snprintf(err, errlen, "DSK: bad sector size code %d on track %d",
			         th[DSK_TRK_N], t->cyl);
			return -1;
		}
		t->sector_size = 128 << th[DSK_TRK_N];
		if (t->nsec > DSK_MAX_SECTORS) {
			snprintf(err, errlen, "DSK: %d sectors on track %d, at most %d"
			         " supported", t->nsec, t->cyl, DSK_MAX_SECTORS);
			return -1;
		}
		if (DSK_TRK_LIST + (long)t->nsec * DSK_SEC_ENTRY > DSK_TRK_SIZE) {
			snprintf(err, errlen, "DSK: sector list of track %d does not fit"
			         " its information block", t->cyl);
			return -1;
		}

		/* Sector data follows the information block in list order. */
		q = DSK_TRK_SIZE;
		for (i = 0; i < t->nsec; i++) {
			const uint8_t *si = th + DSK_TRK_LIST + i * DSK_SEC_ENTRY;
			long dlen = extended ? (si[6] | (si[7] << 8)) : t->sector_size;

			t->id[i] = si[2];
			t->st1[i] = si[4];
			t->st2[i] = si[5];
			if (q + dlen > tsize) {
				snprintf(err, errlen, "DSK: sector data of track %d runs past"
				         " the track", t->cyl);
				return -1;
			}
			t->len[i] = (int)dlen;
			t->data[i] = dlen > 0 ? th + q : NULL;
			q += dlen;
		}
		n++;
	}
	return n;
}

int mos_dsk_load(const uint8_t *buf, long len, int want_heads, mos_dsk *out,
                 char *err, size_t errlen)
{
	dsk_track *tracks;
	int ntracks, i, j;
	int maxsec = 0, ssize = 0, minsec = 255, maxcyl = 0, maxhead = 0;
	long total;

	memset(out, 0, sizeof *out);
	out->extended = memcmp(buf, "EXTENDED", 8) == 0;
	for (i = 0; i < (int)sizeof out->creator - 1; i++)
		out->creator[i] = (char)buf[DSK_CREATOR + i];
	out->creator[i] = '\0';
	for (i = (int)strlen(out->creator) - 1;
	     i >= 0 && (out->creator[i] == ' ' || out->creator[i] < 0x20); i--)
		out->creator[i] = '\0';

	if ((tracks = calloc(DSK_MAX_TRACKS, sizeof *tracks)) == NULL) {
		snprintf(err, errlen, "out of memory");
		return -1;
	}
	ntracks = parse_tracks(buf, len, out->extended, tracks, DSK_MAX_TRACKS,
	                       err, errlen);
	if (ntracks < 0) {
		free(tracks);
		return -1;
	}

	for (i = 0; i < ntracks; i++) {
		dsk_track *t = &tracks[i];

		if (t->nsec == 0)
			continue;
		out->heads_seen |= 1 << t->head;
		if (ssize == 0)
			ssize = t->sector_size;
		else if (ssize != t->sector_size) {
			snprintf(err, errlen, "DSK: mixed sector sizes (%d and %d), not"
			         " supported", ssize, t->sector_size);
			free(tracks);
			return -1;
		}
		if (t->nsec > maxsec)
			maxsec = t->nsec;
		if (t->cyl > maxcyl)
			maxcyl = t->cyl;
		if (t->head > maxhead)
			maxhead = t->head;
		for (j = 0; j < t->nsec; j++)
			if (t->id[j] < minsec)
				minsec = t->id[j];
	}
	if (ssize == 0) {
		snprintf(err, errlen, "DSK: no track holds any sector");
		free(tracks);
		return -1;
	}

	/*
	 * A side the writer declared but never filled with sectors is dropped;
	 * asking for fewer sides than the image holds decodes only the first
	 * ones, which is how a single sided disk captured on a double sided
	 * drive is read back.
	 */
	out->heads = maxhead + 1;
	if (want_heads > 0 && want_heads < out->heads)
		out->heads = want_heads;
	out->tracks = maxcyl + 1;
	out->sectors = maxsec;
	out->sector_size = ssize;
	out->first_sector = minsec;

	total = (long)out->tracks * out->heads * out->sectors * out->sector_size;
	if ((out->data = calloc(1, (size_t)total)) == NULL ||
	    (out->status = malloc((size_t)(total / out->sector_size))) == NULL) {
		snprintf(err, errlen, "out of memory");
		mos_dsk_free(out);
		free(tracks);
		return -1;
	}
	out->size = total;
	memset(out->status, MOS_SEC_MISSING, (size_t)(total / out->sector_size));

	for (i = 0; i < ntracks; i++) {
		dsk_track *t = &tracks[i];

		if (t->nsec == 0 || t->head >= out->heads || t->cyl >= out->tracks)
			continue;

		for (j = 0; j < t->nsec; j++) {
			int index = t->id[j] - minsec;
			int stored = t->len[j];
			long lsn, off;

			if (index < 0 || index >= out->sectors) {
				out->outside++;         /* misnumbered, e.g. protection */
				continue;
			}
			/*
			 * A sector stored several times over is a weak one - the tool
			 * read it repeatedly and got different data.  Take the first
			 * copy and say so; nothing here can tell which one is right.
			 */
			if (stored > out->sector_size) {
				out->weak++;
				stored = out->sector_size;
			}
			lsn = ((long)t->cyl * out->heads + t->head) * out->sectors + index;
			off = lsn * out->sector_size;
			out->status[lsn] = (uint8_t)status_of(t->st1[j], t->st2[j], stored);
			if (t->data[j] != NULL)
				memcpy(out->data + off, t->data[j], (size_t)stored);
		}
	}

	for (i = 0; i < (int)(total / out->sector_size); i++)
		switch (out->status[i]) {
		case MOS_SEC_ERROR:   out->bad++; break;
		case MOS_SEC_DELETED: out->deleted++; break;
		case MOS_SEC_MISSING: out->missing++; break;
		default: break;
		}

	free(tracks);
	return 0;
}

void mos_dsk_free(mos_dsk *dsk)
{
	free(dsk->data);
	free(dsk->status);
	dsk->data = NULL;
	dsk->status = NULL;
}
