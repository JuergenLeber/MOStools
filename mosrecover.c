/*
 * mosrecover - rebuild the cluster chains of a MOS disk from the file data
 *
 * MOS keeps the allocation table in RAM and writes it back when the disk is
 * dismounted.  A disk pulled before that carries a stale FAT: the directory
 * and the data are current, the chains are not.  This tool ignores the FAT
 * and follows the files themselves instead.
 *
 * A tokenised BASIC program is a chain of lines, each starting with the
 * absolute address of the next line, so the only cluster that can follow the
 * current one is the cluster whose first bytes continue that chain.  A BASIC
 * data file is a sequence of quoted records separated by CR LF and terminated
 * by 0x1a, which constrains the continuation the same way.  Both give a scoring
 * function: append a candidate cluster, see how much further the file parses.
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

static const char *progname = "mosrecover";

/* result of parsing what we have so far */
enum { WALK_END, WALK_NEED_MORE, WALK_BROKEN };

typedef struct {
	int  status;
	long covered;   /* bytes that belong to the file for certain */
	long items;     /* BASIC lines, or text records */
} walk;

enum { KIND_UNKNOWN, KIND_BASIC, KIND_TEXT };

static const char *kind_name(int kind)
{
	return kind == KIND_BASIC ? "basic" : kind == KIND_TEXT ? "text" : "?";
}

/* ------------------------------------------------------------------ *
 * Microsoft BASIC: [link:2][line:2][tokens][00], a 0000 link ends it.
 * "base" is the load address the program was saved from, so a line at
 * file offset n has address base + n.
 * ------------------------------------------------------------------ */
static walk basic_walk(const uint8_t *b, long len, long base)
{
	walk w;
	long pos = 0, line = -1;

	w.status = WALK_BROKEN;
	w.covered = 0;
	w.items = 0;
	for (;;) {
		long link, ln, next;

		if (pos + 2 > len) {
			w.status = WALK_NEED_MORE;
			w.covered = pos;
			return w;
		}
		link = b[pos] | ((long)b[pos + 1] << 8);
		if (link == 0) {
			w.status = WALK_END;
			w.covered = pos + 2;
			return w;
		}
		if (pos + 4 > len) {
			w.status = WALK_NEED_MORE;
			w.covered = pos;
			return w;
		}
		ln = b[pos + 2] | ((long)b[pos + 3] << 8);
		next = link - base;
		if (next < pos + 5 || ln < line) {
			w.covered = pos;
			return w;                       /* structurally impossible */
		}
		if (next > len) {
			w.status = WALK_NEED_MORE;
			w.covered = pos;
			return w;                       /* line crosses the end    */
		}
		if (b[next - 1] != 0) {
			w.covered = pos;
			return w;                       /* lines end in 00         */
		}
		line = ln;
		pos = next;
		w.items++;
	}
}

/*
 * The load address is not stored in the file, so derive it: try every possible
 * length of the first line and keep the base that parses furthest.
 */
static long basic_base(const uint8_t *b, long len)
{
	long link, l, best_base = -1;
	walk best;

	best.status = WALK_BROKEN;
	best.covered = -1;
	best.items = -1;
	if (len < 6)
		return -1;
	link = b[0] | ((long)b[1] << 8);

	for (l = 5; l <= 256 && l <= len; l++) {
		long base = link - l;
		walk w;

		if (base <= 0 || b[l - 1] != 0)
			continue;
		w = basic_walk(b, len, base);
		if (w.items < 1)
			continue;
		if (w.items > best.items ||
		    (w.items == best.items && w.covered > best.covered)) {
			best = w;
			best_base = base;
		}
	}
	return best_base;
}

/* ------------------------------------------------------------------ *
 * BASIC data file: printable records separated by CR LF, ended by 0x1a.
 * Records are normally quoted, but not always - one sample disk holds a
 * record written as S" - so only printability and the line ends are
 * required here.
 * ------------------------------------------------------------------ */
static walk text_walk(const uint8_t *b, long len)
{
	walk w;
	long pos = 0;

	w.status = WALK_BROKEN;
	w.covered = 0;
	w.items = 0;
	for (;;) {
		long i;

		if (pos >= len) {
			w.status = WALK_NEED_MORE;
			w.covered = pos;
			return w;
		}
		if (b[pos] == 0x1a) {
			w.status = WALK_END;
			w.covered = pos + 1;
			return w;
		}
		for (i = pos; i < len && b[i] != '\r'; i++)
			if (b[i] < 0x20 || b[i] > 0x7e) {
				w.covered = pos;
				return w;               /* not text               */
			}
		if (i + 1 >= len) {
			w.status = WALK_NEED_MORE;
			w.covered = pos;
			return w;                       /* record crosses the end */
		}
		if (b[i + 1] != '\n') {
			w.covered = pos;
			return w;
		}
		pos = i + 2;
		w.items++;
	}
}

