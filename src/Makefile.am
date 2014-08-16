AUTOMAKE_OPTIONS = foreign

lib_LTLIBRARIES = libmpc.la

libdir = @BEEP_PLUGIN_DIR@

INCLUDES = @GLIB_2_0_CFLAGS@ @GTK_CFLAGS@ -DVERSION=\"@VERSION@\"
libmpc_la_LIBADD = @PTHREAD_LIBS@ @GLIB_2_0_LIBS@ @GTK_LIBS@

libmpc_la_SOURCES = equalizer.cpp tags.cpp libmpc.cpp
libmpc_la_LDFLAGS = -lmusepack -avoid-version
noinst_HEADERS = tags.h equalizer.h

