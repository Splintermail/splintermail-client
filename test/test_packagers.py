#!/usr/bin/env python3

import argparse
import re
from sys import argv, exit, stderr, exc_info
from subprocess import Popen, PIPE
from time import sleep, time, strftime
from socket import socket, AF_INET, SOCK_STREAM
import threading
import traceback

import os
import hashlib
import contextlib
import sys

class PrepareError(Exception): pass
class BuildError(Exception): pass
class InstallError(Exception): pass
class UpgradeError(Exception): pass
class SoftUninstallError(Exception): pass
class UninstallError(Exception): pass
class RageCleanError(Exception): pass

def log_action_start(lf, args, expect_exit=0, bytesin=b''):
    timestr = strftime('%H:%M:%S').encode('utf8')
    if lf is not None:
        with open(lf, 'ab') as f:
            temp = [b'------------',
                    b'args: %s'%str(args).encode('utf8'),
                    b'starttime %s ; expect exit code %d'%(timestr, expect_exit)]
            f.write(b'\n'.join(temp) + b'\n')
            if len(bytesin) > 0:
                f.write(b'stdin:\n' + bytesin)
    return time()

def log_action_end(lf, starttime, ret, out, err):
    timestr = strftime('%H:%M:%S').encode('utf8')
    duration = time() - starttime
    if lf is not None:
        with open(lf, 'ab') as f:
            if len(out) > 0:
                f.write(b'stdout:\n' + out)
            if len(err) > 0:
                f.write(b'stderr:\n' + err)
            f.write(b'(exited %d, endtime %s, took %.2f seconds)\n'%(ret, timestr, duration))


test_descr = {
    # # build checks
    # '1.0':[BuildError, 'Installer builds without error'],
    # '1.1':[BuildError, 'Installer passes linter without warning'],
    # install checks
    '2.0':[InstallError, 'install succeeds'],
    '2.1':[InstallError, 'reinstall succeeds'],
    '2.2':[InstallError, 'sm_dir exists, owned by service user'],
    '2.3':[InstallError, 'config file exists'],
    '2.4':[InstallError, 'existing config file not overwritten'],
    '2.8':[InstallError, 'splintermail on path'],
    '2.9':[InstallError, 'splintermail service works'],
    '2.10':[InstallError, 'splintermail service owned by service user'],
    '2.11':[InstallError, 'service users exists'],
    '2.12':[InstallError, 'sm_dir exists, owned by service user'],
    # soft uninstall checks
    '3.0':[SoftUninstallError, 'soft uninstall succeeds'],
    '3.1':[SoftUninstallError, 'sm_dir remains'],
    '3.3':[SoftUninstallError, 'config file exists'],
    '3.4':[SoftUninstallError, 'service stopped'],
    # uninstall checks
    '4.0':[UninstallError, 'uninstall succeeds'],
    '4.1':[UninstallError, 'sm_dir deleted'],
    '4.3':[UninstallError, 'unchanged config file removed'],
    '4.4':[UninstallError, 'service stopped'],
    # rageclean checks
    '5.0':[RageCleanError, 'rageclean succeeds'],
    '5.1':[RageCleanError, 'sm_dir deleted'],
    '5.3':[RageCleanError, 'changed config file removed'],
    '5.4':[RageCleanError, 'service stopped'],
    # upgrade checks
    '6.0':[UpgradeError, 'upgrade succeeds'],
    '6.1':[UpgradeError, 'unchanged config files are overwritten'],
    '6.2':[UpgradeError, 'changed config files not overwritten'],
    '6.5':[UpgradeError, 'splintermail service started'],
    '6.6':[UpgradeError, 'splintermail service works'],
}

@contextlib.contextmanager
def testcode(c):
    try:
        yield
    except Exception as e:
        err, msg = test_descr[c]
        raise err(msg) from e

# steps necessary to pass through each test:
#   1   build installer
#   2   run linter
#   3   clean install
#   4   soft uninstall
#   5   uninstall
#   6   rageclean (for windows, which has neither of the other two)
# -------- basic reqs met ------------
#   7   create config, cert, key ,ca, and trust them
#   8   re install
#   9   change config file
#   10   soft uninstall
#   11  uninstall
#   12  rageclean
#   13  re install
#   14  upgrade
#   15  change config file
#   16  upgrade again
# -------- all reqs met ------------

