/*
 * The initial "port" of dwm to curses was done by
 *
 * © 2007-2016 Marc André Tanner <mat at brain-dump dot org>
 * © 2020 Jeremy Bobbin <jer at jer dot cx>
 *
 * It is highly inspired by the original X11 dwm and
 * reuses some code of it which is mostly
 *
 * © 2006-2007 Anselm R. Garbe <garbeam at gmail dot com>
 *
 * See LICENSE for details.
 */
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <wchar.h>
#include <limits.h>
#include <libgen.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sys/types.h>
#include <fcntl.h>
#include <curses.h>
#include <stdio.h>
#include <stdarg.h>
#include <signal.h>
#include <locale.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <errno.h>
#include <pwd.h>
#if defined __CYGWIN__ || defined __sun
# include <termios.h>
#endif
#include <libst.h>

typedef struct {
	int history;
	int w;
	int h;
	volatile sig_atomic_t need_resize;
} Screen;

typedef struct Client Client;
struct Client {
	Term *term;
	const char *cmd;
	char title[255];
	unsigned int mode;
	unsigned int scroll; /* how far back client is scrolled */
};

#define MAX_ARGS 8

typedef struct {
	void (*cmd)(const char *args[]);
	const char *args[3];
} Action;

typedef struct {
	const char *name;
	Action action;
} Cmd;

typedef struct {
	const char *name;
	int fd;
} File;

#define LENGTH(arr) (sizeof(arr) / sizeof((arr)[0]))
#define MAX(x, y)   ((x) > (y) ? (x) : (y))
#define MIN(x, y)   ((x) < (y) ? (x) : (y))

#ifdef NDEBUG
 #define debug(format, args...)
#else
 #define debug eprint
#endif

/* commands for use via fifo */
static void create(const char *args[]);
static void dump(const char *args[]);
static void quit(const char *args[]);
static void redraw(const char *args[]);
static void scrollback(const char *args[]);
static void send(const char *args[]);

/* functions and variables available to layouts via config.h */
static void cleanup(void);
extern Screen screen;
static Client *c = NULL;
static char *title;
static bool has_default_colors;
static short color_pairs_reserved, color_pairs_max, color_pair_current;
static short *color2palette;
short defaultfg = -1;
short defaultbg = -1;

#include "config.h"

/* global variables */
static const char *svt_name = "svt";
Screen screen = { .history = SCROLL_HISTORY };
static File cmdfifo = { .fd = -1 };
static File dmpfile = { .fd = -1 };
static const char *shell;
static volatile sig_atomic_t running = true;

static void
eprint(const char *errstr, ...) {
	va_list ap;
	va_start(ap, errstr);
	vfprintf(stderr, errstr, ap);
	va_end(ap);
}

static void
error(const char *errstr, ...) {
	va_list ap;
	va_start(ap, errstr);
	vfprintf(stderr, errstr, ap);
	va_end(ap);
	exit(EXIT_FAILURE);
}

static
unsigned int color_hash(short fg, short bg)
{
	if (fg == -1)
		fg = COLORS;
	if (bg == -1)
		bg = COLORS + 1;
	return fg * (COLORS + 2) + bg;
}

/* the following 3 "colour" functions have been stolen from tmux */
static int
colour_dist_sq(int R, int G, int B, int r, int g, int b)
{
	return ((R - r) * (R - r) + (G - g) * (G - g) + (B - b) * (B - b));
}

static int
colour_to_6cube(int v)
{
	if (v < 48)
		return (0);
	if (v < 114)
		return (1);
	return ((v - 35) / 40);
}

int
colour_find_rgb(unsigned char r, unsigned char g, unsigned char b)
{
	static const int	q2c[6] = { 0x00, 0x5f, 0x87, 0xaf, 0xd7, 0xff };
	int			qr, qg, qb, cr, cg, cb, d, idx;
	int			grey_avg, grey_idx, grey;

	/* Map RGB to 6x6x6 cube. */
	qr = colour_to_6cube(r); cr = q2c[qr];
	qg = colour_to_6cube(g); cg = q2c[qg];
	qb = colour_to_6cube(b); cb = q2c[qb];

	/* If we have hit the colour exactly, return early. */
	if (cr == r && cg == g && cb == b)
		return ((16 + (36 * qr) + (6 * qg) + qb));

	/* Work out the closest grey (average of RGB). */
	grey_avg = (r + g + b) / 3;
	if (grey_avg > 238)
		grey_idx = 23;
	else
		grey_idx = (grey_avg - 3) / 10;
	grey = 8 + (10 * grey_idx);

	/* Is grey or 6x6x6 colour closest? */
	d = colour_dist_sq(cr, cg, cb, r, g, b);
	if (colour_dist_sq(grey, grey, grey, r, g, b) < d)
		idx = 232 + grey_idx;
	else
		idx = 16 + (36 * qr) + (6 * qg) + qb;
	return (idx);
}

