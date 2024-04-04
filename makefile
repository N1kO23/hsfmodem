TOP= .

include $(TOP)/config.mak
-include $(TOP)/modules/imported/makeflags.mak

ifneq ($(IMPORTED_TARGET),TARGET_DGC_LINUX)
DEBUG_TOOLS+= diag
ifeq ($(IMPORTED_DMP_SUPPORT),)
TARDIST_EXCLUDES+= *dmp*
endif
ifeq ($(IMPORTED_SCR_SUPPORT),yes)
DEBUG_TOOLS+= diag/qtmodemon
else
TARDIST_EXCLUDES+= scr.* scr_* qtmodemon
endif
#endif
endif

SYMLINK_SUPPORT := $(shell if ln -sf symlinktest .lntst.$$$$ > /dev/null 2>&1; then echo yes; else echo no; fi; rm -f .lntst.*)


ifeq ($(IMPORTED_TARGET),TARGET_DGC_LINUX)
SUBDIRS = scripts modules
else
SUBDIRS = nvm scripts modules $(DEBUG_TOOLS)
endif

.PHONY: default
default:
    @echo "Use:"
    @echo "    \"make install\" to install this software"
    @echo "    \"make uninstall\" to uninstall this software"
#   @echo "    \"make modules\" to recompile the kernel modules (normally done by $(CNXTTARGET)config)"
    @echo "    \"make clean\" to remove objects and other derived files"
    @echo "    \"$(CNXTTARGET)config\" (after installation) to setup your modem"
    @echo "    \"make rpmprecomp\" to build a pre-compiled RPM package for the `uname -r` kernel"
    @echo "    \"make debprecomp\" to build a pre-compiled DEB package for the `uname -r` kernel"
    @echo ""
    @false

all: scripts $(DEBUG_TOOLS)

modules/imported:
    @[ -d modules/imported ] || (echo "\"modules/imported\" directory missing!"; exit 1)

$(SUBDIRS) install:: modules/imported

.PHONY: $(SUBDIRS)
$(SUBDIRS)::
    $(MAKE) -C $@ all

.PHONY: uninstall
uninstall::
    if [ -x $(CNXTSBINDIR)/$(CNXTTARGET)config ]; then \
        $(CNXTSBINDIR)/$(CNXTTARGET)config -remove; \
    else \
        true; \
    fi

.PHONY: symlink-support
symlink-support:
ifeq ($(SYMLINK_SUPPORT),no)
    @echo "ERROR: The partition holding the '`pwd`' directory doesn't have a file system"
    @echo "which supports symbolic links. Please move the source tree to a partition with a"
    @echo "native Linux file system and try again to install the software."
    @false
endif

.PHONY: clean install
clean install uninstall:: symlink-support
    @for subdir in $(SUBDIRS); do \
        $(MAKE) -C $$subdir $@ || exit $$?; \
    done

install:: $(CNXTLIBDIR) $(CNXTETCDIR) LICENSE $(CNXTETCDIR)/log
    $(INSTALL) -m 444 LICENSE $(CNXTLIBDIR)
    @(echo -n "TAR " ; pwd ) > "$(CNXTETCDIR)/package"
    @echo ""
    @echo "To complete the installation and configuration of your modem,"
    @echo "please run \"$(CNXTTARGET)config\" (or \"$(CNXTSBINDIR)/$(CNXTTARGET)config\")"

$(CNXTLIBDIR) $(CNXTETCDIR):
    $(MKDIR) -p $@

uninstall::
    rm -f $(CNXTLIBDIR)/LICENSE
    rm -f $(CNXTETCDIR)/package

# Only install docs if present
PDFDOC= $(shell f=100498D_RM_HxF_Released.pdf; test -f $$f && echo $$f)

TARDIST_EXCLUDES+= OLD *,v $(CNXTTARGET)modem*.tar.gz binaries/* GPL/hda-* $(PDFDOC)
ifneq ($(IMPORTED_TARGET),TARGET_HCF_USB_LINUX)
ifneq ($(IMPORTED_TARGET),TARGET_HSF_LINUX)
ifneq ($(IMPORTED_TARGET),TARGET_DGC_LINUX)
TARDIST_EXCLUDES+= *usb*
endif
endif
endif
ifneq ($(IMPORTED_TARGET),TARGET_HCF_PCI_LINUX)
ifneq ($(IMPORTED_TARGET),TARGET_HSF_LINUX)
TARDIST_EXCLUDES+= *pci*
endif
endif

TARPKG=$(CNXTTARGET)modem-$(CNXTLINUXVERSION).tar.gz

.PHONY: tardist
tardist: clean
    $(MAKE) $(TARPKG)

CNXTTEMPDIST:=/tmp/$(CNXTTARGET)dist-$(shell echo $$$$)/$(CNXTTARGET)modem-$(CNXTLINUXVERSION)

$(TARPKG): $(CNXTTARGET)modem.spec
    rm -rf $(CNXTTEMPDIST)
    $(MKDIR) -p $(CNXTTEMPDIST)
    [ -d modules/binaries ] || $(MKDIR) modules/binaries
    find . -depth -print | grep -v '^\.\/packages\/' | cpio -pdmu $(CNXTTEMPDIST)
    (cd $(CNXTTEMPDIST)/.. && tar $(patsubst %, --exclude '%', $(TARDIST_EXCLUDES)) -cf - $(CNXTTARGET)modem-$(CNXTLINUXVERSION)) | gzip > $@
    rm -rf $(dir $(CNXTTEMPDIST))

# Test if our rpm supports --define and --eval options. Early versions didn't
RPMOPTDEFINE=$(shell rpm --define 'test test' >/dev/null 2>&1 && echo yes)
RPMOPTEVAL=$(shell rpm --eval 'test' >/dev/null 2>&1 && echo yes)

ifeq ($(RPMOPTDEFINE)$(RPMOPTEVAL),yesyes)
RPMDIRDEF = --define "_rpmdir `pwd`"
RPMDIR = -D '_rpmdir `pwd`'
else
RPMDIRDEF =
RPMDIR =
endif

CNXTMODNAME=$(shell echo $(CNXTTARGET) | sed -e 's/^lt//')

.PHONY: rpmprecomp
rpmprecomp: $(CNXTTARGET)modem.spec
    $(MAKE) $(RPMDIRDEF) -f makefile.rpm rpmprecomp

.PHONY: debprecomp
debprecomp: $(CNXTTARGET)modem.spec
    $(MAKE) $(RPMDIR) -f makefile.deb debprecomp

CNXTMODDEVDIR := $(CNXTMODNAME)_$(CNXTLINUXVERSION)_$(shell uname -m)

.PHONY: module-precomp-clean
module-precomp-clean:
    [ ! -d $(CNXTMODDEVDIR) ] || rm -rf $(CNXTMODDEVDIR)

.PHONY: debprecomp-clean
debprecomp-clean: module-precomp-clean
    $(MAKE) $(RPMDIR) -f makefile.deb debprecomp-clean

.PHONY: rpmprecomp-clean
rpmprecomp-clean: module-precomp-clean
    $(MAKE) $(RPMDIRDEF) -f makefile.rpm rpmprecomp-clean