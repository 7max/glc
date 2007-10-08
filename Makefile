
LD = gcc
CC = gcc

CFLAGS = -g -Wall -ansi -D_XOPEN_SOURCE=500
LDFLAGS = -Wall -ansi

MLIBDIR = lib
DESTDIR = 
BUILD = build
SRC = src
LIB = $(SRC)/lib
COMMON = $(SRC)/common
STREAM = $(SRC)/stream
SCRIPTS = scripts

VERSION=0
RELEASE=$(VERSION).2.8

#MINILZO = ../minilzo/
#LZO_OBJ = build/minilzo.o
#USE_LZO = -D__MINILZO -I$(MINILZO)
LZO_LIB = -llzo2

#DESTDIR = /home/pyry/Documents/projects/glc
#CFLAGS = -O2 -march=nocona -msse3 -fomit-frame-pointer -Wall
#LDFLAGS = -Wall -Wl,-O1


LIBS = -lpthread -lpacketstream -lGL -ldl -lelfhacks -lasound $(LZO_LIB)

HEADERS = $(COMMON)/glc.h \
	  $(COMMON)/util.h \
	  $(COMMON)/thread.h \
	  $(STREAM)/gl.h \
	  $(STREAM)/pack.h \
	  $(STREAM)/file.h \
	  $(STREAM)/img.h \
	  $(STREAM)/scale.h \
	  $(STREAM)/info.h \
	  $(STREAM)/audio.h \
	  $(STREAM)/wav.h \
	  $(STREAM)/demux.h \
	  $(STREAM)/ycbcr.h \
	  $(STREAM)/yuv4mpeg.h \
	  $(STREAM)/rgb.h

LIB_OBJS = $(BUILD)/gl.o \
           $(BUILD)/util.o \
           $(BUILD)/pack.o \
           $(BUILD)/file.o \
           $(BUILD)/img.o \
           $(BUILD)/scale.o \
           $(BUILD)/info.o \
           $(BUILD)/thread.o \
           $(BUILD)/audio.o \
           $(BUILD)/wav.o \
           $(BUILD)/demux.o \
           $(BUILD)/ycbcr.o \
           $(BUILD)/yuv4mpeg.o \
           $(BUILD)/rgb.o \
           $(LZO_OBJ)

CAPT_OBJS = $(BUILD)/main.o \
            $(BUILD)/alsa.o \
            $(BUILD)/opengl.o \
            $(BUILD)/x11.o

all: $(BUILD) $(BUILD)/libglc-capture.so.$(RELEASE) $(BUILD)/glc-play

$(BUILD):
	mkdir $(BUILD)

# capture library
$(BUILD)/libglc-capture.so.$(RELEASE): $(BUILD)/libglc.so.$(RELEASE) $(CAPT_OBJS)
	$(LD) $(LDFLAGS) -Wl,-soname,libglc-capture-so.$(VERSION) -L$(BUILD) -lglc -shared \
		-o $(BUILD)/libglc-capture.so.$(RELEASE) $(CAPT_OBJS)
	ln -sf libglc-capture.so.$(RELEASE) $(BUILD)/libglc-capture.so.$(VERSION)
	ln -sf libglc-capture.so.$(RELEASE) $(BUILD)/libglc-capture.so

$(BUILD)/main.o: $(LIB)/main.c $(LIB)/lib.h $(HEADERS)
	$(CC) $(CFLAGS) -fPIC -o $(BUILD)/main.o -c $(LIB)/main.c

$(BUILD)/alsa.o: $(LIB)/alsa.c $(LIB)/lib.h $(HEADERS)
	$(CC) $(CFLAGS) -fPIC -o $(BUILD)/alsa.o -c $(LIB)/alsa.c

$(BUILD)/opengl.o: $(LIB)/opengl.c $(LIB)/lib.h $(HEADERS)
	$(CC) $(CFLAGS) -fPIC -o $(BUILD)/opengl.o -c $(LIB)/opengl.c

$(BUILD)/x11.o: $(LIB)/x11.c $(LIB)/lib.h $(HEADERS)
	$(CC) $(CFLAGS) -fPIC -o $(BUILD)/x11.o -c $(LIB)/x11.c

# player / tool
$(BUILD)/glc-play: $(BUILD)/play.o $(BUILD)/libglc.so.$(RELEASE)
	$(LD) $(LDFLAGS) -L$(BUILD) -lglc -o $(BUILD)/glc-play $(BUILD)/play.o

$(BUILD)/play.o: $(SRC)/play.c $(HEADERS)
	$(CC) $(CFLAGS) -o $(BUILD)/play.o -c $(SRC)/play.c


# libglc
$(BUILD)/libglc.so.$(RELEASE): $(LIB_OBJS)
	$(LD) $(LDFLAGS) -Wl,-soname,libglc.so.$(VERSION) $(LIBS) -shared \
		$(LIB_OBJS) -o $(BUILD)/libglc.so.$(RELEASE)
	ln -sf libglc.so.$(RELEASE) $(BUILD)/libglc.so.$(VERSION)
	ln -sf libglc.so.$(RELEASE) $(BUILD)/libglc.so


# common objects
$(BUILD)/util.o: $(COMMON)/util.c $(HEADERS)
	$(CC) $(CFLAGS) -fPIC -o $(BUILD)/util.o -c $(COMMON)/util.c