/* ------------------------------------------------------------------ */

typedef struct {
	int      kind;
	long     base;              /* BASIC load address, -1 for text        */
	uint8_t *data;              /* concatenated clusters                  */
	long     size;
	uint8_t *chain;
	int      chain_len;
	walk     result;
	int      choices;           /* steps decided by us, not by the FAT     */
} recovery;

static walk parse(const recovery *r, const uint8_t *b, long len)
{
	return r->kind == KIND_BASIC ? basic_walk(b, len, r->base)
	                             : text_walk(b, len);
}

/*
 * Decide what kind of file this is.  The directory attribute says what MOS
 * thinks it is; fall back to whichever parser locks on when that fails, since
 * on a damaged disk the attribute may be all we have - or may be wrong.
 */
static void classify(recovery *r, const mos_image *img, const mos_file *file)
{
	const uint8_t *c = mos_cluster(img, file->start);
	long csize = mos_cluster_size(img);
	walk wt, wb;
	long base;

	r->kind = KIND_UNKNOWN;
	r->base = -1;
	if (c == NULL)
		return;

	base = basic_base(c, csize);
	wt = text_walk(c, csize);
	memset(&wb, 0, sizeof wb);
	if (base > 0)
		wb = basic_walk(c, csize, base);

	if (file->attr == MOS_ATTR_DATA && wt.items >= 1) {
		r->kind = KIND_TEXT;
		return;
	}
	if ((file->attr & MOS_ATTR_PROGRAM) && wb.items >= 1) {
		r->kind = KIND_BASIC;
		r->base = base;
		return;
	}
	if (wb.items >= 2 || (wb.items >= 1 && wt.items < 2)) {
		r->kind = KIND_BASIC;
		r->base = base;
		return;
	}
	if (wt.items >= 1)
		r->kind = KIND_TEXT;
}

/*
 * Grow the file one cluster at a time.  Only clusters that actually carry the
 * structure further are candidates, which is what makes this reliable rather
 * than a guess.  Among those, the cluster the FAT names wins: a stale FAT is
 * wrong in places, not everywhere, and a disk full of older copies of the same
 * program offers several clusters that continue a line chain plausibly.  Only
 * where the FAT's choice does not parse do we pick the best candidate instead.
 */
static int recover(recovery *r, const mos_image *img, int start,
                   const uint8_t *taken)
{
	long csize = mos_cluster_size(img);
	uint8_t *used;
	int cap = img->nclusters;

	memset(&r->result, 0, sizeof r->result);
	r->choices = 0;
	r->chain_len = 0;
	r->size = 0;
	r->data = NULL;
	r->chain = NULL;

	if (mos_cluster(img, start) == NULL)
		return -1;
	if ((used = calloc((size_t)img->nclusters, 1)) == NULL ||
	    (r->chain = malloc((size_t)cap)) == NULL ||
	    (r->data = malloc((size_t)cap * (size_t)csize)) == NULL) {
		free(used);
		return -1;
	}

	memcpy(r->data, mos_cluster(img, start), (size_t)csize);
	r->size = csize;
	r->chain[r->chain_len++] = (uint8_t)start;
	used[start] = 1;

	for (;;) {
		int best = -1, fits = 0, c, hint;
		long best_cov = -1;
		walk best_walk, hint_walk;

		r->result = parse(r, r->data, r->size);
		if (r->result.status != WALK_NEED_MORE)
			break;
		if (r->chain_len >= cap)
			break;

		hint = -1;
		memset(&hint_walk, 0, sizeof hint_walk);
		memset(&best_walk, 0, sizeof best_walk);
		for (c = 0; c < img->nclusters; c++) {
			walk w;

			if (used[c] || taken[c] || mos_cluster_is_meta(img, c) ||
			    mos_cluster(img, c) == NULL)
				continue;
			memcpy(r->data + r->size, mos_cluster(img, c), (size_t)csize);
			w = parse(r, r->data, r->size + csize);
			if (w.covered <= r->result.covered)
				continue;               /* did not carry us further */
			if (c == img->fat[r->chain[r->chain_len - 1]]) {
				hint = c;
				hint_walk = w;
			}
			fits++;
			if (w.covered > best_cov) {
				best_cov = w.covered;
				best = c;
				best_walk = w;
			}
		}
		if (hint >= 0) {                        /* the FAT still fits here  */
			best = hint;
			best_walk = hint_walk;
		}
		if (best < 0)
			break;                          /* nothing continues it     */
		if (hint < 0 && fits > 1)
			r->choices++;                   /* had to choose ourselves  */
		memcpy(r->data + r->size, mos_cluster(img, best), (size_t)csize);
		r->size += csize;
		r->chain[r->chain_len++] = (uint8_t)best;
		used[best] = 1;
		r->result = best_walk;
		if (best_walk.status != WALK_NEED_MORE)
			break;
	}
	free(used);
	return 0;
}

