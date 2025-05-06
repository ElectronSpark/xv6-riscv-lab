TOOLPREFIX :=
EXTRAFLAGS :=		# add "-Wl,-verbose" to print linker script
CFLAGS := -ggdb -Wall
LDFLAGS := -ggdb
VALGRINDFLAGS :=
CC := $(TOOLPREFIX)-gcc
VALGRIND :=valgrind

# LDSCRIPT = linkscript.ld
# LDFLAGS += -T$(LDSCRIPT)
CFLAGS += $(LDFLAGS)

PROGRAMS =	list_test.out

SRC =					\
	common.h			\
	../kernel/list.h	\
	../kernel/types.h

%.out: %.c $(SRC) $(LDSCRIPT)
	$(CC) $(CFLAGS) $(EXTRAFLAGS) -o $@ $<

all: $(PROGRAMS)

test: all
	@for each in $(PROGRAMS); do		\
		echo "running test $$each";		\
		./$$each;						\
	done

valgrind: all
	@for each in $(PROGRAMS); do				\
		echo "running valgrind test on $$each";	\
		$(VALGRIND) $(VALGRINDFLAGS) ./$$each;	\
	done

valgrind-full: all
	@for each in $(PROGRAMS); do									\
		echo "running valgrind full test on $$each";				\
		$(VALGRIND) $(VALGRINDFLAGS) --leak-check=full ./$$each;	\
	done

clean:
	rm $(PROGRAMS)

.PHONY: test valgrind valgrind-full clean