/*
 * Copyright (c) 1977-1995 by Robert Swartz.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * To compile use
 * cc cgrep.c -lmisc
 * This uses libmisc.a and the misc.h header from libmisc.a.Z
 */

/*
 * cgrep is egrep for c source programs.
 * 	cgrep [-r new] [-clnsA] [pattern] [file ...]
 *
 * cgrep checks all c identifiers (cgrep considers if, etc to be
 * identifiers) against an egrep type pattern for a full match.
 *
 * 	cgrep tmp *.c
 * will find tmp as an identifier, but not tmpname, or tmp in a string or
 * comment.
 * 	cgrep "x|abc|d" *.c
 * will find x, ab, or d. Note these are egrep style patterns with a
 * surrounding ^( )$ Thus "reg*" will not match register but "reg.*"
 * will.
 *
 * cgrep accumulates names with included . and -> for testing against the
 * egrep pattern. Thus you can look for ptr->val with
 * 	cgrep "ptr->val" x.c
 * This will find ptr->val even if it contains spaces, comments or is
 * spread across lines. If it is spread across lines it will be reported
 * on the line containing the last token unless the -A option is used
 * in which case it will be reported on the line containing the first
 * token.
 *
 * For structure.member use
 * 	cgrep "structure\.member" x.c
 * because . has the egrep meaning any character. Do not include spaces
 * in any pattern. Only identifiers and . or -> between identifiers are
 * included in the tokens checked for pattern match.
 *
 * Where no files are given cgrep will read stdin. Options -lA require
 * a file name. Where stdin is used with option -r a new file is written
 * out stdout.
 *
 * Options.
 *
 * -l lists the files with hits not the lines.
 *
 * -n puts a line number on found lines.
 *
 * -s List all strings. This form takes no pattern.
 *
 * -c List all comments. This form takes no pattern.
 *
 * -A builds a tmp file and calls 'me' to process the file with the tmp file
 *    as an "error" list like the -A option of cc. Each line of this list
 *    shows the found pattern and where multiple patterns are found on a
 *    line there are multiple lines, just like cc -A showing multiple
 *    errors on a line. This is to allow emacs scripts to make systematic
 *    changes. Where a patttern is split across multiple lines -A causes
 *    it to be reported on the line with the first token, also to
 *    simplify emacs scripts.
 *
 *    If cgrep -A is used to process a number of files it will search each
 *    file for patttern matches and call 'me' only if the file has a hit. To
 *    stop the process exit 'me' with <ctl-u><ctl-x><ctl-c>. See the new emacs
 *    initialization macro feature for a way to process these files
 *    automatically.
 *
 * -r Replaces all occurances of the pattern with "new". This form only matches
 *    simple tokens, not things like "ptr->val". -r is incompatible with all
 *    other options.
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>
#include "regexp.h"

__dead
static void
usage(void)
{

	fprintf(stderr, "%s [-r newStr] [-clnsA] [pattern] filename ...\n",
		getprogname());
	exit(1);
}

static void *
alloc(size_t n)
{
	void *buf;

	buf = calloc(1, n);
	if (buf == NULL) {
		fprintf(stderr, "Out of memory\n");
		exit(1);
	}

	return buf;
}

__dead
static void
fatal(char *s, ...)
{
	va_list ap;

	va_start(ap, s);
	vfprintf(stderr, s, ap);
	va_end(ap);

	exit(1);
}

/*
 * Cgrep never runs out of room on lines or buffers until malloc fails
 * these are buffer expanders.
 */
#define TROOM(buf, has, needs)	while ((needs) >= has) \
 if (NULL == (buf = realloc(buf, sizeof(*buf) * (has += 10)))) \
  fatal(outSpace)

#define ROOM(buf, has, needs)	while ((needs) >= has) \
 if (NULL == (buf = realloc(buf, has += 512))) \
  fatal(outSpace)

static char outSpace[] = "cgrep: out of space";

struct token {	/* collected token array */
	int start;	/* token index on buff */
	int atline;	/* line number where token spotted */
};

enum fstate {	/* lexical processing state */
	start,
	slash,		/* slash encountered in normal state */
	comment,	/* in comment */
	star,		/* * in comment */
	bsl,		/* back slash */
	dquote,		/* double quote */
	squote,		/* single quote */
	token,		/* c word */
	minus		/* - maybe -> */
};

enum wstate { /* word processing states */
	word,	/* some c identifier */
	dot,	/* . or -> */
	other	/* anything else */
};

/* cgrep has no fixed size arrays only growable buffers */
static char *buff = NULL;	/* accumulate stuff */
static int buffLen;		/* buffer length */

static struct token *tokens = NULL; /* token array */
static int tokenLen;		/* size of token array */

static char *line;		/* expandable input line */
static int  lineLen;		/* current length of input line */

static char lswitch;		/* list files found */
static char aswitch;		/* call emacs with line list */
static char nswitch;		/* print line number */
static char sswitch;		/* print all strings */
static char cswitch;		/* print all comments */
static char rswitch;		/* replace found pattern */

static regexp *pat;		/* a compiled regular expression */

static char *newstr;		/* The new string with rswitch */