static void free_recovery(recovery *r)
{
	free(r->data);
	free(r->chain);
	r->data = NULL;
	r->chain = NULL;
}

/* ------------------------------------------------------------------ */

/*
 * How the FAT compares to what the data says:
 *   same   chain agrees and its allocation covers the recovered data
 *   short  same chain, but the FAT allocates fewer sectors than the data needs,
 *          so following the FAT would cut the file off
 *   diff   the FAT names other clusters
 *   err    the FAT chain could not be followed at all
 *
 * The FAT allocating more sectors than the file needs is normal and counts as
 * agreement - MOS rounds up to whole sectors.
 */
static const char *fat_verdict(const mos_image *img, const mos_file *file,
                               const recovery *r)
{
	long ssize = img->fmt->sector_size;
	long sectors;
	int i;

	if (file->error != NULL)
		return "err";
	if (file->chain_len != r->chain_len)
		return "diff";
	for (i = 0; i < r->chain_len; i++)
		if (file->chain[i] != r->chain[i])
			return "diff";
	sectors = (r->result.covered + ssize - 1) / ssize;
	return sectors <= file->sectors ? "same" : "short";
}

static void print_chain(const char *label, const uint8_t *chain, int len)
{
	int i;

	printf("    %s", label);
	for (i = 0; i < len; i++)
		printf(" %02x", chain[i]);
	printf("\n");
}

static int write_out(const char *dir, const mos_file *file, const uint8_t *buf,
                     long len, int overwrite, int text, int utf8)
{
	char path[1024], name[MOS_NAME_LEN + 1];
	uint8_t *tmp = NULL;
	char *conv = NULL;
	FILE *fp;

	mos_local_name(file, name, sizeof name);
	snprintf(path, sizeof path, "%s/%s", dir, name);

	if (text || utf8) {
		if ((tmp = malloc((size_t)(len > 0 ? len : 1))) == NULL) {
			fprintf(stderr, "%s: out of memory\n", progname);
			return -1;
		}
		memcpy(tmp, buf, (size_t)len);
		if (text) {
			len = mos_text_len(tmp, len);
			len = mos_crlf_to_lf(tmp, len);
		}
		if (utf8) {
			long clen;

			if ((conv = mos_de_to_utf8(tmp, len, &clen)) == NULL) {
				fprintf(stderr, "%s: out of memory\n", progname);
				free(tmp);
				return -1;
			}
			free(tmp);
			tmp = (uint8_t *)conv;
			len = clen;
		}
		buf = tmp;
	}
	if (!overwrite && access(path, F_OK) == 0) {
		fprintf(stderr, "%s: %s: exists, use -o to overwrite\n", progname, path);
		free(tmp);
		return -1;
	}
	if ((fp = fopen(path, "wb")) == NULL) {
		fprintf(stderr, "%s: %s: %s\n", progname, path, strerror(errno));
		free(tmp);
		return -1;
	}
	if ((len > 0 && fwrite(buf, 1, (size_t)len, fp) != (size_t)len) ||
	    fclose(fp) != 0) {
		fprintf(stderr, "%s: %s: %s\n", progname, path, strerror(errno));
		free(tmp);
		return -1;
	}
	free(tmp);
	return 0;
}

