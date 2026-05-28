SCDOC ?= scdoc

MAN_SCD := docs/tinyproxy.1.scd \
           docs/tinyproxy.conf.5.scd

MAN := $(MAN_SCD:.scd=)
MAN_DIST := $(PROJECT_ROOT)/dist

.PHONY: man clean-man install-man uninstall-man

man: $(MAN)

docs/%: docs/%.scd
	$(SCDOC) < $< > $@

install-man: man
	mkdir -p $(MAN_DIST)/usr/share/man/man1
	mkdir -p $(MAN_DIST)/usr/share/man/man5
	install -Dm644 docs/tinyproxy.1 \
		$(MAN_DIST)/usr/share/man/man1/tinyproxy.1
	install -Dm644 docs/tinyproxy.conf.5 \
		$(MAN_DIST)/usr/share/man/man5/tinyproxy.conf.5

uninstall-man:
	rm -f $(DESTDIR)$(PREFIX)/share/man/man1/tinyproxy.1
	rm -f $(DESTDIR)$(PREFIX)/share/man/man5/tinyproxy.conf.5

clean-man:
	rm -f $(MAN)
