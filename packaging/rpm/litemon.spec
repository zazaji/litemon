Name:           litemon
Version:        2.0.0
Release:        1%{?dist}
Summary:        Lightweight local historical system monitor

License:        MIT
URL:            https://github.com/litemon/litemon
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  gcc-c++
BuildRequires:  cmake
BuildRequires:  ninja-build
BuildRequires:  pkg-config
BuildRequires:  qt6-qtbase-devel
BuildRequires:  qt6-qttools-devel

%description
LiteMon is a local-first Qt 6 monitor for Linux desktops. It records
CPU, memory, storage, network, battery, Intel GPU and NVIDIA GPU
history into a bounded SQLite database, with compact charts and a
sidebar-driven UI.

%prep
%autosetup -n %{name}-%{version}

%build
%cmake -G Ninja -DCMAKE_BUILD_TYPE=Release
%cmake_build

%install
%cmake_install

%files
%license LICENSE
%doc README.md
%{_bindir}/litemon
%{_bindir}/litemon-collector
%{_datadir}/applications/io.github.litemon.LiteMon.desktop
%{_datadir}/metainfo/io.github.litemon.LiteMon.metainfo.xml
%{_datadir}/icons/hicolor/scalable/apps/io.github.litemon.LiteMon.svg
%{_unitdir}/litemon-collector.service

%changelog
* Thu Sep 20 2026 zazaji <zazaji@sina.com> - 2.0.0-1
- Initial RPM release of LiteMon 2.0.0.