short
vt_color_get(Term *t, int fg, int bg)
{
	unsigned int r, g, b;
	if (fg < 0)
		fg = defaultfg;
	else if (fg > COLORS) {
		r = TRUERED(fg);
		g = TRUEGREEN(fg);
		b = TRUEBLUE(fg);
		fg = colour_find_rgb(r, g, b);
	}

	if (bg < 0)
		bg = defaultbg;
	else if (bg > COLORS) {
		r = TRUERED(bg);
		g = TRUEGREEN(bg);
		b = TRUEBLUE(bg);
		fg = colour_find_rgb(r, g, b);
	}

	if (!color2palette)
		return 0;
	unsigned int index = color_hash(fg, bg);
	if (color2palette[index] == 0) {
		short oldfg, oldbg;
		for (;;) {
			if (++color_pair_current >= color_pairs_max)
				color_pair_current = color_pairs_reserved + 1;
			pair_content(color_pair_current, &oldfg, &oldbg);
			unsigned int old_index = color_hash(oldfg, oldbg);
			if (color2palette[old_index] >= 0) {
				if (init_pair(color_pair_current, fg, bg) == OK) {
					color2palette[old_index] = 0;
					color2palette[index] = color_pair_current;
				}
				break;
			}
		}
	}

	short color_pair = color2palette[index];
	return color_pair >= 0 ? color_pair : -color_pair;
}

short
vt_color_reserve(short fg, short bg)
{
	if (!color2palette || fg >= COLORS || bg >= COLORS)
		return 0;
	if (fg == -1)
		fg = defaultfg;
	if (bg == -1)
		bg = defaultbg;
	if (fg == -1 && bg == -1)
		return 0;
	unsigned int index = color_hash(fg, bg);
	if (color2palette[index] >= 0) {
		if (init_pair(color_pairs_reserved + 1, fg, bg) == OK)
			color2palette[index] = -(++color_pairs_reserved);
	}
	short color_pair = color2palette[index];
	return color_pair >= 0 ? color_pair : -color_pair;
}

static void
init_colors(void)
{
	pair_content(0, &defaultfg, &defaultbg);
	start_color();
	has_default_colors = (use_default_colors() == OK);
	color_pairs_max = MIN(COLOR_PAIRS, SHRT_MAX);
	if (COLORS)
		color2palette = calloc(2 * (COLORS + 2) * (COLORS + 2), sizeof(short));

	/*
	 * XXX: On undefined color-pairs NetBSD curses pair_content() set fg
	 *      and bg to default colors while ncurses set them respectively to
	 *      0 and 0. Initialize all color-pairs in order to have consistent
	 *      behaviour despite the implementation used.
	 */
	for (short i = 1; i < color_pairs_max; i++)
		init_pair(i, 0, 0);
	vt_color_reserve(COLOR_WHITE, COLOR_BLACK);
}

int
stattr_to_curses(enum glyph_attribute in)
{
	int attr = 0;
	/* HACK? */
	if (in & ATTR_BOLD)
		attr |= A_BOLD;
	if (in & ATTR_FAINT)
		attr |= A_DIM;
	if (in & ATTR_ITALIC)
		attr |= A_STANDOUT;
	if (in & ATTR_UNDERLINE)
		attr |= A_UNDERLINE;
	if (in & ATTR_BLINK)
		attr |= A_BLINK;
	if (in & ATTR_REVERSE)
		attr |= A_REVERSE;
	if (in & ATTR_INVISIBLE)
		attr |= A_INVIS;
	return attr;
}

