#! /bin/sh
gcc -DBYTESEX_LITTLE -I . -MM $(find . -name '*.c') \
 | sed -E 's,^([^ ]),obj/\1,' \
 > depend.mk
