# $NetBSD$

DISTNAME=	browse-sqlite3-1.0
CATEGORIES=	databases
MASTER_SITES=	https://github.com/moebiusV/bs3/releases/download/v1.0/

MAINTAINER=	MAINTAINER_EMAIL
HOMEPAGE=	https://github.com/moebiusV/bs3
COMMENT=	Interactive terminal browser for SQLite databases
LICENSE=	2-clause-bsd

DEPENDS+=	ncurses-[0-9]*:../../devel/ncurses
DEPENDS+=	sqlite3-[0-9]*:../../databases/sqlite3

do-build:
	cd ${WRKSRC} && ./configure --prefix=${PREFIX} --with-system-sqlite && ${MAKE}

do-install:
	cd ${WRKSRC} && ${MAKE} DESTDIR=${DESTDIR} install

.include "../../mk/bsd.pkg.mk"
