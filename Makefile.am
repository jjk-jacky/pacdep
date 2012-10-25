
CLEANFILES = pacdep.1

bin_PROGRAMS = pacdep
nodist_man_MANS = pacdep.1
dist_doc_DATA = AUTHORS COPYING HISTORY README.md

dist-hook:
	cp $(srcdir)/pacdep.pod $(distdir)/

AM_CFLAGS = -g -std=c99 \
		-Wall -Wextra -pedantic -Wshadow -Wpointer-arith -Wcast-align \
		-Wwrite-strings -Wmissing-prototypes -Wmissing-declarations \
		-Wredundant-decls -Wnested-externs -Winline -Wno-long-long \
		-Wuninitialized -Wconversion -Wstrict-prototypes

pacdep_SOURCES = main.c

pacdep.1: pacdep.pod
	pod2man --center="Package Dependencies listing" --section=1 --release=$(PACKAGE_VERSION) pacdep.pod pacdep.1