void
tdraw(Client *c, Term *t)
{
	Glyph *row, *prev_cell, *cell;
	int i, j;
	for (i = 0; i < t->row; i++) {
		if (!t->dirty[i])
			continue;

		row = *tgetline(t, i-c->scroll);
		move(i, 0);
		for (j = 0, cell = row, prev_cell = NULL; j < t->col; j++, prev_cell = cell, cell = row + j) {
			if (!prev_cell || cell->mode != prev_cell->mode
			    || cell->fg != prev_cell->fg
			    || cell->bg != prev_cell->bg) {
				attrset(stattr_to_curses(cell->mode));
				color_set(vt_color_get(t, cell->fg, cell->bg), NULL);
			}

			/* if (is_utf8 && cell->u >= 128) { */
			if (1 && cell->u >= 128) {
				cchar_t c = {
					.attr = stattr_to_curses(cell->mode),
					.chars = { cell->u }
				};
				add_wch(&c);
			} else {
				addch(cell->u > ' ' ? cell->u : ' ');
			}
		}

		t->dirty[i] = false;
	}

	move(t->c.y+c->scroll, t->c.x);
}

static void
sigchld_handler(int sig) {
	int errsv = errno;
	int status;
	pid_t pid;

	if ((pid = waitpid(-1, &status, WNOHANG)) != 0) {
		if (pid == -1) {
			eprint("waitpid: %s\n", strerror(errno));
		}

		debug("child died\n");
		running = false;
	}

	errno = errsv;
}

static void
sigwinch_handler(int sig) {
	screen.need_resize = true;
}

static void
sigterm_handler(int sig) {
	running = false;
}

static void
resize_screen(void) {
	struct winsize ws;
	if (ioctl(0, TIOCGWINSZ, &ws) == -1) {
		error("%s\n", strerror(errno));
		return;
	}
	screen.w = ws.ws_col;
	screen.h = ws.ws_row;
	debug("resize_screen(), w: %d h: %d\n", screen.w, screen.h);
	resizeterm(screen.h, screen.w);
	if (c == NULL)
		return;
	tresize(c->term, screen.w, screen.h);
	ttyresize(c->term, screen.w, screen.h);
	clear();
	tdraw(c, c->term);
	/* we double up here because NCurses assumes that
	 * the terminal emulator is keeping track of whether
	 * the cursor is hidden
	 */
	curs_set((c->mode & MODE_HIDE) ? 1 : 0);
	curs_set((c->mode & MODE_HIDE) ? 0 : 1);
	refresh();
}

static bool
checkshell(const char *shell) {
	if (shell == NULL || *shell == '\0' || *shell != '/')
		return false;
	if (!strcmp(strrchr(shell, '/')+1, svt_name))
		return false;
	if (access(shell, X_OK))
		return false;
	return true;
}

static const char *
getshell(void) {
	const char *shell = getenv("SHELL");
	struct passwd *pw;

	if (checkshell(shell))
		return shell;
	if ((pw = getpwuid(getuid())) && checkshell(pw->pw_shell))
		return pw->pw_shell;
	return "/bin/sh";
}

static void
setup(void) {
	shell = getshell();
	setlocale(LC_CTYPE, "");
	initscr();
	noecho();
	nonl();
	keypad(stdscr, TRUE);
	raw();
	init_colors();
	resize_screen();
	struct sigaction sa;
	memset(&sa, 0, sizeof sa);
	sa.sa_flags = 0;
	sigemptyset(&sa.sa_mask);
	sa.sa_handler = sigwinch_handler;
	sigaction(SIGWINCH, &sa, NULL);
	sa.sa_handler = sigchld_handler;
	sigaction(SIGCHLD, &sa, NULL);
	sa.sa_handler = sigterm_handler;
	sigaction(SIGTERM, &sa, NULL);
	sa.sa_handler = SIG_IGN;
	sigaction(SIGPIPE, &sa, NULL);
}

static void
cleanup(void) {
	if (c == NULL)
		return;
	tfree(c->term);
	if (!strcmp(c->cmd, shell))
		quit(NULL);
	else
		create(NULL);
	free(c);
	free(color2palette);
	endwin();
	if (cmdfifo.fd > 0)
		close(cmdfifo.fd);
	if (cmdfifo.name)
		unlink(cmdfifo.name);
}

