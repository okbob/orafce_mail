# $PostgreSQL: pgsql/contrib/orafce_mail/Makefile

MODULE_big = orafce_mail
OBJS = orafce_mail.o
DATA = orafce_mail--1.0.sql
EXTENSION = orafce_mail

REGRESS = init orafce_mail

CURL_CONFIG = curl-config

CFLAGS += $(shell $(CURL_CONFIG) --cflags)
LIBS += $(shell $(CURL_CONFIG) --libs)
SHLIB_LINK := $(LIBS)

ifdef NO_PGXS
subdir = contrib/orafce_mail
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
else
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
endif

override CFLAGS += -Wextra

