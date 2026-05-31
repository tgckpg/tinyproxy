SCDOC ?= scdoc

MAN_SCD := docs/tinyproxy.1.scd \
           docs/tinyproxy.conf.5.scd \
           docs/tinyproxy.7.scd

MAN := $(MAN_SCD:.scd=)

ifeq ($(shell id -u),0)
MAN_DIST ?=
else
MAN_DIST ?= $(PROJECT_ROOT)/dist
endif

.PHONY: man clean-man install-man uninstall-man

man: $(MAN)

docs/%: docs/%.scd
	$(SCDOC) < $< > $@

install-man: man
	mkdir -p $(MAN_DIST)/usr/share/man/man1
	mkdir -p $(MAN_DIST)/usr/share/man/man5
	mkdir -p $(MAN_DIST)/usr/share/man/man7
	install -Dm644 docs/tinyproxy.1 \
		$(MAN_DIST)/usr/share/man/man1/tinyproxy.1
	install -Dm644 docs/tinyproxy.conf.5 \
		$(MAN_DIST)/usr/share/man/man5/tinyproxy.conf.5
	install -Dm644 docs/tinyproxy.7 \
		$(MAN_DIST)/usr/share/man/man7/tinyproxy.7

uninstall-man:
	rm -f $(MAN_DIST)/share/man/man1/tinyproxy.1
	rm -f $(MAN_DIST)/share/man/man5/tinyproxy.conf.5
	rm -f $(MAN_DIST)/share/man/man7/tinyproxy.7

clean-man:
	rm -f $(MAN)
