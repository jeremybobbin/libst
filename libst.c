/* See LICENSE for license details. */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <wchar.h>

#include "config.h"
#include "libst.h"

#if   defined(__linux)
 #include <pty.h>
#elif defined(__OpenBSD__) || defined(__NetBSD__) || defined(__APPLE__)
 #include <util.h>
#elif defined(__FreeBSD__) || defined(__DragonFly__)
 #include <libutil.h>
#endif

/* macros */
#define IS_SET(flag)		((term->mode & (flag)) != 0)
#define ISCONTROLC0(c)		(BETWEEN(c, 0, 0x1f) || (c) == 0x7f)
#define ISCONTROLC1(c)		(BETWEEN(c, 0x80, 0x9f))
#define ISCONTROL(c)		(ISCONTROLC0(c) || ISCONTROLC1(c))

enum term_mode {
	MODE_WRAP        = 1 << 0,
	MODE_INSERT      = 1 << 1,
	MODE_ALTSCREEN   = 1 << 2,
	MODE_CRLF        = 1 << 3,
	MODE_ECHO        = 1 << 4,
	MODE_PRINT       = 1 << 5,
	MODE_UTF8        = 1 << 6,
};

enum cursor_movement {
	CURSOR_SAVE,
	CURSOR_LOAD
};

enum cursor_state {
	CURSOR_DEFAULT  = 0,
	CURSOR_WRAPNEXT = 1,
	CURSOR_ORIGIN   = 2
};

enum charset {
	CS_GRAPHIC0,
	CS_GRAPHIC1,
	CS_UK,
	CS_USA,
	CS_MULTI,
	CS_GER,
	CS_FIN
};

static void execsh(char *, char **);
static void ttywriteraw(Term *term, const char *, size_t);

static void csidump(Term *);
static void csihandle(Term *);
static void csiparse(Term *);
static void csireset(Term *);
static int eschandle(Term *, uchar);
static void strdump(Term *);
static void strhandle(Term *);
static void strparse(Term *);
static void strreset(Term *);

static void tprinter(Term *, char *, size_t);
static void tdumpline(Term *, int);
static void tdump(Term *);
static void tclearregion(Term *, int, int, int, int);
static void tcursor(Term *, int);
static void tdeletechar(Term *, int);
static void tdeleteline(Term *, int);
static void tinsertblank(Term *, int);
static void tinsertblankline(Term *, int);
static int tlinelen(Term *, int);
static void tmoveto(Term *, int, int);
static void tmoveato(Term *, int, int);
static void tnewline(Term *, int);
static void tputtab(Term *, int);
static void tputc(Term *, Rune);
static void treset(Term *);
static void tscrollup(Term *, int, int, int);
static void tscrolldown(Term *, int, int, int);
static void tsetattr(Term *, int *, int);
static void tsetchar(Term *, Rune, Glyph *, int, int);
static void tsetdirt(Term *, int, int);
static void tsetscroll(Term *, int, int);
static void tswapscreen(Term *);
static void tsetmode(Term *, int, int, int *, int);
static int twrite(Term *, const char *, int, int);
static void tcontrolcode(Term *, uchar );
static void tdectest(Term *, char );
static void tdefutf8(Term *, char);
static int32_t tdefcolor(Term *, int *, int *, int);
static void tdeftran(Term *, char);
static void tstrsequence(Term *, uchar);

static void drawregion(Term *, int, int, int, int);

static size_t utf8decode(const char *, Rune *, size_t);
static Rune utf8decodebyte(char, size_t *);
static char utf8encodebyte(Rune, size_t);
static size_t utf8validate(Rune *, size_t);

static char *base64dec(const char *);
static char base64dec_getc(const char **);

static ssize_t xwrite(int, const char *, size_t);

static uchar utfbyte[UTF_SIZ + 1] = {0x80,    0, 0xC0, 0xE0, 0xF0};
static uchar utfmask[UTF_SIZ + 1] = {0xC0, 0x80, 0xE0, 0xF0, 0xF8};
static Rune utfmin[UTF_SIZ + 1] = {       0,    0,  0x80,  0x800,  0x10000};
static Rune utfmax[UTF_SIZ + 1] = {0x10FFFF, 0x7F, 0x7FF, 0xFFFF, 0x10FFFF};

ssize_t
xwrite(int fd, const char *s, size_t len)
{
	size_t aux = len;
	ssize_t r;

	while (len > 0) {
		r = write(fd, s, len);
		if (r < 0)
			return r;
		len -= r;
		s += r;
	}

	return aux;
}

void *
xmalloc(size_t len)
{
	void *p;

	if (!(p = malloc(len)))
		die("malloc: %s\n", strerror(errno));

	return p;
}

void *
xrealloc(void *p, size_t len)
{
	if ((p = realloc(p, len)) == NULL)
		die("realloc: %s\n", strerror(errno));

	return p;
}

char *
xstrdup(char *s)
{
	if ((s = strdup(s)) == NULL)
		die("strdup: %s\n", strerror(errno));

	return s;
}

size_t
utf8decode(const char *c, Rune *u, size_t clen)
{
	size_t i, j, len, type;
	Rune udecoded;

	*u = UTF_INVALID;
	if (!clen)
		return 0;
	udecoded = utf8decodebyte(c[0], &len);
	if (!BETWEEN(len, 1, UTF_SIZ))
		return 1;
	for (i = 1, j = 1; i < clen && j < len; ++i, ++j) {
		udecoded = (udecoded << 6) | utf8decodebyte(c[i], &type);
		if (type != 0)
			return j;
	}
	if (j < len)
		return 0;
	*u = udecoded;
	utf8validate(u, len);

	return len;
}

Rune
utf8decodebyte(char c, size_t *i)
{
	for (*i = 0; *i < LEN(utfmask); ++(*i))
		if (((uchar)c & utfmask[*i]) == utfbyte[*i])
			return (uchar)c & ~utfmask[*i];

	return 0;
}

size_t
utf8encode(Rune u, char *c)
{
	size_t len, i;

	len = utf8validate(&u, 0);
	if (len > UTF_SIZ)
		return 0;

	for (i = len - 1; i != 0; --i) {
		c[i] = utf8encodebyte(u, 0);
		u >>= 6;
	}
	c[0] = utf8encodebyte(u, len);

	return len;
}

char
utf8encodebyte(Rune u, size_t i)
{
	return utfbyte[i] | (u & ~utfmask[i]);
}

size_t
utf8validate(Rune *u, size_t i)
{
	if (!BETWEEN(*u, utfmin[i], utfmax[i]) || BETWEEN(*u, 0xD800, 0xDFFF))
		*u = UTF_INVALID;
	for (i = 1; *u > utfmax[i]; ++i)
		;

	return i;
}

