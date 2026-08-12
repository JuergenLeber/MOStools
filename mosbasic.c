/*
 * mosbasic - list the BASIC programs of an alphatronic P2 MOS disk
 *
 * MOS BASIC stores a program the way Microsoft BASIC does: per line a 16 bit
 * link to the next line, a 16 bit line number, the tokenised text, and a 00
 * terminator; a 0000 link ends the program.  Keywords are single bytes >= 0x80,
 * functions two bytes starting with 0xff, and numbers are stored in binary.
 *
 * The token table is not guesswork: it was read out of the BASIC interpreter
 * itself, which sits in the reserved system area of a MOS system disk as a
 * table of keywords grouped by first letter.  -T re-reads that table from any
 * system disk, so a different BASIC revision can be listed with its own tokens.
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
#include "mosfs.h"

#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *progname = "mosbasic";

/* Tokens of MOS BASIC Rev. 3.00, extracted from the interpreter on a
 * system disk.  Index 0 is token 0x80. */
static const char *mos_keyword[0x80] = {
	NULL, "END", "FOR", "NEXT",                          /* 80 */
	"DATA", "INPUT", "DIM", "READ",                      /* 84 */
	"LET", "GOTO", "RUN", "IF",                          /* 88 */
	"RESTORE", "GOSUB", "RETURN", "REM",                 /* 8c */
	"STOP", "PRINT", "CLEAR", "LIST",                    /* 90 */
	"NEW", "ON", "NULL", "WAIT",                         /* 94 */
	"DEF", "POKE", "CONT", NULL,                         /* 98 */
	NULL, "OUT", "LPRINT", "LLIST",                      /* 9c */
	NULL, "WIDTH", "ELSE", "TRON",                       /* a0 */
	"TROFF", "SWAP", "ERASE", "EDIT",                    /* a4 */
	"ERROR", "RESUME", "DELETE", "AUTO",                 /* a8 */
	"RENUM", "DEFSTR", "DEFINT", "DEFSNG",               /* ac */
	"DEFDBL", "LINE", "WHILE", "WEND",                   /* b0 */
	"CALL", "WRITE", "COMMON", "CHAIN",                  /* b4 */
	"OPTION", "RANDOMIZE", "DSKO$", "REMOVE",            /* b8 */
	"MOUNT", "SET", "FIELD", "OPEN",                     /* bc */
	"GET", "PUT", "CLOSE", "LOAD",                       /* c0 */
	"MERGE", "FILES", "NAME", "KILL",                    /* c4 */
	"LSET", "RSET", "SAVE", "LFILES",                    /* c8 */
	"DIS", "PSET", "PRESET", "SVC",                      /* cc */
	"TIME", NULL, "TO", "THEN",                          /* d0 */
	"TAB(", "STEP", "USR", "FN",                         /* d4 */
	"SPC(", "NOT", "ERL", "ERR",                         /* d8 */
	"STRING$", "USING", "INSTR", "'",                    /* dc */
	"VARPTR", "ATTR$", "DSKI$", "POINT",                 /* e0 */
	"INKEY$", ">", "=", "<",                             /* e4 */
	"+", "-", "*", "/",                                  /* e8 */
	"^", "AND", "OR", "XOR",                             /* ec */
	"EQV", "IMP", "MOD", "\\",                           /* f0 */
	NULL, NULL, NULL, NULL,                              /* f4 */
	NULL, NULL, NULL, NULL,                              /* f8 */
	NULL, NULL, NULL, NULL                               /* fc */
};

