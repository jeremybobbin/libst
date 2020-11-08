/*
 * The initial "port" of dwm to curses was done by
 *
 * © 2007-2016 Marc André Tanner <mat at brain-dump dot org>
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

#ifdef PDCURSES
int ESCDELAY;
#endif

#ifndef NCURSES_REENTRANT
# define set_escdelay(d) (ESCDELAY = (d))
#endif

typedef struct {
	float mfact;
	unsigned int nmaster;
	int history;
	int w;
	int h;
	volatile sig_atomic_t need_resize;
} Screen;

typedef struct {
	const char *symbol;
	void (*arrange)(void);
} Layout;

typedef struct Client Client;
struct Client {
	WINDOW *window;
	Term *term;
	Term *editor, *app;
	int editor_fds[2];
	int cursor_vis;
	volatile sig_atomic_t editor_died;
	const char *cmd;
	char title[255];
	int order;
	pid_t pid;
	unsigned short int id;
	unsigned short int x;
	unsigned short int y;
	unsigned short int w;
	unsigned short int h;
	bool has_title_line;
	bool minimized;
	bool urgent;
	volatile sig_atomic_t died;
	Client *next;
	Client *prev;
	Client *snext;
	unsigned int mode;
	unsigned int tags;
	unsigned int scroll; /* how far back client is scrolled */
};

typedef struct {
	short fg;
	short bg;
	short fg256;
	short bg256;
	short pair;
} Color;

typedef struct {
	const char *title;
	attr_t attrs;
	Color *color;
} ColorRule;

#define ALT(k)      ((k) + (161 - 'a'))
#if defined CTRL && defined _AIX
  #undef CTRL
#endif
#ifndef CTRL
  #define CTRL(k)   ((k) & 0x1F)
#endif
#define CTRL_ALT(k) ((k) + (129 - 'a'))

#define MAX_ARGS 8

typedef struct {
	void (*cmd)(const char *args[]);
	const char *args[3];
} Action;

#define MAX_KEYS 3

typedef unsigned int KeyCombo[MAX_KEYS];

typedef struct {
	KeyCombo keys;
	Action action;
} KeyBinding;

typedef struct {
	mmask_t mask;
	Action action;
} Button;

typedef struct {
	const char *name;
	Action action;
} Cmd;

enum { BAR_TOP, BAR_BOTTOM, BAR_OFF };

typedef struct {
	int fd;
	int pos, lastpos;
	bool autohide;
	unsigned short int h;
	unsigned short int y;
	char text[512];
	const char *file;
} StatusBar;

typedef struct {
	int fd;
	const char *file;
	unsigned short int id;
} CmdFifo;

typedef struct {
	char *data;
	size_t len;
	size_t size;
} Register;

typedef struct {
	char *name;
	const char *argv[4];
	bool filter;
	bool color;
} Editor;

#define LENGTH(arr) (sizeof(arr) / sizeof((arr)[0]))
#define MAX(x, y)   ((x) > (y) ? (x) : (y))
#define MIN(x, y)   ((x) < (y) ? (x) : (y))
#define TAGMASK     ((1 << LENGTH(tags)) - 1)

#ifdef NDEBUG
 #define debug(format, args...)
#else
 #define debug eprint
#endif

/* commands for use by keybindings */
static void create(const char *args[]);
static void copymode(const char *args[]);
static void focusn(const char *args[]);
static void focusid(const char *args[]);
static void focusnext(const char *args[]);
static void focusnextnm(const char *args[]);
static void focusprev(const char *args[]);
static void focusprevnm(const char *args[]);
static void focuslast(const char *args[]);
static void focusup(const char *args[]);
static void focusdown(const char *args[]);
static void focusleft(const char *args[]);
static void focusright(const char *args[]);
static void killclient(const char *args[]);
static void paste(const char *args[]);
static void quit(const char *args[]);
static void redraw(const char *args[]);
static void scrollback(const char *args[]);
static void send(const char *args[]);
static void setlayout(const char *args[]);
static void incnmaster(const char *args[]);
static void setmfact(const char *args[]);
static void startup(const char *args[]);
static void tag(const char *args[]);
static void tagid(const char *args[]);
static void togglebar(const char *args[]);
static void togglebarpos(const char *args[]);
static void toggleminimize(const char *args[]);
static void togglemouse(const char *args[]);
static void togglerunall(const char *args[]);
static void toggletag(const char *args[]);
static void toggleview(const char *args[]);
static void viewprevtag(const char *args[]);
static void view(const char *args[]);
static void zoom(const char *args[]);

/* commands for use by mouse bindings */
static void mouse_focus(const char *args[]);
static void mouse_fullscreen(const char *args[]);
static void mouse_minimize(const char *args[]);
static void mouse_zoom(const char *args[]);

/* functions and variables available to layouts via config.h */
static Client* nextvisible(Client *c);
static void focus(Client *c);
static void resize(Client *c, int x, int y, int w, int h);
extern Screen screen;
static unsigned int waw, wah, wax, way;
static Client *clients = NULL;
static char *title;
static bool is_utf8, has_default_colors;
static short color_pairs_reserved, color_pairs_max, color_pair_current;
static short *color2palette;
short defaultfg = COLOR_WHITE;
short defaultbg = COLOR_BLACK;

#include "config.h"

/* global variables */
static const char *dvtm_name = "dvtm";
Screen screen = { .mfact = MFACT, .nmaster = NMASTER, .history = SCROLL_HISTORY };
static Client *stack = NULL;
static Client *sel = NULL;
static Client *lastsel = NULL;
static Client *msel = NULL;
static unsigned int seltags;
static unsigned int tagset[2] = { 1, 1 };
static bool mouse_events_enabled = ENABLE_MOUSE;
static Layout *layout = layouts;
static StatusBar bar = { .fd = -1, .lastpos = BAR_POS, .pos = BAR_POS, .autohide = BAR_AUTOHIDE, .h = 1 };
static CmdFifo cmdfifo = { .fd = -1 };
static const char *shell;
static Register copyreg;
static volatile sig_atomic_t running = true;
static bool runinall = false;

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

static bool
isarrange(void (*func)()) {
	return func == layout->arrange;
}

