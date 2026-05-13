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
TARGET  = uwific
PREFIX  = /usr/local
 
.PHONY: all clean install uninstall
 
all: $(TARGET)
 
$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)
 
$(SRCDIR)/%.o: $(SRCDIR)/%.c
	$(CC) $(CFLAGS) -c -o $@ $<
 
install: $(TARGET)
	install -Dm755 $(TARGET) $(DESTDIR)$(PREFIX)/bin/$(TARGET)
 
uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(TARGET)
 
clean:
	rm -f $(OBJS) $(TARGET)
