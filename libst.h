/* See LICENSE for license details. */

#include <stdint.h>
#include <sys/types.h>

/* macros */
#define TRUECOLOR(r,g,b)	(1 << 24 | (r) << 16 | (g) << 8 | (b))
#define IS_TRUECOL(x)		(1 << 24 & (x))
#define TRUERED(x)		(((x) & 0xff0000) >> 16)
#define TRUEGREEN(x)		(((x) & 0xff00) >> 8)
#define TRUEBLUE(x)		((x) & 0xff)

enum glyph_attribute {
	ATTR_NULL       = 0,
	ATTR_BOLD       = 1 << 0,
	ATTR_FAINT      = 1 << 1,
	ATTR_ITALIC     = 1 << 2,
	ATTR_UNDERLINE  = 1 << 3,
	ATTR_BLINK      = 1 << 4,
	ATTR_REVERSE    = 1 << 5,
	ATTR_INVISIBLE  = 1 << 6,
	ATTR_STRUCK     = 1 << 7,
	ATTR_WRAP       = 1 << 8,
	ATTR_WIDE       = 1 << 9,
	ATTR_WDUMMY     = 1 << 10,
	ATTR_BOLD_FAINT = ATTR_BOLD | ATTR_FAINT,
};

enum selection_mode {
	SEL_IDLE = 0,
	SEL_EMPTY = 1,
	SEL_READY = 2
};

enum selection_type {
	SEL_REGULAR = 1,
	SEL_RECTANGULAR = 2
};

enum selection_snap {
	SNAP_WORD = 1,
	SNAP_LINE = 2
};

typedef uint_least32_t Rune;

#define Glyph Glyph_
typedef struct {
	Rune u;              /* character code */
	unsigned short mode; /* attribute flags */
	uint32_t fg;         /* foreground  */
	uint32_t bg;         /* background  */
} Glyph;

typedef Glyph *Line;

typedef union {
	int i;
	unsigned int ui;
	float f;
	void *v;
	char *s;
} Arg;

typedef enum {
	ST_TITLE,       /* char * - xsettitle */
	ST_SET,
	ST_UNSET,
	ST_ICONTITLE,  /* char * - xseticontitle */
	ST_CSI_ERROR,   /* char * - parse error with CSI string */
	ST_STR_ERROR,   /* char * - parse error with CSI string */
	ST_BELL,        /* unintialized - ascii bell, maybe xbell */
	ST_RESET,       /* uninitialized - reset to initial state */
	ST_POINTERMOTION, /* int - whether pointermotion is set */
	ST_CURSORSTYLE, /* int - cursor style */
	ST_COPY,        /* char * - copy to clipboard */
	ST_COLORNAME,   /* void * - color: arg->v[0].i, name: arg->v[1].s */
	ST_EOF          /* uninitialized - EOF on TTY */
} Event;

enum win_mode {
	MODE_VISIBLE     = 1 << 0,
	MODE_FOCUSED     = 1 << 1,
	MODE_APPKEYPAD   = 1 << 2,
	MODE_MOUSEBTN    = 1 << 3,
	MODE_MOUSEMOTION = 1 << 4,
	MODE_REVERSE     = 1 << 5,
	MODE_KBDLOCK     = 1 << 6,
	MODE_HIDE        = 1 << 7,
	MODE_APPCURSOR   = 1 << 8,
	MODE_MOUSESGR    = 1 << 9,
	MODE_8BIT        = 1 << 10,
	MODE_BLINK       = 1 << 11,
	MODE_FBLINK      = 1 << 12,
	MODE_FOCUS       = 1 << 13,
	MODE_MOUSEX10    = 1 << 14,
	MODE_MOUSEMANY   = 1 << 15,
	MODE_BRCKTPASTE  = 1 << 16,
	MODE_NUMLOCK     = 1 << 17,
	MODE_MOUSE       = MODE_MOUSEBTN|MODE_MOUSEMOTION|MODE_MOUSEX10\
	                  |MODE_MOUSEMANY,
};

