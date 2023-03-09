Name:           splintermail
Vendor:         Splintermail, LLC
Version:        QW version WQ
Release:        1%{?dist}
Summary:        QW pkgdescr_short WQ

License:    Unlicense
URL:        https://github.com/splintermail/splintermail-client
# No `Source:`, we're using a pre-configured source and build directory

BuildRequires:  cmake openssl-devel systemd
Requires(post): shadow-utils openssl
#Requires:      openssl-libs

%description
QW pkgdescr_long("72") WQ

%global debug_package %{nil}

%prep
# If we used a "Source:" tag, normal setup would be:
#%%setup -q
# But we only need to ensure that the %%license directive works.
cp "QW src_dir WQ/UNLICENSE" .

%build
cmake --build "QW build_dir WQ"
strip -s "QW build_dir WQ/splintermail"

%install
DESTDIR="%{buildroot}" cmake --build "QW build_dir WQ" --target install


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
    useradd -r -g splintermail -d "QW sm_dir WQ" -s /sbin/nologin \
    -c "Splintermail service account" splintermail
# create the splintermail directory
if [ ! -d "QW sm_dir WQ" ] ; then
    mkdir -p "QW sm_dir WQ"
    chmod 700 "QW sm_dir WQ"
    chown splintermail:splintermail "QW sm_dir WQ"
fi
# migrate old certificates from v0.2 installations
if [ -f "QW sm_dir WQ/QW old_cert_name WQ" ] ; then
    mv "QW sm_dir WQ/QW old_cert_name WQ" "QW sm_dir WQ/QW cert_name WQ"
fi
if [ -f "QW sm_dir WQ/QW old_key_name WQ" ] ; then
    mv "QW sm_dir WQ/QW old_key_name WQ" "QW sm_dir WQ/QW key_name WQ"
fi
if [ -f "QW sm_dir WQ/QW old_ca_name WQ" ] ; then
    mv "QW sm_dir WQ/QW old_ca_name WQ" "QW sm_dir WQ/QW ca_name WQ"
fi
# remove the cert_file that may accidentally have been installed in 0.2.0
if [ -f "/etc/pki/ca-trust/source/anchors/QW old_cert_name WQ" ] ; then
    rm "/etc/pki/ca-trust/source/anchors/QW old_cert_name WQ" \
        && update-ca-trust extract || true
fi
# generate the SSL certificates, if they don't exist already
if [ ! -f "QW sm_dir WQ/QW cert_name WQ" ] \
        || [ ! -f "QW sm_dir WQ/QW key_name WQ" ] \
        || [ ! -f "QW sm_dir WQ/QW ca_name WQ" ] ; then
    # generate the files
    sh "QW share_dir WQ/keygen.sh" "QW share_dir WQ/openssl.cnf" "QW sm_dir WQ"
    # make sure splintermail can read them
    chown splintermail:splintermail "QW sm_dir WQ/QW ca_name WQ"
    chown splintermail:splintermail "QW sm_dir WQ/QW cert_name WQ"
    chown splintermail:splintermail "QW sm_dir WQ/QW key_name WQ"
    # trust the generated certificate authority
    cp "QW sm_dir WQ/QW ca_name WQ" "/etc/pki/ca-trust/source/anchors/"
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
# remove the cert_file that may accidentally have been installed in 0.2.0
if [ -f "/etc/pki/ca-trust/source/anchors/QW old_cert_name WQ" ] ; then
    rm "/etc/pki/ca-trust/source/anchors/QW old_cert_name WQ" \
        && update-ca-trust extract || true
fi
# untrust the certificate generated at install
if [ "$1" == 0 ] ; then
    # (remove the cert_file that may accidentally have been installed in 0.2.0)
    rm "/etc/pki/ca-trust/source/anchors/QW ca_name WQ" \
        && update-ca-trust extract || true
    # remove the splintermail directory
    rm -rf "QW sm_dir WQ" || true
fi
exit 0

%changelog
* Mon Sep 3 2018 Splintermail Dev <dev@splintermail.com> - 0.2.0-1
  - Initial packaging