class TestSuite():
    def __init__(self):
        self.has_soft_uninstall = True
        self.has_uninstall = True
        self.has_rageclean = True
        self.succeeded = False
        self.testtime = None
    # actions that need to be implemented
    def install(self): raise NotImplemented('install')
    def soft_uninstall(self): raise NotImplemented('soft_uninstall')
    def uninstall(self): raise NotImplemented('uninstall')
    def rageclean(self): raise NotImplemented('rageclean')
    # tests that need to be implemented
    def assert_owner(self, f, uid): raise NotImplemented('assert_owner')
    def assert_perms(self, f, perms): raise NotImplemented('assert_perms')
    def get_user_uid(self, username): raise NotImplemented('get_user_uid')
    def run_pkg_linter(self): raise NotImplemented('run_pkg_linter')
    def assert_service_owner(self): raise NotImplemented('assert_service_owner')
    # these are OS-specific, so they're not required
    def start_non_automatic_service(self): pass
    def stop_non_automatic_service(self): pass

    #### end of things-to-be-implemented

    def assert_exe_on_path(self, cmd):
        p = Popen(("splintermail", "--version"))
        ret = p.wait()
        assert ret == 0, ret

    def assert_service_works(self):
        for i in range(10):
            try:
                with socket() as s:
                    s.connect(('127.0.0.1', 143))
                    # we connected, that's all we needed
                    return
            except ConnectionRefusedError:
                # service might just not be up yet
                sleep(.1)
        raise ValueError("connection was rejected!")

    def assert_service_stopped(self):
        with socket() as s:
            try:
                s.connect(('127.0.0.1', 143))
            except ConnectionRefusedError:
                pass
            else:
                raise ValueError("connection was accepted!")

    def assert_file_md5(self, f, md5):
        with open(f, "rb") as f:
            text = f.read()
        m = hashlib.md5()
        m.update(text)
        assert m.hexdigest() == md5

    def run(self):
        teststart = time()
        with testcode('2.0'):
            self.install()
        self.check_install(clean=True)
        with testcode('3.0'):
            self.soft_uninstall()
        self.check_soft_uninstall(updates=False)
        with testcode('4.0'):
            self.uninstall()
        self.check_uninstall(updates=False)
        with testcode('5.0'):
            self.rageclean()
        self.check_rageclean()
        testtime = time() - teststart
        print("#### PASS in %.1fs ####"%testtime)

    def check_install(self, clean=True):
        with testcode('2.2'):
            assert os.path.isdir(self.sm_dir), self.sm_dir
            uid = self.get_user_uid(self.service_username)
            self.assert_owner(self.sm_dir, uid)
            self.assert_perms(self.sm_dir, 0o700)
        with testcode('2.3'):
            assert os.path.isfile(self.config_file), self.config_file
        with testcode('2.8'):
            self.assert_exe_on_path(('splintermail', '--version'))
        with testcode('2.9'):
            self.start_non_automatic_service()
            self.assert_service_works()
        with testcode('2.10'):
            self.assert_service_owner()
        if clean != True:
            # 2.4
            raise NotImplemented

    def check_soft_uninstall(self, updates=False):
        if self.has_soft_uninstall == False:
            return
        if updates == True:
            raise NotImplemented
        with testcode('3.1'):
            assert os.path.isdir(self.sm_dir), self.sm_dir
        with testcode('3.3'):
            assert os.path.isfile(self.config_file), self.config_file
        with testcode('3.4'):
            self.assert_service_stopped()

    def check_uninstall(self, updates=False):
        if self.has_uninstall == False:
            return
        if updates == True:
            raise NotImplemented
        with testcode('4.1'):
            assert not os.path.isdir(self.sm_dir), self.sm_dir
        with testcode('4.3'):
            assert not os.path.isfile(self.config_file), self.config_file
        with testcode('4.4'):
            self.assert_service_stopped()

    def check_rageclean(self):
        if self.has_rageclean == False:
            return
        with testcode('5.1'):
            assert not os.path.isdir(self.sm_dir), self.sm_dir
        with testcode('5.3'):
            assert not os.path.isfile(self.config_file), self.config_file
        with testcode('5.4'):
            self.assert_service_stopped()

    def check_upgrade(self, updates=False):
        pass


class WindowsTestSuite(TestSuite):
    def __init__(self, installer):
        self.installer = installer
        self.sm_dir = 'C:/ProgramData/splintermail'
        self.config_file = 'C:/Program Files/Splintermail/Splintermail/splintermail.conf'
        self.service_username = None
        super().__init__()

    def install(self):
        # start install
        # cmd = ("msiexec", "/i", self.installer, "/quiet", "/log", "install_log.txt")
        cmd = (self.installer, "/install", "/quiet", "/log", "install_log.txt")
        p = Popen(cmd)
        ret = p.wait()
        assert ret == 0, ret

    def soft_uninstall(self):
        self.has_soft_uninstall = False

    def uninstall(self):
        self.has_uninstall = False

    def rageclean(self):
        # start install
        #cmd = ("msiexec", "/x", self.installer, "/quiet", "/log", "uninstall_log.txt")
        cmd = (self.installer, "/uninstall", "/quiet", "/log", "uninstall_log.txt")
        p = Popen(cmd)
        ret = p.wait()
        assert ret == 0, ret

    def assert_owner(self, f, uid): pass

    def assert_perms(self, f, perms): pass

    def get_user_uid(self, username): return 'asdf'

    def run_pkg_linter(self): pass

    def assert_service_owner(self): pass


