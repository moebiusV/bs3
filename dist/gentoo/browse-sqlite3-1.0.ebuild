# Copyright 2026 Gentoo Authors
# Distributed under the terms of the BSD-2-Clause license

EAPI=8

DESCRIPTION="Interactive terminal browser for SQLite databases"
HOMEPAGE="https://github.com/moebiusV/bs3"
SRC_URI="https://github.com/moebiusV/bs3/releases/download/v${PV}/${P}.tar.gz"

LICENSE="BSD-2"
SLOT="0"
KEYWORDS="~amd64 ~x86"

BDEPEND="virtual/pkgconfig"
RDEPEND="
	sys-libs/ncurses:=
	dev-db/sqlite:3
"

src_configure() {
	./configure --prefix=/usr --with-system-sqlite
}

src_compile() {
	emake
}

src_test() {
	emake check
}

src_install() {
	emake DESTDIR="${D}" install
	dodoc README.md NEWS AUTHORS
}