static void usage(FILE *fp, int status)
{
	const mos_format *f;
	int i;

	fprintf(fp, "usage: %s [-x dir] [-t] [-u] [-o] [-c] [-p] [-d] [-S]"
	        " [-f format] image [pattern ...]\n", progname);
	fprintf(fp, "\n"
	        "Rebuilds every file's cluster chain from the data itself instead of\n"
	        "from the FAT, for disks whose allocation table is out of date.\n"
	        "\n"
	        "  -x dir      write the recovered files into dir\n"
	        "  -t          text mode for data files (as in moscp)\n"
	        "  -u          translate German 7 bit characters to UTF-8\n"
	        "  -o          overwrite existing local files\n"
	        "  -c          show the recovered and the FAT cluster chain\n"
	        "  -p          write partial recoveries too, not only complete ones\n"
	        "  -d          also try deleted directory entries\n"
	        "  -S          match patterns case sensitively\n"
	        "  -f format   force a disk format instead of guessing by size\n"
	        "  -V          print version and exit\n"
	        "  -h          print this help and exit\n"
	        "\n"
	        "Without -x nothing is written; the report alone tells you which\n"
	        "files the FAT still describes correctly.\n"
	        "\nformats:\n");
	for (i = 0; (f = mos_format_at(i)) != NULL; i++)
		fprintf(fp, "  %-8s %s%s\n", f->name, f->desc,
		        f->verified ? "" : " (unverified)");
	exit(status);
}

