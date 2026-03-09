PORTNAME=	browse-sqlite3
DISTVERSION=	1.0
CATEGORIES=	databases
MASTER_SITES=	https://github.com/moebiusV/bs3/releases/download/v${DISTVERSION}/

MAINTAINER=	MAINTAINER_EMAIL
COMMENT=	Interactive terminal browser for SQLite databases
WWW=		https://github.com/moebiusV/bs3

LICENSE=	BSD2CLAUSE
LICENSE_FILE=	${WRKSRC}/COPYING

USES=		ncurses pkgconfig sqlite:3

do-build:
	cd ${WRKSRC} && ./configure --prefix=${PREFIX} --with-system-sqlite && ${MAKE}

do-install:
	cd ${WRKSRC} && ${MAKE} DESTDIR=${STAGEDIR} install

.include <bsd.port.mk>