static bool
isvisible(Client *c) {
	return c->tags & tagset[seltags];
}

static bool
is_content_visible(Client *c) {
	if (!c)
		return false;
	if (isarrange(fullscreen))
		return sel == c;
	return isvisible(c) && !c->minimized;
}

static Client*
nextvisible(Client *c) {
	for (; c && !isvisible(c); c = c->next);
	return c;
}

static Client*
bypid(pid_t pid) {
	for (Client *c = clients; c; c = c->next)
		if (c->term->pid == pid)
			return c;
	return NULL;
}

static void
updatebarpos(void) {
	bar.y = 0;
	wax = 0;
	way = 0;
	wah = screen.h;
	waw = screen.w;
	if (bar.pos == BAR_TOP) {
		wah -= bar.h;
		way += bar.h;
	} else if (bar.pos == BAR_BOTTOM) {
		wah -= bar.h;
		bar.y = wah;
	}
}

static void
hidebar(void) {
	if (bar.pos != BAR_OFF) {
		bar.lastpos = bar.pos;
		bar.pos = BAR_OFF;
	}
}

static void
showbar(void) {
	if (bar.pos == BAR_OFF)
		bar.pos = bar.lastpos;
}

static void
drawbar(void) {
	int sx, sy, x, y, width;
	unsigned int occupied = 0, urgent = 0;
	if (bar.pos == BAR_OFF)
		return;

	for (Client *c = clients; c; c = c->next) {
		occupied |= c->tags;
		if (c->urgent)
			urgent |= c->tags;
	}

	getyx(stdscr, sy, sx);
	attrset(BAR_ATTR);
	move(bar.y, 0);

	for (unsigned int i = 0; i < LENGTH(tags); i++){
		if (tagset[seltags] & (1 << i))
			attrset(TAG_SEL);
		else if (urgent & (1 << i))
			attrset(TAG_URGENT);
		else if (occupied & (1 << i))
			attrset(TAG_OCCUPIED);
		else
			attrset(TAG_NORMAL);
		printw(TAG_SYMBOL, tags[i]);
	}

	attrset(runinall ? TAG_SEL : TAG_NORMAL);
	addstr(layout->symbol);
	attrset(TAG_NORMAL);

	getyx(stdscr, y, x);
	(void)y;
	int maxwidth = screen.w - x - 2;

	addch(BAR_BEGIN);
	attrset(BAR_ATTR);

	wchar_t wbuf[sizeof bar.text];
	size_t numchars = mbstowcs(wbuf, bar.text, sizeof bar.text);

	if (numchars != (size_t)-1 && (width = wcswidth(wbuf, maxwidth)) != -1) {
		int pos;
		for (pos = 0; pos + width < maxwidth; pos++)
			addch(' ');

		for (size_t i = 0; i < numchars; i++) {
			pos += wcwidth(wbuf[i]);
			if (pos > maxwidth)
				break;
			addnwstr(wbuf+i, 1);
		}

		clrtoeol();
	}

	attrset(TAG_NORMAL);
	mvaddch(bar.y, screen.w - 1, BAR_END);
	attrset(NORMAL_ATTR);
	move(sy, sx);
	wnoutrefresh(stdscr);
}

static int
show_border(void) {
	return (bar.pos != BAR_OFF) || (clients && clients->next);
}