/* Two byte tokens: 0xff followed by these, index 0 is 0xff 0x80. */
static const char *mos_function[0x40] = {
	NULL, "LEFT$", "RIGHT$", "MID$",                     /* ff 80 */
	"SGN", "INT", "ABS", "SQR",                          /* ff 84 */
	"RND", "SIN", "LOG", "EXP",                          /* ff 88 */
	"COS", "TAN", "ATN", "FRE",                          /* ff 8c */
	"INP", "POS", "LEN", "STR$",                         /* ff 90 */
	"VAL", "ASC", "CHR$", "PEEK",                        /* ff 94 */
	"SPACE$", "OCT$", "HEX$", "LPOS",                    /* ff 98 */
	"CINT", "CSNG", "CDBL", "FIX",                       /* ff 9c */
	"CVI", "CVS", "CVD", "DSKF",                         /* ff a0 */
	"EOF", "LOC", "LOF", "FPOS",                         /* ff a4 */
	"MKI$", "MKS$", "MKD$", "ROW",                       /* ff a8 */
	NULL, NULL, NULL, NULL,                              /* ff ac */
	NULL, NULL, NULL, NULL,                              /* ff b0 */
	NULL, NULL, NULL, NULL,                              /* ff b4 */
	NULL, NULL, NULL, NULL,                              /* ff b8 */
	NULL, NULL, NULL, NULL                               /* ff bc */
};

/* ------------------------------------------------------------------ *
 * Reading the token table out of an interpreter on a system disk.
 *
 * The system area does not hold a flat code image: every sector starts with one
 * marker byte, so the code has to be stitched back together from 255 byte
 * pieces first.  In the result the keyword table is a run of groups, one per
 * first letter A to Z, each entry being the rest of the keyword with the last
 * character's bit 7 set, followed by the token.  A token below 0x80 means a
 * function, written as 0xff, 0x80 + token.  After the Z group come the
 * operators as (character | 0x80, token) pairs.
 * ------------------------------------------------------------------ */
static int load_table(const char *path, char *err, size_t errlen)
{
	mos_image img;
	uint8_t *code;
	long clen = 0, i, p = -1;
	int letter, count = 0;

	if (mos_open(&img, path, NULL, 0, err, errlen) != 0)
		return -1;

	/* strip the per sector marker byte over the whole disk */
	if ((code = malloc((size_t)img.size)) == NULL) {
		snprintf(err, errlen, "out of memory");
		mos_close(&img);
		return -1;
	}
	for (i = 0; i + img.fmt->sector_size <= img.size; i += img.fmt->sector_size) {
		memcpy(code + clen, img.data + i + 1,
		       (size_t)img.fmt->sector_size - 1);
		clen += img.fmt->sector_size - 1;
	}

	for (i = 0; i + 4 < clen; i++)          /* the AUTO entry: "UT" O|80 ab */
		if (code[i] == 'U' && code[i + 1] == 'T' && code[i + 2] == 0xcf &&
		    code[i + 3] == 0xab) {
			p = i;
			break;
		}
	if (p < 0) {
		snprintf(err, errlen, "%s: no BASIC keyword table found", path);
		free(code);
		mos_close(&img);
		return -1;
	}

	memset(mos_keyword, 0, sizeof mos_keyword);
	memset(mos_function, 0, sizeof mos_function);

	for (letter = 'A'; letter <= 'Z' && p < clen; letter++) {
		while (p < clen && code[p] != 0x00) {
			char word[32];
			size_t n = 1;
			int tok;

			word[0] = (char)letter;
			while (p < clen && !(code[p] & 0x80) && n + 1 < sizeof word)
				word[n++] = (char)code[p++];
			if (p >= clen)
				break;
			word[n++] = (char)(code[p++] & 0x7f);
			word[n] = '\0';
			if (p >= clen)
				break;
			tok = code[p++];
			if (tok >= 0x80) {
				if (mos_keyword[tok - 0x80] == NULL)
					mos_keyword[tok - 0x80] = strdup(word);
			} else if (tok < 0x40) {
				if (mos_function[tok] == NULL)
					mos_function[tok] = strdup(word);
			}
			count++;
		}
		p++;
	}
	/* operators, as (character | 0x80, token) pairs */
	while (p + 1 < clen && (code[p] & 0x80) && code[p + 1] >= 0x80) {
		char c = (char)(code[p] & 0x7f);
		int tok = code[p + 1];

		if (c < 0x20 || c > 0x7e)
			break;
		if (mos_keyword[tok - 0x80] == NULL) {
			char *s = malloc(2);

			if (s != NULL) {
				s[0] = c;
				s[1] = '\0';
				mos_keyword[tok - 0x80] = s;
			}
		}
		p += 2;
		count++;
	}

	free(code);
	mos_close(&img);
	if (count < 50) {
		snprintf(err, errlen, "%s: only %d tokens found, not a BASIC system"
		         " disk?", path, count);
		return -1;
	}
	return 0;
}

