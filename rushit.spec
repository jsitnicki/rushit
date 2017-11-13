%define rushit_version %(cat VERSION)

Name:		rushit
Version:	%{rushit_version}
Release:	1%{?dist}
Summary:	scriptable performance tool

Group:		Applications/Internet
License:	ASL 2.0
URL:		https://github.com/jsitnicki/rushit/
Source0:	rushit-%{version}.tar.gz

BuildRequires:	libcmocka-devel

%description
rushit is a tool for performance testing highly configurable
via lua scripts

%prep
%setup -q

%build
make %{?_smp_mflags}

%install
install -p -D -t $RPM_BUILD_ROOT/%{_bindir}/ tcp_stream
install -p -D -m 0644 -t $RPM_BUILD_ROOT/%{_datadir}/rushit/ scripts/*.lua
install -p -D -m 0644 -t $RPM_BUILD_ROOT/%{_docdir}/rushit/ README.md README.neper.md

%files
%defattr(-,root,root)
%{_bindir}/tcp_stream
%{_datadir}/rushit/
%{_docdir}/rushit/