int main(int argc, char **argv)
{
	const mos_format *fmt = NULL;
	const char *dir = NULL;
	unsigned flags = 0;
	int text = 0, utf8 = 0, overwrite = 0, chains = 0, casefold = 1;
	int partials = 0;
	int opt, i, j, rc = 0;
	int complete = 0, partial = 0, unknown = 0, differs = 0, written = 0;
	int skipped = 0, guessed = 0, collisions = 0, pass;
	int *owner = NULL;
	recovery *recs = NULL;
	uint8_t *fixed = NULL, *taken = NULL, *ambiguous = NULL;
	char err[512];
	mos_image img;

	if (argv[0] != NULL && argv[0][0] != '\0')
		progname = argv[0];

	while ((opt = getopt(argc, argv, "x:tuocpdSf:Vh")) != -1) {
		switch (opt) {
		case 'x': dir = optarg; break;
		case 't': text = 1; break;
		case 'u': utf8 = 1; break;
		case 'o': overwrite = 1; break;
		case 'c': chains = 1; break;
		case 'p': partials = 1; break;
		case 'd': flags |= MOS_KEEP_DELETED; break;
		case 'S': casefold = 0; break;
		case 'f':
			if ((fmt = mos_format_find(optarg)) == NULL) {
				fprintf(stderr, "%s: unknown format '%s'\n", progname, optarg);
				return 1;
			}
			break;
		case 'V':
			printf("mosrecover (MOStools) %s\n", MOS_VERSION);
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
	if (dir != NULL) {
		struct stat st;

		if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
			fprintf(stderr, "%s: %s: not a directory\n", progname, dir);
			return 1;
		}
	}
	if (mos_open(&img, argv[optind], fmt, flags, err, sizeof err) != 0) {
		fprintf(stderr, "%s: %s\n", progname, err);
		return 1;
	}

	/*
	 * Solve the disk as a whole, not one file at a time.  A cluster belongs to
	 * at most one file, and no file continues into the first cluster of
	 * another, so every chain that comes out forced takes its clusters off the
	 * table for everybody else.  Repeat while that keeps settling files: the
	 * certain chains constrain the ambiguous ones.
	 */
	recs = calloc((size_t)img.nfiles, sizeof *recs);
	fixed = calloc((size_t)img.nfiles, 1);
	ambiguous = calloc((size_t)img.nfiles, 1);
	taken = calloc((size_t)img.nclusters, 1);
	owner = calloc((size_t)img.nclusters, sizeof *owner);
	if (recs == NULL || fixed == NULL || taken == NULL || owner == NULL ||
	    ambiguous == NULL) {
		fprintf(stderr, "%s: out of memory\n", progname);
		return 1;
	}
	for (i = 0; i < img.nfiles; i++)
		if (img.files[i].start < img.nclusters)
			taken[img.files[i].start] = 1;      /* no file starts twice */

	for (pass = 0; pass < img.nfiles + 1; pass++) {
		int settled = 0, cand = -1;

		for (i = 0; i < img.nfiles; i++) {
			if (fixed[i])
				continue;
			free_recovery(&recs[i]);
			classify(&recs[i], &img, &img.files[i]);
			if (recs[i].kind == KIND_UNKNOWN)
				continue;
			if (recover(&recs[i], &img, img.files[i].start, taken) != 0)
				continue;
			if (recs[i].result.status == WALK_END && recs[i].choices == 0) {
				fixed[i] = 1;
				settled++;
				for (j = 0; j < recs[i].chain_len; j++)
					taken[recs[i].chain[j]] = 1;
			}
		}
		if (settled > 0)
			continue;               /* forced chains may force more */

		/*
		 * Nothing is forced any more.  Settle the most constrained of the
		 * remaining complete chains - fewest ambiguous steps, then fewest
		 * clusters - and let its clusters constrain the rest.  Doing this one
		 * at a time keeps every chain disjoint, so no two files can end up
		 * claiming the same cluster.
		 */
		for (i = 0; i < img.nfiles; i++) {
			if (fixed[i] || recs[i].chain_len == 0 ||
			    recs[i].result.status != WALK_END)
				continue;
			if (cand < 0 || recs[i].choices < recs[cand].choices ||
			    (recs[i].choices == recs[cand].choices &&
			     recs[i].chain_len < recs[cand].chain_len))
				cand = i;
		}
		if (cand < 0)
			break;
		fixed[cand] = 1;
		ambiguous[cand] = 1;
		for (j = 0; j < recs[cand].chain_len; j++)
			taken[recs[cand].chain[j]] = 1;
	}

	printf("kind   status      bytes  fat   name\n");
	for (i = 0; i < img.nfiles; i++) {
		const mos_file *file = &img.files[i];
		const char *status, *fatcol;
		recovery r = recs[i];
		int match = (optind + 1 >= argc);

		for (j = optind + 1; !match && j < argc; j++)
			match = mos_match(argv[j], file->name, casefold);
		if (!match)
			continue;

		if (r.kind == KIND_UNKNOWN || r.chain_len == 0) {
			printf("%-6s %-9s %7s  %-5s %s\n", kind_name(r.kind),
			       "no parse", "-", "-",
			       file->name[0] != '\0' ? file->name : "(blank)");
			unknown++;
			continue;
		}
		if (r.result.status == WALK_END) {
			status = (fixed[i] && !ambiguous[i]) ? "complete" : "complete*";
			complete++;
			if (!fixed[i] || ambiguous[i])
				guessed++;
		} else {
			status = "partial";
			partial++;
		}
		fatcol = fat_verdict(&img, file, &r);
		if (strcmp(fatcol, "diff") == 0 || strcmp(fatcol, "err") == 0)
			differs++;

		printf("%-6s %-9s %7ld  %-5s %s%s\n", kind_name(r.kind), status,
		       r.result.covered, fatcol,
		       file->name[0] != '\0' ? file->name : "(blank)",
		       file->deleted ? "  [deleted]" : "");
		if ((j = mos_file_bad_sectors(&img, file)) > 0)
			fprintf(stderr, "%s: %s: %d sector(s) of the FAT chain were not"
			        " read cleanly\n", progname, file->name, j);
		if (owner != NULL) {
			for (j = 0; j < r.chain_len; j++) {
				if (owner[r.chain[j]] == 0)
					owner[r.chain[j]] = i + 1;
				else if (owner[r.chain[j]] != i + 1)
					collisions++;
			}
		}
		if (chains) {
			print_chain("recovered:", r.chain, r.chain_len);
			if (file->error == NULL)
				print_chain("fat:      ", file->chain, file->chain_len);
			else
				printf("    fat:       %s\n", file->error);
		}
		if (dir != NULL && r.result.covered > 0) {
			if (r.result.status != WALK_END && !partials) {
				skipped++;
			} else if (write_out(dir, file, r.data, r.result.covered,
			                     overwrite, text, utf8) != 0) {
				rc = 1;
			} else {
				written++;
			}
		}
	}
	for (i = 0; i < img.nfiles; i++)
		free_recovery(&recs[i]);
	free(recs);
	free(fixed);
	free(taken);
	free(ambiguous);

	printf("\n%d complete, %d partial, %d not recognised",
	       complete, partial, unknown);
	printf("; %d differ from the FAT\n", differs);
	if (guessed > 0)
		printf("%d recovery/recoveries marked * had more than one possible"
		       " continuation\n", guessed);
	printf("recovered chains overlap each other in %d cluster(s)%s\n", collisions,
	       collisions == 0 ? " - globally consistent" : "");
	free(owner);
	if (dir != NULL) {
		printf("%d file(s) written to %s", written, dir);
		if (skipped > 0)
			printf(", %d partial one(s) skipped, use -p to write them", skipped);
		printf("\n");
	}

	mos_close(&img);
	return rc;
}