/* ------------------------------------------------------------------ *
 * Microsoft binary format numbers.
 * ------------------------------------------------------------------ */
static double mbf_single(const uint8_t *b)
{
	long mantissa = b[0] | ((long)b[1] << 8) | ((long)(b[2] & 0x7f) << 16);
	int exp = b[3];
	double v;

	if (exp == 0)
		return 0.0;
	v = (double)(mantissa | 0x800000L) / 16777216.0 * pow(2.0, exp - 128);
	return (b[2] & 0x80) ? -v : v;
}

static double mbf_double(const uint8_t *b)
{
	double m = 0.0, v;
	int exp = b[7], i;

	if (exp == 0)
		return 0.0;
	for (i = 5; i >= 0; i--)
		m = m * 256.0 + b[i];
	m += (double)(b[6] & 0x7f) * 281474976710656.0;         /* 2^48 */
	m += 36028797018963968.0;                               /* 2^55, implicit */
	v = m / 72057594037927936.0 * pow(2.0, exp - 128);      /* 2^56 */
	return (b[6] & 0x80) ? -v : v;
}

/* ------------------------------------------------------------------ */

/* A listed line is collected here first, so that -u can translate all of it -
 * comments and DATA text carry umlauts just like string literals do. */
#define LINE_MAX 8192

static void add(char *out, size_t *n, const char *s)
{
	size_t l = strlen(s);

	if (*n + l + 1 >= LINE_MAX)
		l = LINE_MAX - *n - 1;
	memcpy(out + *n, s, l);
	*n += l;
	out[*n] = '\0';
}

static void addf(char *out, size_t *n, const char *fmt, ...)
{
	char buf[64];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	add(out, n, buf);
}

static void addc(char *out, size_t *n, int c, int utf8)
{
	const char *rep = utf8 ? mos_de_char((uint8_t)c) : NULL;
	char buf[2];

	if (rep != NULL) {
		add(out, n, rep);
		return;
	}
	buf[0] = (char)c;
	buf[1] = '\0';
	add(out, n, buf);
}

static void addnum(char *out, size_t *n, double v)
{
	if (v == floor(v) && fabs(v) < 1e15)
		addf(out, n, "%.0f", v);
	else
		addf(out, n, "%g", v);
}

/*
 * Print one program.  Returns the number of lines, or -1 when the program does
 * not hold a valid line structure.
 */
