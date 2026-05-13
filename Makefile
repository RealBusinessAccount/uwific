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
INSTALL_DIR = /usr/local/bin

.PHONY: all clean install uninstall

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

$(SRCDIR)/%.o: $(SRCDIR)/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

install: $(TARGET)
	install -Dm755 $(TARGET) $(INSTALL_DIR)/$(TARGET)

uninstall:
rm -f $(INSTALL_DIR)/$(TARGET)

clean:
	rm -f $(OBJS) $(TARGET)
