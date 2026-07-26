# Makefile for mrepod and mrepo client

.SILENT: all install clean

CC      ?= gcc
CFLAGS  += -Wall -Wextra -O2 -g -m64
CFLAGS  += -pthread
# Link with jansson for JSON parsing
# sudo ln -s /usr/lib/x86_64-linux-gnu/libjansson.so.4 /usr/lib/x86_64-linux-gnu/libjansson.so
LDFLAGS += -L/usr/lib/x86_64-linux-gnu -l:libjansson.so.4
LDFLAGS += -Wl,-rpath,/usr/lib/x86_64-linux-gnu
LDFLAGS += -pthread

VERSION := 1.0.$(shell date +%Y%m%d)
BUILDDIR := build
BINDIR  := $(BUILDDIR)/usr/bin
SBINDIR := $(BUILDDIR)/usr/sbin

INCLUDES = -Iinclude

DAEMON_SRCS = \
    daemon/main.c daemon/broker.c daemon/mount_ops.c \
    daemon/umount_ops.c daemon/post_mount.c \
    daemon/namespace.c daemon/mountinfo.c \
    daemon/locks.c daemon/fault.c daemon/util.c \
    daemon/logger.c daemon/promote.c daemon/ownership.c \
    daemon/daemon_config.c daemon/monitor.c

CLIENT_SRCS = \
	client/main.c \
	client/cli_util.c \
	client/mount_util.c

TEST_SRCS = test/test_unit.c daemon/mountinfo.c \
    daemon/util.c daemon/daemon_config.c

DAEMON = daemon_mrepo
CLIENT = mrepo
TEST   = test_unit

all: $(DAEMON) $(CLIENT)

test: $(TEST)
	./$(TEST)

$(TEST): $(TEST_SRCS)
	$(CC) $(CFLAGS) $(INCLUDES) $^ -o $@ $(LDFLAGS)

$(DAEMON): $(DAEMON_SRCS)
	$(CC) $(CFLAGS) $(INCLUDES) $^ -o $@ $(LDFLAGS)

$(CLIENT): $(CLIENT_SRCS)
	$(CC) $(CFLAGS) $(INCLUDES) $^ -o $@

install: all
	mkdir -p $(BUILDDIR)/usr $(BUILDDIR)/etc/mrepod $(BUILDDIR)/etc/systemd/system $(BUILDDIR)/etc/init.d $(BUILDDIR)/DEBIAN
	install -d $(SBINDIR) $(BINDIR)
	install -m 0755 $(DAEMON) $(SBINDIR)
	install -m 0755 $(CLIENT) $(BINDIR)
	install -m 0644 etc/mrepod/mrepod.conf $(BUILDDIR)/etc/mrepod/mrepod.conf
	install -m 0644 etc/systemd/system/mrepod.service $(BUILDDIR)/etc/systemd/system/mrepod.service
	install -m 0755 etc/init.d/mrepod $(BUILDDIR)/etc/init.d/mrepod
	install -m 0644 etc/DEBIAN/control $(BUILDDIR)/DEBIAN/control
	# Update version in control file
	sed -i 's/^Version: .*/Version: $(VERSION)/' $(BUILDDIR)/DEBIAN/control
	# Create deb package with version in filename
	dpkg-deb --build $(BUILDDIR) mrepod_$(VERSION)_amd64.deb



clean:
	rm -f $(DAEMON) $(CLIENT) $(TEST)
	rm -f mrepod_*.deb
	rm -rf $(BUILDDIR)

.PHONY: all install clean test
