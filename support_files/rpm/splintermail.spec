Name:           splintermail
Vendor:         Splintermail, LLC
Version:        QWER version REWQ
Release:        1%{?dist}
BuildArch:      %{_arch}
Summary:        QWER pkgdescr_short REWQ

License:    Unlicense
URL:        https://github.com/splintermail/splintermail-client
Source:     %{name}-%{version}.tar.gz

BuildRequires:  cmake openssl-devel systemd
Requires(post): shadow-utils openssl
#Requires:      openssl-libs

%description
QWER ["call", "pkgdescr_long", {"width":"72"}] REWQ

%global debug_package %{nil}

%prep
%setup -q

%build
mkdir build
cd build
cmake -DCMAKE_INSTALL_PREFIX=/usr -DPACKAGE_FOR=RHEL ../
make %{?_smp_mflags}
strip -s splintermail


%install
cd build
%make_install PREFIX=/usr


%files
%license UNLICENSE
%{_bindir}/splintermail
%config(noreplace) %{_sysconfdir}/splintermail.conf
%{_datadir}/bash-completion/completions/splintermail
%{_datadir}/zsh/site-functions/_splintermail
%{_datadir}/splintermail/keygen.sh
%{_datadir}/splintermail/openssl.cnf
%{_unitdir}/splintermail.service
%{_mandir}/man1/splintermail.1.gz
%dir %{_datadir}/splintermail

%post
# install if $1==1, upgrade if $1==0
# this just enables/disables the service according to the preset policy
%systemd_post splintermail.service
# create service user
getent group splintermail >/dev/null || groupadd -r splintermail
getent passwd splintermail >/dev/null || \
    useradd -r -g splintermail -d "QWER sm_dir REWQ" -s /sbin/nologin \
    -c "Splintermail service account" splintermail
# create the splintermail directory
if [ ! -d "QWER sm_dir REWQ" ] ; then
    mkdir -p "QWER sm_dir REWQ"
    chmod 700 "QWER sm_dir REWQ"
    chown -R splintermail:splintermail "QWER sm_dir REWQ"
fi
# migrate old certificates from v0.2 installations
if [ -f "QWER sm_dir REWQ/QWER old_cert_name REWQ" ] ; then
    mv "QWER sm_dir REWQ/QWER old_cert_name REWQ" "QWER sm_dir REWQ/QWER cert_name REWQ"
fi
if [ -f "QWER sm_dir REWQ/QWER old_key_name REWQ" ] ; then
    mv "QWER sm_dir REWQ/QWER old_key_name REWQ" "QWER sm_dir REWQ/QWER key_name REWQ"
fi
if [ -f "QWER sm_dir REWQ/QWER old_ca_name REWQ" ] ; then
    mv "QWER sm_dir REWQ/QWER old_ca_name REWQ" "QWER sm_dir REWQ/QWER ca_name REWQ"
fi
# generate the SSL certificates, if they don't exist already
if [ ! -f "QWER sm_dir REWQ/QWER cert_name REWQ" ] \
        || [ ! -f "QWER sm_dir REWQ/QWER key_name REWQ" ] \
        || [ ! -f "QWER sm_dir REWQ/QWER ca_name REWQ" ] ; then
    # generate the files
    sh "QWER share_dir REWQ/keygen.sh" "QWER share_dir REWQ/openssl.cnf" "QWER sm_dir REWQ"
    # trust the generated certificate authority
    cp "QWER sm_dir REWQ/QWER cert_name REWQ" "/etc/pki/ca-trust/source/anchors/"
    update-ca-trust extract
fi
exit 0

%preun
# upgrade if $1==1, uninstall if $1==0
# this stops/disables the service on package removal
%systemd_preun splintermail.service

%postun
# upgrade if $1=1, uninstall if $1==0
# this restarts the service if it is running
%systemd_postun_with_restart splintermail.service
# untrust the certificate generated at install
if [ "$1" == 0 ] ; then
    rm "/etc/pki/ca-trust/source/anchors/QWER ca_name REWQ" \
        && update-ca-trust extract || true
    # remove the splintermail directory
    rm -rf "QWER sm_dir REWQ"
fi
exit 0

%changelog
* Mon Sep 3 2018 Splintermail Dev <dev@splintermail.com> - 0.2.0-1
  - Initial packaging
