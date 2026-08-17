/*
 * mosdsk.h - decode CPCEMU .dsk images into a flat sector image
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
#ifndef MOSDSK_H
#define MOSDSK_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
	uint8_t *data;         /* flat image, malloc()ed                       */
	long     size;
	uint8_t *status;       /* one MOS_SEC_* byte per sector, malloc()ed    */
	int      tracks;       /* cylinders                                    */
	int      heads;        /* sides decoded                                */
	int      heads_seen;   /* bitmask of sides that carry sectors          */
	int      sectors;      /* sectors per track                            */
	int      sector_size;
	int      first_sector; /* lowest sector number in the maps             */
	int      extended;     /* "EXTENDED CPC DSK File" rather than "MV - "  */
	int      bad;          /* sectors the FDC flagged as a data error      */
	int      deleted;      /* sectors with a deleted address mark          */
	int      missing;      /* sectors absent from the image                */
	int      outside;      /* physical sectors outside the logical layout  */
	int      weak;         /* sectors stored more than once, first used    */
	char     creator[15];  /* header field, trailing blanks removed        */
} mos_dsk;

/* Does this buffer look like a CPCEMU disk image? */
int  mos_dsk_detect(const uint8_t *buf, long len);

/*
 * Decode into a flat image.  want_heads is the number of sides the caller
 * needs, or 0 to decode every side that carries sectors.  Returns 0, or -1
 * with a message in err.
 */
int  mos_dsk_load(const uint8_t *buf, long len, int want_heads, mos_dsk *out,
                  char *err, size_t errlen);

void mos_dsk_free(mos_dsk *dsk);

#endif /* MOSDSK_H */
