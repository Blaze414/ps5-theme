/* What the parsers link against from the rest of the plugin, for fuzzing. */
#include "platform.h"

void plat_notify(const char *msg) { (void)msg; }
uint64_t plat_free_bytes(const char *path) { (void)path; return UINT64_MAX; }
int plat_mounted_under(const char *path) { (void)path; return 0; }
void plat_toast(const char *title, const char *detail, const char *icon) { (void)title; (void)detail; (void)icon; }