int
event_handler(Term *term, Event e, Arg arg) {
	switch (e) {
	case ST_BELL:
		break;
	case ST_RESET:
		arg.s = "";
		break;
	case ST_TITLE:
	case ST_ICONTITLE:
		strncpy(c->title, arg.s ? arg.s : "", sizeof(c->title) - 1);
		break;
	case ST_EOF:
		if (term == c->term) {
			running = false;
		}
		break;
	case ST_SET:
		c->mode |= arg.ui;
		break;
	case ST_UNSET:
		c->mode &= ~(arg.ui);
		tfulldirt(term);
		break;
	case ST_POINTERMOTION:
	case ST_CURSORSTYLE:
	case ST_COPY:
	case ST_COLORNAME:
	case ST_CSI_ERROR:
	case ST_STR_ERROR:
		break;
	}
	return 0;
}

static void
create(const char *args[]) {
	char *cwd = NULL;
	c = calloc(1, sizeof(Client));
	if (!c)
		return;

	/* Term *term, int col, int row, int hist, int alt, int deffg, int defbg, int ts */
	c->term = tnew(screen.w, screen.h, screen.history, 1, defaultfg, defaultbg, 8);
	if (!c->term) {
		free(c);
		return;
	}

	if (args && args[0]) {
		c->cmd = args[0];
		char name[PATH_MAX];
		strncpy(name, args[0], sizeof(name));
		name[sizeof(name)-1] = '\0';
		strncpy(c->title, basename(name), sizeof(c->title));
	} else {
		c->cmd = shell;
	}

	c->term->handler = event_handler;
	/* ttynew(Term *term, char *cmd, char *iofname, char **args, int *in, int *out, int *err) */
	ttynew(c->term, (char *)shell, NULL, (char **)args, NULL, NULL, NULL);

	debug("client with pid %d forked\n", c->term->pid);
}

size_t tgetcontent(Term *t, char **buf, bool colored)
{
	Glyph *row, *curr = NULL, *prev;
	int i, j, b, e;
	size_t size;
	char *s;
	mbstate_t ps;
	memset(&ps, 0, sizeof(ps));

	if (t->seen >= t->maxrow) {
		b = (t->line - t->buf) + t->row+1;
		e = b + t->maxrow-1;
	} else {
		b = 0;
		e = t->seen-1;
	}

	size = (e - b) * ((t->col + 1) * ((colored ? 64 : 0) + MB_CUR_MAX));

	if (!(s = *buf = malloc(size)))
		return 0;

	for (i = b; i < e; i++) {
		row = t->buf[i % t->maxrow];

		size_t len = 0;
		char *last_non_space = s;
		for (j = 0; j < t->col; j++) {
			prev = curr;
			curr = &row[j];
			if (colored) {
				int esclen = 0;
				if (!prev || curr->mode != prev->mode) {
					attr_t attr = curr->mode;
					esclen = sprintf(s, "\033[0%s%s%s%s%s%sm",
						attr & A_BOLD ? ";1" : "",
						attr & A_DIM ? ";2" : "",
						attr & A_UNDERLINE ? ";4" : "",
						attr & A_BLINK ? ";5" : "",
						attr & A_REVERSE ? ";7" : "",
						attr & A_INVIS ? ";8" : "");
					if (esclen > 0)
						s += esclen;
				}
				if (!prev || curr->fg != prev->fg || curr->mode != prev->mode) {
					esclen = sprintf(s, "\033[38;5;%dm", curr->fg);
					if (esclen > 0)
						s += esclen;
				}
				if (!prev || curr->bg != prev->bg || curr->mode != prev->mode) {
					esclen = sprintf(s, "\033[48;5;%dm", curr->bg);
					if (esclen > 0)
						s += esclen;
				}
				prev = curr;
			}
			if (curr->u) {
				len = wcrtomb(s, curr->u, &ps);
				if (len > 0)
					s += len;
				if (curr->u != ' ')
					last_non_space = s;
			} else if (len) {
				len = 0;
			} else {
				*s++ = ' ';
			}
		}

		s = last_non_space;
		*s++ = '\n';
	}

	return s - *buf;
}

