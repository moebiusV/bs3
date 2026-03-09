COMMENT =	interactive terminal browser for SQLite databases

DISTNAME =	browse-sqlite3-1.0

CATEGORIES =	databases

HOMEPAGE =	https://github.com/moebiusV/bs3

MAINTAINER =	MAINTAINER_EMAIL

# BSD-2-Clause
PERMIT_PACKAGE =	Yes

MASTER_SITES =	https://github.com/moebiusV/bs3/releases/download/v1.0/

WANTLIB =	c curses sqlite3

LIB_DEPENDS =	databases/sqlite3

do-build:
	cd ${WRKSRC} && ./configure --prefix=${PREFIX} --with-system-sqlite && ${MAKE}

do-install:
	cd ${WRKSRC} && ${MAKE} DESTDIR=${WRKINST} install

.include <bsd.port.mk>