static const char base64_digits[] = {
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 62, 0, 0, 0,
	63, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 0, 0, 0, -1, 0, 0, 0, 0, 1,
	2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21,
	22, 23, 24, 25, 0, 0, 0, 0, 0, 0, 26, 27, 28, 29, 30, 31, 32, 33, 34,
	35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

char
base64dec_getc(const char **src)
{
	while (**src && !isprint(**src))
		(*src)++;
	return **src ? *((*src)++) : '=';  /* emulate padding if string ends */
}

char *
base64dec(const char *src)
{
	size_t in_len = strlen(src);
	char *result, *dst;

	if (in_len % 4)
		in_len += 4 - (in_len % 4);
	result = dst = xmalloc(in_len / 4 * 3 + 1);
	while (*src) {
		int a = base64_digits[(unsigned char) base64dec_getc(&src)];
		int b = base64_digits[(unsigned char) base64dec_getc(&src)];
		int c = base64_digits[(unsigned char) base64dec_getc(&src)];
		int d = base64_digits[(unsigned char) base64dec_getc(&src)];

		/* invalid input. 'a' can be -1, e.g. if src is "\n" (c-str) */
		if (a == -1 || b == -1)
			break;

		*dst++ = (a << 2) | ((b & 0x30) >> 4);
		if (c == -1)
			break;
		*dst++ = ((b & 0x0f) << 4) | ((c & 0x3c) >> 2);
		if (d == -1)
			break;
		*dst++ = ((c & 0x03) << 6) | d;
	}
	*dst = '\0';
	return result;
}

Line *
tgetline(Term *term, int n)
{
	n += term->line - term->buf;
	n %= term->maxrow;
	return term->buf + (n >= 0 ? n : n + term->maxrow);
}

Line *
tgetaltline(Term *term, int n)
{
	n += term->alt - term->altbuf;
	n %= term->maxrow;
	return term->altbuf + (n >= 0 ? n : n + term->maxrow);
}

int
tlinelen(Term *term, int y)
{
	int i = term->col;

	if ((*tgetline(term, y))[i - 1].mode & ATTR_WRAP)
		return i;

	while (i > 0 && (*tgetline(term, y))[i - 1].u == ' ')
		--i;

	return i;
}

void
die(const char *errstr, ...)
{
	va_list ap;

	va_start(ap, errstr);
	vfprintf(stderr, errstr, ap);
	va_end(ap);
	exit(1);
}

void
execsh(char *cmd, char **args)
{
	char *sh, *prog, *arg;
	const struct passwd *pw;

	errno = 0;
	if ((pw = getpwuid(getuid())) == NULL) {
		if (errno)
			die("getpwuid: %s\n", strerror(errno));
		else
			die("who are you?\n");
	}

	if ((sh = getenv("SHELL")) == NULL)
		sh = (pw->pw_shell[0]) ? pw->pw_shell : cmd;

	if (args) {
		prog = args[0];
		arg = NULL;
	} else if (scroll) {
		prog = scroll;
		arg = utmp ? utmp : sh;
	} else if (utmp) {
		prog = utmp;
		arg = NULL;
	} else {
		prog = sh;
		arg = NULL;
	}
	DEFAULT(args, ((char *[]) {prog, arg, NULL}));

	unsetenv("COLUMNS");
	unsetenv("LINES");
	unsetenv("TERMCAP");
	setenv("LOGNAME", pw->pw_name, 1);
	setenv("USER", pw->pw_name, 1);
	setenv("SHELL", sh, 1);
	setenv("HOME", pw->pw_dir, 1);
	setenv("TERM", termname, 1);

	signal(SIGCHLD, SIG_DFL);
	signal(SIGHUP, SIG_DFL);
	signal(SIGINT, SIG_DFL);
	signal(SIGQUIT, SIG_DFL);
	signal(SIGTERM, SIG_DFL);
	signal(SIGALRM, SIG_DFL);

	execvp(prog, args);
	_exit(1);
}

int
ttynew(Term *term, char *cmd, char *iofname, char **args, int *in, int *out, int *err)
{
	int m, s;
	struct winsize ws;
	ws.ws_row = term->row;
	ws.ws_col = term->col;
	int ipipe[2], opipe[2], epipe[2];

	if (in && pipe(ipipe) == -1) {
		*in = -1;
		in = NULL;
	}
	if (out && pipe(opipe) == -1) {
		*out = -1;
		out = NULL;
	}
	if (err && pipe(epipe) == -1) {
		*err = -1;
		err = NULL;
	}


	if (iofname) {
		term->mode |= MODE_PRINT;
		term->iofd = (!strcmp(iofname, "-")) ?
			  1 : open(iofname, O_WRONLY | O_CREAT, 0666);
		if (term->iofd < 0) {
			fprintf(stderr, "Error opening %s:%s\n",
				iofname, strerror(errno));
		}
	}

	/* seems to work fine on linux, openbsd and freebsd */
	if (openpty(&m, &s, NULL, NULL, &ws) < 0)
		die("openpty failed: %s\n", strerror(errno));

	switch (term->pid = fork()) {
	case -1:
		die("fork failed: %s\n", strerror(errno));
		break;
	case 0:
		close(term->iofd);
		setsid(); /* create a new process group */
		if (in) {
			close(ipipe[1]);
			dup2(ipipe[0], STDIN_FILENO);
			close(ipipe[0]);
		} else dup2(s, 0);
		if (out) {
			close(opipe[0]);
			dup2(opipe[1], STDOUT_FILENO);
			close(opipe[1]);
		} else dup2(s, 1);
		if (err) {
			close(epipe[0]);
			dup2(epipe[1], STDERR_FILENO);
			close(epipe[1]);
		} else dup2(s, 2);
		if (ioctl(s, TIOCSCTTY, NULL) < 0)
			die("ioctl TIOCSCTTY failed: %s\n", strerror(errno));
		close(s);
		close(m);
#ifdef __OpenBSD__
		if (pledge("stdio getpw proc exec", NULL) == -1)
			die("pledge\n");
#endif
		execsh(cmd, args);
		break;
	default:
#ifdef __OpenBSD__
		if (pledge("stdio rpath tty proc", NULL) == -1)
			die("pledge\n");
#endif
		close(s);
		term->cmdfd = m;
		break;
	}

	if (in) {
		close(ipipe[0]);
		*in = ipipe[1];
	}
	if (out) {
		close(opipe[1]);
		*out = opipe[0];
	}
	if (err) {
		close(epipe[1]);
		*out = epipe[0];
	}
	return term->cmdfd;
}

size_t
ttyread(Term *term)
{
	static char buf[BUFSIZ];
	static int buflen = 0;
	int ret, written;

	/* append read bytes to unprocessed bytes */
	ret = read(term->cmdfd, buf+buflen, LEN(buf)-buflen);

	switch (ret) {
	case 0:
		exit(0);
	case -1:
		die("couldn't read from shell: %s\n", strerror(errno));
	default:
		buflen += ret;
		written = twrite(term, buf, buflen, 0);
		buflen -= written;
		/* keep any incomplete UTF-8 byte sequence for the next call */
		if (buflen > 0)
			memmove(buf, buf + written, buflen);
		return ret;

	}
}

void
ttywrite(Term *term, const char *s, size_t n, int may_echo)
{
	const char *next;

	if (may_echo && IS_SET(MODE_ECHO))
		twrite(term, s, n, 1);

	if (!IS_SET(MODE_CRLF)) {
		ttywriteraw(term, s, n);
		return;
	}

	/* This is similar to how the kernel handles ONLCR for ttys */
	while (n > 0) {
		if (*s == '\r') {
			next = s + 1;
			ttywriteraw(term, "\r\n", 2);
		} else {
			next = memchr(s, '\r', n);
			DEFAULT(next, s + n);
			ttywriteraw(term, s, next - s);
		}
		n -= next - s;
		s = next;
	}
}

void
ttywriteraw(Term *term, const char *s, size_t n)
{
	fd_set wfd, rfd;
	ssize_t r;
	size_t lim = 256;

	/*
	 * Remember that we are using a pty, which might be a modem line.
	 * Writing too much will clog the line. That's why we are doing this
	 * dance.
	 * FIXME: Migrate the world to Plan 9.
	 */
	while (n > 0) {
		FD_ZERO(&wfd);
		FD_ZERO(&rfd);
		FD_SET(term->cmdfd, &wfd);
		FD_SET(term->cmdfd, &rfd);

		/* Check if we can write. */
		if (pselect(term->cmdfd+1, &rfd, &wfd, NULL, NULL, NULL) < 0) {
			if (errno == EINTR)
				continue;
			die("select failed: %s\n", strerror(errno));
		}
		if (FD_ISSET(term->cmdfd, &wfd)) {
			/*
			 * Only write the bytes written by ttywrite() or the
			 * default of 256. This seems to be a reasonable value
			 * for a serial line. Bigger values might clog the I/O.
			 */
			if ((r = write(term->cmdfd, s, (n < lim)? n : lim)) < 0)
				goto write_error;
			if (r < n) {
				/*
				 * We weren't able to write out everything.
				 * This means the buffer is getting full
				 * again. Empty it.
				 */
				if (n < lim)
					lim = ttyread(term);
				n -= r;
				s += r;
			} else {
				/* All bytes have been written. */
				break;
			}
		}
		if (FD_ISSET(term->cmdfd, &rfd))
			lim = ttyread(term);
	}
	return;

write_error:
	die("write error on tty: %s\n", strerror(errno));
}

void
ttyresize(Term *term, int tw, int th)
{
	struct winsize w;

	w.ws_row = term->row;
	w.ws_col = term->col;
	w.ws_xpixel = tw;
	w.ws_ypixel = th;
	if (ioctl(term->cmdfd, TIOCSWINSZ, &w) < 0)
		fprintf(stderr, "Couldn't set window size: %s\n", strerror(errno));
}

void
ttyhangup(Term *term)
{
	/* Send SIGHUP to shell */
	kill(term->pid, SIGHUP);
}

int
tattrset(Term *term, int attr)
{
	int i, j;

	for (i = 0; i < term->row-1; i++) {
		for (j = 0; j < term->col-1; j++) {
			if ((*tgetline(term, i))[j].mode & attr)
				return 1;
		}
	}

	return 0;
}

void
tsetdirt(Term *term, int top, int bot)
{
	int i;

	LIMIT(top, 0, term->row-1);
	LIMIT(bot, 0, term->row-1);

	for (i = top; i <= bot; i++)
		term->dirty[i] = 1;
}

void
tsetdirtattr(Term *term, int attr)
{
	int i, j;

	for (i = 0; i < term->row-1; i++) {
		for (j = 0; j < term->col-1; j++) {
			if ((*tgetline(term, i))[j].mode & attr) {
				tsetdirt(term, i, i);
				break;
			}
		}
	}
}

void
tfulldirt(Term *term)
{
	tsetdirt(term, 0, term->row-1);
}

void
tcursor(Term *term, int mode)
{
	int alt = IS_SET(MODE_ALTSCREEN);

	if (mode == CURSOR_SAVE) {
		term->cs[alt] = term->c;
	} else if (mode == CURSOR_LOAD) {
		term->c = term->cs[alt];
		tmoveto(term, term->cs[alt].x, term->cs[alt].y);
	}
}

void
treset(Term *term)
{
	uint i;

	term->c = (TCursor){{
		.mode = ATTR_NULL,
		.fg = term->defaultfg,
		.bg = term->defaultbg
	}, .x = 0, .y = 0, .state = CURSOR_DEFAULT};

	memset(term->tabs, 0, term->col * sizeof(*term->tabs));
	for (i = term->tabspaces; i < term->col; i += term->tabspaces)
		term->tabs[i] = 1;
	term->top = 0;
	term->bot = term->row - 1;
	term->mode = MODE_WRAP|MODE_UTF8;
	memset(term->trantbl, CS_USA, sizeof(term->trantbl));
	term->charset = 0;

	for (i = 0; i < 2; i++) {
		tmoveto(term, 0, 0);
		tcursor(term, CURSOR_SAVE);
		tclearregion(term, 0, 0, term->col-1, term->row-1);
		if (term->alt == NULL)
			break;
		tswapscreen(term);
	}
}

Term *
tnew(int col, int row, int hist, int alt, int deffg, int defbg, int ts)
{
	Term *term;
	if ((term = calloc(1, sizeof(Term))) == NULL)
		return NULL;
	term->c.attr.fg = term->defaultfg = deffg;
	term->c.attr.bg = term->defaultbg = defbg;
	term->maxcol = 0;
	term->maxrow = hist;
	term->seen = row;
	term->line = term->buf = xmalloc(hist * row * sizeof(Line));
	term->alt = term->altbuf = alt ? xmalloc(hist * row * sizeof(Line)) : NULL;
	term->tabspaces = ts;

	tresize(term, col, row);
	treset(term);
	return term;
}

void
tfree(Term *term)
{
	int i;
	for (i = 0; i < term->maxcol; i++) {
		free(term->buf[i]);
	}
	if (term->altbuf) {
		for (i = 0; i < term->maxcol; i++) {
			free(term->altbuf[i]);
		}
	}
	free(term->buf);
	free(term->altbuf);
	free(term->dirty);
	free(term->tabs);
	free(term->strescseq.buf);
	free(term);
}

void
tswapscreen(Term *term)
{
	Line *tmp = term->line;

	/* swap line pointers */
	term->line = term->alt;
	term->alt = tmp;

	/* swap underlying buffer pointers */
	tmp = term->buf;
	term->buf = term->altbuf;
	term->altbuf = tmp;

	term->mode ^= MODE_ALTSCREEN;
	tfulldirt(term);
}

void
tscrolldown(Term *term, int orig, int n, int copyhist)
{
	int i;
	Line temp;

	LIMIT(n, 0, term->bot-orig+1);

	tsetdirt(term, orig, term->bot-n);
	tclearregion(term, 0, term->bot-n+1, term->col-1, term->bot);

	for (i = term->bot; i >= orig+n; i--) {
		temp = *tgetline(term, i);
		*tgetline(term, i) = (*tgetline(term, i-n));
		(*tgetline(term, i-n)) = temp;
	}
}

void
tscrollup(Term *term, int orig, int n, int copyhist)
{

	int i;
	Line temp;

	LIMIT(n, 0, term->bot-orig+1);

	tclearregion(term, 0, orig, term->col-1, orig+n-1);
	tsetdirt(term, orig+n, term->bot);

	for (i = orig; i <= term->bot-n; i++) {
		temp = *tgetline(term, i);
		*tgetline(term, i) = (*tgetline(term, i+n));
		(*tgetline(term, i+n)) = temp;
	}
}

void
tnewline(Term *term, int first_col)
{
	int y = term->c.y;

	if (y == term->bot) {
		tscrollup(term, term->top, 1, 1);
	} else {
		y++;
	}
	tmoveto(term, first_col ? 0 : term->c.x, y);
}

void
csiparse(Term *term)
{
	char *p = term->csiescseq.buf, *np;
	long int v;

	term->csiescseq.narg = 0;
	if (*p == '?') {
		term->csiescseq.priv = 1;
		p++;
	}

	term->csiescseq.buf[term->csiescseq.len] = '\0';
	while (p < term->csiescseq.buf+term->csiescseq.len) {
		np = NULL;
		v = strtol(p, &np, 10);
		if (np == p)
			v = 0;
		if (v == LONG_MAX || v == LONG_MIN)
			v = -1;
		term->csiescseq.arg[term->csiescseq.narg++] = v;
		p = np;
		if (*p != ';' || term->csiescseq.narg == ESC_ARG_SIZ)
			break;
		p++;
	}
	term->csiescseq.mode[0] = *p++;
	term->csiescseq.mode[1] = (p < term->csiescseq.buf+term->csiescseq.len) ? *p : '\0';
}

/* for absolute user moves, when decom is set */
void
tmoveato(Term *term, int x, int y)
{
	tmoveto(term, x, y + ((term->c.state & CURSOR_ORIGIN) ? term->top: 0));
}

void
tmoveto(Term *term, int x, int y)
{
	int miny, maxy;

	if (term->c.state & CURSOR_ORIGIN) {
		miny = term->top;
		maxy = term->bot;
	} else {
		miny = 0;
		maxy = term->row - 1;
	}
	term->c.state &= ~CURSOR_WRAPNEXT;
	term->c.x = LIMIT(x, 0, term->col-1);
	term->c.y = LIMIT(y, miny, maxy);
}

void
tsetchar(Term *term, Rune u, Glyph *attr, int x, int y)
{
	static char *vt100_0[62] = { /* 0x41 - 0x7e */
		"↑", "↓", "→", "←", "█", "▚", "☃", /* A - G */
		0, 0, 0, 0, 0, 0, 0, 0, /* H - O */
		0, 0, 0, 0, 0, 0, 0, 0, /* P - W */
		0, 0, 0, 0, 0, 0, 0, " ", /* X - _ */
		"◆", "▒", "␉", "␌", "␍", "␊", "°", "±", /* ` - g */
		"␤", "␋", "┘", "┐", "┌", "└", "┼", "⎺", /* h - o */
		"⎻", "─", "⎼", "⎽", "├", "┤", "┴", "┬", /* p - w */
		"│", "≤", "≥", "π", "≠", "£", "·", /* x - ~ */
	};

	/*
	 * The table is proudly stolen from rxvt.
	 */
	if (term->trantbl[term->charset] == CS_GRAPHIC0 &&
	   BETWEEN(u, 0x41, 0x7e) && vt100_0[u - 0x41])
		utf8decode(vt100_0[u - 0x41], &u, UTF_SIZ);

	if ((*tgetline(term, y))[x].mode & ATTR_WIDE) {
		if (x+1 < term->col) {
			(*tgetline(term, y))[x+1].u = ' ';
			(*tgetline(term, y))[x+1].mode &= ~ATTR_WDUMMY;
		}
	} else if ((*tgetline(term, y))[x].mode & ATTR_WDUMMY) {
		(*tgetline(term, y))[x-1].u = ' ';
		(*tgetline(term, y))[x-1].mode &= ~ATTR_WIDE;
	}

	term->dirty[y] = 1;
	(*tgetline(term, y))[x] = *attr;
	(*tgetline(term, y))[x].u = u;
}

void
tclearregion(Term *term, int x1, int y1, int x2, int y2)
{
	int x, y, temp;
	Glyph *gp;

	if (x1 > x2)
		temp = x1, x1 = x2, x2 = temp;
	if (y1 > y2)
		temp = y1, y1 = y2, y2 = temp;

	for (y = y1; y <= y2; y++) {
		term->dirty[y] = 1;
		for (x = x1; x <= x2; x++) {
			gp = *tgetline(term, y) + x;
			gp->fg = term->c.attr.fg;
			gp->bg = term->c.attr.bg;
			gp->mode = 0;
			gp->u = ' ';
		}
	}
}

void
tdeletechar(Term *term, int n)
{
	int dst, src, size;
	Glyph *line;

	LIMIT(n, 0, term->col - term->c.x);

	dst = term->c.x;
	src = term->c.x + n;
	size = term->col - src;
	line = (*tgetline(term, term->c.y));

	memmove(&line[dst], &line[src], size * sizeof(Glyph));
	tclearregion(term, term->col-n, term->c.y, term->col-1, term->c.y);
}

void
tinsertblank(Term *term, int n)
{
	int dst, src, size;
	Glyph *line;

	LIMIT(n, 0, term->col - term->c.x);

	dst = term->c.x + n;
	src = term->c.x;
	size = term->col - dst;
	line = (*tgetline(term, term->c.y));

	memmove(&line[dst], &line[src], size * sizeof(Glyph));
	tclearregion(term, src, term->c.y, dst - 1, term->c.y);
}

void
tinsertblankline(Term *term, int n)
{
	if (BETWEEN(term->c.y, term->top, term->bot))
		tscrolldown(term, term->c.y, n, 0);
}

void
tdeleteline(Term *term, int n)
{
	if (BETWEEN(term->c.y, term->top, term->bot))
		tscrollup(term, term->c.y, n, 0);
}

int32_t
tdefcolor(Term *term, int *attr, int *npar, int l)
{
	int32_t idx = -1;
	uint r, g, b;

	switch (attr[*npar + 1]) {
	case 2: /* direct color in RGB space */
		if (*npar + 4 >= l) {
			fprintf(stderr,
				"erresc(38): Incorrect number of parameters (%d)\n",
				*npar);
			break;
		}
		r = attr[*npar + 2];
		g = attr[*npar + 3];
		b = attr[*npar + 4];
		*npar += 4;
		if (!BETWEEN(r, 0, 255) || !BETWEEN(g, 0, 255) || !BETWEEN(b, 0, 255))
			fprintf(stderr, "erresc: bad rgb color (%u,%u,%u)\n",
				r, g, b);
		else
			idx = TRUECOLOR(r, g, b);
		break;
	case 5: /* indexed color */
		if (*npar + 2 >= l) {
			fprintf(stderr,
				"erresc(38): Incorrect number of parameters (%d)\n",
				*npar);
			break;
		}
		*npar += 2;
		if (!BETWEEN(attr[*npar], 0, 255))
			fprintf(stderr, "erresc: bad fgcolor %d\n", attr[*npar]);
		else
			idx = attr[*npar];
		break;
	case 0: /* implemented defined (only foreground) */
	case 1: /* transparent */
	case 3: /* direct color in CMY space */
	case 4: /* direct color in CMYK space */
	default:
		fprintf(stderr,
		        "erresc(38): gfx attr %d unknown\n", attr[*npar]);
		break;
	}

	return idx;
}

void
tsetattr(Term *term, int *attr, int l)
{
	int i;
	int32_t idx;

	for (i = 0; i < l; i++) {
		switch (attr[i]) {
		case 0:
			term->c.attr.mode &= ~(
				ATTR_BOLD       |
				ATTR_FAINT      |
				ATTR_ITALIC     |
				ATTR_UNDERLINE  |
				ATTR_BLINK      |
				ATTR_REVERSE    |
				ATTR_INVISIBLE  |
				ATTR_STRUCK     );
			term->c.attr.fg = term->defaultfg;
			term->c.attr.bg = term->defaultbg;
			break;
		case 1:
			term->c.attr.mode |= ATTR_BOLD;
			break;
		case 2:
			term->c.attr.mode |= ATTR_FAINT;
			break;
		case 3:
			term->c.attr.mode |= ATTR_ITALIC;
			break;
		case 4:
			term->c.attr.mode |= ATTR_UNDERLINE;
			break;
		case 5: /* slow blink */
			/* FALLTHROUGH */
		case 6: /* rapid blink */
			term->c.attr.mode |= ATTR_BLINK;
			break;
		case 7:
			term->c.attr.mode |= ATTR_REVERSE;
			break;
		case 8:
			term->c.attr.mode |= ATTR_INVISIBLE;
			break;
		case 9:
			term->c.attr.mode |= ATTR_STRUCK;
			break;
		case 22:
			term->c.attr.mode &= ~(ATTR_BOLD | ATTR_FAINT);
			break;
		case 23:
			term->c.attr.mode &= ~ATTR_ITALIC;
			break;
		case 24:
			term->c.attr.mode &= ~ATTR_UNDERLINE;
			break;
		case 25:
			term->c.attr.mode &= ~ATTR_BLINK;
			break;
		case 27:
			term->c.attr.mode &= ~ATTR_REVERSE;
			break;
		case 28:
			term->c.attr.mode &= ~ATTR_INVISIBLE;
			break;
		case 29:
			term->c.attr.mode &= ~ATTR_STRUCK;
			break;
		case 38:
			if ((idx = tdefcolor(term, attr, &i, l)) >= 0)
				term->c.attr.fg = idx;
			break;
		case 39:
			term->c.attr.fg = term->defaultfg;
			break;
		case 48:
			if ((idx = tdefcolor(term, attr, &i, l)) >= 0)
				term->c.attr.bg = idx;
			break;
		case 49:
			term->c.attr.bg = term->defaultbg;
			break;
		default:
			if (BETWEEN(attr[i], 30, 37)) {
				term->c.attr.fg = attr[i] - 30;
			} else if (BETWEEN(attr[i], 40, 47)) {
				term->c.attr.bg = attr[i] - 40;
			} else if (BETWEEN(attr[i], 90, 97)) {
				term->c.attr.fg = attr[i] - 90 + 8;
			} else if (BETWEEN(attr[i], 100, 107)) {
				term->c.attr.bg = attr[i] - 100 + 8;
			} else {
				fprintf(stderr,
					"erresc(default): gfx attr %d unknown\n",
					attr[i]);
				csidump(term);
			}
			break;
		}
	}
}

void
tsetscroll(Term *term, int t, int b)
{
	int temp;

	LIMIT(t, 0, term->row-1);
	LIMIT(b, 0, term->row-1);
	if (t > b) {
		temp = t;
		t = b;
		b = temp;
	}
	term->top = t;
	term->bot = b;
}

void
tsetmode(Term *term, int priv, int set, int *args, int narg)
{
	int alt, *lim;

	for (lim = args + narg; args < lim; ++args) {
		if (priv) {
			switch (*args) {
			case 1: /* DECCKM -- Cursor key */
				term->handler(term, set ? ST_SET : ST_UNSET, (Arg){.ui = MODE_APPCURSOR});
				break;
			case 5: /* DECSCNM -- Reverse video */
				term->handler(term, set ? ST_SET : ST_UNSET, (Arg){.ui = MODE_REVERSE});
				break;
			case 6: /* DECOM -- Origin */
				MODBIT(term->c.state, set, CURSOR_ORIGIN);
				tmoveato(term, 0, 0);
				break;
			case 7: /* DECAWM -- Auto wrap */
				MODBIT(term->mode, set, MODE_WRAP);
				break;
			case 0:  /* Error (IGNORED) */
			case 2:  /* DECANM -- ANSI/VT52 (IGNORED) */
			case 3:  /* DECCOLM -- Column  (IGNORED) */
			case 4:  /* DECSCLM -- Scroll (IGNORED) */
			case 8:  /* DECARM -- Auto repeat (IGNORED) */
			case 18: /* DECPFF -- Printer feed (IGNORED) */
			case 19: /* DECPEX -- Printer extent (IGNORED) */
			case 42: /* DECNRCM -- National characters (IGNORED) */
			case 12: /* att610 -- Start blinking cursor (IGNORED) */
				break;
			case 25: /* DECTCEM -- Text Cursor Enable Mode */
				term->handler(term, !set ? ST_SET : ST_UNSET, (Arg){.ui = MODE_HIDE});
				break;
			case 9:    /* X10 mouse compatibility mode */
				term->handler(term, ST_POINTERMOTION, (Arg){.i = 0});
				term->handler(term, ST_UNSET, (Arg){.ui = MODE_MOUSE});
				term->handler(term, set ? ST_SET : ST_UNSET, (Arg){.ui = MODE_MOUSEX10});
				break;
			case 1000: /* 1000: report button press */
				term->handler(term, ST_POINTERMOTION, (Arg){.i = 0});
				term->handler(term, ST_UNSET, (Arg){.ui = MODE_MOUSE});
				term->handler(term, set ? ST_SET : ST_UNSET, (Arg){.ui = MODE_MOUSEBTN});
				break;
			case 1002: /* 1002: report motion on button press */
				term->handler(term, ST_POINTERMOTION, (Arg){.i = 0});
				term->handler(term, ST_UNSET, (Arg){.ui = MODE_MOUSE});
				term->handler(term, set ? ST_SET : ST_UNSET, (Arg){.ui = MODE_MOUSEMOTION});
				break;
			case 1003: /* 1003: enable all mouse motions */
				term->handler(term, ST_POINTERMOTION, (Arg){.i = set});
				term->handler(term, ST_UNSET, (Arg){.ui = MODE_MOUSE});
				term->handler(term, set ? ST_SET : ST_UNSET, (Arg){.ui = MODE_MOUSEMANY});
				break;
			case 1004: /* 1004: send focus events to tty */
				term->handler(term, set ? ST_SET : ST_UNSET, (Arg){.ui = MODE_FOCUS});
				break;
			case 1006: /* 1006: extended reporting mode */
				term->handler(term, set ? ST_SET : ST_UNSET, (Arg){.ui = MODE_MOUSESGR});
				break;
			case 1034:
				term->handler(term, set ? ST_SET : ST_UNSET, (Arg){.ui = MODE_8BIT});
				break;
			case 1049: /* swap screen & set/restore cursor as xterm */
				if (!term->alt)
					break;
				tcursor(term, (set) ? CURSOR_SAVE : CURSOR_LOAD);
				/* FALLTHROUGH */
			case 47: /* swap screen */
			case 1047:
				if (term->alt == NULL)
					break;
				alt = IS_SET(MODE_ALTSCREEN);
				if (alt) {
					tclearregion(term, 0, 0, term->col-1,
							term->row-1);
				}
				if (set ^ alt) /* set is always 1 or 0 */
					tswapscreen(term);
				if (*args != 1049)
					break;
				/* FALLTHROUGH */
			case 1048:
				tcursor(term, (set) ? CURSOR_SAVE : CURSOR_LOAD);
				break;
			case 2004: /* 2004: bracketed paste mode */
				term->handler(term, set ? ST_SET : ST_UNSET, (Arg){.ui = MODE_BRCKTPASTE});
				break;
			/* Not implemented mouse modes. See comments there. */
			case 1001: /* mouse highlight mode; can hang the
				      terminal by design when implemented. */
			case 1005: /* UTF-8 mouse mode; will confuse
				      applications not supporting UTF-8
				      and luit. */
			case 1015: /* urxvt mangled mouse mode; incompatible
				      and can be mistaken for other control
				      codes. */
				break;
			default:
				fprintf(stderr,
					"erresc: unknown private set/reset mode %d\n",
					*args);
				break;
			}
		} else {
			switch (*args) {
			case 0:  /* Error (IGNORED) */
				break;
			case 2:
				term->handler(term, set ? ST_SET : ST_UNSET, (Arg){.ui = MODE_KBDLOCK});
				break;
			case 4:  /* IRM -- Insertion-replacement */
				MODBIT(term->mode, set, MODE_INSERT);
				break;
			case 12: /* SRM -- Send/Receive */
				MODBIT(term->mode, !set, MODE_ECHO);
				break;
			case 20: /* LNM -- Linefeed/new line */
				MODBIT(term->mode, set, MODE_CRLF);
				break;
			default:
				fprintf(stderr,
					"erresc: unknown set/reset mode %d\n",
					*args);
				break;
			}
		}
	}
}

void
csihandle(Term *term)
{
	char buf[40];
	int len;

	switch (term->csiescseq.mode[0]) {
	default:
	unknown:
		fprintf(stderr, "erresc: unknown csi ");
		csidump(term);
		/* die(""); */
		break;
	case '@': /* ICH -- Insert <n> blank char */
		DEFAULT(term->csiescseq.arg[0], 1);
		tinsertblank(term, term->csiescseq.arg[0]);
		break;
	case 'A': /* CUU -- Cursor <n> Up */
		DEFAULT(term->csiescseq.arg[0], 1);
		tmoveto(term, term->c.x, term->c.y-term->csiescseq.arg[0]);
		break;
	case 'B': /* CUD -- Cursor <n> Down */
	case 'e': /* VPR --Cursor <n> Down */
		DEFAULT(term->csiescseq.arg[0], 1);
		tmoveto(term, term->c.x, term->c.y+term->csiescseq.arg[0]);
		break;
	case 'i': /* MC -- Media Copy */
		switch (term->csiescseq.arg[0]) {
		case 0:
			tdump(term);
			break;
		case 1:
			tdumpline(term, term->c.y);
			break;
		case 2:
			/* TODO - return event to dump selection */
			/* tdumpsel(term); */
			break;
		case 4:
			term->mode &= ~MODE_PRINT;
			break;
		case 5:
			term->mode |= MODE_PRINT;
			break;
		}
		break;
	case 'c': /* DA -- Device Attributes */
		if (term->csiescseq.arg[0] == 0)
			ttywrite(term, vtiden, strlen(vtiden), 0);
		break;
	case 'b': /* REP -- if last char is printable print it <n> more times */
		DEFAULT(term->csiescseq.arg[0], 1);
		if (term->lastc)
			while (term->csiescseq.arg[0]-- > 0)
				tputc(term, term->lastc);
		break;
	case 'C': /* CUF -- Cursor <n> Forward */
	case 'a': /* HPR -- Cursor <n> Forward */
		DEFAULT(term->csiescseq.arg[0], 1);
		tmoveto(term, term->c.x+term->csiescseq.arg[0], term->c.y);
		break;
	case 'D': /* CUB -- Cursor <n> Backward */
		DEFAULT(term->csiescseq.arg[0], 1);
		tmoveto(term, term->c.x-term->csiescseq.arg[0], term->c.y);
		break;
	case 'E': /* CNL -- Cursor <n> Down and first col */
		DEFAULT(term->csiescseq.arg[0], 1);
		tmoveto(term, 0, term->c.y+term->csiescseq.arg[0]);
		break;
	case 'F': /* CPL -- Cursor <n> Up and first col */
		DEFAULT(term->csiescseq.arg[0], 1);
		tmoveto(term, 0, term->c.y-term->csiescseq.arg[0]);
		break;
	case 'g': /* TBC -- Tabulation clear */
		switch (term->csiescseq.arg[0]) {
		case 0: /* clear current tab stop */
			term->tabs[term->c.x] = 0;
			break;
		case 3: /* clear all the tabs */
			memset(term->tabs, 0, term->col * sizeof(*term->tabs));
			break;
		default:
			goto unknown;
		}
		break;
	case 'G': /* CHA -- Move to <col> */
	case '`': /* HPA */
		DEFAULT(term->csiescseq.arg[0], 1);
		tmoveto(term, term->csiescseq.arg[0]-1, term->c.y);
		break;
	case 'H': /* CUP -- Move to <row> <col> */
	case 'f': /* HVP */
		DEFAULT(term->csiescseq.arg[0], 1);
		DEFAULT(term->csiescseq.arg[1], 1);
		tmoveato(term, term->csiescseq.arg[1]-1, term->csiescseq.arg[0]-1);
		break;
	case 'I': /* CHT -- Cursor Forward Tabulation <n> tab stops */
		DEFAULT(term->csiescseq.arg[0], 1);
		tputtab(term, term->csiescseq.arg[0]);
		break;
	case 'J': /* ED -- Clear screen */
		switch (term->csiescseq.arg[0]) {
		case 0: /* below */
			tclearregion(term, term->c.x, term->c.y, term->maxcol-1, term->c.y);
			if (term->c.y < term->row-1) {
				tclearregion(term, 0, term->c.y+1, term->maxcol-1,
						term->row-1);
			}
			break;
		case 1: /* above */
			if (term->c.y > 1)
				tclearregion(term, 0, 0, term->maxcol-1, term->c.y-1);
			tclearregion(term, 0, term->c.y, term->c.x, term->c.y);
			break;
		case 2: /* all */
			tclearregion(term, 0, 0, term->maxcol-1, term->row-1);
			break;
		default:
			goto unknown;
		}
		break;
	case 'K': /* EL -- Clear line */
		switch (term->csiescseq.arg[0]) {
		case 0: /* right */
			tclearregion(term, term->c.x, term->c.y, term->col-1,
					term->c.y);
			break;
		case 1: /* left */
			tclearregion(term, 0, term->c.y, term->c.x, term->c.y);
			break;
		case 2: /* all */
			tclearregion(term, 0, term->c.y, term->col-1, term->c.y);
			break;
		}
		break;
	case 'S': /* SU -- Scroll <n> line up */
		DEFAULT(term->csiescseq.arg[0], 1);
		tscrollup(term, term->top, term->csiescseq.arg[0], 0);
		break;
	case 'T': /* SD -- Scroll <n> line down */
		DEFAULT(term->csiescseq.arg[0], 1);
		tscrolldown(term, term->top, term->csiescseq.arg[0], 0);
		break;
	case 'L': /* IL -- Insert <n> blank lines */
		DEFAULT(term->csiescseq.arg[0], 1);
		tinsertblankline(term, term->csiescseq.arg[0]);
		break;
	case 'l': /* RM -- Reset Mode */
		tsetmode(term, term->csiescseq.priv, 0, term->csiescseq.arg, term->csiescseq.narg);
		break;
	case 'M': /* DL -- Delete <n> lines */
		DEFAULT(term->csiescseq.arg[0], 1);
		tdeleteline(term, term->csiescseq.arg[0]);
		break;
	case 'X': /* ECH -- Erase <n> char */
		DEFAULT(term->csiescseq.arg[0], 1);
		tclearregion(term, term->c.x, term->c.y,
				term->c.x + term->csiescseq.arg[0] - 1, term->c.y);
		break;
	case 'P': /* DCH -- Delete <n> char */
		DEFAULT(term->csiescseq.arg[0], 1);
		tdeletechar(term, term->csiescseq.arg[0]);
		break;
	case 'Z': /* CBT -- Cursor Backward Tabulation <n> tab stops */
		DEFAULT(term->csiescseq.arg[0], 1);
		tputtab(term, term->csiescseq.arg[0]);
		break;
	case 'd': /* VPA -- Move to <row> */
		DEFAULT(term->csiescseq.arg[0], 1);
		tmoveato(term, term->c.x, term->csiescseq.arg[0]-1);
		break;
	case 'h': /* SM -- Set terminal mode */
		tsetmode(term, term->csiescseq.priv, 1, term->csiescseq.arg, term->csiescseq.narg);
		break;
	case 'm': /* SGR -- Terminal attribute (color) */
		tsetattr(term, term->csiescseq.arg, term->csiescseq.narg);
		break;
	case 'n': /* DSR – Device Status Report (cursor position) */
		if (term->csiescseq.arg[0] == 6) {
			len = snprintf(buf, sizeof(buf),"\033[%i;%iR",
					term->c.y+1, term->c.x+1);
			ttywrite(term, buf, len, 0);
		}
		break;
	case 'r': /* DECSTBM -- Set Scrolling Region */
		if (term->csiescseq.priv) {
			goto unknown;
		} else {
			DEFAULT(term->csiescseq.arg[0], 1);
			DEFAULT(term->csiescseq.arg[1], term->row);
			tsetscroll(term, term->csiescseq.arg[0]-1, term->csiescseq.arg[1]-1);
			tmoveato(term, 0, 0);
		}
		break;
	case 's': /* DECSC -- Save cursor position (ANSI.SYS) */
		tcursor(term, CURSOR_SAVE);
		break;
	case 'u': /* DECRC -- Restore cursor position (ANSI.SYS) */
		tcursor(term, CURSOR_LOAD);
		break;
	case ' ':
		switch (term->csiescseq.mode[1]) {
		case 'q': /* DECSCUSR -- Set Cursor Style */
			term->handler(term, ST_CURSORSTYLE, (Arg){.i = term->csiescseq.arg[0]});
			break;
		default:
			goto unknown;
		}
		break;
	}
}

void
csidump(Term *term)
{
	size_t i;
	uint c;

	fprintf(stderr, "ESC[");
	for (i = 0; i < term->csiescseq.len; i++) {
		c = term->csiescseq.buf[i] & 0xff;
		if (isprint(c)) {
			putc(c, stderr);
		} else if (c == '\n') {
			fprintf(stderr, "(\\n)");
		} else if (c == '\r') {
			fprintf(stderr, "(\\r)");
		} else if (c == 0x1b) {
			fprintf(stderr, "(\\e)");
		} else {
			fprintf(stderr, "(%02x)", c);
		}
	}
	putc('\n', stderr);
}

void
csireset(Term *term)
{
	memset(&term->csiescseq, 0, sizeof(term->csiescseq));
}

void
strhandle(Term *term)
{
	char *p = NULL, *dec;
	int j, narg, par;

	term->esc &= ~(ESC_STR_END|ESC_STR);
	strparse(term);
	par = (narg = term->strescseq.narg) ? atoi(term->strescseq.args[0]) : 0;

	switch (term->strescseq.type) {
	case ']': /* OSC -- Operating System Command */
		switch (par) {
		case 0:
			if (narg > 1) {
				term->handler(term, ST_BELL, (Arg){.s = term->strescseq.args[1]});
				term->handler(term, ST_ICONTITLE, (Arg){.s = term->strescseq.args[1]});
			}
			return;
		case 1:
			if (narg > 1)
				term->handler(term, ST_ICONTITLE, (Arg){.s = term->strescseq.args[1]});
			return;
		case 2:
			if (narg > 1)
				term->handler(term, ST_BELL, (Arg){.s = term->strescseq.args[1]});
			return;
		case 52:
			if (narg > 2) {
				dec = base64dec(term->strescseq.args[2]);
				if (dec) {
					term->handler(term, ST_COPY, (Arg){.s = dec});
				} else {
					fprintf(stderr, "erresc: invalid base64\n");
				}
			}
			return;
		case 4: /* color set */
			if (narg < 3)
				break;
			p = term->strescseq.args[2];
			/* FALLTHROUGH */
		case 104: /* color reset, here p = NULL */
			j = (narg > 1) ? atoi(term->strescseq.args[1]) : -1;
			/* we comment out the if & else because all colors are assumed to be valid */
			if (term->handler(term, ST_COLORNAME, (Arg){.v = (Arg[2]){{.i = j}, {.s = p}}})) {
				if (par == 104 && narg <= 1)
					return; /* color reset without parameter */
				fprintf(stderr, "erresc: invalid color j=%d, p=%s\n",
				        j, p ? p : "(null)");
			} else {
				/*
				 * TODO if term->defaultbg color is changed, borders
				 * are dirty
				 */
				/* tredraw(term); */
			}
			return;
		}
		break;
	case 'k': /* old title set compatibility */
		term->handler(term, ST_BELL, (Arg){.s = term->strescseq.args[0]});
		return;
	case 'P': /* DCS -- Device Control String */
	case '_': /* APC -- Application Program Command */
	case '^': /* PM -- Privacy Message */
		return;
	}

	fprintf(stderr, "erresc: unknown str ");
	strdump(term);
}

void
strparse(Term *term)
{
	int c;
	char *p = term->strescseq.buf;

	term->strescseq.narg = 0;
	term->strescseq.buf[term->strescseq.len] = '\0';

	if (*p == '\0')
		return;

	while (term->strescseq.narg < STR_ARG_SIZ) {
		term->strescseq.args[term->strescseq.narg++] = p;
		while ((c = *p) != ';' && c != '\0')
			++p;
		if (c == '\0')
			return;
		*p++ = '\0';
	}
}

void
strdump(Term *term)
{
	size_t i;
	uint c;

	fprintf(stderr, "ESC%c", term->strescseq.type);
	for (i = 0; i < term->strescseq.len; i++) {
		c = term->strescseq.buf[i] & 0xff;
		if (c == '\0') {
			putc('\n', stderr);
			return;
		} else if (isprint(c)) {
			putc(c, stderr);
		} else if (c == '\n') {
			fprintf(stderr, "(\\n)");
		} else if (c == '\r') {
			fprintf(stderr, "(\\r)");
		} else if (c == 0x1b) {
			fprintf(stderr, "(\\e)");
		} else {
			fprintf(stderr, "(%02x)", c);
		}
	}
	fprintf(stderr, "ESC\\\n");
}

void
strreset(Term *term)
{
	term->strescseq = (STREscape){
		.buf = xrealloc(term->strescseq.buf, STR_BUF_SIZ),
		.siz = STR_BUF_SIZ,
	};
}

void
tsendbreak(Term *term)
{
	if (tcsendbreak(term->cmdfd, 0))
		perror("Error sending break");
}

void
tprinter(Term *term, char *s, size_t len)
{
	if (term->iofd != -1 && xwrite(term->iofd, s, len) < 0) {
		perror("Error writing to output file");
		close(term->iofd);
		term->iofd = -1;
	}
}

void
ttoggleprinter(Term *term)
{
	term->mode ^= MODE_PRINT;
}

void
tprintscreen(Term *term)
{
	tdump(term);
}

void
tdumpline(Term *term, int n)
{
	char buf[UTF_SIZ];
	Glyph *bp, *end;

	bp = tgetline(term, n)[0];
	end = &bp[MIN(tlinelen(term, n), term->col) - 1];
	if (bp != end || bp->u != ' ') {
		for ( ;bp <= end; ++bp)
			tprinter(term, buf, utf8encode(bp->u, buf));
	}
	tprinter(term, "\n", 1);
}

void
tdump(Term *term)
{
	int i;

	for (i = 0; i < term->row; ++i)
		tdumpline(term, i);
}

void
tputtab(Term *term, int n)
{
	uint x = term->c.x;

	if (n > 0) {
		while (x < term->col && n--)
			for (++x; x < term->col && !term->tabs[x]; ++x)
				/* nothing */ ;
	} else if (n < 0) {
		while (x > 0 && n++)
			for (--x; x > 0 && !term->tabs[x]; --x)
				/* nothing */ ;
	}
	term->c.x = LIMIT(x, 0, term->col-1);
}

void
tdefutf8(Term *term, char ascii)
{
	if (ascii == 'G')
		term->mode |= MODE_UTF8;
	else if (ascii == '@')
		term->mode &= ~MODE_UTF8;
}

void
tdeftran(Term *term, char ascii)
{
	static char cs[] = "0B";
	static int vcs[] = {CS_GRAPHIC0, CS_USA};
	char *p;

	if ((p = strchr(cs, ascii)) == NULL) {
		fprintf(stderr, "esc unhandled charset: ESC ( %c\n", ascii);
	} else {
		term->trantbl[term->icharset] = vcs[p - cs];
	}
}

void
tdectest(Term *term, char c)
{
	int x, y;

	if (c == '8') { /* DEC screen alignment test. */
		for (x = 0; x < term->col; ++x) {
			for (y = 0; y < term->row; ++y)
				tsetchar(term, 'E', &term->c.attr, x, y);
		}
	}
}

void
tstrsequence(Term *term, uchar c)
{
	strreset(term);

	switch (c) {
	case 0x90:   /* DCS -- Device Control String */
		c = 'P';
		break;
	case 0x9f:   /* APC -- Application Program Command */
		c = '_';
		break;
	case 0x9e:   /* PM -- Privacy Message */
		c = '^';
		break;
	case 0x9d:   /* OSC -- Operating System Command */
		c = ']';
		break;
	}
	term->strescseq.type = c;
	term->esc |= ESC_STR;
}

void
tcontrolcode(Term *term, uchar ascii)
{
	switch (ascii) {
	case '\t':   /* HT */
		tputtab(term, 1);
		return;
	case '\b':   /* BS */
		tmoveto(term, term->c.x-1, term->c.y);
		return;
	case '\r':   /* CR */
		tmoveto(term, 0, term->c.y);
		return;
	case '\f':   /* LF */
	case '\v':   /* VT */
	case '\n':   /* LF */
		/* go to first col if the mode is set */
		tnewline(term, IS_SET(MODE_CRLF));
		return;
	case '\a':   /* BEL */
		if (term->esc & ESC_STR_END) {
			/* backwards compatibility to xterm */
			strhandle(term);
		} else {
			term->handler(term, ST_BELL, (Arg){0});
		}
		break;
	case '\033': /* ESC */
		csireset(term);
		term->esc &= ~(ESC_CSI|ESC_ALTCHARSET|ESC_TEST);
		term->esc |= ESC_START;
		return;
	case '\016': /* SO (LS1 -- Locking shift 1) */
	case '\017': /* SI (LS0 -- Locking shift 0) */
		term->charset = 1 - (ascii - '\016');
		return;
	case '\032': /* SUB */
		tsetchar(term, '?', &term->c.attr, term->c.x, term->c.y);
	case '\030': /* CAN */
		csireset(term);
		break;
	case '\005': /* ENQ (IGNORED) */
	case '\000': /* NUL (IGNORED) */
	case '\021': /* XON (IGNORED) */
	case '\023': /* XOFF (IGNORED) */
	case 0177:   /* DEL (IGNORED) */
		return;
	case 0x80:   /* TODO: PAD */
	case 0x81:   /* TODO: HOP */
	case 0x82:   /* TODO: BPH */
	case 0x83:   /* TODO: NBH */
	case 0x84:   /* TODO: IND */
		break;
	case 0x85:   /* NEL -- Next line */
		tnewline(term, 1); /* always go to first col */
		break;
	case 0x86:   /* TODO: SSA */
	case 0x87:   /* TODO: ESA */
		break;
	case 0x88:   /* HTS -- Horizontal tab stop */
		term->tabs[term->c.x] = 1;
		break;
	case 0x89:   /* TODO: HTJ */
	case 0x8a:   /* TODO: VTS */
	case 0x8b:   /* TODO: PLD */
	case 0x8c:   /* TODO: PLU */
	case 0x8d:   /* TODO: RI */
	case 0x8e:   /* TODO: SS2 */
	case 0x8f:   /* TODO: SS3 */
	case 0x91:   /* TODO: PU1 */
	case 0x92:   /* TODO: PU2 */
	case 0x93:   /* TODO: STS */
	case 0x94:   /* TODO: CCH */
	case 0x95:   /* TODO: MW */
	case 0x96:   /* TODO: SPA */
	case 0x97:   /* TODO: EPA */
	case 0x98:   /* TODO: SOS */
	case 0x99:   /* TODO: SGCI */
		break;
	case 0x9a:   /* DECID -- Identify Terminal */
		ttywrite(term, vtiden, strlen(vtiden), 0);
		break;
	case 0x9b:   /* TODO: CSI */
	case 0x9c:   /* TODO: ST */
		break;
	case 0x90:   /* DCS -- Device Control String */
	case 0x9d:   /* OSC -- Operating System Command */
	case 0x9e:   /* PM -- Privacy Message */
	case 0x9f:   /* APC -- Application Program Command */
		tstrsequence(term, ascii);
		return;
	}
	/* only CAN, SUB, \a and C1 chars interrupt a sequence */
	term->esc &= ~(ESC_STR_END|ESC_STR);
}

/*
 * returns 1 when the sequence is finished and it hasn't to read
 * more characters for this sequence, otherwise 0
 */
int
eschandle(Term *term, uchar ascii)
{
	switch (ascii) {
	case '[':
		term->esc |= ESC_CSI;
		return 0;
	case '#':
		term->esc |= ESC_TEST;
		return 0;
	case '%':
		term->esc |= ESC_UTF8;
		return 0;
	case 'P': /* DCS -- Device Control String */
	case '_': /* APC -- Application Program Command */
	case '^': /* PM -- Privacy Message */
	case ']': /* OSC -- Operating System Command */
	case 'k': /* old title set compatibility */
		tstrsequence(term, ascii);
		return 0;
	case 'n': /* LS2 -- Locking shift 2 */
	case 'o': /* LS3 -- Locking shift 3 */
		term->charset = 2 + (ascii - 'n');
		break;
	case '(': /* GZD4 -- set primary charset G0 */
	case ')': /* G1D4 -- set secondary charset G1 */
	case '*': /* G2D4 -- set tertiary charset G2 */
	case '+': /* G3D4 -- set quaternary charset G3 */
		term->icharset = ascii - '(';
		term->esc |= ESC_ALTCHARSET;
		return 0;
	case 'D': /* IND -- Linefeed */
		if (term->c.y == term->bot) {
			tscrollup(term, term->top, 1, 1);
		} else {
			tmoveto(term, term->c.x, term->c.y+1);
		}
		break;
	case 'E': /* NEL -- Next line */
		tnewline(term, 1); /* always go to first col */
		break;
	case 'H': /* HTS -- Horizontal tab stop */
		term->tabs[term->c.x] = 1;
		break;
	case 'M': /* RI -- Reverse index */
		if (term->c.y == term->top) {
			tscrolldown(term, term->top, 1, 1);
		} else {
			tmoveto(term, term->c.x, term->c.y-1);
		}
		break;
	case 'Z': /* DECID -- Identify Terminal */
		ttywrite(term, vtiden, strlen(vtiden), 0);
		break;
	case 'c': /* RIS -- Reset to initial state */
		treset(term);
		term->handler(term, ST_RESET, (Arg){0});
		break;
	case '=': /* DECPAM -- Application keypad */
		term->handler(term, ST_SET, (Arg){.ui = MODE_APPKEYPAD});
		break;
	case '>': /* DECPNM -- Normal keypad */
		term->handler(term, ST_SET, (Arg){.ui = MODE_APPKEYPAD});
		break;
	case '7': /* DECSC -- Save Cursor */
		tcursor(term, CURSOR_SAVE);
		break;
	case '8': /* DECRC -- Restore Cursor */
		tcursor(term, CURSOR_LOAD);
		break;
	case '\\': /* ST -- String Terminator */
		if (term->esc & ESC_STR_END)
			strhandle(term);
		break;
	default:
		fprintf(stderr, "erresc: unknown sequence ESC 0x%02X '%c'\n",
			(uchar) ascii, isprint(ascii)? ascii:'.');
		break;
	}
	return 1;
}

void
tputc(Term *term, Rune u)
{
	char c[UTF_SIZ];
	int control;
	int width, len;
	Glyph *gp;

	control = ISCONTROL(u);
	if (u < 127 || !IS_SET(MODE_UTF8)) {
		c[0] = u;
		width = len = 1;
	} else {
		len = utf8encode(u, c);
		if (!control && (width = wcwidth(u)) == -1) {
			memcpy(c, "\357\277\275", 4); /* UTF_INVALID */
			width = 1;
		}
	}

	if (IS_SET(MODE_PRINT))
		tprinter(term, c, len);

	/*
	 * STR sequence must be checked before anything else
	 * because it uses all following characters until it
	 * receives a ESC, a SUB, a ST or any other C1 control
	 * character.
	 */
	if (term->esc & ESC_STR) {
		if (u == '\a' || u == 030 || u == 032 || u == 033 ||
		   ISCONTROLC1(u)) {
			term->esc &= ~(ESC_START|ESC_STR);
			term->esc |= ESC_STR_END;
			goto check_control_code;
		}

		if (term->strescseq.len+len >= term->strescseq.siz) {
			/*
			 * Here is a bug in terminals. If the user never sends
			 * some code to stop the str or esc command, then st
			 * will stop responding. But this is better than
			 * silently failing with unknown characters. At least
			 * then users will report back.
			 *
			 * In the case users ever get fixed, here is the code:
			 */
			/*
			 * term->esc = 0;
			 * strhandle();
			 */
			if (term->strescseq.siz > (SIZE_MAX - UTF_SIZ) / 2)
				return;
			term->strescseq.siz *= 2;
			term->strescseq.buf = xrealloc(term->strescseq.buf, term->strescseq.siz);
		}

		memmove(&term->strescseq.buf[term->strescseq.len], c, len);
		term->strescseq.len += len;
		return;
	}

check_control_code:
	/*
	 * Actions of control codes must be performed as soon they arrive
	 * because they can be embedded inside a control sequence, and
	 * they must not cause conflicts with sequences.
	 */
	if (control) {
		tcontrolcode(term, u);
		/*
		 * control codes are not shown ever
		 */
		if (!term->esc)
			term->lastc = 0;
		return;
	} else if (term->esc & ESC_START) {
		if (term->esc & ESC_CSI) {
			term->csiescseq.buf[term->csiescseq.len++] = u;
			if (BETWEEN(u, 0x40, 0x7E)
					|| term->csiescseq.len >= \
					sizeof(term->csiescseq.buf)-1) {
				term->esc = 0;
				csiparse(term);
				csihandle(term);
			}
			return;
		} else if (term->esc & ESC_UTF8) {
			tdefutf8(term, u);
		} else if (term->esc & ESC_ALTCHARSET) {
			tdeftran(term, u);
		} else if (term->esc & ESC_TEST) {
			tdectest(term, u);
		} else {
			if (!eschandle(term, u))
				return;
			/* sequence already finished */
		}
		term->esc = 0;
		/*
		 * All characters which form part of a sequence are not
		 * printed
		 */
		return;
	}

	gp = &(*tgetline(term, term->c.y))[term->c.x];
	if (IS_SET(MODE_WRAP) && (term->c.state & CURSOR_WRAPNEXT)) {
		gp->mode |= ATTR_WRAP;
		tnewline(term, 1);
		gp = &(*tgetline(term, term->c.y))[term->c.x];
	}

	if (IS_SET(MODE_INSERT) && term->c.x+width < term->col)
		memmove(gp+width, gp, (term->col - term->c.x - width) * sizeof(Glyph));

	if (term->c.x+width > term->col) {
		tnewline(term, 1);
		gp = &(*tgetline(term, term->c.y))[term->c.x];
	}

	tsetchar(term, u, &term->c.attr, term->c.x, term->c.y);

	if (width == 2) {
		gp->mode |= ATTR_WIDE;
		if (term->c.x+1 < term->col) {
			gp[1].u = '\0';
			gp[1].mode = ATTR_WDUMMY;
		}
	}
	if (term->c.x+width < term->col) {
		tmoveto(term, term->c.x+width, term->c.y);
	} else {
		term->c.state |= CURSOR_WRAPNEXT;
	}
}

int
twrite(Term *term, const char *buf, int buflen, int show_ctrl)
{
	int charsize;
	Rune u;
	int n;

	for (n = 0; n < buflen; n += charsize) {
		if (IS_SET(MODE_UTF8)) {
			/* process a complete utf8 char */
			charsize = utf8decode(buf + n, &u, buflen - n);
			if (charsize == 0)
				break;
		} else {
			u = buf[n] & 0xFF;
			charsize = 1;
		}
		if (show_ctrl && ISCONTROL(u)) {
			if (u & 0x80) {
				u &= 0x7f;
				tputc(term, '^');
				tputc(term, '[');
			} else if (u != '\n' && u != '\r' && u != '\t') {
				u ^= 0x40;
				tputc(term, '^');
			}
		}
		tputc(term, u);
	}
	return n;
}

void
tresize(Term *term, int col, int row)
{

	int i;
	int minrow = MIN(row, term->row);
	int mincol = MIN(col, term->col);
	int maxrow = MAX(row, term->maxrow);
	int maxcol = MAX(col, term->maxcol);
	int *bp;
	/* offsets into views */
	TCursor c;

	if (col < 1 || row < 1) {
		fprintf(stderr,
		        "tresize: error resizing to %dx%d\n", col, row);
		return;
	}

	/*
	 * slide screen to keep cursor where we expect it -
	 * tscrollup would work here, but we can optimize to
	 * memmove because we're freeing the earlier lines
	 */
	i = term->c.y - row;
	/* ensure that both src and dst are not NULL */
	if (i > 0) {
		term->line = tgetline(term, i);
		if (term->alt)
			term->alt = tgetaltline(term, i);
	}
	/* resize to new height */
	term->dirty = xrealloc(term->dirty, row * sizeof(*term->dirty));
	term->tabs = xrealloc(term->tabs, maxcol * sizeof(*term->tabs));

	/* resize each row to new width, zero-pad if needed */
	for (i = 0; i < maxrow; i++) {
		if (term->alt)
			*tgetaltline(term, i)  = xrealloc(*tgetaltline(term, i),  maxcol * sizeof(Glyph));
		*tgetline(term, i) = xrealloc(*tgetline(term, i), maxcol * sizeof(Glyph));
	}

	/* allocate any new rows */
	if (col > term->col) {
		bp = term->tabs + term->col;

		memset(bp, 0, sizeof(*term->tabs) * (col - term->col));
		while (--bp > term->tabs && !*bp)
			/* nothing */ ;
		for (bp += term->tabspaces; bp < term->tabs + col; bp += term->tabspaces)
			*bp = 1;
	}
	/* update terminal size */
	term->col = col;
	term->row = row;
	/* reset scrolling region */
	tsetscroll(term, 0, row-1);
	/* make use of the LIMIT in tmoveto */
	tmoveto(term, term->c.x, term->c.y);
	/* Clearing both screens (it makes dirty all lines) */
	c = term->c;
	for (i = 0; i < 2; i++) {
		if (col > term->maxcol && minrow > 0) {
			tclearregion(term, mincol, 0, maxcol - 1, row - 1);
		}
		if (row > term->maxrow && mincol > 0) {
			tclearregion(term, 0, minrow, maxcol - 1, maxrow - 1);
		}
		tcursor(term, CURSOR_LOAD);
		if (term->alt == NULL) {
			tfulldirt(term);
			break;
		}
		tswapscreen(term);
	}
	term->c = c;
	term->maxcol = maxcol;
}

void
resettitle(Term *term)
{
	term->handler(term, ST_TITLE, (Arg){0});
}
