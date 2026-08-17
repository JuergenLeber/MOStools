/*
 * mosls - list the contents of an alphatronic MOS disk image
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *progname = "mosls";

static void usage(FILE *fp, int status)
{
	const mos_format *f;
	int i;

	fprintf(fp, "usage: %s [-l] [-c] [-d] [-a] [-v] [-S] [-f format] image"
	        " [pattern ...]\n", progname);
	fprintf(fp, "\n"
	        "  -l          long listing (attribute, cluster, size)\n"
	        "  -c          show the cluster chain of every file\n"
	        "  -d          include deleted directory entries\n"
	        "  -a          scan the whole directory area, ignoring the end marker\n"
	        "  -v          show disk label, autostart command and space usage\n"
	        "  -S          match patterns case sensitively\n"
	        "  -f format   force a disk format instead of guessing by size\n"
	        "  -V          print version and exit\n"
	        "  -h          print this help and exit\n"
	        "\nformats:\n");
	for (i = 0; (f = mos_format_at(i)) != NULL; i++)
		fprintf(fp, "  %-8s %s%s\n", f->name, f->desc,
		        f->verified ? "" : " (unverified)");
	exit(status);
}

static void print_chain(const mos_file *file)
{
	int i;

	printf("    clusters:");
	for (i = 0; i < file->chain_len; i++)
		printf(" %02x", file->chain[i]);
	printf("\n");
}

static void print_verbose_header(const mos_image *img)
{
	const mos_format *f = img->fmt;
	int used, freecl, reserved, lost, unknown, i;

	mos_space(img, &used, &freecl, &reserved, &lost, &unknown);

	printf("Image:      %s (%ld bytes)\n", img->path, img->size);
	printf("Source:     %s\n", img->source);
	printf("Format:     %s - %s\n", f->name, f->desc);
	if (f->heads > 1)
		printf("Geometry:   %d tracks x %d sides x %d sectors x %d bytes,"
		       " %d sectors/cluster, %d clusters\n", f->tracks, f->heads,
		       f->sectors, f->sector_size, f->cluster_sectors, img->nclusters);
	else
		printf("Geometry:   %d tracks x %d sectors x %d bytes,"
		       " %d sectors/cluster, %d clusters\n", f->tracks, f->sectors,
		       f->sector_size, f->cluster_sectors, img->nclusters);
	if (f->heads > 1)
		printf("Directory:  track %d side %d, sectors %d-%d\n", f->dir_track,
		       f->dir_head, f->dir_sector, f->dir_sector + f->dir_sectors - 1);
	else
		printf("Directory:  track %d, sectors %d-%d\n", f->dir_track,
		       f->dir_sector, f->dir_sector + f->dir_sectors - 1);
	if (img->label[0] != '\0' || img->date[0] != '\0')
		printf("Label:      %s%s%s\n", img->label,
		       img->date[0] != '\0' ? "  " : "", img->date);
	if (img->autostart[0] != '\0')
		printf("Autostart:  %s\n", img->autostart);
	if (img->cfg_drives != 0 || img->cfg_files != 0)
		printf("Boot cfg:   %d drive(s), %d file(s)\n", img->cfg_drives,
		       img->cfg_files);
	printf("Space:      %d clusters used, %d free (%ld bytes), %d reserved\n",
	       used, freecl, (long)freecl * f->cluster_sectors * f->sector_size,
	       reserved);
	if (lost > 0)
		printf("Warning:    %d allocated cluster(s) not owned by any file\n",
		       lost);
	if (unknown > 0)
		printf("Warning:    FAT area ends at cluster %02x (junk from %02x on),"
		       " %d cluster(s) unaccounted for\n", img->fat_len,
		       img->fat_first_invalid, unknown);
	if (img->fat_mismatch)
		printf("Warning:    FAT copies disagree, majority vote used\n");
	if (img->sec_error + img->sec_missing + img->sec_deleted > 0)
		printf("Warning:    %d sector(s) unreadable, %d missing, %d with a"
		       " deleted address mark\n", img->sec_error, img->sec_missing,
		       img->sec_deleted);
	if ((i = mos_overlaps(img)) > 0)
		printf("Warning:    %d cluster(s) claimed by more than one file; the"
		       " FAT does not\n            match the directory, affected files"
		       " extract as garbage\n", i);
	printf("\n");
}

int main(int argc, char **argv)
{
	const mos_format *fmt = NULL;
	unsigned flags = 0;
	int longfmt = 0, chains = 0, verbose = 0, casefold = 1;
	int opt, i, j, shown = 0, rc = 0;
	long total = 0;
	char err[512];
	mos_image img;

	if (argv[0] != NULL && argv[0][0] != '\0')
		progname = argv[0];

	while ((opt = getopt(argc, argv, "lcdavSf:Vh")) != -1) {
		switch (opt) {
		case 'l': longfmt = 1; break;
		case 'c': chains = 1; break;
		case 'd': flags |= MOS_KEEP_DELETED; break;
		case 'a': flags |= MOS_SCAN_ALL; break;
		case 'v': verbose = 1; break;
		case 'S': casefold = 0; break;
		case 'f':
			if ((fmt = mos_format_find(optarg)) == NULL) {
				fprintf(stderr, "%s: unknown format '%s'\n", progname, optarg);
				return 1;
			}
			break;
		case 'V':
			printf("mosls (MOStools) %s\n", MOS_VERSION);
			printf("License GPLv3+: GNU GPL version 3 or later"
			       " <https://gnu.org/licenses/gpl.html>\n"
			       "This is free software: you are free to change and"
			       " redistribute it.\nThere is NO WARRANTY, to the extent"
			       " permitted by law.\n");
			return 0;
		case 'h': usage(stdout, 0); break;
		default: usage(stderr, 1); break;
		}
	}
	if (optind >= argc)
		usage(stderr, 1);

	if (mos_open(&img, argv[optind], fmt, flags, err, sizeof err) != 0) {
		fprintf(stderr, "%s: %s\n", progname, err);
		return 1;
	}
	if (verbose)
		print_verbose_header(&img);
	if (longfmt)
		printf("attr start clst  sec     size  name\n");

	for (i = 0; i < img.nfiles; i++) {
		const mos_file *file = &img.files[i];
		char name[MOS_NAME_LEN + 1];
		int match = (optind + 1 >= argc);

		for (j = optind + 1; !match && j < argc; j++)
			match = mos_match(argv[j], file->name, casefold);
		if (!match)
			continue;

		snprintf(name, sizeof name, "%s", file->name);
		if (longfmt) {
			printf("  %02x    %02x  %4d %4ld %8ld  %s%s\n", file->attr,
			       file->start, file->chain_len, file->sectors, file->size,
			       name[0] != '\0' ? name : "(blank)",
			       file->deleted ? "  [deleted]" : "");
		} else {
			printf("%s%s\n", name[0] != '\0' ? name : "(blank)",
			       file->deleted ? "  [deleted]" : "");
		}
		if (file->error != NULL) {
			fprintf(stderr, "%s: %s: %s\n", progname,
			        name[0] != '\0' ? name : "(blank)", file->error);
			rc = 1;
		}
		if ((j = mos_file_bad_sectors(&img, file)) > 0) {
			fprintf(stderr, "%s: %s: %d sector(s) were not read cleanly\n",
			        progname, name[0] != '\0' ? name : "(blank)", j);
			rc = 1;
		}
		if (chains && file->chain_len > 0)
			print_chain(file);
		if (!file->deleted)
			total += file->size;
		shown++;
	}

	if (longfmt)
		printf("%d file(s), %ld bytes\n", shown, total);

	mos_close(&img);
	return rc;
}
