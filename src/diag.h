/* diag.c: what the plugin found on the console, written to DUMP_DIR to find out why something went wrong. */
#ifndef PS5_THEME_DIAG_H
#define PS5_THEME_DIAG_H

void dump_assets_once(void);   /* the console's replaceable files, for the manager (before any theme is mounted) */
void dump_shell_files(void);
void dump_mount_table(void);

#endif