typedef struct {
	Glyph attr; /* current char attributes */
	int x;
	int y;
	char state;
} TCursor;

/* Arbitrary sizes */
#define UTF_INVALID   0xFFFD
#define UTF_SIZ       4
#define ESC_BUF_SIZ   (128*UTF_SIZ)
#define ESC_ARG_SIZ   16
#define STR_BUF_SIZ   ESC_BUF_SIZ
#define STR_ARG_SIZ   ESC_ARG_SIZ

enum escape_state {
	ESC_START      = 1,
	ESC_CSI        = 2,
	ESC_STR        = 4,  /* DCS, OSC, PM, APC */
	ESC_ALTCHARSET = 8,
	ESC_STR_END    = 16, /* a final string was encountered */
	ESC_TEST       = 32, /* Enter in test mode */
	ESC_UTF8       = 64,
};

/* CSI Escape sequence structs */
/* ESC '[' [[ [<priv>] <arg> [;]] <mode> [<mode>]] */
typedef struct {
	char buf[ESC_BUF_SIZ]; /* raw string */
	size_t len;            /* raw string length */
	char priv;
	int arg[ESC_ARG_SIZ];
	int narg;              /* nb of args */
	char mode[2];
} CSIEscape;

/* STR Escape sequence structs */
/* ESC type [[ [<priv>] <arg> [;]] <mode>] ESC '\' */
typedef struct {
	char type;             /* ESC type ... */
	char *buf;             /* allocated raw string */
	size_t siz;            /* allocation size */
	size_t len;            /* raw string length */
	char *args[STR_ARG_SIZ];
	int narg;              /* nb of args */
} STREscape;

/* Internal representation of the screen */
typedef struct Term Term;
struct Term {
	pid_t pid;
	int cmdfd;    /* tty fd */ 
	int iofd;     /* copied fd */ 
	int row;      /* nb row */
	int col;      /* nb col */
	int maxrow;   /* max row in the ring buffer */
	int maxcol;   /* max col in the ring buffer */
	int seen;
	Line *line;   /* screen */
	Line *alt;    /* alternate screen */
	Line *buf;    /* top of the history/line ring buffer */
	Line *altbuf; /* top of alternate screen ring buffer */
	int *dirty;   /* dirtyness of lines */
	TCursor c;    /* cursor */
	TCursor cs[2];/* save points for alt & primary cursor  */
	int ocx;      /* old cursor col */
	int ocy;      /* old cursor row */
	int top;      /* top    scroll limit */
	int bot;      /* bottom scroll limit */
	int mode;     /* terminal mode flags */
	int esc;      /* escape state flags */
	char trantbl[4]; /* charset table translation */
	int charset;  /* current charset */
	int icharset; /* selected charset for sequence */
	int *tabs;
	int tabspaces;
	unsigned int defaultfg;
	unsigned int defaultbg;
	int (*handler)(Term *, Event, Arg);
	Rune lastc;   /* last printed char outside of sequence, 0 if control */
	CSIEscape csiescseq;
	STREscape strescseq;
};

void die(const char *, ...);

void tprintscreen(Term *);
void tsendbreak(Term *);
void ttoggleprinter(Term *);

Line *tgetline(Term *, int); /* gets the line % rows */
int tattrset(Term *, int);
Term *tnew(int, int, int, int, int, int, int);
void tfree(Term *);
void tresize(Term *, int, int);
void tfulldirt(Term *);
void tsetdirtattr(Term *, int);
void ttyhangup(Term *);
int ttynew(Term *, char *, char *, char **, int *, int *, int *);
size_t ttyread(Term *);
void ttyresize(Term *, int, int);
void ttywrite(Term *, const char *, size_t, int);

void resettitle(Term *);

size_t utf8encode(Rune, char *);

void *xmalloc(size_t);
void *xrealloc(void *, size_t);
char *xstrdup(char *);
