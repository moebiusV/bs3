# Copyright 2026 Gentoo Authors
# Distributed under the terms of the BSD-2-Clause license

EAPI=8

PYTHON_COMPAT=( python3_{10..13} )
inherit python-single-r1

DESCRIPTION="Interactive terminal browser for SQLite databases"
HOMEPAGE="https://github.com/moebiusV/bs3"
SRC_URI="https://github.com/moebiusV/bs3/releases/download/v${PV}/${P}.tar.gz"

LICENSE="BSD-2"
SLOT="0"
KEYWORDS="~amd64 ~x86"
REQUIRED_USE="${PYTHON_REQUIRED_USE}"

RDEPEND="${PYTHON_DEPS}"
BDEPEND="${PYTHON_DEPS}"

src_configure() {
	./configure --prefix=/usr --with-python="${PYTHON}"
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