static void
tscroll(Client *c, int n)
{
	int limit = c->term->seen - c->term->row;
	if (n) {
		n += c->scroll;
		n = MAX(n, 0);
		/* user should not be able to loop back to the end of the ring buffer */
		if (n >= limit) {
			fprintf(stderr, "\a");
			flash();
			n = limit;
		}
	}
	c->scroll = n;
	tfulldirt(c->term);
	tdraw(c, c->term);
	curs_set(!(c->mode & MODE_HIDE));
}


static void
dump(const char *args[]) {
	size_t len;
	char *buf, *cur;
	bool colored;
	if (!c || dmpfile.name == NULL)
		return;

	if ((dmpfile.fd = open(dmpfile.name, O_WRONLY|O_CREAT, 0600)) == -1) {
		error("%s\n", strerror(errno));
		return;
	}

	if (args && args[0]) {
		colored = strstr(args[0], "uncolored") == NULL;
	} else {
		colored = false;
	}

	len = tgetcontent(c->term, &buf, colored);
	cur = buf;
	while (len > 0) {
		ssize_t res = write(dmpfile.fd, cur, len);
		if (res < 0) {
			if (errno == EAGAIN || errno == EINTR)
				continue;
			break;
		}
		cur += res;
		len -= res;
	}
	free(buf);
	close(dmpfile.fd);
}

static void
quit(const char *args[]) {
	running = false;
}

static void
redraw(const char *args[]) {
	tfulldirt(c->term);
	clear();
	curs_set(!(c->mode & MODE_HIDE));
	refresh();
	resize_screen();
}

static void
scrollback(const char *args[]) {
	if (!args[0])
		return;
	tscroll(c, atoi(args[0]));
}

static void
send(const char *args[]) {
	if (c && args && args[0])
		ttywrite(c->term, args[0], strlen(args[0]), 0);
}

static Cmd *
get_cmd_by_name(const char *name) {
	for (unsigned int i = 0; i < LENGTH(commands); i++) {
		if (!strcmp(name, commands[i].name))
			return &commands[i];
	}
	return NULL;
}

static void
handle_cmdfifo(void) {
	int r;
	char *p, *s, cmdbuf[512], c;
	Cmd *cmd;

	r = read(cmdfifo.fd, cmdbuf, sizeof cmdbuf - 1);
	if (r <= 0) {
		cmdfifo.fd = -1;
		return;
	}

	cmdbuf[r] = '\0';
	p = cmdbuf;
	while (*p) {
		/* find the command name */
		for (; *p == ' ' || *p == '\n'; p++);
		for (s = p; *p && *p != ' ' && *p != '\n'; p++);
		if ((c = *p))
			*p++ = '\0';
		if (*s && (cmd = get_cmd_by_name(s)) != NULL) {
			bool quote = false;
			int argc = 0;
			const char *args[MAX_ARGS], *arg;
			memset(args, 0, sizeof(args));
			/* if arguments were specified in config.h ignore the one given via
			 * the named pipe and thus skip everything until we find a new line
			 */
			if (cmd->action.args[0] || c == '\n') {
				debug("execute %s", s);
				cmd->action.cmd(cmd->action.args);
				while (*p && *p != '\n')
					p++;
				continue;
			}
			/* no arguments were given in config.h so we parse the command line */
			while (*p == ' ')
				p++;
			arg = p;
			for (; (c = *p); p++) {
				switch (*p) {
				case '\\':
					/* remove the escape character '\\' move every
					 * following character to the left by one position
					 */
					switch (p[1]) {
						case '\\':
						case '\'':
						case '\"': {
							char *t = p+1;
							do {
								t[-1] = *t;
							} while (*t++);
						}
					}
					break;
				case '\'':
				case '\"':
					quote = !quote;
					break;
				case ' ':
					if (!quote) {
				case '\n':
						/* remove trailing quote if there is one */
						if (*(p - 1) == '\'' || *(p - 1) == '\"')
							*(p - 1) = '\0';
						*p++ = '\0';
						/* remove leading quote if there is one */
						if (*arg == '\'' || *arg == '\"')
							arg++;
						if (argc < MAX_ARGS)
							args[argc++] = arg;

						while (*p == ' ')
							++p;
						arg = p--;
					}
					break;
				}

				if (c == '\n' || *p == '\n') {
					if (!*p)
						p++;
					debug("execute %s", s);
					for(int i = 0; i < argc; i++)
						debug(" %s", args[i]);
					debug("\n");
					cmd->action.cmd(args);
					break;
				}
			}
		}
	}
}