static char *filen = NULL;	/* the file currently being processed */
static char *tname = NULL;	/* temp file name */
static FILE *tfp;		/* tmp file pointer */

static int lineno;		/* current line number */
static int marked;		/* 1 if pattern found on line. */

/*
 * Report errors for public domain regexp package.
 */
void
regerror(s)
char *s;
{
	fatal("%s: pattern error %s\n", getprogname(), s);
}

/*
 * Pattern found with -A mode. It is assumed that users of
 * this mode will want to know about all hits on a line.
 */
static void
emacsLine(char *found, int atline)
{
	extern char *tempnam();

	/* if tmp file not opened open it. */
	if ((NULL == tname) &&
	    ((NULL == (tname = tempnam(NULL, "cgr"))) ||
             (NULL == (tfp = fopen(tname, "w")))))
		fatal("%s: Cannot open tmp file", getprogname());
	fprintf(tfp, "%d: %s: found '%s'\n", atline, filen, found);
}

/*
 * When we get a word, dot, arrow or other we come here.
 *
 * cgrep accumulates things like ptr->memb.x in buff.
 * as these are accumulated it is nessisary to match buff
 * against the regular expression one slice at a time
 * that is when the x in ptr->memb.x was found cgrep would check
 * ptr->memb.x then memb.x then x
 * against the pattern. Thus memb.x matches ptr->memb.x
 */
static void
gota(got, what)
register enum wstate got;	/* what we have */
char *what;		/* the string we have or NULL */
{
	static int tokenCt;	    /* number of tokens */
	static enum wstate state;   /* current word processing state */
	static int wlen, blen;	    /* strlen(what) and strlen(buff) + 1 */
	int i;

	if (sswitch || cswitch)
		return;

	if (rswitch) {	/* replace mode works on tokens only */
		marked = (word == got) && regexec(pat, what);
		return;
	}

	switch (got) {
	case word:
		wlen = strlen(what);
		switch (state) {
		case other:
		case word:
			/* store start and line number of token */
			tokenCt = 1;
			TROOM(tokens, tokenLen, tokenCt);
			tokens[0].start = 0;
			tokens[0].atline = lineno;

			/* store token */
			blen = wlen;
			ROOM(buff, buffLen, blen);
			strcpy(buff, what);
			break;
		case dot:
			/* store start and line number of token */
			TROOM(tokens, tokenLen, tokenCt);
			tokens[tokenCt].start = blen;
			tokens[tokenCt++].atline = lineno;

			/* store token */
			blen += wlen;
			ROOM(buff, buffLen, blen);
			strcat(buff, what);
		}

		/* Check the accumulated token for matches */
		for (i = 0; i < tokenCt; i++) {
			char *p;

			if (regexec(pat, p = buff + tokens[i].start)) {
				if (aswitch)
					emacsLine(p, tokens[i].atline);
				else {
					marked = 1;
					break;
				}
			}
		}

		state = got;
		break;

	case dot:
		if (word == state) {
			blen += strlen(what);
			ROOM(buff, buffLen, blen);
			strcat(buff, what);
			state = got;
			break;
		}
	case other:
		state = other;
	}
}

/*
 * call emacs with file and tmpfile.
 */
static void
callEmacs()
{
#ifdef GEMDOS
#include <path.h>
	extern  char *path(), *getenv();
	extern char **environ;
	static char* cmda[5] = { NULL, "-e", NULL, NULL, NULL };
#endif
	int quit;

	fclose(tfp);
#ifdef MSDOS
	sprintf(line, "-e %s %s", tname, filen);
	if (0x7f == (quit = execall("emacs", line)))
#endif
//#ifdef COHERENT
	sprintf(line, "emacs -e %s %s ", tname, filen);
	if (0x7f == (quit = system(line)))
//#endif
#ifdef GEMDOS
	cmda[2] = tname;
	cmda[3] = filen;
	if ((NULL == cmda[0]) &&
	   (NULL == (cmda[0] = path(getenv("PATH"), "me.tos", 1))))
		fatal("cgrep: Cannot locate me.tos\n");
	else if ((quit = execve(cmda[0], cmda, environ)) < 0)
#endif
		fatal("cgrep: cannot execute 'me'");
	unlink(tname);
	free(tname);
	tname = NULL;
	if (quit)
		exit(0);
}

/*
 * print a hit for options -s or -c.
 */
static void
printx(s)
char *s;
{
	if (aswitch)
		emacsLine(s, lineno);
	else {
		if (NULL != filen)
			printf("%s: ", filen);
		if (nswitch)
			printf("%4d: ", lineno);
		printf("%s\n", s);
	}
}

/*
 * Lexically process a file.
 */
