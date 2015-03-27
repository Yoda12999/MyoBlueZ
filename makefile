CC = gcc
LIBS = dbus-1 dbus-glib-1 glib-2.0 gio-2.0 bluez
CFLAGS = -c -I. `pkg-config --cflags $(LIBS)`
LDFLAGS = `pkg-config --libs $(LIBS)`
DEPS = myo-bluez.h
SOURCES = myo-bluez.cpp
OBJECTS = $(SOURCES:.cpp=.o)

%.o: %.c $(DEPS)
	$(CC) $(CFLAGS) $< -o $@

myo-bluez: myo-bluez.o
	$(CC) $(OBJECTS) $(LDFLAGS) -o myo-bluez

all:
	myo-bluez

.PHONY: clean

clean:
	rm -f *.o