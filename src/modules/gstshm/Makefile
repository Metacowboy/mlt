CFLAGS += -I../..

LDFLAGS += -L../../framework -lmlt -lrt -lpthread

CFLAGS += `pkg-config $(PKGCONFIG_PREFIX) --cflags glib-2.0`
CFLAGS += -DSHM_PIPE_USE_GLIB
LDFLAGS += `pkg-config $(PKGCONFIG_PREFIX) --libs glib-2.0`

include ../../../config.mak

TARGET = ../libmltgstshm$(LIBSUF)

OBJS = factory.o \
       producer_gstshm.o \
       consumer_gstshm.o \
       shmalloc.o \
       shmpipe.o \
       common.o

SRCS := $(OBJS:.o=.c)

all: 	$(TARGET)

$(TARGET): $(OBJS)
		$(CC) $(SHFLAGS) -o $@ $(OBJS) $(LDFLAGS)

depend:	$(SRCS)
		$(CC) -MM $(CFLAGS) $^ 1>.depend

distclean:	clean
		rm -f .depend

clean:
		rm -f $(OBJS) $(TARGET)

install: all
	install -m 755 $(TARGET) "$(DESTDIR)/$(moduledir)"
	install -d "$(DESTDIR)/$(mltdatadir)/gstshm"
	install -m 644 *.yml "$(DESTDIR)/$(mltdatadir)/gstshm"

ifneq ($(wildcard .depend),)
include .depend
endif
