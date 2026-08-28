PROG = Metrics
SRCS = main.c metrics.c collect.c store.c api.c
OBJS = $(SRCS:.c=.o)
PKGS = axparameter glib-2.0 gio-2.0

CFLAGS += -Wall -Wextra -Werror $(shell pkg-config --cflags $(PKGS))
LDFLAGS += -Wl,-z,noexecstack
LDLIBS += $(shell pkg-config --libs $(PKGS)) -lm

all: $(PROG)

$(PROG): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

clean:
	rm -f $(PROG) $(OBJS)
