#! /bin/sh
libtoolize --copy
aclocal
autoheader
automake --gnu --add-missing --copy
autoconf
