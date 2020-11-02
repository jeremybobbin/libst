/* See LICENSE file for copyright and license details. */

/*
 * What program is execed by st depends of these precedence rules:
 * 1: program passed with -e
 * 2: scroll and/or utmp
 * 3: SHELL environment variable
 * 4: value of shell in /etc/passwd
 * 5: value of shell in config.h
 */
#define SHELL "/bin/sh"
#define UTMP NULL
/* scroll program: to enable use a string like "scroll" */
#define SCROLL NULL

/* identification sequence returned in DA and DECID */
#define VTIDEN "\033[?6c"

/* default TERM value */
#define TERMNAME "st-256color"
