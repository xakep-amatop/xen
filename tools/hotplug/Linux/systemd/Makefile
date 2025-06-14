XEN_ROOT = $(CURDIR)/../../../..
include $(XEN_ROOT)/tools/Rules.mk

XEN_SYSTEMD_MODULES := xen.conf

XEN_SYSTEMD_MOUNT := proc-xen.mount

XEN_SYSTEMD_SLEEP_SCRIPTS := xen-watchdog-sleep.sh
XEN_SYSTEMD_SLEEP_DIR := $(XEN_SYSTEMD_DIR)/../system-sleep

XEN_SYSTEMD_SERVICE := xenstored.service
XEN_SYSTEMD_SERVICE += xenconsoled.service
XEN_SYSTEMD_SERVICE += xen-qemu-dom0-disk-backend.service
XEN_SYSTEMD_SERVICE += xendomains.service
XEN_SYSTEMD_SERVICE += xen-watchdog.service
XEN_SYSTEMD_SERVICE += xen-init-dom0.service
XEN_SYSTEMD_SERVICE += xendriverdomain.service

ALL_XEN_SYSTEMD :=	$(XEN_SYSTEMD_MODULES)  \
			$(XEN_SYSTEMD_MOUNT)	\
			$(XEN_SYSTEMD_SERVICE)	\
			$(XEN_SYSTEMD_SLEEP_SCRIPTS)

.PHONY: all
all:	$(ALL_XEN_SYSTEMD)

.PHONY: clean
clean:
	rm -f $(XEN_SYSTEMD_MODULES)

.PHONY: distclean
distclean: clean

.PHONY: install
install: $(ALL_XEN_SYSTEMD)
	$(INSTALL_DIR) $(DESTDIR)$(XEN_SYSTEMD_DIR)
	$(INSTALL_DIR) $(DESTDIR)$(XEN_SYSTEMD_MODULES_LOAD)
	$(INSTALL_DIR) $(DESTDIR)$(XEN_SYSTEMD_SLEEP_DIR)
	$(INSTALL_DATA) *.service $(DESTDIR)$(XEN_SYSTEMD_DIR)
	$(INSTALL_DATA) *.mount $(DESTDIR)$(XEN_SYSTEMD_DIR)
	$(INSTALL_DATA) *.conf $(DESTDIR)$(XEN_SYSTEMD_MODULES_LOAD)
	set -e; for i in $(XEN_SYSTEMD_SLEEP_SCRIPTS); \
	    do \
	    $(INSTALL_PROG) $$i $(DESTDIR)$(XEN_SYSTEMD_SLEEP_DIR); \
	done

.PHONY: uninstall
uninstall:
	rm -f $(DESTDIR)$(XEN_SYSTEMD_MODULES_LOAD)/*.conf
	rm -f $(DESTDIR)$(XEN_SYSTEMD_DIR)/*.mount
	rm -f $(DESTDIR)$(XEN_SYSTEMD_DIR)/*.service
	rm -f $(addprefix $(DESTDIR)$(XEN_SYSTEMD_SLEEP_DIR)/, $(XEN_SYSTEMD_SLEEP_SCRIPTS))

$(XEN_SYSTEMD_MODULES):
	rm -f $@.tmp
	for mod in $(LINUX_BACKEND_MODULES) ; do \
		echo $$mod ; \
	done > $@.tmp
	$(call move-if-changed,$@.tmp,$@)
