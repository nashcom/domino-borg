
############################################################################
# Copyright Nash!Com, Daniel Nashed 2023 - APACHE 2.0 see LICENSE
############################################################################


CC=gcc
CFLAGS= -g -Wall -c -fPIC -pedantic
LIBS=

PROGRAM=nshborg

all: $(PROGRAM)

$(PROGRAM): $(PROGRAM).o
	$(CC) $(PROGRAM).o $(LIBS) -o $(PROGRAM)

$(PROGRAM).o: $(PROGRAM).cpp
	$(CC)  $(CFLAGS) $(PROGRAM).cpp -DLINUX -DUNIX -O1 #-D_FORTIFY_SOURCE=3

clean:
	rm -f $(PROGRAM) *.o

test: all
	$(PROGRAM)

install: all
	sudo cp $(PROGRAM) /usr/bin/$(PROGRAM)
	$(MAKE) clean

uninstall:
	sudo rm -f /usr/bin/$(PROGRAM)
	$(MAKE) clean

publish: all
	mkdir -p /local/software/nashcom.de/linux-bin
	cp -f ./$(PROGRAM) /local/software/nashcom.de/linux-bin

