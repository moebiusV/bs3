#!/bin/sh
# Download the SQLite amalgamation into lib/
set -e

VERSION=3490100
YEAR=2025
URL="https://www.sqlite.org/${YEAR}/sqlite-amalgamation-${VERSION}.zip"

if [ -f lib/sqlite3.c ] && [ -f lib/sqlite3.h ]; then
    echo "SQLite amalgamation already present in lib/"
    exit 0
fi

echo "Downloading SQLite amalgamation..."
mkdir -p lib

if command -v curl >/dev/null 2>&1; then
    curl -L -o /tmp/sqlite-amalgamation.zip "$URL"
elif command -v wget >/dev/null 2>&1; then
    wget -O /tmp/sqlite-amalgamation.zip "$URL"
else
    echo "error: neither curl nor wget found" >&2
    exit 1
fi

cd /tmp
unzip -o sqlite-amalgamation.zip
cd -
cp /tmp/sqlite-amalgamation-${VERSION}/sqlite3.c lib/
cp /tmp/sqlite-amalgamation-${VERSION}/sqlite3.h lib/
rm -rf /tmp/sqlite-amalgamation-${VERSION} /tmp/sqlite-amalgamation.zip

echo "SQLite amalgamation installed to lib/"
