/* valid curses attributes are listed below they can be ORed
 *
 * A_NORMAL        Normal display (no highlight)
 * A_STANDOUT      Best highlighting mode of the terminal.
 * A_UNDERLINE     Underlining
 * A_REVERSE       Reverse video
 * A_BLINK         Blinking
 * A_DIM           Half bright
 * A_BOLD          Extra bright or bold
 * A_PROTECT       Protected mode
 * A_INVIS         Invisible or blank mode
 */

/* scroll back buffer size in lines */
#define SCROLL_HISTORY 500

static Cmd commands[] = {
	/* create [cmd]: create a new window, run `cmd` in the shell if specified */
	{ "dump",   { dump,                              { NULL } } },
	{ "scroll", { scrollback,                        { NULL } } },
	{ "redraw", { redraw,                            { NULL } } },
	{ "send",   { send,                              { NULL } } },
	{ "quit",   { quit,                              { NULL } } },
};
