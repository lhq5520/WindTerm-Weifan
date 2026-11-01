# Fedora / RPM package. Build from a release tarball:
#   spectool -g -R packaging/rpm/windterm-weifan.spec && rpmbuild -ba packaging/rpm/windterm-weifan.spec
# or from a checkout with `fedpkg --release f41 local` after copying the spec.

Name:           windterm-weifan
Version:        0.1.0
Release:        1%{?dist}
Summary:        Terminal emulator and SSH, SFTP, Telnet and serial client
License:        Apache-2.0
URL:            https://github.com/lhq5520/WindTerm-Weifan
Source0:        %{url}/archive/v%{version}/WindTerm-Weifan-%{version}.tar.gz

BuildRequires:  cmake >= 3.21
BuildRequires:  ninja-build
BuildRequires:  gcc-c++
BuildRequires:  cmake(Qt6Core) >= 6.5
BuildRequires:  cmake(Qt6Widgets)
BuildRequires:  cmake(Qt6DBus)
BuildRequires:  cmake(Qt6Test)
BuildRequires:  cmake(Qt6LinguistTools)
BuildRequires:  pkgconfig(libssh2) >= 1.11
BuildRequires:  pkgconfig(openssl) >= 3.2
BuildRequires:  pkgconfig(zlib)
BuildRequires:  libappstream-glib
BuildRequires:  desktop-file-utils
# For the tests that run a private sshd / D-Bus daemon
BuildRequires:  openssh-server
BuildRequires:  openssh-clients
BuildRequires:  dbus-daemon

Requires:       hicolor-icon-theme

%description
A fast, cross-platform terminal emulator with a built-in client for remote sessions,
inspired by WindTerm: tabs and split panes, SSH (agent, keys, certificates, security keys,
jump hosts, proxies, port/agent/X11 forwarding), SFTP, Telnet and serial sessions, session
logging, triggers, an encrypted credential vault or the system keyring, Sixel and kitty
graphics.

%prep
%autosetup -n WindTerm-Weifan-%{version}

%build
%cmake -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DWT_ENABLE_SSH=ON \
    -DWT_CRYPTO_BACKEND=openssl \
    -DWT_BUILD_TESTS=ON
%cmake_build

%install
%cmake_install

%check
desktop-file-validate %{buildroot}%{_datadir}/applications/windterm-weifan.desktop
appstream-util validate-relax --nonet %{buildroot}%{_metainfodir}/dev.weifan.windterm.metainfo.xml
# The GUI tests need a display server; mock builds have none.
export QT_QPA_PLATFORM=offscreen
%ctest

%files
%license src/LICENSE
%doc README.md docs/USER_GUIDE.md docs/SECURITY.md
%{_bindir}/WindTerm-Weifan
%{_datadir}/applications/windterm-weifan.desktop
%{_datadir}/icons/hicolor/*/apps/windterm-weifan.png
%{_metainfodir}/dev.weifan.windterm.metainfo.xml

%changelog
* Sun Nov 01 2026 WindTerm-Weifan contributors - 0.1.0-1
- First package
