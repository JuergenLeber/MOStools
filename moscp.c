/*
 * moscp - copy files out of an alphatronic P2 MOS disk image
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

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *progname = "moscp";

static void usage(FILE *fp, int status)
{
	const mos_format *f;
	int i;

	fprintf(fp, "usage: %s [-t] [-u] [-o] [-S] [-f format] image pattern ..."
	            " destdir\n"
	            "       %s [-t] [-u] [-o] [-S] [-f format] image file destfile\n",
	        progname, progname);
	fprintf(fp, "\n"
	        "  -t          text mode: stop at the 0x1a end-of-file mark and\n"
	        "              convert CR LF line ends to LF\n"
	        "  -u          translate German 7 bit characters (DIN 66003) to UTF-8\n"
	        "  -o          overwrite existing local files\n"
	        "  -S          match patterns case sensitively\n"
	        "  -f format   force a disk format instead of guessing by size\n"
	        "  -d          also consider deleted directory entries\n"
	        "  -V          print version and exit\n"
	        "  -h          print this help and exit\n"
	        "\n"
	        "A destination of '-' writes to standard output.\n"
	        "Patterns are matched case insensitively unless -S is given; quote\n"
	        "them to keep the shell away from '*' and from spaces in names.\n"
	        "\nformats:\n");
	for (i = 0; (f = mos_format_at(i)) != NULL; i++)
		fprintf(fp, "  %-8s %s%s\n", f->name, f->desc,
		        f->verified ? "" : " (unverified)");
	exit(status);
}

static int is_dir(const char *path)
{
	struct stat st;

	return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static int write_out(const char *path, const uint8_t *buf, long len,
                     int overwrite)
{
	FILE *fp;

	if (strcmp(path, "-") == 0) {
		if (len > 0 && fwrite(buf, 1, (size_t)len, stdout) != (size_t)len) {
			fprintf(stderr, "%s: stdout: %s\n", progname, strerror(errno));
			return -1;
		}
		return 0;
	}
	if (!overwrite && access(path, F_OK) == 0) {
		fprintf(stderr, "%s: %s: exists, use -o to overwrite\n", progname, path);
		return -1;
	}
	if ((fp = fopen(path, "wb")) == NULL) {
		fprintf(stderr, "%s: %s: %s\n", progname, path, strerror(errno));
		return -1;
	}
	if (len > 0 && fwrite(buf, 1, (size_t)len, fp) != (size_t)len) {
		fprintf(stderr, "%s: %s: %s\n", progname, path, strerror(errno));
		fclose(fp);
		return -1;
	}
	if (fclose(fp) != 0) {
		fprintf(stderr, "%s: %s: %s\n", progname, path, strerror(errno));
		return -1;
	}
	return 0;
}

static int copy_one(const mos_image *img, const mos_file *file,
                    const char *dest, int destdir, int text, int utf8,
                    int overwrite)
{
	char path[1024], name[MOS_NAME_LEN + 1];
	uint8_t *buf;
	char *conv = NULL;
	long len;
	char err[512];
	int rc, bad;

	if (mos_read_file(img, file, &buf, &len, err, sizeof err) != 0) {
		fprintf(stderr, "%s: %s\n", progname, err);
		return -1;
	}
	if ((bad = mos_file_bad_sectors(img, file)) > 0)
		fprintf(stderr, "%s: %s: %d sector(s) were not read cleanly, the copy"
		        " is incomplete\n", progname, file->name, bad);
	/* Only attribute 0x00 is a MOS BASIC data file.  A tokenised BASIC
	 * program is binary: cutting it at the first 0x1a or rewriting line ends
	 * and umlauts would corrupt it. */
	if (file->attr != MOS_ATTR_DATA && (text || utf8)) {
		fprintf(stderr, "%s: %s: attribute %02x, not a data file,"
		        " copied in binary mode\n", progname, file->name, file->attr);
		text = utf8 = 0;
	}
	if (text) {
		len = mos_text_len(buf, len);
		len = mos_crlf_to_lf(buf, len);
	}
	if (utf8) {
		long clen;

		if ((conv = mos_de_to_utf8(buf, len, &clen)) == NULL) {
			fprintf(stderr, "%s: out of memory\n", progname);
			free(buf);
			return -1;
		}
		free(buf);
		buf = (uint8_t *)conv;
		len = clen;
	}

	if (destdir) {
		mos_local_name(file, name, sizeof name);
		snprintf(path, sizeof path, "%s/%s", dest, name);
	} else {
		snprintf(path, sizeof path, "%s", dest);
	}
	rc = write_out(path, buf, len, overwrite);
	free(buf);
	return rc != 0 ? -1 : bad;      /* > 0: written, but data is incomplete */
}

int main(int argc, char **argv)
{
	const mos_format *fmt = NULL;
	unsigned flags = 0;
	int text = 0, utf8 = 0, overwrite = 0, casefold = 1;
	int opt, i, i2, j, destdir, npatterns, copied = 0, rc = 0;
	const char *image, *dest;
	char err[512];
	mos_image img;

	if (argv[0] != NULL && argv[0][0] != '\0')
		progname = argv[0];

	while ((opt = getopt(argc, argv, "tuodSf:Vh")) != -1) {
		switch (opt) {
		case 't': text = 1; break;
		case 'u': utf8 = 1; break;
		case 'o': overwrite = 1; break;
		case 'S': casefold = 0; break;
		case 'd': flags |= MOS_KEEP_DELETED; break;
		case 'f':
			if ((fmt = mos_format_find(optarg)) == NULL) {
				fprintf(stderr, "%s: unknown format '%s'\n", progname, optarg);
				return 1;
			}
			break;
		case 'V':
			printf("moscp (MOStools) %s\n", MOS_VERSION);
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
	if (argc - optind < 3)
		usage(stderr, 1);

	image = argv[optind];
	dest = argv[argc - 1];
	npatterns = argc - optind - 2;
	destdir = is_dir(dest);

	if (!destdir && npatterns > 1) {
		fprintf(stderr, "%s: %s: not a directory\n", progname, dest);
		return 1;
	}
	if (mos_open(&img, image, fmt, flags, err, sizeof err) != 0) {
		fprintf(stderr, "%s: %s\n", progname, err);
		return 1;
	}

	for (j = 0; j < npatterns; j++) {
		const char *pattern = argv[optind + 1 + j];
		int found = 0;

		for (i = 0; i < img.nfiles; i++) {
			const mos_file *file = &img.files[i];

			if (!mos_match(pattern, file->name, casefold))
				continue;
			found++;
			if (!destdir && found > 1) {
				fprintf(stderr, "%s: pattern '%s' matches more than one file,"
				        " use a destination directory or -S\n", progname, pattern);
				rc = 1;
				break;
			}
			i2 = copy_one(&img, file, dest, destdir, text, utf8, overwrite);
			if (i2 < 0) {
				rc = 1;
			} else {
				copied++;
				if (i2 > 0)
					rc = 1;         /* copied, but not everything was read */
			}
		}
		if (found == 0) {
			fprintf(stderr, "%s: %s: no such file on %s\n", progname, pattern,
			        image);
			rc = 1;
		}
	}

	if (copied == 0 && rc == 0)
		rc = 1;
	mos_close(&img);
	return rc;
}