$(BUILD)/thread.o: $(COMMON)/thread.c $(HEADERS)
	$(CC) $(CFLAGS) -fPIC -o $(BUILD)/thread.o -c $(COMMON)/thread.c


# stream processor objects
$(BUILD)/gl.o: $(STREAM)/gl.c $(HEADERS)
	$(CC) $(CFLAGS) -fPIC -o $(BUILD)/gl.o -c $(STREAM)/gl.c

$(BUILD)/pack.o: $(STREAM)/pack.c $(HEADERS)
	$(CC) $(CFLAGS) -fPIC -o $(BUILD)/pack.o -c $(STREAM)/pack.c $(USE_LZO)

$(BUILD)/file.o: $(STREAM)/file.c $(HEADERS)
	$(CC) $(CFLAGS) -fPIC -o $(BUILD)/file.o -c $(STREAM)/file.c

$(BUILD)/img.o: $(STREAM)/img.c $(HEADERS)
	$(CC) $(CFLAGS) -fPIC -o $(BUILD)/img.o -c $(STREAM)/img.c

$(BUILD)/scale.o: $(STREAM)/scale.c $(HEADERS)
	$(CC) $(CFLAGS) -fPIC -o $(BUILD)/scale.o -c $(STREAM)/scale.c

$(BUILD)/info.o: $(STREAM)/info.c $(HEADERS)
	$(CC) $(CFLAGS) -fPIC -o $(BUILD)/info.o -c $(STREAM)/info.c

$(BUILD)/audio.o: $(STREAM)/audio.c $(HEADERS)
	$(CC) $(CFLAGS) -fPIC -o $(BUILD)/audio.o -c $(STREAM)/audio.c

$(BUILD)/wav.o: $(STREAM)/wav.c $(HEADERS)
	$(CC) $(CFLAGS) -fPIC -o $(BUILD)/wav.o -c $(STREAM)/wav.c

$(BUILD)/demux.o: $(STREAM)/demux.c $(HEADERS)
	$(CC) $(CFLAGS) -fPIC -o $(BUILD)/demux.o -c $(STREAM)/demux.c

$(BUILD)/ycbcr.o: $(STREAM)/ycbcr.c $(HEADERS)
	$(CC) $(CFLAGS) -fPIC -o $(BUILD)/ycbcr.o -c $(STREAM)/ycbcr.c

$(BUILD)/yuv4mpeg.o: $(STREAM)/yuv4mpeg.c $(HEADERS)
	$(CC) $(CFLAGS) -fPIC -o $(BUILD)/yuv4mpeg.o -c $(STREAM)/yuv4mpeg.c

$(BUILD)/rgb.o: $(STREAM)/rgb.c $(HEADERS)
	$(CC) $(CFLAGS) -fPIC -o $(BUILD)/rgb.o -c $(STREAM)/rgb.c

$(LZO_OBJ): $(MINILZO)minilzo.c $(MINILZO)lzoconf.h $(MINILZO)lzodefs.h $(MINILZO)minilzo.h
	$(CC) $(CFLAGS) -fPIC -o $(LZO_OBJ) -c $(MINILZO)minilzo.c


install-scripts: $(SCRIPTS)/glc-encode $(SCRIPTS)/glc-capture
	install -Dm 0644 $(SCRIPTS)/glc-encode $(DESTDIR)/usr/share/glc/glc-encode
	install -Dm 0644 $(SCRIPTS)/glc-capture $(DESTDIR)/usr/share/glc/glc-capture
	install -Dm 0644 $(SCRIPTS)/glc-play-mplayer $(DESTDIR)/usr/share/glc/glc-play-mplayer

install-libs: $(BUILD)/libglc.so $(BUILD)/libglc-capture.so 
	install -Dm 0755 $(BUILD)/libglc.so.$(RELEASE) $(DESTDIR)/usr/$(MLIBDIR)/libglc.so.$(RELEASE)
	ln -sf libglc.so.$(RELEASE) $(DESTDIR)/usr/$(MLIBDIR)/libglc.so.$(VERSION)
	ln -sf libglc.so.$(RELEASE) $(DESTDIR)/usr/$(MLIBDIR)/libglc.so

	install -Dm 0755 $(BUILD)/libglc-capture.so.$(RELEASE) $(DESTDIR)/usr/$(MLIBDIR)/libglc-capture.so.$(RELEASE)
	ln -sf libglc-capture.so.$(RELEASE) $(DESTDIR)/usr/$(MLIBDIR)/libglc-capture.so.$(VERSION)
	ln -sf libglc-capture.so.$(RELEASE) $(DESTDIR)/usr/$(MLIBDIR)/libglc-capture.so

install: install-libs $(BUILD)/glc-play
	install -Dm 0755 $(BUILD)/glc-play $(DESTDIR)/usr/bin/glc-play

clean:
	rm -f $(LIB_OBJS) \
	      $(CAPT_OBJS) \
	      $(BUILD)/libglc.so.$(RELEASE) \
	      $(BUILD)/libglc.so.$(VERSION) \
	      $(BUILD)/libglc.so \
	      $(BUILD)/libglc-capture.so.$(RELEASE) \
	      $(BUILD)/libglc-capture.so.$(VERSION) \
	      $(BUILD)/libglc-capture.so \
	      $(BUILD)/play.o \
	      $(BUILD)/glc-play