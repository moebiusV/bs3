Name:           browse-sqlite3
Version:        1.0
Release:        1%{?dist}
Summary:        Interactive terminal browser for SQLite databases
License:        BSD-2-Clause
URL:            https://github.com/moebiusV/bs3
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  gcc
BuildRequires:  ncurses-devel
BuildRequires:  sqlite-devel
BuildRequires:  pkgconfig

%description
browse-sqlite3 is a full-screen TUI browser for SQLite databases with
vi-style navigation, readline-style inline editing, multi-column sort,
structured search, and a Turbo Pascal-inspired CGA color scheme.

%prep
%setup -q

%build
./configure --prefix=%{_prefix} --bindir=%{_bindir} --mandir=%{_mandir} --with-system-sqlite
make %{?_smp_mflags}

%check
make check

%install
make DESTDIR=%{buildroot} install

%files
%license COPYING
%doc README.md NEWS AUTHORS
%{_bindir}/browse-sqlite3
%{_bindir}/bs3
%{_mandir}/man1/browse-sqlite3.1*
%{_mandir}/man1/bs3.1*

%changelog
