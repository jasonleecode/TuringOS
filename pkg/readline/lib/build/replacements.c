#include <string.h>
#include <unistd.h>
#include "tcap.h"

#include <stdio.h>
#include <stdlib.h>

#ifdef DEBUG
static int _DEBUG = 1;
#else
static int _DEBUG = 0;
#endif

#define LOGd(l, a...) do { if (l) printf(a); } while(0)


int tputs( const char *str, int affcnt, int (*putc)(int))
{
    int i;
    if (!str) return 0;
    for(i=0; i<(int)strlen(str); i++)
    {
        (*putc)(str[i]);
    }
    return 0;
}

char *tgoto( const char* cap, int col, int row )
{
    char *goto_string;
    sprintf( goto_string, "\033[%d,%dH%s", row, col, cap );
    return goto_string;
}

int tgetflag( const char *id )
{
    LOGd(_DEBUG,"tgetflag: %s\n", id);
    return 0;
}

int tcflow( int fd, int action )
{
    LOGd(_DEBUG,"tcflow: %d\n", action);
    return 0;
}

int tgetnum( const char *id )
{
    LOGd(_DEBUG,"tgetnum: %s\n", id);
    return 0;
}

char *tgetstr( const char *id, char **area )
{
    LOGd(_DEBUG,"tgetstr: %s\n", id);
    (void)area;
    /* Return the backspace sequence for "le"; NULL for everything else.
     * NULL means "no capability", which is safe — readline falls back to
     * hardcoded defaults (e.g. "\r" for cr) or simply skips the operation. */
    if (strcmp(id, "le") == 0) return strdup("\b");
    return NULL;
}

int tgetent( char *bp, const char *name )
{
    LOGd(_DEBUG,"tgetent: name=%s\n", name);
    (void)bp;
    /* Return 1 (TGETENT_SUCCESS) so readline keeps term_string_buffer and
     * term_buffer alive.  Returning 0 triggers FREE() of both — the sequential
     * free of two adjacent heap chunks corrupts uclibc's malloc arena. */
    return 1;
}
