CC      = gcc
CFLAGS  = -Wall -Wextra -std=c11 -g \
           $(shell pkg-config --cflags libsystemd ncurses)
LIBS    = $(shell pkg-config --libs libsystemd ncurses)

SRCDIR  = src
SRCS    = $(SRCDIR)/main.c   \
          $(SRCDIR)/dbus.c   \
          $(SRCDIR)/wifi.c   \
          $(SRCDIR)/agent.c  \
          $(SRCDIR)/ui.c
OBJS    = $(SRCS:.c=.o)
TARGET  = calwifi

.PHONY: all clean install

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

$(SRCDIR)/%.o: $(SRCDIR)/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

install: $(TARGET)
	install -Dm755 $(TARGET) $(DESTDIR)/usr/bin/$(TARGET)

clean:
	rm -f $(OBJS) $(TARGET)
