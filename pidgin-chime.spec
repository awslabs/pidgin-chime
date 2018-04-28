#%%global commit f4fa0044c2d5f68a056447c990bc1e5c2cdf4ecf
#%%global gittag v%{version}
#%%global shortcommit %(c=%{commit}; echo ${c:0:7})

%bcond_without evolution # with

Name:           pidgin-chime
Summary:        libpurple / Pidgin protocol plugin for Amazon Chime
Version:        0.02
Release:        0%{?dist}

Group:          Applications/Communications
License:        LGPLv2
URL:            https://github.com/awslabs/pidgin-chime
Source0:        %{name}-%{version}.tar.gz
#Source0:        https://github.com/awslabs/%{name}/archive/%{commit}/%{name}-%{shortcommit}.tar.gz
#Source0:        https://github.com/awslabs/%{name}/archive/%{gittag}/%{name}-%{version}.tar.gz

BuildRequires:  pkgconfig(purple) >= 2.4.0
BuildRequires:  pkgconfig(gnutls) >= 3.2.0
BuildRequires:  pkgconfig(farstream-0.2)
BuildRequires:  pkgconfig(gstreamer-1.0)
BuildRequires:  pkgconfig(gstreamer-app-1.0)
BuildRequires:  pkgconfig(gstreamer-rtp-1.0)
BuildRequires:  pkgconfig(opus)
BuildRequires:  pkgconfig(libprotobuf-c)
BuildRequires:  pkgconfig(json-glib-1.0)
BuildRequires:  pkgconfig(libxml-2.0)
BuildRequires:  pkgconfig(libsoup-2.4) >= 2.50
%if %{with evolution}
BuildRequires:  pkgconfig(evolution-calendar-3.0)
BuildRequires:  pkgconfig(evolution-shell-3.0)
BuildRequires:  pkgconfig(evolution-data-server-1.2)
BuildRequires:  pkgconfig(libebook-1.2)
BuildRequires:  pkgconfig(libecal-1.2)
%endif # with evolution
BuildRequires:  autoconf
BuildRequires:  automake
BuildRequires:  gcc
BuildRequires:  gettext
BuildRequires:  libtool

%description
A libpurple / Pidgin protocol plugin for Amazon Chime.

%if %{with evolution}
%package -n evolution-chime
Summary:  Evolution plugin for Amazon Chime
Group:    Applications/Productivity
Requires: pidgin-chime = %{version}-%{release}

%description -n evolution-chime
A plugin for Evolution that allows you to create meetings in Amazon Chime.
%endif # with evolution

%prep
%setup -q
#%%setup -q -n %{name}-%{commit}
#%%setup -q -n %{name}-%{gittag}
#autoreconf -f -i -Wnone

%build
%configure \
%if %{without evolution}
  --without-evolution \
%endif # without evolution
  ;
make %{?_smp_mflags}

%install
%make_install
find %{buildroot} -type f -name "*.la" -delete -print
%find_lang %{name}

%check
make %{?_smp_mflags} check

%files -f %{name}.lang
%defattr(-,root,root,-)
%doc README TODO
%license LICENSE
%{_datadir}/pixmaps/pidgin/protocols/*/chime*
%{_libdir}/farstream-0.2/libapp-transmitter.so
%{_libdir}/gstreamer-1.0/libgstchime.so
%{_libdir}/purple-2/libchimeprpl.so
%{_libdir}/pidgin/chimeseen.so

%if %{with evolution}
%files -n evolution-chime
%defattr(-,root,root,-)
%{_libdir}/evolution/modules/module-event-from-template.so
%endif # with evolution

%changelog
* Fri Apr 13 2018 Andrew Jorgensen <ajorgens@amazon.com> - 0.02-0
- Initial packaging.