class UnixTestSuite(TestSuite):
    def __init__(self):
        self.sm_dir = '/var/lib/splintermail'
        self.config_file = '/etc/splintermail.conf'
        self.status_socket = '/run/splintermail.sock'
        super().__init__()

    def assert_owner(self, f, uid):
        got = os.stat(f).st_uid
        assert got == uid, "%s: got != expected (%d != %d)"%(f, got, uid)

    def assert_perms(self, f, perms):
        got = os.stat(f).st_mode & 0o777
        assert got == perms, "%s: got != expected (%d != %d)"%(f, got, perms)

    def get_user_uid(self, username):
        import pwd
        return pwd.getpwnam(username).pw_uid

    def assert_service_owner(self):
        # previously, we checked lsof to see owner of the port, but that
        # doesn't seem to work inside rootless podman containers, so now we
        # check the ownership of the splintermail citm command instead
        # cmd = ('lsof', '-i', 'TCP:143', '-F', 'cu')

        # since we support on-demand daemons, we need to connect first
        with socket() as s:
            s.settimeout(1)
            s.connect(('localhost', 143))
            # Read the initial `BYE needs configuring` message to ensure
            # citm process is actually running
            _ = s.recv(4096)

        cmd = ('ps', 'U', self.service_username, '-o', 'pid,command')
        p = Popen(cmd, stdout=PIPE)
        out = p.stdout.read()
        ret = p.wait()
        assert ret == 0, "ps exited %d"%ret
        assert b"splintermail citm" in out, out

        cmd = ('ps', 'U', self.service_username, '-o', 'pid,command')
        p = Popen(cmd, stdout=PIPE)
        out = p.stdout.read()
        ret = p.wait()
        assert ret == 0, "ps exited %d"%ret
        assert b"splintermail citm" in out, out

        # also check for world read/write on the socket
        mode = os.stat(self.status_socket).st_mode
        assert (mode & 0o666) == 0o666, mode


class BSDTestSuite(UnixTestSuite):
    # Nothing to override right now
    pass


class OSXTestSuite(BSDTestSuite):
    def __init__(self, installer):
        if len(installer.split(":")) != 2:
            raise ValueError("--osx arg must be INSTALLER:UNINSTALLER")
        self.installer, self.uninstaller = installer.split(":")
        self.service_username = '_splintermail'
        # include /usr/local/bin on the PATH, if it isn't already, so we can
        # automate this test from non-interactive ssh, which restricts PATH
        if "/usr/local/bin" not in os.environ["PATH"]:
            os.environ["PATH"] = os.environ["PATH"] + ":/usr/local/bin"
        super().__init__()
        self.status_socket = '/var/run/splintermail.sock'

    def get_user_uid(self, username):
        cmd = ('dscl', '.', 'list', '/Users', 'UniqueID')
        p = Popen(cmd, stdout=PIPE)
        out = p.stdout.read()
        ret = p.wait()
        assert ret == 0, ret
        for line in out.split(b'\n'):
            if username.encode('utf8') in line:
                return int(line.split()[1])
        raise ValueError(
            "%s user not found in:\n%s"%(username, out.decode('utf8'))
        )

    def run_pkg_linter(self):
        # TODO implement this
        pass

    def install(self):
        cmd = ('installer',
               '-pkg', self.installer,
               '-tgt', '/Volumes/Macintosh HD',
               '-dumplog')
        p = Popen(cmd)
        ret = p.wait()
        assert ret == 0, ret

    def soft_uninstall(self):
        self.has_soft_uninstall = False

    def uninstall(self):
        cmd = ('installer',
               '-pkg', self.uninstaller,
               '-tgt', '/Volumes/Macintosh HD',
               '-dumplog')
        p = Popen(cmd)
        ret = p.wait()
        assert ret == 0, ret

        # Mac-specific step: make sure the package receipt is gone
        cmd = ('pkgutil', '--pkgs')
        p = Popen(cmd, stdout=PIPE)
        out = p.stdout.read()
        ret = p.wait()
        assert ret == 0, ret
        assert b"splintermail" not in out, out

    def rageclean(self):
        self.has_rageclean = False

    def assert_service_stopped(self):
        cmd = ('launchctl', 'list')
        p = Popen(cmd, stdout=PIPE)
        out = p.stdout.read()
        ret = p.wait()
        assert ret == 0, ret
        assert b"splintermail" not in out, out.decode('utf8')


