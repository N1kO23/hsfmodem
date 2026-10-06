IMPORTED_ARCH ?= $(shell uname -m)

include $(TOP)/modules/imported/makeflags-$(IMPORTED_ARCH).mak
