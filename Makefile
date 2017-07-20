
ROOT :=			$(PWD)

CFLAGS =		-I$(ROOT)/include \
			-I$(ROOT)/deps/list/include \
			-std=gnu99 \
			-gdwarf-2 \
			-m64 \
			-Wall -Wextra -Werror \
			-Wno-unused-parameter \
			-Wno-unused-function

LIBS =			-lumem

IOSTOMP_OBJS =		iostomp.o \
			list.o

PROGS =			iostomp

.PHONY: all
all: $(PROGS)

obj:
	mkdir -p $@

obj/%.o: src/%.c | obj
	gcc -c $(CFLAGS) -o $@ $^

obj/list.o:
	cd deps/list && \
	    gmake CFLAGS=-m64 BUILD_DIR=$(ROOT)/obj $(ROOT)/obj/list.o

iostomp: $(IOSTOMP_OBJS:%=obj/%)
	gcc $(CFLAGS) -o $@ $^ $(LIBS)

.PHONY: clean
clean:
	-rm -f obj/*.o
	-rm -f $(PROGS)
