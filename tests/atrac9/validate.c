/* The plugin's ATRAC9 check (src/atrac9.c) as a command: validate <file> <stock>. */
#include <stdio.h>
#include <stddef.h>

int at9_validate(const char *cand, const char *stock, char *why, size_t len);

int
main(int argc, char **argv) {
  char why[256] = "";
  if(argc != 3) return 2;
  int rc = at9_validate(argv[1], argv[2], why, sizeof(why));
  printf("%s\n", rc ? why : "ok");
  return rc ? 1 : 0;
}
