lib_LTLIBRARIES = libeegdev.la
include_HEADERS = eegdev.h eegdev-common.h

libeegdev_la_SOURCES = eegdev.h eegdev-common.h core.c	\
		       coreinternals.h typecast.c	\
		       opendev.c

libeegdev_la_LDFLAGS = $(AM_LDFLAGS) -no-undefined			\
                       -version-info $(CURRENT):$(REVISION):$(AGE)
libeegdev_la_LIBADD = $(LTLIBOBJS) $(CORE_LIBS)
libeegdev_la_DEPENDENCIES = $(LTLIBOBJS)

pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA = eegdev.pc