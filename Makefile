# $PostgreSQL: pgsql/contrib/plpgsql_check/Makefile

MODULE_big = utl_mail
OBJS = utl_mail.o
DATA = utl_mail--1.0.sql
EXTENSION = utl_mail

REGRESS = init utl_mail

ifdef NO_PGXS
subdir = contrib/utl_mail
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
else
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
endif

override CFLAGS += -Wextra

