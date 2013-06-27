CFLAGS += -I../..

LDFLAGS += -L../../framework -lmlt -lrt -lpthread

include ../../../config.mak

TARGET = ../libmltunixsock(LIBSUF)

OBJS = factory.o \
	   producer_libunixsock.o \
	   consumer_libunixsock.o

CFLAGS += `pkg-config --cflags libunixsock`

LDFLAGS += `pkg-config --libs libunixsock`

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
	install -m 755 $(TARGET) "$(DESTDIR)$(moduledir)"
	install -d "$(DESTDIR)$(mltdatadir)/unixsock"
	install -m 644 *.yml "$(DESTDIR)$(mltdatadir)/unixsock"

ifneq ($(wildcard .depend),)
include .depend
endif