static long list_program(const uint8_t *b, long len, int utf8, int hexdump)
{
	long pos = 0, lines = 0, base = -1;
	int last = -1;

	/* The load address is not stored, so derive it from the first line. */
	if (len >= 6) {
		long link = b[0] | ((long)b[1] << 8), l;

		for (l = 5; l <= 256 && l <= len; l++)
			if (b[l - 1] == 0 && link - l > 0) {
				base = link - l;
				break;
			}
	}
	if (base < 0)
		return -1;

	while (pos + 4 <= len) {
		long link = b[pos] | ((long)b[pos + 1] << 8);
		long line = b[pos + 2] | ((long)b[pos + 3] << 8);
		long next = link - base;
		long i;

		if (link == 0)
			break;
		if (next <= pos || next > len || line < last)
			return lines > 0 ? lines : -1;

		char out[LINE_MAX];
		size_t n = 0;

		out[0] = '\0';
		for (i = pos + 4; i < next - 1 && i < len; i++) {
			int c = b[i];

			if (c == 0) {
				break;
			} else if (c == '"') {                  /* string literal */
				long j;

				addc(out, &n, '"', utf8);
				for (j = i + 1; j < len && b[j] != '"'; j++)
					if (utf8 && b[j] >= 0x80)
						addf(out, &n, "{%02x}", b[j]);  /* graphics char */
					else
						addc(out, &n, b[j], utf8);
				if (j < len && b[j] == '"')
					addc(out, &n, '"', utf8);
				else
					j--;
				i = j;
			} else if (c == ':' && i + 2 < len && b[i + 1] == 0x8f &&
			           b[i + 2] == 0xdf) {
				add(out, &n, "'");      /* how an apostrophe comment is stored */
				i += 2;
			} else if (c == 0xff && i + 1 < len &&
			           b[i + 1] >= 0x80 && b[i + 1] < 0xc0 &&
			           mos_function[b[i + 1] - 0x80] != NULL) {
				add(out, &n, mos_function[b[i + 1] - 0x80]);
				if (hexdump)
					addf(out, &n, "{ff%02x}", b[i + 1]);
				i++;
			} else if (c >= 0x80 && mos_keyword[c - 0x80] != NULL) {
				add(out, &n, mos_keyword[c - 0x80]);
				if (hexdump)
					addf(out, &n, "{%02x}", c);
			} else if (c == 0x0e && i + 2 < len) {  /* line number */
				addf(out, &n, "%d", b[i + 1] | (b[i + 2] << 8));
				i += 2;
			} else if (c == 0x0d && i + 2 < len) {  /* address of a line */
				addf(out, &n, "<addr %04x>", b[i + 1] | (b[i + 2] << 8));
				i += 2;
			} else if (c == 0x0f && i + 1 < len) {  /* one byte constant */
				addf(out, &n, "%d", b[i + 1]);
				i++;
			} else if (c == 0x0c && i + 2 < len) {
				addf(out, &n, "&H%X", b[i + 1] | (b[i + 2] << 8));
				i += 2;
			} else if (c == 0x0b && i + 2 < len) {
				addf(out, &n, "&O%o", b[i + 1] | (b[i + 2] << 8));
				i += 2;
			} else if (c >= 0x11 && c <= 0x1b) {    /* constants 0 to 10 */
				addf(out, &n, "%d", c - 0x11);
			} else if (c == 0x1c && i + 2 < len) {
				addf(out, &n, "%d", (short)(b[i + 1] | (b[i + 2] << 8)));
				i += 2;
			} else if (c == 0x1d && i + 4 < len) {
				addnum(out, &n, mbf_single(b + i + 1));
				i += 4;
			} else if (c == 0x1f && i + 8 < len) {
				addnum(out, &n, mbf_double(b + i + 1));
				i += 8;
			} else if (c >= 0x20 && c < 0x7f) {
				addc(out, &n, c, utf8);
			} else {
				addf(out, &n, "{%02x}", c);
			}
		}
		printf("%ld %s\n", line, out);
		last = (int)line;
		pos = next;
		lines++;
	}
	return lines;
}

static void usage(FILE *fp, int status)
{
	fprintf(fp, "usage: %s [-u] [-T sysimage] [-S] [-f format] image"
	        " [pattern ...]\n"
	        "       %s [-u] [-T sysimage] -r file ...\n", progname, progname);
	fprintf(fp, "\n"
	        "Lists tokenised MOS BASIC programs as readable text.\n"
	        "\n"
	        "  -u            translate German 7 bit characters to UTF-8\n"
	        "  -r            arguments are already extracted program files\n"
	        "  -T sysimage   take the token table from this system disk instead\n"
	        "                of using the built in one (BASIC Rev. 3.00)\n"
	        "  -S            match patterns case sensitively\n"
	        "  -f format     force a disk format instead of guessing by size\n"
	        "  -x            append {xx} after every token, to inspect encodings\n"
	        "  -V            print version and exit\n"
	        "  -h            print this help and exit\n"
	        "\n"
	        "Only files with attribute 80 hold a program; others are skipped.\n");
	exit(status);
}

