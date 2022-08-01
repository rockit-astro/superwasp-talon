Name:      superwasp-talon
Version:   20220801
Release:   0
Summary:   Stripped down Talon installation for the SuperWASP telescope
License:   proprietary
Group:     Unspecified
BuildArch: x86_64
Requires: xorg-x11-fonts-misc

%description

%build

cmake %{_sourcedir}
make
make DESTDIR=%{buildroot} install

%files
%defattr(0644,root,root,-)
/etc/profile.d/talon.sh

%defattr(0755,root,root,-)
/usr/local/telescope

%defattr(0777,root,root,-)
/usr/local/telescope/archive/config
/usr/local/telescope/archive/logs
/usr/local/telescope/comm

%changelog
