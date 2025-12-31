PKGNAME=pafari
PKGVER=26.1

.PHONY: build clean install

build:
	makepkg -sr --skipinteg

clean:
	rm -rf pkg/
	rm -rf ${PKGNAME}-${PKGVER}*
	rm -rf *.zst
	rm -rf *.sig
	rm -rf build/

install:
	sudo pacman -U ${PKGNAME}-${PKGVER}-*.pkg.tar.zst
