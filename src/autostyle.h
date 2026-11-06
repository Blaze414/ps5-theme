/* autostyle.c: the icon style for games and apps installed later, run in the background. */
#ifndef PS5_THEME_AUTOSTYLE_H
#define PS5_THEME_AUTOSTYLE_H

#include <stddef.h>

void *autostyle_thread(void *arg);
int   autostyle_settle(void);              /* seconds nothing in a title's folders may change before it counts */
int   autostyle_wait(const char *title);   /* seconds until it does (0: installed), -1: no such title or no icon yet */
void  titles_stamp(char *out, size_t size);   /* changes when the installed titles do, or the styler did something */
void  title_name(const char *title, char *out, size_t size);   /* the name the console shows, else the ID */
/* What the console says once the theme is applied again for icons styled and titles whose pictures went back. */
void  autostyle_note(char *out, size_t size, const char (*styled)[16], int nstyled, const char (*back)[16], int nback);

#endif