int main(int argc, char **argv)
{
	const mos_format *fmt = NULL;
	int utf8 = 0, raw = 0, casefold = 1, hexdump = 0;
	int opt, i, j, listed = 0, rc = 0;
	char err[512];
	mos_image img;

	if (argv[0] != NULL && argv[0][0] != '\0')
		progname = argv[0];

	while ((opt = getopt(argc, argv, "urT:Sxf:Vh")) != -1) {
		switch (opt) {
		case 'u': utf8 = 1; break;
		case 'r': raw = 1; break;
		case 'S': casefold = 0; break;
		case 'x': hexdump = 1; break;
		case 'T':
			if (load_table(optarg, err, sizeof err) != 0) {
				fprintf(stderr, "%s: %s\n", progname, err);
				return 1;
			}
			break;
		case 'f':
			if ((fmt = mos_format_find(optarg)) == NULL) {
				fprintf(stderr, "%s: unknown format '%s'\n", progname, optarg);
				return 1;
			}
			break;
		case 'V':
			printf("mosbasic (MOStools) %s\n", MOS_VERSION);
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

	if (raw) {
		for (i = optind; i < argc; i++) {
			FILE *fp = fopen(argv[i], "rb");
			uint8_t *buf;
			long len;

			if (fp == NULL) {
				fprintf(stderr, "%s: %s: %s\n", progname, argv[i],
				        strerror(errno));
				rc = 1;
				continue;
			}
			fseek(fp, 0, SEEK_END);
			len = ftell(fp);
			fseek(fp, 0, SEEK_SET);
			if (len <= 0 || (buf = malloc((size_t)len)) == NULL ||
			    fread(buf, 1, (size_t)len, fp) != (size_t)len) {
				fprintf(stderr, "%s: %s: cannot read\n", progname, argv[i]);
				fclose(fp);
				rc = 1;
				continue;
			}
			fclose(fp);
			if (argc - optind > 1)
				printf("%s%s:\n", listed ? "\n" : "", argv[i]);
			if (list_program(buf, len, utf8, hexdump) < 0) {
				fprintf(stderr, "%s: %s: not a tokenised BASIC program\n",
				        progname, argv[i]);
				rc = 1;
			}
			listed++;
			free(buf);
		}
		return rc;
	}

	if (mos_open(&img, argv[optind], fmt, 0, err, sizeof err) != 0) {
		fprintf(stderr, "%s: %s\n", progname, err);
		return 1;
	}
	for (i = 0; i < img.nfiles; i++) {
		const mos_file *file = &img.files[i];
		uint8_t *buf;
		long len;
		int match = (optind + 1 >= argc);

		for (j = optind + 1; !match && j < argc; j++)
			match = mos_match(argv[j], file->name, casefold);
		if (!match)
			continue;
		if (!(file->attr & MOS_ATTR_PROGRAM)) {
			if (optind + 1 < argc) {        /* asked for by name */
				fprintf(stderr, "%s: %s: attribute %02x, not a BASIC"
				        " program\n", progname, file->name, file->attr);
				rc = 1;
			}
			continue;
		}
		if (mos_read_file(&img, file, &buf, &len, err, sizeof err) != 0) {
			fprintf(stderr, "%s: %s\n", progname, err);
			rc = 1;
			continue;
		}
		if (mos_file_bad_sectors(&img, file) > 0)
			fprintf(stderr, "%s: %s: sectors of this file were not read"
			        " cleanly, the listing may be wrong\n", progname,
			        file->name);
		printf("%s%s:\n", listed ? "\n" : "", file->name);
		if (list_program(buf, len, utf8, hexdump) < 0) {
			fprintf(stderr, "%s: %s: not a tokenised BASIC program\n",
			        progname, file->name);
			rc = 1;
		}
		listed++;
		free(buf);
	}
	if (listed == 0) {
		fprintf(stderr, "%s: no BASIC program listed\n", progname);
		rc = 1;
	}
	mos_close(&img);
	return rc;
}
