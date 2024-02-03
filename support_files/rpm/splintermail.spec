Name:           splintermail
Vendor:         Splintermail, LLC
Version:        QW version WQ
Release:        1%{?dist}
Summary:        QW pkgdescr_short WQ

License:    Unlicense
URL:        https://github.com/splintermail/splintermail-client
# No `Source:`, we're using a pre-configured source and build directory

BuildRequires:  cmake openssl-devel systemd-devel libuv-devel
Requires(post): shadow-utils openssl libuv systemd

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
%{_unitdir}/splintermail.service
%{_unitdir}/splintermail.socket
%{_mandir}/man1/splintermail.1.gz

%post
# install if $1==1, upgrade if $1==0
# This just enables/disables the service according to the preset policy.
# To test the enabled behavior, you can:
#
#     mkdir -p /etc/systemd/system-preset
#     echo enable splintermail.socket > \
#         /etc/systemd/system-preset/01-splintermail.preset
#
# As far as I can tell, even then this will only enable the socket, not
# actually start it, so no effect occurs until reboot.
#
# See also:
#   - www.freedesktop.org/software/systemd/man/latest/systemd.preset.html
#   - docs.fedoraproject.org/en-US/packaging-guidelines/Scriptlets
%systemd_post splintermail.socket

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
exit 0

%preun
# upgrade if $1==1, uninstall if $1==0
# this stops/disables the service on package removal
%systemd_preun splintermail.socket
%systemd_preun splintermail.service

%postun
# upgrade if $1=1, uninstall if $1==0
# this restarts the service if it is running
%systemd_postun_with_restart splintermail.socket
%systemd_postun_with_restart splintermail.service
if [ "$1" == 0 ] ; then
    # remove the splintermail directory
    rm -rf "QW sm_dir WQ" || true
fi
exit 0

%changelog
* Sat Feb 3 2024 Splintermail Dev <dev@splintermail.com> - 0.4.1-1
  - Fix ACME timer to properly renew letsencrypt certs.
* Tue Nov 14 2023 Splintermail Dev <dev@splintermail.com> - 0.4.0-1
  - Use ACME to obtain TLS certificates.
  - Expand IMAP support.
* Tue Oct 1 2019 Splintermail Dev <dev@splintermail.com> - 0.3.0-1
  - Partial IMAP support.
* Mon Sep 3 2018 Splintermail Dev <dev@splintermail.com> - 0.2.0-1
  - Initial packaging.