class LinuxTestSuite(UnixTestSuite):
    def __init__(self):
        self.service_username = 'splintermail'
        super().__init__()


class ArchTestSuite(LinuxTestSuite):
    def __init__(self, installer):
        self.installer = installer
        super().__init__()

    def run_pkg_linter(self):
        # TODO implement this
        pass

    def install(self):
        # update package manager cache
        cmd = ('pacman', '-Sy', '--noconfirm')
        p = Popen(cmd)
        assert p.wait() == 0
        # install the package
        cmd = ('pacman', '--noconfirm', '-U', self.installer)
        p = Popen(cmd)
        assert p.wait() == 0

    def soft_uninstall(self):
        self.has_soft_uninstall = False

    def uninstall(self):
        # update package manager cache
        cmd = ('pacman', '--noconfirm', '-R', 'splintermail')
        p = Popen(cmd)
        ret = p.wait()
        assert ret == 0, "exit code %d"%ret

    def rageclean(self):
        # OK, technically archlinux does have this but it doesn't
        # work sequentially after the install step
        self.has_rageclean = False

    def start_non_automatic_service(self):
        cmd = ('systemctl', 'start', 'splintermail.service')
        p = Popen(cmd)
        ret = p.wait()
        assert ret == 0, "exit code %d"%ret

    def stop_non_automatic_service(self):
        cmd = ('systemctl', 'stop', 'splintermail.service')
        p = Popen(cmd)
        ret = p.wait()
        assert ret == 0, "exit code %d"%ret


class DebianTestSuite(LinuxTestSuite):
    def __init__(self, installer):
        self.installer = installer
        super().__init__()

    def run_pkg_linter(self):
        # TODO implement this
        pass

    def install(self):
        # update package manager cache
        cmd = ('apt-get', 'update')
        ret = Popen(cmd).wait()
        assert ret == 0, ret
        cmd = ('apt', 'install', '-y', self.installer)
        ret = Popen(cmd).wait()
        assert ret == 0, ret

    def soft_uninstall(self):
        cmd = ('apt-get', '-y', 'remove', 'splintermail')
        ret = Popen(cmd).wait()
        assert ret == 0, ret

    def uninstall(self):
        self.has_uninstall = False

    def rageclean(self):
        cmd = ('apt-get', '-y', 'purge', 'splintermail')
        ret = Popen(cmd).wait()
        assert ret == 0, ret


class RHELTestSuite(LinuxTestSuite):
    def __init__(self, installer):
        self.installer = installer
        super().__init__()

    def run_pkg_linter(self):
        # TODO implement this
        pass

    def install(self):
        # install package
        cmd = ('dnf', 'install', '-y', self.installer)
        ret = Popen(cmd).wait()
        assert ret == 0, ret
        # keep openssl even after uninstall
        cmd = ('dnf', 'mark', 'install', 'openssl')
        ret = Popen(cmd).wait()
        assert ret == 0, ret

    def soft_uninstall(self):
        self.has_soft_uninstall = False

    def uninstall(self):
        cmd = ('dnf', 'remove', '-y', 'splintermail')
        ret = Popen(cmd).wait()
        assert ret == 0, ret

    def rageclean(self):
        self.has_rageclean = False

    def start_non_automatic_service(self):
        # the default policy doesn't start splintermail
        cmd = ('systemctl', 'start', 'splintermail.service')
        ret = Popen(cmd).wait()
        assert ret == 0, ret

    def stop_non_automatic_service(self):
        cmd = ('systemctl', 'stop', 'splintermail.service')
        ret = Popen(cmd).wait()
        assert ret == 0, ret


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument("--arch")
    parser.add_argument("--debian")
    parser.add_argument("--osx")
    parser.add_argument("--rhel")
    parser.add_argument("--windows")
    parser.add_argument("--wait", action="store_true", help="pause after failures")
    args = parser.parse_args()

    if not any((args.arch, args.debian, args.osx, args.rhel, args.windows)):
        print("no installer specified", file=sys.stderr)
        sys.exit(1)

    try:
        if args.arch:
            ArchTestSuite(args.arch).run()
        if args.debian:
            DebianTestSuite(args.debian).run()
        if args.osx:
            OSXTestSuite(args.osx).run()
        if args.rhel:
            RHELTestSuite(args.rhel).run()
        if args.windows:
            WindowsTestSuite(args.windows).run()

        print('PASS')
        sys.exit(0)

    except Exception as e:
        traceback.print_exc()

    if args.wait:
        if os.isatty(0):
            input("pausing for debugging, press enter to continue...")
        else:
            print(
                "no tty; waiting 1000 seconds for debugging",
                file=sys.stderr
            )
            sleep(1000)

    sys.exit(1)
