MAKEFLAGS:=-Rr -I-

all: lib src hello.tsk
	@date; echo built $@

hello.tsk: 1.x 2.x 3.x 4.x 5.x 6.x 7.x 8.x 9.x 10.x 11.x 21.x 31.x 41.x 51.x 61.x 71.x 81.x 91.x 101.x
	@date; echo built $@

%.x:; @date; echo built $@

lib src:; $(MAKE) -C $@ -W 1.x -W 2.x 2>&1

.PHONY: lib src
makefile::;
