{ lib, stdenv, ncurses, pkg-config, fetchFromGitHub }:

stdenv.mkDerivation rec {
  pname = "browse-sqlite3";
  version = "1.0";

  src = fetchFromGitHub {
    owner = "moebiusV";
    repo = "bs3";
    rev = "v${version}";
    sha256 = lib.fakeSha256;
  };

  buildInputs = [ ncurses ];
  nativeBuildInputs = [ pkg-config ];

  configurePhase = ''
    ./configure --prefix=$out
  '';

  buildPhase = "make";

  checkPhase = "make check";
  doCheck = true;

  installPhase = "make install";

  meta = with lib; {
    description = "Interactive terminal browser for SQLite databases";
    homepage = "https://github.com/moebiusV/bs3";
    license = licenses.bsd2;
    maintainers = [ ];
    platforms = platforms.unix;
  };
}
