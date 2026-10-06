# SPDX-License-Identifier: GPL-2.0-or-later
# Out-of-tree build of snd-hda-codec-hsfmodem.
#   make [KDIR=/lib/modules/<version>/build]
#   make install    (as root; signs the module if the kernel requires it)
KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

install:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules_install

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

.PHONY: all install clean
