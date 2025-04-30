############################################################################
# Copyright Nash!Com, Daniel Nashed 2023-2025 - APACHE 2.0 see LICENSE
############################################################################


CC=gcc
CFLAGS=-g -Wall -c -fPIC -pedantic
LIBS=

PROGRAM=nshborg
TARGET?=$(PROGRAM)

all: $(TARGET)

$(TARGET): $(PROGRAM).o
	$(CC) $(PROGRAM).o $(LIBS) -o $(TARGET) $(SPECIAL_LINK_OPTIONS)

$(PROGRAM).o: $(PROGRAM).cpp
	$(CC)  $(CFLAGS) $(PROGRAM).cpp -DLINUX -DUNIX -O1 #-D_FORTIFY_SOURCE=3

clean:
	rm -f $(TARGET) *.o

test: all
	$(TARGET)

install: all
	sudo cp $(TARGET) /usr/bin/$(TARGET)
	$(MAKE) clean

uninstall:
	sudo rm -f /usr/bin/$(TARGET)
	$(MAKE) clean

publish: all
	mkdir -p /local/software/nashcom.de/linux-bin
	cp -f ./$(TARGET) /local/software/nashcom.de/linux-bin

container_build: 
	docker run --rm -v .:/src -w /src -u 0 nashcom/alpine_build_env:latest sh -c 'SPECIAL_LINK_OPTIONS=-static make'