static void
lex()
{
	int  c, i;
	enum fstate state, pstate;
	char *w, changed;
	FILE *ifp, *tfp;

	if (NULL == filen)
		ifp = stdin;
	else if (NULL == (ifp = fopen(filen, "r"))) {
		fprintf(stderr, "cgrep: warning cannot open %s\n", filen);
		return;
	}

	if (rswitch) {
		changed = 0;	/* no changes so far */

		if (NULL == filen)
			tfp = stdout;
		else if ((NULL == (tname = tempnam(NULL, "cse"))) ||
			 (NULL == (tfp = fopen(tname, "w"))))
		  	fatal("csed: Cannot open tmp file");
	}

	lineno = 1;
	i = marked = 0;
	gota(other, NULL);	/* initialize word machine */
	for (state = start; ; ) {
		line[i] = '\0';
		c = fgetc(ifp);

		switch (state) {
		case minus:
			if ('>' == c) {
				gota(dot, "->");
				state = start;
				break;
			}
			goto isstart;
		case token:
			if (isalnum(c) || c == '_')
				break;
			gota(word, w);

			/* we have a word to replace */
			if (rswitch && marked) {
				i += strlen(newstr) - strlen(w);
				ROOM(line, lineLen, i);
				strcpy(w, newstr);
				changed = 1;
			}
isstart:		state = start;
		case start:
			switch (c) {
			case '.':
				gota(dot, ".");
				break;
			case '-':
				state = minus;
				break;
			case '/':
				state = slash;
				break;
			case '\\':
				pstate = state;
				state = bsl;
				break;
			case '"':
				w = line + i;
				state = dquote;
				break;
			case '\'':
				state = squote;
				break;
			default:
				if (isalpha(c)) {
					w = line + i;
					state = token;
				}
				else if (!isspace(c))
					gota(other, NULL);
			}
			break;
		case slash:
			if ('*' != c)
				goto isstart;
			w = line + i + 1;
			state = comment;
			break;
		case star:
			if ('/' == c) {
				if (cswitch) { /* report comment */
					line[i - 1] = '\0';
					printx(w);
					line[i - 1] = '*';
				}
				state = start;
				break;
			}
			state = comment;
		case comment:
			if ('*' == c)
				state = star;
			break;
		case bsl:
			state = pstate;
			break;
		case dquote:
			switch (c) {
			case '"':
			case '\n':
				state = start;
				if (sswitch)
					printx(w + 1);
				else
					gota(other, NULL);
				break;
			case '\\':
				pstate = state;
				state  = bsl;
				break;
			}
			break;
		case squote:
			switch (c) {
			case '\'':
			case '\n':
				gota(other, NULL);
				state = start;
				break;
			case '\\':
				pstate = state;
				state  = bsl;
				break;
			}
			break;
		}
		if (('\n' != c) && (EOF != c)) {
			line[i++] = c;
			ROOM(line, lineLen, i);
		}
		else {	/* end of line */
			if (rswitch) {
				if (EOF == c) {
					if (!i) /* null line */
						break;
				}
				else
					line[i++] = c;
				ROOM(line, lineLen, i);
				line[i] = 0;
				fputs(line, tfp);
				marked = 0;
			}

			if (cswitch && (comment == state)) {
				printx(w);
				w = line;
			}

			if (marked) {
				marked = 0;			
				if (lswitch) {
					printf("%s\n", filen);
					break;
				}
				printx(line);
			}

			lineno++;
			i = 0;
			if (EOF == c)
				break;
		}
	}

	fclose(ifp);

	if (rswitch) {
		fclose(tfp);

		if (NULL == filen)
			; /* do nothing file is already out */
		else if (changed) {
			unlink(filen);
			sprintf(line, "mv %s %s", tname, filen);
			system(line);
		}
		else
			unlink(tname);
	}

	if (aswitch && (NULL != tname)) /* tmp file opened for -A option */
		callEmacs();
}

int
main(int argc, char **argv)
{
	char c;
	int errsw = 0;
	char *p, *q;

	setprogname(argv[0]);

	if (1 == argc)
		usage();

	while (-1 != (c = getopt(argc, argv, "cslnA?r:"))) {
		switch (c) {
		case 'c':
			cswitch = 1;	/* comments only */
			break;
		case 's':
			sswitch = 1;	/* strings only */
			break;
		case 'l':
			lswitch = 1;	/* print filenames only */
			break;
		case 'n':
			nswitch = 1;	/* print line numbers */
			break;
		case 'A':
			aswitch = 1;	/* interact with emacs */
			break;
		case 'r':
			rswitch = 1;	/* replace hits */
			newstr = optarg;
			break;
		default:
			errsw = 1;
		}
	}

	/* check unknown switches and rswitch goes with no other switches */
	if (errsw || 
	    (rswitch && (aswitch | nswitch | lswitch | cswitch | sswitch)))
		usage();

	if (!sswitch && !cswitch) {	/* process pattern */
		if (optind == argc)		/* no pattern */
			usage();

		/* inclose pattern in ^(  )$ to force full match */
		p = alloc(5 + strlen(q = argv[optind++]));
		sprintf(p, "^(%s)$", q);
		if (NULL == (pat = regcomp(p)))
			fatal("Illegal pattern\n");
	}

	ROOM(line, lineLen, 1);	/* get input line started */

	if (optind == argc) {
		if (aswitch | lswitch)
			fatal("-A and -l require a filename");
		lex();
	}
	else {
		while (optind < argc) {
			filen = argv[optind++];
			lex();
		}
	}

	return 0;
}