static int
open_or_create_fifo(const char *name, const char **name_created) {
	struct stat info;
	int fd;

	do {
		if ((fd = open(name, O_RDWR|O_NONBLOCK)) == -1) {
			if (errno == ENOENT && !mkfifo(name, S_IRUSR|S_IWUSR)) {
				*name_created = name;
				continue;
			}
			error("%s\n", strerror(errno));
		}
	} while (fd == -1);

	if (fstat(fd, &info) == -1)
		error("%s\n", strerror(errno));
	if (!S_ISFIFO(info.st_mode))
		error("%s is not a named pipe\n", name);
	return fd;
}

static void
usage(void) {
	cleanup();
	eprint("usage: svt [-v] [-M] [-m mod] [-d delay] [-h lines] [-t title] "
	       "[-s status-fifo] [-c cmd-fifo] [-o dmpfile-file] [cmd...]\n");
	exit(EXIT_FAILURE);
}

static bool
parse_args(int argc, char *argv[]) {
	bool init = false;
	const char *name = argv[0];

	if (name && (name = strrchr(name, '/')))
		svt_name = name + 1;
	for (int arg = 1; arg < argc; arg++) {
		if (argv[arg][0] != '-') {
			if (!init) {
				setup();
				init = true;
			}
			create((const char **)&argv[arg]);
			break;
		}
		if (argv[arg][1] != 'v' && argv[arg][1] != 'M' && (arg + 1) >= argc)
			usage();
		switch (argv[arg][1]) {
			case 'v':
				puts("svt-"VERSION" © 2020 Jeremy Bobbin");
				exit(EXIT_SUCCESS);
			case 'h':
				screen.history = atoi(argv[++arg]);
				break;
			case 't':
				title = argv[++arg];
				break;
			case 'c': {
				const char *fname;
				cmdfifo.fd = open_or_create_fifo(argv[++arg], &cmdfifo.name);
				if (!(fname = realpath(argv[arg], NULL)))
					error("%s\n", strerror(errno));
				setenv("SVT_CMD_FIFO", fname, 1);
				break;
			}
			case 'o': {
				const char *fname = dmpfile.name = argv[++arg];
				setenv("SVT_DUMP_FILE", fname, 1);
				break;
			}
			default:
				usage();
		}
	}
	return init;
}

int
main(int argc, char *argv[]) {
	char buf[8];
	sigset_t emptyset, blockset;

	setenv("SVT", VERSION, 1);
	if (!parse_args(argc, argv)) {
		setup();
		create(NULL);
	}

	sigemptyset(&emptyset);
	sigemptyset(&blockset);
	sigaddset(&blockset, SIGWINCH);
	sigaddset(&blockset, SIGCHLD);
	sigprocmask(SIG_BLOCK, &blockset, NULL);

	while (running) {
		int r, nfds = 0;
		fd_set rd;

		if (screen.need_resize) {
			resize_screen();
			screen.need_resize = false;
		}

		FD_ZERO(&rd);
		FD_SET(STDIN_FILENO, &rd);

		if (cmdfifo.fd != -1) {
			FD_SET(cmdfifo.fd, &rd);
			nfds = cmdfifo.fd;
		}

		FD_SET(c->term->cmdfd, &rd);
		nfds = MAX(nfds, c->term->cmdfd);

		doupdate();
		r = pselect(nfds + 1, &rd, NULL, NULL, NULL, &emptyset);

		if (r < 0) {
			if (errno == EINTR)
				continue;
			perror("select()");
			exit(EXIT_FAILURE);
		}

		if (FD_ISSET(STDIN_FILENO, &rd)) {
			if ((r = read(STDIN_FILENO, buf, sizeof(buf))) < 0) {
				continue;
			}
			tscroll(c, 0);
			ttywrite(c->term, buf, r, 1);
		}

		if (cmdfifo.fd != -1 && FD_ISSET(cmdfifo.fd, &rd))
			handle_cmdfifo();

		if (FD_ISSET(c->term->cmdfd, &rd)) {
			if ((r = ttyread(c->term)) < 0 && errno == EIO) {
				break;
			}
		}
		tdraw(c, c->term);
		refresh();
	}

	cleanup();
	return 0;
}
