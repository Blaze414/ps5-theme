/* What the parsers link against from the rest of the plugin, for fuzzing. */
#include "platform.h"

void plat_notify(const char *msg) { (void)msg; }
