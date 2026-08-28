PROG = Metrics
SRCS = main.c metrics.c collect.c store.c api.c persist.c mqtt.c vapix.c
OBJS = $(SRCS:.c=.o)
PKGS = axparameter glib-2.0 gio-2.0 libcurl

CFLAGS += -Wall -Wextra -Werror $(shell pkg-config --cflags $(PKGS))
LDFLAGS += -Wl,-z,noexecstack
# paho is static because AXIS OS 13 does not ship it; OpenSSL is dynamic because
# both OS versions do, and the device has a real CA trust store.
LDLIBS += $(shell pkg-config --libs $(PKGS)) -lpaho-mqtt3as -lssl -lcrypto -lpthread -lm

all: $(PROG)

$(PROG): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

clean:
	rm -f $(PROG) $(OBJS)
