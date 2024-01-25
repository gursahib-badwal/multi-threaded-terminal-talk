
all: test

test: s-talk.c list.o
	gcc s-talk.c list.o -o test -pthread

clean:
	rm -f test
	