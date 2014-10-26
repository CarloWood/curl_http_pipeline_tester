AUTOMAKE_OPTIONS = foreign
bin_PROGRAMS = http_server http_client
DEFS = @DEFS@

http_server_SOURCES = http_server.cpp
http_server_LDADD = -lboost_system

http_client_SOURCES = http_client.c
http_client_CFLAGS = -std=c11 $(LIBCURL_CFLAGS)
http_client_LDADD = $(LIBCURL_LIBS)

MAINTAINERCLEANFILES = $(srcdir)/*~ $(srcdir)/config.h.in $(srcdir)/Makefile.in $(srcdir)/aclocal.m4 $(srcdir)/configure $(srcdir)/depcomp $(srcdir)/install-sh $(srcdir)/missing