static void
draw_border(Client *c) {
	char t = '\0';
	int x, y, maxlen, attrs = NORMAL_ATTR;

	if (!show_border())
		return;
	if (sel != c && c->urgent)
		attrs = URGENT_ATTR;
	if (sel == c || (runinall && !c->minimized))
		attrs = SELECTED_ATTR;

	wattrset(c->window, attrs);
	getyx(c->window, y, x);
	mvwhline(c->window, 0, 0, ACS_HLINE, c->w);
	maxlen = c->w - 10;
	if (maxlen < 0)
		maxlen = 0;
	if ((size_t)maxlen < sizeof(c->title)) {
		t = c->title[maxlen];
		c->title[maxlen] = '\0';
	}

	mvwprintw(c->window, 0, 2, "[%s%s#%d]",
	          *c->title ? c->title : "",
	          *c->title ? " | " : "",
	          c->order);
	if (t)
		c->title[maxlen] = t;
	wmove(c->window, y, x);
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

short
vt_color_get(Term *t, short fg, short bg)
{
	if (fg >= COLORS || fg < 0)
		fg = defaultfg;
	if (bg >= COLORS || bg < 0)
		bg = defaultbg;

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
	has_default_colors = (use_default_colors() == OK);
	color_pairs_max = MIN(COLOR_PAIRS, SHRT_MAX);
	if (COLORS)
		color2palette = calloc((COLORS + 2) * (COLORS + 2), sizeof(short));
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

void tkeypress(Term *t, int keycode)
{
	/* vt_noscroll(t); */

	if (keycode >= 0 && keycode <= KEY_MAX && keytable[keycode]) {
		switch (keycode) {
		case KEY_UP:
		case KEY_DOWN:
		case KEY_RIGHT:
		case KEY_LEFT: {
			char keyseq[3] = { '\e', (/* t->curskeymode ? */ 'O' /* : '['*/) , keytable[keycode][0] };
			ttywrite(t, keyseq, sizeof keyseq, 0);
			break;
		}
		default:
			ttywrite(t, keytable[keycode], strlen(keytable[keycode]), 0);
		}
	} else if (keycode <= UCHAR_MAX) {
		char c = keycode;
		ttywrite(t, &c, 1, 0);
	} else {
#ifndef NDEBUG
		fprintf(stderr, "unhandled key %#o\n", keycode);
#endif
	}
}

/* scroll for end user */
void
tscroll(Client *c, int n)
{
	if (n) {
		n += c->scroll;
		n = MAX(n, 0);
		/* user should not be able to loop back to the end of the ring buffer*/
		c->scroll = MIN(n, c->term->maxrow - c->term->row);
	} else {
		c->scroll = 0;
	}
	tfulldirt(c->term);
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
tdraw(Client *c, Term *t, WINDOW *win, int srow, int scol)
{
	Glyph *row, *prev_cell, *cell;
	int i, j;
	for (i = 0; i < t->row; i++) {
		if (!t->dirty[i])
			continue;

		row = *tgetline(t, i-c->scroll);
		wmove(win, srow + i, scol);
		for (j = 0, cell = row, prev_cell = NULL; j < t->col; j++, prev_cell = cell, cell = row + j) {
			if (!prev_cell || cell->mode != prev_cell->mode
			    || cell->fg != prev_cell->fg
			    || cell->bg != prev_cell->bg) {
				if (cell->fg == -1)
					cell->fg = COLOR_WHITE;
				if (cell->bg == -1)
					cell->bg = COLOR_BLACK;
				wattrset(win, stattr_to_curses(cell->mode));
				wcolor_set(win, vt_color_get(t, cell->fg, cell->bg), NULL);
			}

			/* if (is_utf8 && cell->u >= 128) { */
			if (1 && cell->u >= 128) {
				cchar_t c = {
					.attr = stattr_to_curses(cell->mode),
					.chars = { cell->u }
				};
				wadd_wch(win, &c);
			} else {
				waddch(win, cell->u > ' ' ? cell->u : ' ');
			}
		}

		int x, y;
		getyx(win, y, x);
		(void)y;
		if (x && x < t->col - 1)
			whline(win, ' ', t->col - x);

		t->dirty[i] = false;
	}

	wmove(win, srow + t->c.y, scol + t->c.x);
}

static void
draw_content(Client *c) {
	tdraw(c, c->term, c->window, c->has_title_line, 0);
}

static void
draw(Client *c) {
	if (is_content_visible(c)) {
		redrawwin(c->window);
		draw_content(c);
	}
	if (!isarrange(fullscreen) || sel == c)
		draw_border(c);
	wnoutrefresh(c->window);
}

static void
draw_all(void) {
	if (!nextvisible(clients)) {
		sel = NULL;
		curs_set(0);
		erase();
		drawbar();
		doupdate();
		return;
	}

	if (!isarrange(fullscreen)) {
		for (Client *c = nextvisible(clients); c; c = nextvisible(c->next)) {
			if (c != sel)
				draw(c);
		}
	}
	/* as a last step the selected window is redrawn,
	 * this has the effect that the cursor position is
	 * accurate
	 */
	if (sel)
		draw(sel);
}

static void
arrange(void) {
	unsigned int m = 0, n = 0;
	for (Client *c = nextvisible(clients); c; c = nextvisible(c->next)) {
		c->order = ++n;
		if (c->minimized)
			m++;
	}
	erase();
	attrset(NORMAL_ATTR);
	if (bar.fd == -1 && bar.autohide) {
		if ((!clients || !clients->next) && n == 1)
			hidebar();
		else
			showbar();
		updatebarpos();
	}
	if (m && !isarrange(fullscreen))
		wah--;
	layout->arrange();
	if (m && !isarrange(fullscreen)) {
		unsigned int i = 0, nw = waw / m, nx = wax;
		for (Client *c = nextvisible(clients); c; c = nextvisible(c->next)) {
			if (c->minimized) {
				resize(c, nx, way+wah, ++i == m ? waw - nx : nw, 1);
				nx += nw;
			}
		}
		wah++;
	}
	focus(NULL);
	wnoutrefresh(stdscr);
	drawbar();
	draw_all();
}

static void
attach(Client *c) {
	if (clients)
		clients->prev = c;
	c->next = clients;
	c->prev = NULL;
	clients = c;
	for (int o = 1; c; c = nextvisible(c->next), o++)
		c->order = o;
}

static void
attachafter(Client *c, Client *a) { /* attach c after a */
	if (c == a)
		return;
	if (!a)
		for (a = clients; a && a->next; a = a->next);

	if (a) {
		if (a->next)
			a->next->prev = c;
		c->next = a->next;
		c->prev = a;
		a->next = c;
		for (int o = a->order; c; c = nextvisible(c->next))
			c->order = ++o;
	}
}

static void
attachstack(Client *c) {
	c->snext = stack;
	stack = c;
}

static void
detach(Client *c) {
	Client *d;
	if (c->prev)
		c->prev->next = c->next;
	if (c->next) {
		c->next->prev = c->prev;
		for (d = nextvisible(c->next); d; d = nextvisible(d->next))
			--d->order;
	}
	if (c == clients)
		clients = c->next;
	c->next = c->prev = NULL;
}

static void
settitle(Client *c) {
	char *term, *t = title;
	if (!t && sel == c && *c->title)
		t = c->title;
	if (t && (term = getenv("TERM")) && !strstr(term, "linux")) {
		printf("\033]0;%s\007", t);
		fflush(stdout);
	}
}

static void
detachstack(Client *c) {
	Client **tc;
	for (tc = &stack; *tc && *tc != c; tc = &(*tc)->snext);
	*tc = c->snext;
}

static void
focus(Client *c) {
	if (!c)
		for (c = stack; c && !isvisible(c); c = c->snext);
	if (sel == c)
		return;
	lastsel = sel;
	sel = c;
	if (lastsel) {
		lastsel->urgent = false;
		if (!isarrange(fullscreen)) {
			draw_border(lastsel);
			wnoutrefresh(lastsel->window);
		}
	}

	if (c) {
		detachstack(c);
		attachstack(c);
		settitle(c);
		c->urgent = false;
		if (isarrange(fullscreen)) {
			draw(c);
		} else {
			draw_border(c);
			wnoutrefresh(c->window);
		}
	}
	curs_set(c && !c->minimized && !(c->mode & MODE_HIDE));
}

static void
applycolorrules(Client *c) {
	const ColorRule *r = colorrules;
	short fg = r->color->fg, bg = r->color->bg;
	attr_t attrs = r->attrs;

	for (unsigned int i = 1; i < LENGTH(colorrules); i++) {
		r = &colorrules[i];
		if (strstr(c->title, r->title)) {
			attrs = r->attrs;
			fg = r->color->fg;
			bg = r->color->bg;
			break;
		}
	}

	/* vt_default_colors_set(c->term, attrs, fg, bg); */
}

static void
term_title_handler(pid_t pid, const char *title) {
	Client *c = bypid(pid);
	if (title)
		strncpy(c->title, title, sizeof(c->title) - 1);
	c->title[title ? sizeof(c->title) - 1 : 0] = '\0';
	settitle(c);
	if (!isarrange(fullscreen) || sel == c)
		draw_border(c);
	applycolorrules(c);
}

static void
term_urgent_handler(pid_t pid) {
	Client *c = bypid(pid);
	c->urgent = true;
	printf("\a");
	fflush(stdout);
	drawbar();
	if (!isarrange(fullscreen) && sel != c && isvisible(c))
		draw_border(c);
}

static void
move_client(Client *c, int x, int y) {
	if (c->x == x && c->y == y)
		return;
	debug("moving, x: %d y: %d\n", x, y);
	if (mvwin(c->window, y, x) == ERR) {
		eprint("error moving, x: %d y: %d\n", x, y);
	} else {
		c->x = x;
		c->y = y;
	}
}

static void
resize_client(Client *c, int w, int h) {
	bool has_title_line = show_border();
	bool resize_window = c->w != w || c->h != h;
	if (resize_window) {
		debug("resizing, w: %d h: %d\n", w, h);
		if (wresize(c->window, h, w) == ERR) {
			eprint("error resizing, w: %d h: %d\n", w, h);
		} else {
			c->w = w;
			c->h = h;
		}
	}
	if (resize_window || c->has_title_line != has_title_line) {
		c->has_title_line = has_title_line;
		tresize(c->app, w, h - has_default_colors);
		ttyresize(c->app, w, h - has_title_line);
		if (c->editor) {
			tresize(c->editor, w, h - has_default_colors);
			ttyresize(c->editor, w, h - has_title_line);
		}
	}
}

static void
resize(Client *c, int x, int y, int w, int h) {
	resize_client(c, w, h);
	move_client(c, x, y);
}

static Client*
get_client_by_coord(unsigned int x, unsigned int y) {
	if (y < way || y >= way+wah)
		return NULL;
	if (isarrange(fullscreen))
		return sel;
	for (Client *c = nextvisible(clients); c; c = nextvisible(c->next)) {
		if (x >= c->x && x < c->x + c->w && y >= c->y && y < c->y + c->h) {
			debug("mouse event, x: %d y: %d client: %d\n", x, y, c->order);
			return c;
		}
	}
	return NULL;
}

static void
sigchld_handler(int sig) {
	int errsv = errno;
	int status;
	pid_t pid;

	while ((pid = waitpid(-1, &status, WNOHANG)) != 0) {
		if (pid == -1) {
			if (errno == ECHILD) {
				/* no more child processes */
				break;
			}
			eprint("waitpid: %s\n", strerror(errno));
			break;
		}

		debug("child with pid %d died\n", pid);

		for (Client *c = clients; c; c = c->next) {
			if (c->term->pid == pid) {
				c->died = true;
				break;
			}
			if (c->editor && c->editor->pid == pid) {
				c->editor_died = true;
				break;
			}
		}
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
		getmaxyx(stdscr, screen.h, screen.w);
	} else {
		screen.w = ws.ws_col;
		screen.h = ws.ws_row;
	}

	debug("resize_screen(), w: %d h: %d\n", screen.w, screen.h);

	resizeterm(screen.h, screen.w);
	wresize(stdscr, screen.h, screen.w);
	updatebarpos();
	clear();
	arrange();
}

static KeyBinding*
keybinding(KeyCombo keys, unsigned int keycount) {
	for (unsigned int b = 0; b < LENGTH(bindings); b++) {
		for (unsigned int k = 0; k < keycount; k++) {
			if (keys[k] != bindings[b].keys[k])
				break;
			if (k == keycount - 1)
				return &bindings[b];
		}
	}
	return NULL;
}

static unsigned int
bitoftag(const char *tag) {
	unsigned int i;
	if (!tag)
		return ~0;
	for (i = 0; (i < LENGTH(tags)) && strcmp(tags[i], tag); i++);
	return (i < LENGTH(tags)) ? (1 << i) : 0;
}

static void
tagschanged() {
	bool allminimized = true;
	for (Client *c = nextvisible(clients); c; c = nextvisible(c->next)) {
		if (!c->minimized) {
			allminimized = false;
			break;
		}
	}
	if (allminimized && nextvisible(clients)) {
		focus(NULL);
		toggleminimize(NULL);
	}
	arrange();
}

static void
tag(const char *args[]) {
	if (!sel)
		return;
	sel->tags = bitoftag(args[0]) & TAGMASK;
	tagschanged();
}

static void
tagid(const char *args[]) {
	if (!args[0] || !args[1])
		return;

	const int win_id = atoi(args[0]);
	for (Client *c = clients; c; c = c->next) {
		if (c->id == win_id) {
			unsigned int ntags = c->tags;
			for (unsigned int i = 1; i < MAX_ARGS && args[i]; i++) {
				if (args[i][0] == '+')
					ntags |= bitoftag(args[i]+1);
				else if (args[i][0] == '-')
					ntags &= ~bitoftag(args[i]+1);
				else
					ntags = bitoftag(args[i]);
			}
			ntags &= TAGMASK;
			if (ntags) {
				c->tags = ntags;
				tagschanged();
			}
			return;
		}
	}
}

static void
toggletag(const char *args[]) {
	if (!sel)
		return;
	unsigned int newtags = sel->tags ^ (bitoftag(args[0]) & TAGMASK);
	if (newtags) {
		sel->tags = newtags;
		tagschanged();
	}
}

static void
toggleview(const char *args[]) {
	unsigned int newtagset = tagset[seltags] ^ (bitoftag(args[0]) & TAGMASK);
	if (newtagset) {
		tagset[seltags] = newtagset;
		tagschanged();
	}
}

static void
view(const char *args[]) {
	unsigned int newtagset = bitoftag(args[0]) & TAGMASK;
	if (tagset[seltags] != newtagset && newtagset) {
		seltags ^= 1; /* toggle sel tagset */
		tagset[seltags] = newtagset;
		tagschanged();
	}
}

static void
viewprevtag(const char *args[]) {
	seltags ^= 1;
	tagschanged();
}

static void
keypress(int code) {
	int key = -1;
	unsigned int len = 1;
	char buf[8] = { '\e' };

	if (code == '\e') {
		/* pass characters following escape to the underlying app */
		nodelay(stdscr, TRUE);
		for (int t; len < sizeof(buf) && (t = getch()) != ERR; len++) {
			if (t > 255) {
				key = t;
				break;
			}
			buf[len] = t;
		}
		nodelay(stdscr, FALSE);
	}

	for (Client *c = runinall ? nextvisible(clients) : sel; c; c = nextvisible(c->next)) {
		if (is_content_visible(c)) {
			c->urgent = false;
			if (code == '\e')
				ttywrite(c->term, buf, len, 0);
			else
				tkeypress(c->term, code);
			if (key != -1)
				tkeypress(c->term, key);
		}
		if (!runinall)
			break;
	}
}

static void
mouse_setup(void) {
#ifdef CONFIG_MOUSE
	mmask_t mask = 0;

	if (mouse_events_enabled) {
		mask = BUTTON1_CLICKED | BUTTON2_CLICKED;
		for (unsigned int i = 0; i < LENGTH(buttons); i++)
			mask |= buttons[i].mask;
	}
	mousemask(mask, NULL);
#endif /* CONFIG_MOUSE */
}

static bool
checkshell(const char *shell) {
	if (shell == NULL || *shell == '\0' || *shell != '/')
		return false;
	if (!strcmp(strrchr(shell, '/')+1, dvtm_name))
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
	start_color();
	noecho();
	nonl();
	keypad(stdscr, TRUE);
	mouse_setup();
	raw();
	init_colors();
	/* vt_keytable_set(keytable, LENGTH(keytable)); */
	for (unsigned int i = 0; i < LENGTH(colors); i++) {
		if (COLORS == 256) {
			if (colors[i].fg256)
				colors[i].fg = colors[i].fg256;
			if (colors[i].bg256)
				colors[i].bg = colors[i].bg256;
		}
		colors[i].pair = vt_color_reserve(colors[i].fg, colors[i].bg) ;
	}
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
destroy(Client *c) {
	if (sel == c)
		focusnextnm(NULL);
	detach(c);
	detachstack(c);
	if (sel == c) {
		Client *next = nextvisible(clients);
		if (next) {
			focus(next);
			toggleminimize(NULL);
		} else {
			sel = NULL;
		}
	}
	if (lastsel == c)
		lastsel = NULL;
	werase(c->window);
	wnoutrefresh(c->window);
	tfree(c->term);
	delwin(c->window);
	if (!clients && LENGTH(actions)) {
		if (!strcmp(c->cmd, shell))
			quit(NULL);
		else
			create(NULL);
	}
	free(c);
	arrange();
}

static void
cleanup(void) {
	while (clients)
		destroy(clients);
	/* vt_shutdown(); */
	endwin();
	free(copyreg.data);
	if (bar.fd > 0)
		close(bar.fd);
	if (bar.file)
		unlink(bar.file);
	if (cmdfifo.fd > 0)
		close(cmdfifo.fd);
	if (cmdfifo.file)
		unlink(cmdfifo.file);
}

static char *getcwd_by_pid(Client *c) {
	if (!c)
		return NULL;
	char buf[32];
	snprintf(buf, sizeof buf, "/proc/%d/cwd", c->term->pid);
	return realpath(buf, NULL);
}

Client *
get_client_by_term(Term *term)
{
	for (Client *c = clients; c; c = c->next) {
		if (c->term == term) {
			return c;
		}
		if (c->editor == term) {
			return c;
		}
	}
	return NULL;

}

int
event_handler(Term *term, Event e, Arg arg) {
	Arg *kv = arg.v;
	Client *c;
	 if ((c = get_client_by_term(term)) == NULL)
	 	return 1;
	switch (e) {
	case ST_BELL:
		focus(c);
		break;
	case ST_RESET:
		arg.s = "";
	case ST_TITLE:
	case ST_ICONTITLE:
		strncpy(c->title, arg.s ? arg.s : "", sizeof(c->title) - 1);
		break;
	case ST_EOF:
		if (term == c->editor) {
			c->editor_died = true;
			break;
		}
		if (term == c->term || term == c->app) {
			c->died = true;
			break;
		}
	case ST_SET:
		c->mode |= arg.ui;
		break;
	case ST_UNSET:
		c->mode &= ~(arg.ui);
		if ((c->mode & MODE_REVERSE) != (c->mode & MODE_REVERSE))
			tfulldirt(term);
		break;
	case ST_POINTERMOTION:
	case ST_CURSORSTYLE:
	case ST_COPY:
	case ST_COLORNAME:
		break;
	}
	return 0;
}

static void
create(const char *args[]) {
	const char *pargs[4] = { shell, NULL };
	char buf[8], *cwd = NULL;
	const char *env[] = {
		"DVTM_WINDOW_ID", buf,
		NULL
	};

	if (args && args[0]) {
		pargs[1] = "-c";
		pargs[2] = args[0];
		pargs[3] = NULL;
	}
	Client *c = calloc(1, sizeof(Client));
	if (!c)
		return;
	c->tags = tagset[seltags];
	c->id = ++cmdfifo.id;
	snprintf(buf, sizeof buf, "%d", c->id);

	if (!(c->window = newwin(wah, waw, way, wax))) {
		free(c);
		return;
	}

	/* Term *term, int col, int row, int hist, int alt, int deffg, int defbg, int ts */
	c->term = c->app = tnew(screen.w, screen.h, screen.history, 1, 7, 0, 8);
	if (!c->term) {
		delwin(c->window);
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

	if (args && args[1])
		strncpy(c->title, args[1], sizeof(c->title));
	c->title[sizeof(c->title)-1] = '\0';

	if (args && args[2])
		cwd = !strcmp(args[2], "$CWD") ? getcwd_by_pid(sel) : (char*)args[2];

	c->term->handler = event_handler;
	/* ttynew(Term *term, char *cmd, char *iofname, char **args, int *in, int *out, int *err) */
	ttynew(c->term, shell, NULL, pargs, NULL, NULL, NULL);
	if (args && args[2] && !strcmp(args[2], "$CWD"))
		free(cwd);

	applycolorrules(c);
	c->x = wax;
	c->y = way;
	debug("client with pid %d forked\n", c->term->pid);
	attach(c);
	focus(c);
	arrange();
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
					if (curr->fg == -1)
						esclen = sprintf(s, "\033[39m");
					else
						esclen = sprintf(s, "\033[38;5;%dm", curr->fg);
					if (esclen > 0)
						s += esclen;
				}
				if (!prev || curr->bg != prev->bg || curr->mode != prev->mode) {
					if (curr->bg == -1)
						esclen = sprintf(s, "\033[49m");
					else
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
copymode(const char *args[]) {
	if (!args || !args[0] || !sel || sel->editor)
		return;

	bool colored = strstr(args[0], "pager") != NULL;

	if (!(sel->editor = tnew(sel->w, sel->h - sel->has_title_line, sel->h, 1, 0, 7, 8)))
		return;

	int *to = &sel->editor_fds[0];
	int *from = strstr(args[0], "editor") ? &sel->editor_fds[1] : NULL;
	sel->editor_fds[0] = sel->editor_fds[1] = -1;

	const char *argv[3] = { args[0], NULL, NULL };
	char argline[32];
	/*
	int line = vt_content_start(sel->app);
	snprintf(argline, sizeof(argline), "+%d", line);
	argv[1] = argline;
	*/

	sel->editor->handler = event_handler;
	if (ttynew(sel->editor, args[0], NULL, argv, to, from, NULL) < 0) {
		tfree(sel->editor);
		sel->editor = NULL;
		return;
	}

	sel->term = sel->editor;

	if (sel->editor_fds[0] != -1) {
		char *buf = NULL;
		size_t len = tgetcontent(sel->app, &buf, colored);
		char *cur = buf;
		while (len > 0) {
			ssize_t res = write(sel->editor_fds[0], cur, len);
			if (res < 0) {
				if (errno == EAGAIN || errno == EINTR)
					continue;
				break;
			}
			cur += res;
			len -= res;
		}
		free(buf);
		close(sel->editor_fds[0]);
		sel->editor_fds[0] = -1;
	}

	if (args[1])
		ttywrite(sel->editor, args[1], strlen(args[1]), 0);
}

static void
focusn(const char *args[]) {
	for (Client *c = nextvisible(clients); c; c = nextvisible(c->next)) {
		if (c->order == atoi(args[0])) {
			focus(c);
			if (c->minimized)
				toggleminimize(NULL);
			return;
		}
	}
}

static void
focusid(const char *args[]) {
	if (!args[0])
		return;

	const int win_id = atoi(args[0]);
	for (Client *c = clients; c; c = c->next) {
		if (c->id == win_id) {
			focus(c);
			if (c->minimized)
				toggleminimize(NULL);
			if (!isvisible(c)) {
				c->tags |= tagset[seltags];
				tagschanged();
			}
			return;
		}
	}
}

static void
focusnext(const char *args[]) {
	Client *c;
	if (!sel)
		return;
	for (c = sel->next; c && !isvisible(c); c = c->next);
	if (!c)
		for (c = clients; c && !isvisible(c); c = c->next);
	if (c)
		focus(c);
}

static void
focusnextnm(const char *args[]) {
	if (!sel)
		return;
	Client *c = sel;
	do {
		c = nextvisible(c->next);
		if (!c)
			c = nextvisible(clients);
	} while (c->minimized && c != sel);
	focus(c);
}

static void
focusprev(const char *args[]) {
	Client *c;
	if (!sel)
		return;
	for (c = sel->prev; c && !isvisible(c); c = c->prev);
	if (!c) {
		for (c = clients; c && c->next; c = c->next);
		for (; c && !isvisible(c); c = c->prev);
	}
	if (c)
		focus(c);
}

static void
focusprevnm(const char *args[]) {
	if (!sel)
		return;
	Client *c = sel;
	do {
		for (c = c->prev; c && !isvisible(c); c = c->prev);
		if (!c) {
			for (c = clients; c && c->next; c = c->next);
			for (; c && !isvisible(c); c = c->prev);
		}
	} while (c && c != sel && c->minimized);
	focus(c);
}

static void
focuslast(const char *args[]) {
	if (lastsel)
		focus(lastsel);
}

static void
focusup(const char *args[]) {
	if (!sel)
		return;
	/* avoid vertical separator, hence +1 in x direction */
	Client *c = get_client_by_coord(sel->x + 1, sel->y - 1);
	if (c)
		focus(c);
	else
		focusprev(args);
}

static void
focusdown(const char *args[]) {
	if (!sel)
		return;
	Client *c = get_client_by_coord(sel->x, sel->y + sel->h);
	if (c)
		focus(c);
	else
		focusnext(args);
}

static void
focusleft(const char *args[]) {
	if (!sel)
		return;
	Client *c = get_client_by_coord(sel->x - 2, sel->y);
	if (c)
		focus(c);
	else
		focusprev(args);
}

static void
focusright(const char *args[]) {
	if (!sel)
		return;
	Client *c = get_client_by_coord(sel->x + sel->w + 1, sel->y);
	if (c)
		focus(c);
	else
		focusnext(args);
}

static void
killclient(const char *args[]) {
	if (!sel)
		return;
	debug("killing client with pid: %d\n", sel->term->pid);
	kill(-sel->term->pid, SIGKILL);
}

static void
paste(const char *args[]) {
	if (sel && copyreg.data)
		ttywrite(sel->term, copyreg.data, copyreg.len, 0);
}

static void
quit(const char *args[]) {
	cleanup();
	exit(EXIT_SUCCESS);
}

static void
redraw(const char *args[]) {
	for (Client *c = clients; c; c = c->next) {
		if (!c->minimized) {
			tfulldirt(c->term);
			wclear(c->window);
			wnoutrefresh(c->window);
		}
	}
	resize_screen();
}

static void
scrollback(const char *args[]) {
	if (!is_content_visible(sel))
		return;

	if (!args[0] || atoi(args[0]) < 0)
		tscroll(sel, -sel->term->row/2);
	else
		tscroll(sel,  sel->term->row/2);

	draw(sel);

	curs_set(!(sel->mode & MODE_HIDE));
}

static void
send(const char *args[]) {
	if (sel && args && args[0])
		ttywrite(sel->term, args[0], strlen(args[0]), 0);
}

static void
setlayout(const char *args[]) {
	unsigned int i;

	if (!args || !args[0]) {
		if (++layout == &layouts[LENGTH(layouts)])
			layout = &layouts[0];
	} else {
		for (i = 0; i < LENGTH(layouts); i++)
			if (!strcmp(args[0], layouts[i].symbol))
				break;
		if (i == LENGTH(layouts))
			return;
		layout = &layouts[i];
	}
	arrange();
}

static void
incnmaster(const char *args[]) {
	int delta;

	if (isarrange(fullscreen) || isarrange(grid))
		return;
	/* arg handling, manipulate nmaster */
	if (args[0] == NULL) {
		screen.nmaster = NMASTER;
	} else if (sscanf(args[0], "%d", &delta) == 1) {
		if (args[0][0] == '+' || args[0][0] == '-')
			screen.nmaster += delta;
		else
			screen.nmaster = delta;
		if (screen.nmaster < 1)
			screen.nmaster = 1;
	}
	arrange();
}

static void
setmfact(const char *args[]) {
	float delta;

	if (isarrange(fullscreen) || isarrange(grid))
		return;
	/* arg handling, manipulate mfact */
	if (args[0] == NULL) {
		screen.mfact = MFACT;
	} else if (sscanf(args[0], "%f", &delta) == 1) {
		if (args[0][0] == '+' || args[0][0] == '-')
			screen.mfact += delta;
		else
			screen.mfact = delta;
		if (screen.mfact < 0.1)
			screen.mfact = 0.1;
		else if (screen.mfact > 0.9)
			screen.mfact = 0.9;
	}
	arrange();
}

static void
startup(const char *args[]) {
	for (unsigned int i = 0; i < LENGTH(actions); i++)
		actions[i].cmd(actions[i].args);
}

static void
togglebar(const char *args[]) {
	if (bar.pos == BAR_OFF)
		showbar();
	else
		hidebar();
	bar.autohide = false;
	updatebarpos();
	redraw(NULL);
}

static void
togglebarpos(const char *args[]) {
	switch (bar.pos == BAR_OFF ? bar.lastpos : bar.pos) {
	case BAR_TOP:
		bar.pos = BAR_BOTTOM;
		break;
	case BAR_BOTTOM:
		bar.pos = BAR_TOP;
		break;
	}
	updatebarpos();
	redraw(NULL);
}

static void
toggleminimize(const char *args[]) {
	Client *c, *m, *t;
	unsigned int n;
	if (!sel)
		return;
	/* the last window can't be minimized */
	if (!sel->minimized) {
		for (n = 0, c = nextvisible(clients); c; c = nextvisible(c->next))
			if (!c->minimized)
				n++;
		if (n == 1)
			return;
	}
	sel->minimized = !sel->minimized;
	m = sel;
	/* check whether the master client was minimized */
	if (sel == nextvisible(clients) && sel->minimized) {
		c = nextvisible(sel->next);
		detach(c);
		attach(c);
		focus(c);
		detach(m);
		for (; c && (t = nextvisible(c->next)) && !t->minimized; c = t);
		attachafter(m, c);
	} else if (m->minimized) {
		/* non master window got minimized move it above all other
		 * minimized ones */
		focusnextnm(NULL);
		detach(m);
		for (c = nextvisible(clients); c && (t = nextvisible(c->next)) && !t->minimized; c = t);
		attachafter(m, c);
	} else { /* window is no longer minimized, move it to the master area */
		tfulldirt(m->term);
		detach(m);
		attach(m);
	}
	arrange();
}

static void
togglemouse(const char *args[]) {
	mouse_events_enabled = !mouse_events_enabled;
	mouse_setup();
}

static void
togglerunall(const char *args[]) {
	runinall = !runinall;
	drawbar();
	draw_all();
}

static void
zoom(const char *args[]) {
	Client *c;

	if (!sel)
		return;
	if (args && args[0])
		focusn(args);
	if ((c = sel) == nextvisible(clients))
		if (!(c = nextvisible(c->next)))
			return;
	detach(c);
	attach(c);
	focus(c);
	if (c->minimized)
		toggleminimize(NULL);
	arrange();
}

/* commands for use by mouse bindings */
static void
mouse_focus(const char *args[]) {
	focus(msel);
	if (msel->minimized)
		toggleminimize(NULL);
}

static void
mouse_fullscreen(const char *args[]) {
	mouse_focus(NULL);
	setlayout(isarrange(fullscreen) ? NULL : args);
}

static void
mouse_minimize(const char *args[]) {
	focus(msel);
	toggleminimize(NULL);
}

static void
mouse_zoom(const char *args[]) {
	focus(msel);
	zoom(NULL);
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

static void
handle_mouse(void) {
#ifdef CONFIG_MOUSE
	MEVENT event;
	unsigned int i;
	if (getmouse(&event) != OK)
		return;
	msel = get_client_by_coord(event.x, event.y);

	if (!msel)
		return;

	debug("mouse x:%d y:%d cx:%d cy:%d mask:%d\n", event.x, event.y, event.x - msel->x, event.y - msel->y, event.bstate);

	/* vt_mouse(msel->term, event.x - msel->x, event.y - msel->y, event.bstate); */

	for (i = 0; i < LENGTH(buttons); i++) {
		if (event.bstate & buttons[i].mask)
			buttons[i].action.cmd(buttons[i].action.args);
	}

	msel = NULL;
#endif /* CONFIG_MOUSE */
}

static void
handle_statusbar(void) {
	char *p;
	int r;
	switch (r = read(bar.fd, bar.text, sizeof bar.text - 1)) {
		case -1:
			strncpy(bar.text, strerror(errno), sizeof bar.text - 1);
			bar.text[sizeof bar.text - 1] = '\0';
			bar.fd = -1;
			break;
		case 0:
			bar.fd = -1;
			break;
		default:
			bar.text[r] = '\0';
			p = bar.text + r - 1;
			for (; p >= bar.text && *p == '\n'; *p-- = '\0');
			for (; p >= bar.text && *p != '\n'; --p);
			if (p >= bar.text)
				memmove(bar.text, p + 1, strlen(p));
			drawbar();
	}
}

static void
handle_editor(Client *c) {
	if (!copyreg.data && (copyreg.data = malloc(screen.history)))
		copyreg.size = screen.history;
	copyreg.len = 0;
	while (c->editor_fds[1] != -1 && copyreg.len < copyreg.size) {
		ssize_t len = read(c->editor_fds[1], copyreg.data + copyreg.len, copyreg.size - copyreg.len);
		if (len == -1) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (len == 0)
			break;
		copyreg.len += len;
		if (copyreg.len == copyreg.size) {
			copyreg.size *= 2;
			if (!(copyreg.data = realloc(copyreg.data, copyreg.size))) {
				copyreg.size = 0;
				copyreg.len = 0;
			}
		}
	}
	c->editor_died = false;
	c->editor_fds[1] = -1;
	tfree(c->editor);
	c->editor = NULL;
	c->term = c->app;
	tfulldirt(c->term);
	draw_content(c);
	wnoutrefresh(c->window);
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
	eprint("usage: dvtm [-v] [-M] [-m mod] [-d delay] [-h lines] [-t title] "
	       "[-s status-fifo] [-c cmd-fifo] [cmd...]\n");
	exit(EXIT_FAILURE);
}

static bool
parse_args(int argc, char *argv[]) {
	bool init = false;
	const char *name = argv[0];

	if (name && (name = strrchr(name, '/')))
		dvtm_name = name + 1;
	if (!getenv("ESCDELAY"))
		set_escdelay(100);
	for (int arg = 1; arg < argc; arg++) {
		if (argv[arg][0] != '-') {
			const char *args[] = { argv[arg], NULL, NULL };
			if (!init) {
				setup();
				init = true;
			}
			create(args);
			continue;
		}
		if (argv[arg][1] != 'v' && argv[arg][1] != 'M' && (arg + 1) >= argc)
			usage();
		switch (argv[arg][1]) {
			case 'v':
				puts("dvtm-"VERSION" © 2007-2016 Marc André Tanner");
				exit(EXIT_SUCCESS);
			case 'M':
				mouse_events_enabled = !mouse_events_enabled;
				break;
			case 'm': {
				char *mod = argv[++arg];
				if (mod[0] == '^' && mod[1])
					*mod = CTRL(mod[1]);
				for (unsigned int b = 0; b < LENGTH(bindings); b++)
					if (bindings[b].keys[0] == MOD)
						bindings[b].keys[0] = *mod;
				break;
			}
			case 'd':
				set_escdelay(atoi(argv[++arg]));
				if (ESCDELAY < 50)
					set_escdelay(50);
				else if (ESCDELAY > 1000)
					set_escdelay(1000);
				break;
			case 'h':
				screen.history = atoi(argv[++arg]);
				break;
			case 't':
				title = argv[++arg];
				break;
			case 's':
				bar.fd = open_or_create_fifo(argv[++arg], &bar.file);
				updatebarpos();
				break;
			case 'c': {
				const char *fifo;
				cmdfifo.fd = open_or_create_fifo(argv[++arg], &cmdfifo.file);
				if (!(fifo = realpath(argv[arg], NULL)))
					error("%s\n", strerror(errno));
				setenv("DVTM_CMD_FIFO", fifo, 1);
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
	KeyCombo keys;
	unsigned int key_index = 0;
	memset(keys, 0, sizeof(keys));
	sigset_t emptyset, blockset;

	setenv("DVTM", VERSION, 1);
	if (!parse_args(argc, argv)) {
		setup();
		startup(NULL);
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

		if (bar.fd != -1) {
			FD_SET(bar.fd, &rd);
			nfds = MAX(nfds, bar.fd);
		}

		for (Client *c = clients; c; ) {
			if (c->editor && c->editor_died)
				handle_editor(c);
			if (!c->editor && c->died) {
				Client *t = c->next;
				destroy(c);
				c = t;
				continue;
			}
			int pty = c->editor ? c->editor->cmdfd : c->app->cmdfd;
			FD_SET(pty, &rd);
			nfds = MAX(nfds, pty);
			c = c->next;
		}

		doupdate();
		r = pselect(nfds + 1, &rd, NULL, NULL, NULL, &emptyset);

		if (r < 0) {
			if (errno == EINTR)
				continue;
			perror("select()");
			exit(EXIT_FAILURE);
		}

		if (FD_ISSET(STDIN_FILENO, &rd)) {
			int code = getch();
			if (code >= 0) {
				keys[key_index++] = code;
				KeyBinding *binding = NULL;
				if (code == KEY_MOUSE) {
					key_index = 0;
					handle_mouse();
				} else if ((binding = keybinding(keys, key_index))) {
					unsigned int key_length = MAX_KEYS;
					while (key_length > 1 && !binding->keys[key_length-1])
						key_length--;
					if (key_index == key_length) {
						binding->action.cmd(binding->action.args);
						key_index = 0;
						memset(keys, 0, sizeof(keys));
					}
				} else {
					key_index = 0;
					memset(keys, 0, sizeof(keys));
					keypress(code);
				}
			}
			if (r == 1) /* no data available on pty's */
				continue;
		}

		if (cmdfifo.fd != -1 && FD_ISSET(cmdfifo.fd, &rd))
			handle_cmdfifo();

		if (bar.fd != -1 && FD_ISSET(bar.fd, &rd))
			handle_statusbar();

		for (Client *c = clients; c; c = c->next) {
			if (FD_ISSET(c->term->cmdfd, &rd)) {
				if ((r = ttyread(c->term)) < 0 && errno == EIO) {
					if (c->editor)
						c->editor_died = true;
					else
						c->died = true;
					continue;
				}
			}

			if (c != sel && is_content_visible(c)) {
				draw_content(c);
				wnoutrefresh(c->window);
			}
		}

		if (is_content_visible(sel)) {
			draw_content(sel);
			curs_set(!(sel->mode & MODE_HIDE));
			wnoutrefresh(sel->window);
		}
	}

	cleanup();
	return 0;
}
