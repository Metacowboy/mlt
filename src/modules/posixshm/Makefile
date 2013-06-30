CFLAGS += -I../..

LDFLAGS += -L../../framework -lmlt -lrt -lpthread

include ../../../config.mak

TARGET = ../libmltposixshm$(LIBSUF)

OBJS = factory.o \
	   producer_posixshm.o \
	   consumer_posixshm.o

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
	install -d "$(DESTDIR)$(mltdatadir)/posixshm"
	install -m 644 *.yml "$(DESTDIR)$(mltdatadir)/posixshm"

ifneq ($(wildcard .depend),)
include .depend
endif
