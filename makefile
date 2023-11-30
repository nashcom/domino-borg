
############################################################################
# Copyright Nash!Com, Daniel Nashed 2023 - APACHE 2.0 see LICENSE
############################################################################


CC=gcc
CFLAGS= -g -Wall -c -fPIC -fpermissive
LIBS=

PROGRAM=nshborg

all: $(PROGRAM)

$(PROGRAM): $(PROGRAM).o
	$(CC) -o $(PROGRAM) $(PROGRAM).o $(LIBS) -o $(PROGRAM)

$(PROGRAM).o: $(PROGRAM).cpp
	$(CC)  $(CFLAGS) $(PROGRAM).cpp -DLINUX -DUNIX

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
