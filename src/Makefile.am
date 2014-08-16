AUTOMAKE_OPTIONS = foreign

lib_LTLIBRARIES = libmpc.la

libdir = @BEEP_PLUGIN_DIR@

INCLUDES = @GLIB_2_0_CFLAGS@ @BEEP_CFLAGS@ @GTK_CFLAGS@ @TAGLIB_CFLAGS@ -DVERSION=\"@VERSION@\"
libmpc_la_LIBADD = @PTHREAD_LIBS@ @GLIB_2_0_LIBS@ @BEEP_LIBS@ @GTK_LIBS@ @TAGLIB_LIBS@

libmpc_la_SOURCES = equalizer.cpp libmpc.cpp
libmpc_la_LDFLAGS = -lmpcdec -avoid-version
noinst_HEADERS = equalizer.h libmpc.h

