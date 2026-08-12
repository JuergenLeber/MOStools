/*
 * mosimd.h - decode ImageDisk (.imd) files into a flat sector image
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
#ifndef MOSIMD_H
#define MOSIMD_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
	uint8_t *data;         /* flat image, malloc()ed                       */
	long     size;
	uint8_t *status;       /* one MOS_SEC_* byte per sector, malloc()ed    */
	int      tracks;       /* logical tracks after collapsing double steps */
	int      sectors;      /* sectors per track                            */
	int      sector_size;
	int      head;         /* head that was decoded                        */
	int      heads_seen;
	int      doublestep;   /* image held only even cylinders               */
	int      first_sector; /* lowest sector number in the maps             */
	int      bad;          /* sectors read with a data error               */
	int      deleted;      /* sectors with a deleted address mark          */
	int      missing;      /* sectors absent from the image                */
	int      outside;      /* physical sectors outside the logical layout  */
} mos_imd;

/* Does this buffer look like an ImageDisk file? */
int  mos_imd_detect(const uint8_t *buf, long len);

/* Decode into a flat image.  Returns 0, or -1 with a message in err. */
int  mos_imd_load(const uint8_t *buf, long len, mos_imd *out,
                  char *err, size_t errlen);

void mos_imd_free(mos_imd *imd);

#endif /* MOSIMD_H */
