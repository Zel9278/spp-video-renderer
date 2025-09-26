# Makefile for MIDI Parser

CC = gcc
CFLAGS = -Wall -Wextra -std=c99 -O2
LDFLAGS = 

# ソースファイル
SOURCES = midi_parser.c
EXAMPLE_SOURCES = example.c midi_parser.c

# オブジェクトファイル
OBJECTS = $(SOURCES:.c=.o)
EXAMPLE_OBJECTS = $(EXAMPLE_SOURCES:.c=.o)

# ターゲット
LIBRARY = libmidi_parser.a
EXAMPLE = midi_example

.PHONY: all clean example library

all: library example

# ライブラリのビルド
library: $(LIBRARY)

$(LIBRARY): $(OBJECTS)
	ar rcs $@ $^

# サンプルプログラムのビルド
example: $(EXAMPLE)

$(EXAMPLE): $(EXAMPLE_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

# オブジェクトファイルのビルドルール
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# 依存関係
midi_parser.o: midi_parser.c midi_parser.h
example.o: example.c midi_parser.h

# クリーンアップ
clean:
	rm -f $(OBJECTS) $(EXAMPLE_OBJECTS) $(LIBRARY) $(EXAMPLE)

# Windows用クリーンアップ
clean-win:
	del /Q *.o *.a *.exe 2>nul || true

# インストール（Unix系）
install: $(LIBRARY)
	install -d $(DESTDIR)/usr/local/lib
	install -d $(DESTDIR)/usr/local/include
	install -m 644 $(LIBRARY) $(DESTDIR)/usr/local/lib/
	install -m 644 midi_parser.h $(DESTDIR)/usr/local/include/

# アンインストール（Unix系）
uninstall:
	rm -f $(DESTDIR)/usr/local/lib/$(LIBRARY)
	rm -f $(DESTDIR)/usr/local/include/midi_parser.h

# デバッグビルド
debug: CFLAGS += -g -DDEBUG
debug: all

# リリースビルド
release: CFLAGS += -DNDEBUG
release: all