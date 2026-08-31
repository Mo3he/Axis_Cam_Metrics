PROG = Metrics
SRCS = main.c metrics.c collect.c store.c api.c persist.c mqtt.c vapix.c alerts.c selection.c influx.c procs.c
OBJS = $(SRCS:.c=.o)
PKGS = axparameter axevent glib-2.0 gio-2.0 libcurl

# The manifest is the one place the version is written, so the binary reads it
# from there rather than carrying a copy that can drift.
APP_VERSION := $(shell sed -n 's/.*"version"[[:space:]]*:[[:space:]]*"\([0-9][^"]*\)".*/\1/p' manifest.json | head -1)

CFLAGS += -Wall -Wextra -Werror -DAPP_VERSION=\"$(APP_VERSION)\" $(shell pkg-config --cflags $(PKGS))
LDFLAGS += -Wl,-z,noexecstack
# paho is static because AXIS OS 13 does not ship it; OpenSSL is dynamic because
# both OS versions do, and the device has a real CA trust store.
LDLIBS += $(shell pkg-config --libs $(PKGS)) -lpaho-mqtt3as -lssl -lcrypto -lpthread -lm

all: $(PROG)

$(PROG): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

clean:
	rm -f $(PROG) $(OBJS)
