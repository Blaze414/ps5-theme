/* The plugin's description, carried in the ELF for any loader or tool to read without running it.

   Two sections the console never loads (no "a" flag: they take no memory): `.ps5meta.json`, UTF-8 JSON with its
   id, name, version, title ID, author, description, homepage and licence, written by tools/payload_meta.py from the
   Makefile's version as it builds; and `.ps5meta.icon`, the logo (the Themes tile's icon), a PNG. The build checks
   both in the finished ELF (payload_meta.py check). Built for the console only: the host test rig has no use for it. */

__asm__(".pushsection .ps5meta.json,\"\",@progbits\n"
        ".incbin \"build-meta/ps5meta.json\"\n"
        ".popsection\n"
        ".pushsection .ps5meta.icon,\"\",@progbits\n"
        ".incbin \"assets/icon0.png\"\n"
        ".popsection\n");
