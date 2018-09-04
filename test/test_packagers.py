#!/usr/bin/env python3

import re
from sys import argv, exit, stderr, exc_info
from subprocess import Popen, PIPE
from time import sleep, time, strftime
from socket import socket, AF_INET, SOCK_STREAM
import threading
import traceback

class LXCError(Exception): pass
class VboxError(Exception): pass
class NoImplError(Exception): pass

class PrepareError(Exception): pass
class BuildError(Exception): pass
class InstallError(Exception): pass
class UpgradeError(Exception): pass
class SoftUninstallError(Exception): pass
class UninstallError(Exception): pass
class RageCleanError(Exception): pass

custom_error_types = (LXCError, VboxError, NoImplError, PrepareError,
                      BuildError, InstallError, UpgradeError,
                      SoftUninstallError, UninstallError, RageCleanError)

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


# Remote EXecute
def rex(host, cmd, ssh_opts=(), bytesin=b'', filein=None, lf=None, cd=None,
        fail_with=ValueError, fail_code=None, expect_exit=0):
    global test_descr
    if filein is not None:
        if len(bytesin) != 0:
            raise ValueError('you can\'t specify bytesin AND filein')
        with open(filein, 'rb') as f:
            bytesin = f.read()
    cdargs = () if cd is None else ('cd', cd, '&&')
    cmd = (cmd,) if type(cmd) is str else cmd
    args = ('ssh', *ssh_opts, host, *cdargs, *cmd)

    starttime = log_action_start(lf, args, expect_exit, bytesin)

    h = Popen(args, stdin=PIPE, stdout=PIPE, stderr=PIPE)
    # write any input
    h.stdin.write(bytesin)
    h.stdin.close()
    # read any output
    out = h.stdout.read()
    err = h.stderr.read()
    h.stdout.close()
    h.stderr.close()
    # get the exit code
    ret = h.wait()
    # remove obnoxious ssh warning
    err = re.sub(b'Warning: Permanently added .* known hosts.\r\n', b'', err)

    log_action_end(lf, starttime, ret, out, err)

    if ret != expect_exit:
        if fail_code is not None:
            print('command failed:\n' + out.decode('utf8') + err.decode('utf8'), file=stderr)
            err, msg = test_descr[fail_code]
            raise err(msg)
        if fail_with is not None:
            print('command failed:\n' + out.decode('utf8') + err.decode('utf8'), file=stderr)
            raise fail_with('Error executing command')

    return ret, out, err

def push(host, floc, frem, lf=None, fail_with=ValueError, fail_code=None):
    global test_descr
    if type(floc) == list or type(floc) == tuple:
        floc_l = floc
    else:
        floc_l = [floc]

    args = ('scp', *floc_l, "%s:%s"%(host, frem))

    starttime = log_action_start(lf, args)

    h = Popen(args, stdout=PIPE, stderr=PIPE)
    # read any output
    out = h.stdout.read()
    err = h.stderr.read()
    h.stdout.close()
    h.stderr.close()
    # get the exit code
    ret = h.wait()
    # remove obnoxious ssh warning
    err = re.sub(b'Warning: Permanently added .* known hosts.\r\n', b'', err)

    log_action_end(lf, starttime, ret, out, err)

    if ret != 0:
        if fail_code is not None:
            print('copy failed:\n' + out.decode('utf8') + err.decode('utf8'), file=stderr)
            err, msg = test_descr[fail_code]
            raise err(msg)
        if fail_with is not None:
            print('copy failed:\n' + out.decode('utf8') + err.decode('utf8'), file=stderr)
            raise fail_with('Error executing command')

def pull(host, frem, floc, lf=None, fail_with=ValueError, fail_code=None):
    global test_descr
    args = ('scp', "%s:%s"%(host, frem), floc)

    starttime = log_action_start(lf, args)

    h = Popen(args, stdout=PIPE, stderr=PIPE)
    # read any output
    out = h.stdout.read()
    err = h.stderr.read()
    h.stdout.close()
    h.stderr.close()
    # get the exit code
    ret = h.wait()
    # remove obnoxious ssh warning
    err = re.sub(b'Warning: Permanently added .* known hosts.\r\n', b'', err)

    log_action_end(lf, starttime, ret, out, err)

    if ret != 0:
        if fail_code is not None:
            print('copy failed:\n' + out.decode('utf8') + err.decode('utf8'), file=stderr)
            err, msg = test_descr[fail_code]
            raise err(msg)
        if fail_with is not None:
            print('copy failed:\n' + out.decode('utf8') + err.decode('utf8'), file=stderr)
            raise fail_with('Error executing command')

class DummyContext():
    def __init__(self, dev): pass
    def __enter__(self): return self
    def __exit__(self, typ, val, tracebk):
        if typ is not None:
            raise
    def wait_for_ssh(self): pass

class RemoteLXC():
    def __init__(self, dev):
        self.dev = dev
        # arch container gets logged in to as *-b but container is *-d
        self.container = dev[:-1]+'d' if dev.endswith('-b') else dev
        self.lf = 'logs/'+dev

    def __enter__(self):
        # first check if the container is already running
        cmd = ('sudo', 'lxc-ls', '-1', '--running')
        _, out, _ = rex('doomtower', cmd, fail_with=LXCError, lf=self.lf)
        if self.container not in out.decode('utf8').split('\n'):
            # if not, start it
            cmd = ('sudo', 'lxc-start', self.container)
            rex('doomtower', cmd, fail_with=LXCError, lf=self.lf)
        self.bootstart = time()
        return self;

    def __exit__(self, typ, val, tracebk):
        # close container
        # cmd = ('sudo', 'lxc-stop', self.container)
        # _, out, _ = rex('doomtower', cmd, fail_with=LXCError, lf=self.lf)
        # rethrow any errors
        if typ is not None:
            raise

    def wait_for_ssh(self):
        # we will always wait at least 2 seconds for the machine to start up
        # or it exhibits "weird" behavior
        sincebootstart = time() - self.bootstart
        if sincebootstart < 2:
            sleep(2 - sincebootstart)
        online = False
        # wait for ssh port to become active
        for i in range(40):
            with socket(AF_INET, SOCK_STREAM) as s:
                s.settimeout(1)
                try:
                    s.connect((self.dev, 22))
                    online = True
                    break
                except:
                    sleep(1)
        if not online:
            raise LXCError('unable to connect to ' + self.dev)

class TempRemoteLXC():
    def __init__(self, basename):
        self.basename = basename
        self.lf = 'logs/'+basename

    def __enter__(self):
        # first stop any stray conflicting clients (ephimeral or not)
        cmd = ('sudo', 'lxc-ls', '-1', '--running')
        _, out, _ = rex('doomtower', cmd, fail_with=LXCError, lf=self.lf)
        for name in out.decode('utf8').split('\n'):
            if name.startswith(self.basename):
                cmd = ('sudo', 'lxc-stop', '-n', name)
                rex('doomtower', cmd, fail_with=LXCError, lf=self.lf)
        # spawn the remote client
        cmd = ('sudo', 'lxc-copy', '-n', self.basename, '-e')
        _, out, _ = rex('doomtower', cmd, fail_with=LXCError, lf=self.lf)
        # save the temporary LXC container name
        self.tempname = re.sub(b'.*('+self.basename.encode('utf8')+b'_[^ \n]*).*', b'\\1', out)
        self.tempname = self.tempname.strip().decode('utf8')
        self.bootstart = time()
        return self;

    def __exit__(self, typ, val, tracebk):
        # close the container, except in case of errors
        if typ is None:
            pass
            # cmd = ('sudo', 'lxc-stop', self.tempname)
            # _, out, _ = rex('doomtower', cmd, fail_with=LXCError, lf=self.lf)
        else:
            print('leaving %s open for inspection'%self.tempname)
        # rethrow any errors
        if typ is not None:
            raise

    def wait_for_ssh(self):
        # we will always wait at least 2 seconds for the machine to start up
        # or it exhibits "weird" behavior
        sincebootstart = time() - self.bootstart
        if sincebootstart < 2:
            sleep(2 - sincebootstart)
        online = False
        # wait for ssh port to become active
        for i in range(40):
            with socket(AF_INET, SOCK_STREAM) as s:
                s.settimeout(1)
                try:
                    s.connect((self.basename, 22))
                    online = True
                    break
                except:
                    sleep(1)
        if not online:
            raise LXCError('unable to connect to ' + self.basename)

class TempRemoteVbox():
    def __init__(self, vmname):
        self.vmname = vmname
        self.lf = 'logs/'+vmname

    def __enter__(self):
        # call script to shutdown the VM if necessary and boot from snapshot
        cmd = ('vm-boot-from-snapshot', self.vmname, 'install_ready')
        rex('doomtower', cmd, fail_with=VboxError, lf=self.lf)
        return self;

    def __exit__(self, typ, val, tracebk):
        # this context manager actually does nothing on cleanup
        # (cleanup happens on the next __enter__)
        # rethrow any errors
        if typ is not None:
            raise

    def wait_for_ssh(self):
        online = False
        # wait for ssh port to become active
        for i in range(40):
            with socket(AF_INET, SOCK_STREAM) as s:
                s.settimeout(1)
                try:
                    s.connect((self.vmname, 22))
                    online = True
                    break
                except:
                    sleep(1)
        if not online:
            raise VboxError('unable to connect to ' + self.vmname)

class Builder(threading.Thread):
    def __init__(self, dev, version):
        self.dev = dev
        self.version = version
        self.lf = 'logs/'+dev
        self.mutex = threading.Lock()
        self.cond = threading.Condition(self.mutex)
        self.finished = False
        self.success = None
        self.buildtime = 0
        # empty logfile
        with open(self.lf, 'w'): pass
        # only OSX needs a separate uninstaller
        self.uninstaller = None
        super().__init__()

    def build(self): raise NoImplError
    def pull_installer(self): raise NoImplError

    def run(self):
        with self.dev_context as dev:
            dev.wait_for_ssh()
            self.mutex.acquire()
            buildstart = time()
            # catch exceptions in build
            try:
                self.build()
            except:
                with open(self.lf, 'a') as f:
                    traceback.print_exc(file=f)
                    f.write('####  FAIL  ####\n')
                self.success = False
            # check time up to now
            self.buildtime = time() - buildstart
            # catch exceptions in pull
            try:
                if self.success is not False:
                        self.pull_installer()
                        self.success = True
            except:
                with open(self.lf, 'a') as f:
                    traceback.print_exc(file=f)
                    f.write('####  FAIL  ####\n')
                self.success = False
        self.finished = True
        # notify any threads waiting
        self.cond.notifyAll()
        self.mutex.release()
        if self.success:
            with open(self.lf, 'a') as f:
                f.write('####  PASS  ####\n')

    def wait_for_build(self):
        self.mutex.acquire()
        # if already finished, just return
        if self.finished:
            self.mutex.release()
            return self.success
        # otherwise wait for signal
        self.cond.wait()
        self.mutex.release()
        return self.success


class WindowsBuilder(Builder):
    def __init__(self, dev, version):
        self.dev_context = DummyContext(dev)
        super().__init__(dev, version)

    def build(self):
        rex(self.dev, ('.\\build32.bat'), lf=self.lf)
        rex(self.dev, ('.\\build64.bat'), lf=self.lf)

    def pull_installer(self):
        self.installer = 'install_splintermail-%s.exe'%self.version
        frem = 'build64/installer/'+self.installer
        floc = 'installers/'+self.installer
        pull(self.dev, frem, floc, lf=self.lf)


class UnixBuilder(Builder):
    def build(self):
        rex(self.dev, ('ninja', 'installer'), cd='build', lf=self.lf)

class LinuxBuilder(UnixBuilder):
    def __init__(self, dev, version):
        self.dev_context = RemoteLXC(dev)
        super().__init__(dev, version)

class FedoraBuilder(LinuxBuilder):
    def pull_installer(self):
        cmd = ''.join(("echo splintermail-%s-1$"%self.version,
                       "(rpm --eval '%{?dist}')"
                       ".$(rpm --eval '%{_arch}').rpm"))
        _, out, _ = rex(self.dev, cmd, lf=self.lf)
        self.installer = out.strip().decode('utf8')
        frem = 'rpmbuild/RPMS/*/%s'%self.installer
        floc = 'installers/'+self.installer
        pull(self.dev, frem, floc, lf=self.lf)


class DebianBuilder(LinuxBuilder):
    def pull_installer(self):
        cmd = ''.join(('echo splintermail_%s-1_'%self.version,
                       '$(dpkg --print-architecture).deb'))
        _, out, _ = rex(self.dev, cmd, lf=self.lf)
        self.installer = out.strip().decode('utf8')
        frem = 'build/installer/%s'%self.installer
        floc = 'installers/'+self.installer
        pull(self.dev, frem, floc, lf=self.lf)

class ArchBuilder(LinuxBuilder):
    def pull_installer(self):
        cmd = 'echo splintermail-%s-1-$(uname -m).pkg.tar.xz'%self.version
        _, out, _ = rex(self.dev, cmd, lf=self.lf)
        self.installer = out.strip().decode('utf8')
        frem = 'build/installer/%s'%self.installer
        floc = 'installers/'+self.installer
        pull(self.dev, frem, floc, lf=self.lf)


class OSXBuilder(UnixBuilder):
    def __init__(self, dev, version):
        self.dev_context = DummyContext(dev)
        super().__init__(dev, version)

    def build(self):
        # as a pre-step, make sure the the samba share is loaded
        script = b'set -x ; ' + \
                 b'test -d /Volumes/Share' + \
                 b' || (sudo mkdir /Volumes/Share && sudo automount -vc)'
        rex(self.dev, 'sh', bytesin=script, lf=self.lf)
        # now actually do the build
        rex(self.dev, ('ninja', 'installer'), cd='build', lf=self.lf)
        rex(self.dev, ('ninja', 'uninstaller'), cd='build', lf=self.lf)

    def pull_installer(self):
        # get the installer...
        self.installer = 'install_splintermail-%s.pkg'%self.version
        frem = 'build/installer/build/%s'%self.installer
        floc = 'installers/'+self.installer
        pull(self.dev, frem, floc, lf=self.lf)
        # ... and the uninstaller
        self.uninstaller = 'uninstall_splintermail.pkg'
        frem = 'build/uninstaller/build/%s'%self.uninstaller
        floc = 'installers/'+self.uninstaller
        pull(self.dev, frem, floc, lf=self.lf)


test_descr = {
    # build checks
    '1.0':[BuildError, 'Installer builds without error'],
    '1.1':[BuildError, 'Installer passes linter without warning'],
    # install checks
    '2.0':[InstallError, 'install succeeds'],
    '2.1':[InstallError, 'reinstall succeeds'],
    '2.2':[InstallError, 'ditm_dir exists, owned by service user'],
    '2.3':[InstallError, 'config file exists'],
    '2.4':[InstallError, 'existing config file not overwritten'],
    '2.5':[InstallError, 'cert, key, and ca exist'],
    '2.6':[InstallError, 'existing cert, key, ca, not overwritten'],
    '2.7':[InstallError, 'ca is trusted (if cert/key/ca are new)'],
    '2.8':[InstallError, 'splintermail on path'],
    '2.9':[InstallError, 'splintermail service works'],
    '2.10':[InstallError, 'splintermail service owned by service user'],
    '2.11':[InstallError, 'service users exists'],
    '2.12':[InstallError, 'ditm_dir exists, owned by service user'],
    # soft uninstall checks
    '3.0':[SoftUninstallError, 'soft uninstall succeeds'],
    '3.1':[SoftUninstallError, 'ditm_dir remains'],
    '3.2':[SoftUninstallError, 'ca still trusted'],
    '3.3':[SoftUninstallError, 'config file exists'],
    '3.4':[SoftUninstallError, 'service stopped'],
    # uninstall checks
    '4.0':[UninstallError, 'uninstall succeeds'],
    '4.1':[UninstallError, 'ditm_dir deleted'],
    '4.2':[UninstallError, 'CA removed from root chain'],
    '4.3':[UninstallError, 'unchanged config file removed'],
    '4.4':[UninstallError, 'service stopped'],
    # rageclean checks
    '5.0':[RageCleanError, 'rageclean succeeds'],
    '5.1':[RageCleanError, 'ditm_dir deleted'],
    '5.2':[RageCleanError, 'CA removed from root chain'],
    '5.3':[RageCleanError, 'changed config file removed'],
    '5.4':[RageCleanError, 'service stopped'],
    # upgrade checks
    '6.0':[UpgradeError, 'upgrade succeeds'],
    '6.1':[UpgradeError, 'unchanged config files are overwritten'],
    '6.2':[UpgradeError, 'changed config files not overwritten'],
    '6.3':[UpgradeError, 'existing cert, key, ca, not overwritten'],
    '6.4':[UpgradeError, 'missing cert, key, ca, repaired'],
    '6.5':[UpgradeError, 'splintermail service started'],
    '6.6':[UpgradeError, 'splintermail service works'],
}

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

class TestSuite(threading.Thread):
    def __init__(self, builder):
        self.cert_file = self.ditm_dir+'/'+'ditm-127.0.0.1-cert.pem'
        self.key_file = self.ditm_dir+'/'+'ditm-127.0.0.1-key.pem'
        self.ca_file = self.ditm_dir+'/'+'ditm-127.0.0.1-ca.crt'
        self.has_soft_uninstall = True
        self.has_uninstall = True
        self.has_rageclean = True
        self.builder = builder
        self.succeeded = False
        self.testtime = None
        # empty logfile
        self.lf = "logs/"+self.client
        with open(self.lf, 'w'): pass
        super().__init__()
    # actions that need to be implemented
    def install(self, code): raise NoImplError('install')
    def soft_uninstall(self, code): raise NoImplError('soft_uninstall')
    def uninstall(self, code): raise NoImplError('uninstall')
    def rageclean(self, code): raise NoImplError('rageclean')
    # test utilities that need to be implemented
    def get_file_md5(self, f): raise NoImplError('get_file_md5')
    # tests that need to be implemented
    def assert_file_exists(self, f, code): raise NoImplError('assert_file_exists')
    def assert_dir_exists(self, f, code): raise NoImplError('assert_dir_exists')
    def assert_file_deleted(self, f, code): raise NoImplError('assert_file_deleted')
    def assert_dir_deleted(self, f, code): raise NoImplError('assert_dir_deleted')
    def assert_file_md5(self, f, md5, code): raise NoImplError('assert_file_md5')
    def assert_owner(self, f, uid, code): raise NoImplError('assert_owner')
    def assert_perms(self, f, perms, code): raise NoImplError('assert_perms')
    def get_user_uid(self, username, code): raise NoImplError('get_user_uid')
    def assert_ca_is_trusted(self, code): raise NoImplError('assert_ca_is_trusted')
    def assert_ca_not_trusted(self, code): raise NoImplError('assert_ca_not_trusted')
    def run_pkg_linter(self, code): raise NoImplError('run_pkg_linter')
    def assert_service_owner(self, uid, code): raise NoImplError('assert_service_owner')
    def assert_service_works(self, code): raise NoImplError('assert_service_works')
    def assert_service_stopped(self, code): raise NoImplError('assert_service_stopped')
    # these are OS-specific, so they're not required
    def prepare_client(self): pass
    def start_non_automatic_service(self, code): pass
    def stop_non_automatic_service(self, code): pass

    #### end of things-to-be-implemented

    def build_package(self, code):
        global test_descr
        # wait for the build
        if not self.builder.wait_for_build():
            err, msg = test_descr[fail_code]
            raise err(msg)

    def push_installer(self):
        self.installer = self.builder.installer
        push(self.client, 'installers/'+self.installer, self.installer,
                lf=self.lf)
        self.uninstaller = self.builder.uninstaller
        if self.uninstaller is not None:
            push(self.client, 'installers/'+self.uninstaller, self.uninstaller,
                    lf=self.lf)

    def assert_exe_on_path(self, cmd, code):
        rex(self.client, cmd, fail_code=code, lf=self.lf)

    def run(self):
        teststart = time()
        try:
            with self.client_context as ctx:
                self.version = '0.2.0'
                # build package while client machine is booting
                self.build_package('1.0')
                self.run_pkg_linter('1.1')
                # wait till client is booted
                ctx.wait_for_ssh()
                self.push_installer()
                self.prepare_client()
                self.install('2.0')
                self.check_install(clean=True)
                self.soft_uninstall('3.0')
                self.check_soft_uninstall(updates=False)
                self.uninstall('4.0')
                self.check_uninstall(updates=False)
                self.rageclean('5.0')
                self.check_rageclean()
                if self.lf is not None:
                    with open(self.lf, 'ab') as f:
                        f.write(b'####  PASS  ####\n')
                self.succeeded = True
        except Exception as e:
            if self.lf is not None:
                with open(self.lf, 'ab') as f:
                    if type(e) in custom_error_types:
                        f.write(b'####  FAIL  ####\n')
                        f.write(('%s: %s\n'%(type(e).__name__, str(e))).encode('utf8'))
                    else:
                        exc = traceback.format_exc()
                        f.write(exc.encode('utf8'))
        self.testtime = time() - teststart

    def check_install(self, clean=True):
        self.assert_dir_exists(self.ditm_dir, '2.2')
        self.service_user = self.get_user_uid(self.service_username, '2.2')
        self.assert_owner(self.ditm_dir, self.service_user, '2.2')
        self.assert_perms(self.ditm_dir, 0o700, '2.2')
        self.assert_file_exists(self.config_file, '2.3')
        self.assert_file_exists(self.cert_file, '2.5')
        self.assert_file_exists(self.key_file, '2.5')
        self.assert_file_exists(self.ca_file, '2.5')
        self.assert_exe_on_path(('splintermail', '--version'), '2.8')
        self.start_non_automatic_service('2.9')
        self.assert_service_works('2.9')
        self.assert_ca_is_trusted('2.7')
        self.assert_service_owner(self.service_user, '2.10')
        if clean != True:
            # 2.4 2.6 2.7
            raise NoImplError

    def check_soft_uninstall(self, updates=False):
        if self.has_soft_uninstall == False:
            return
        if updates == True:
            raise NoImplError
        self.assert_dir_exists(self.ditm_dir, '3.1')
        self.assert_ca_is_trusted('3.2')
        self.assert_file_exists(self.config_file, '3.3')
        self.stop_non_automatic_service('3.4')
        self.assert_service_stopped('3.4')

    def check_uninstall(self, updates=False):
        if self.has_uninstall == False:
            return
        if updates == True:
            raise NoImplError
        self.assert_dir_deleted(self.ditm_dir, '4.1')
        self.assert_ca_not_trusted('4.2')
        self.assert_file_deleted(self.config_file, '4.3')
        self.stop_non_automatic_service('4.4')
        self.assert_service_stopped('4.4')

    def check_rageclean(self):
        if self.has_rageclean == False:
            return
        self.assert_dir_deleted(self.ditm_dir, '5.1')
        self.assert_ca_not_trusted('5.2')
        self.assert_file_deleted(self.config_file, '5.3')
        self.stop_non_automatic_service('5.4')
        self.assert_service_stopped('5.4')

    def check_upgrade(self, updates=False):
        pass


class WindowsTestSuite(TestSuite):
    def __init__(self, client, builder):
        self.client = client
        self.ditm_dir = 'C:/ProgramData/splintermail'
        self.config_file = 'C:/Program Files/Splintermail/Splintermail/splintermail.conf'
        self.service_username = None
        self.client_context = TempRemoteVbox(client)
        super().__init__(builder)

    def gen_installer_script(self):
        # need bytes-like installer name without exe
        exename = self.installer[:-4].encode('utf8')
        script = \
        b'''%s.exe /install /quiet /log install_log.txt || exit /B 2
            :: wait until installer process has stopped
            powershell while(1){if(ps -Name %s){sleep .3}else{break}}
            :: nothing seems to execute after the above line for some reason
        '''%(exename, exename)
        # remove obnoxious whitespace
        script = re.sub(b'\n *', b'\n', script)
        # make sure we have windows line endings (two steps to make it easy)
        script = re.sub(b'\r\n', b'\n', script)
        script = re.sub(b'\n', b'\r\n', script)
        return script

    def gen_sshd_restart_script(self):
        script = \
        b''':: stop and start sshd in a single ssh session
            net stop sshd || exit /B 1
            net start sshd || exit /B 2
        '''
        # remove obnoxious whitespace
        script = re.sub(b'\n *', b'\n', script)
        # make sure we have windows line endings (two steps to make it easy)
        script = re.sub(b'\r\n', b'\n', script)
        script = re.sub(b'\n', b'\r\n', script)
        return script

    def gen_uninstaller_script(self):
        # need bytes-like installer name without exe
        exename = self.installer[:-4].encode('utf8')
        script = \
        b'''%s.exe /uninstall /quiet /log uninstall_log.txt || exit /B 2
            :: wait until uninstaller process has stopped
            powershell while(1){if(ps -Name %s){sleep .3}else{break}}
            :: nothing seems to execute after the above line for some reason
        '''%(exename, exename)
        # remove obnoxious whitespace
        script = re.sub(b'\n *', b'\n', script)
        # make sure we have windows line endings (two steps to make it easy)
        script = re.sub(b'\r\n', b'\n', script)
        script = re.sub(b'\n', b'\r\n', script)
        return script

    def prepare_client(self):
        # copy helper scripts into place
        scriptnames =  ('isdir', 'isfile', 'md5', 'try_connect')
        scriptlist = ['test/files/packagers/%s.py'%n for n in scriptnames]
        push(self.client, scriptlist, '', lf=self.lf, fail_with=PrepareError)

    def install(self, code):
        script = self.gen_installer_script()
        rex(self.client, 'cmd', bytesin=script, fail_code=code, lf=self.lf)
        # restart sshd as well
        # can't seem to get this to work as one script...
        script = self.gen_sshd_restart_script()
        rex(self.client, 'cmd', bytesin=script, fail_code=code, lf=self.lf)

    def soft_uninstall(self, code):
        self.has_soft_uninstall = False

    def uninstall(self, code):
        self.has_uninstall = False

    def rageclean(self, code):
        script = self.gen_uninstaller_script()
        rex(self.client, 'cmd', bytesin=script, fail_code=code, lf=self.lf)

    def get_file_md5(self, f):
        _, out, _ = rex(self.client, ('python', 'md5.py', f),
                        fail_code=code, lf=self.lf)
        return out

    # tests that need to be implemented
    def assert_file_exists(self, f, code):
        rex(self.client, ('python', 'isfile.py', '"%s"'%f),
            fail_code=code, lf=self.lf)

    def assert_dir_exists(self, f, code):
        rex(self.client, ('python', 'isdir.py', '"%s"'%f),
            fail_code=code, lf=self.lf)

    def assert_file_deleted(self, f, code):
        rex(self.client, ('python', 'isfile.py', '"%s"'%f),
            fail_code=code, expect_exit=1, lf=self.lf)

    def assert_dir_deleted(self, f, code):
        rex(self.client, ('python', 'isdir.py', '"%s"'%f),
            fail_code=code, expect_exit=1, lf=self.lf)

    def assert_file_md5(self, f, md5, code):
        _, out, _ = rex(self.client, ('python', 'md5.py', '"%s"'%f),
                        fail_code=code, lf=self.lf)
        if out.decode('utf8').split()[0] != md5:
            err, msg = test_descr[code]
            raise err(msg)

    def assert_owner(self, f, uid, code): pass

    def assert_perms(self, f, perms, code): pass

    def get_user_uid(self, username, code): return 'asdf'

    def assert_ca_is_trusted(self, code):
        cmd = ('certutil', '-store', 'Root')
        _, out, _ = rex(self.client, cmd, fail_code=code, lf=self.lf)
        if b'splintermail.localhost' not in out:
            err, msg = test_descr[code]
            raise err(msg)

    def assert_ca_not_trusted(self, code):
        cmd = ('certutil', '-store', 'Root')
        _, out, _ = rex(self.client, cmd, fail_code=code, lf=self.lf)
        if b'splintermail.localhost' in out:
            err, msg = test_descr[code]
            raise err(msg)

    def run_pkg_linter(self, code): pass

    def assert_service_owner(self, uid, code): pass

    def assert_service_works(self, code):
        cmd = ('python', 'try_connect.py', '127.0.0.1', '1995')
        rex(self.client, cmd, fail_code=code, lf=self.lf)

    def assert_service_stopped(self, code):
        cmd = ('python', 'try_connect.py', '127.0.0.1', '1995')
        rex(self.client, cmd, fail_code=code, expect_exit=1, lf=self.lf)


class UnixTestSuite(TestSuite):
    def __init__(self, builder):
        self.ditm_dir = '/var/lib/splintermail'
        self.config_file = '/etc/splintermail.conf'
        super().__init__(builder)

    def assert_file_exists(self, f, code):
        rex(self.client, ('test', '-f', f), fail_code=code, lf=self.lf)

    def assert_dir_exists(self, f, code):
        rex(self.client, ('test', '-d', f), fail_code=code, lf=self.lf)

    def assert_file_deleted(self, f, code):
        rex(self.client, ('test', '!', '-f', f), fail_code=code, lf=self.lf)

    def assert_dir_deleted(self, f, code):
        rex(self.client, ('test', '!', '-d', f), fail_code=code, lf=self.lf)

    def assert_service_works(self, code):
        # actually is checked in assert_service_owner()
        pass


class BSDTestSuite(UnixTestSuite):
    def assert_file_md5(self, f, md5, code):
        _, out, _ = rex(self.client, ('md5', '-r', f),
                        fail_code=code, lf=self.lf)
        if out.decode('utf8').split()[0] != md5:
            err, msg = test_descr[code]
            raise err(msg)

    def assert_owner(self, f, uid, code):
        cmd = ('stat', '-f', '%u', f)
        _, out, _ = rex(self.client, cmd, fail_code=code, lf=self.lf)
        if int(out.decode('utf8')) != uid:
            err, msg = test_descr[code]
            raise err(msg)

    def assert_perms(self, f, perms, code):
        cmd = ('stat', '-f', '%p', f)
        _, out, _ = rex(self.client, cmd, fail_code=code, lf=self.lf)
        # have to strip extra info out of perms with a mask
        p = int(out.decode('utf8'), base=8) & int('777', base=8)
        if p != perms:
            err, msg = test_descr[code]
            raise err(msg)


class OSXTestSuite(BSDTestSuite):
    def __init__(self, client, builder):
        self.client = client
        self.service_username = '_splintermail'
        self.client_context = TempRemoteVbox(client)
        super().__init__(builder)

    def get_user_uid(self, username, code):
        script = b'set -x ; ' + \
                 b'dscl . list /Users UniqueID ' + \
                 b'| grep _splintermail ' + \
                 b'| awk "{print \\$2}"'
        _, out, _ = rex(self.client, 'sh', bytesin=script,
                        fail_code=code, lf=self.lf)
        return int(out.decode('utf8'))

    def run_pkg_linter(self, code):
        # TODO implement this
        pass

    def install(self, code):
        cmd = ('sudo', 'installer',
               '-pkg', self.installer,
               '-tgt', '/Volumes/clienthd',
               '-dumplog')
        rex(self.client, cmd, fail_code=code, lf=self.lf)

    def soft_uninstall(self, code):
        self.has_soft_uninstall = False

    def uninstall(self, code):
        cmd = ('sudo', 'installer',
               '-pkg', 'uninstall_splintermail.pkg',
               '-tgt', '/Volumes/clienthd',
               '-dumplog')
        rex(self.client, cmd, fail_code=code, lf=self.lf)
        # Mac-specific step: make sure the package receipt is gones
        cmd = ('pkgutil', '--pkgs', '|', 'grep', '-q', 'splintermail')
        rex(self.client, cmd, fail_code=code, expect_exit=1, lf=self.lf)

    def rageclean(self, code):
        self.has_rageclean = False

    # with sudo because we won't always be able to see the file otherwise
    def assert_file_exists(self, f, code):
        rex(self.client, ('sudo', 'test', '-f', f), fail_code=code, lf=self.lf)

    def assert_ca_is_trusted(self, code):
        cmd = ('security', 'dump-trust-settings', '-d',
               '|', 'grep', '-q', 'splintermail.localhost')
        rex(self.client, cmd, fail_code=code, lf=self.lf)

    def assert_ca_not_trusted(self, code):
        cmd = ('security', 'dump-trust-settings', '-d',
               '|', 'grep', '-q', 'splintermail.localhost')
        rex(self.client, cmd, expect_exit=1, fail_code=code, lf=self.lf)

    def assert_service_owner(self, uid, code):
        # make sure the service is bound to the port
        cmd = ('sudo', 'lsof', '-i', 'TCP:1995', '-F', 'cu')
        _, out, _ = rex(self.client, cmd, fail_code=code, lf=self.lf)
        # make sure the owner is correct
        if b'u%d\n'%uid not in out:
            err, msg = test_descr[code]
            raise err(msg)

    def assert_service_stopped(self, code):
        cmd = ('launchctl list | grep -q splintermail')
        rex(self.client, cmd, fail_code=code, expect_exit=1, lf=self.lf)


class LinuxTestSuite(UnixTestSuite):
    def __init__(self, client, builder):
        self.service_username = 'splintermail'
        self.client_context = TempRemoteLXC(client)
        super().__init__(builder)

    def assert_file_md5(self, f, md5, code):
        _, out, _ = rex(self.client, ('md5sum', f), fail_code=code, lf=self.lf)
        if out.decode('utf8').split()[0] != md5:
            err, msg = test_descr[code]
            raise err(msg)

    def assert_owner(self, f, uid, code):
        cmd = ('stat', '-c', '%u', f)
        _, out, _ = rex(self.client, cmd, fail_code=code, lf=self.lf)
        if int(out.decode('utf8')) != uid:
            err, msg = test_descr[code]
            raise err(msg)

    def assert_perms(self, f, perms, code):
        cmd = ('stat', '-c', '%a', f)
        _, out, _ = rex(self.client, cmd, fail_code=code, lf=self.lf)
        if int(out.decode('utf8'), base=8) != perms:
            err, msg = test_descr[code]
            raise err(msg)

    def get_user_uid(self, username, code):
        cmd = ('getent', 'passwd', username)
        _, out, _ = rex(self.client, cmd, fail_code=code, lf=self.lf)
        return int(out.decode('utf8').split(':')[2])

    def assert_service_owner(self, uid, code):
        # make sure the service is bound to the port
        cmd = ('lsof', '-i', 'TCP:1995', '-F', 'cu')
        _, out, _ = rex(self.client, cmd, fail_code=code, lf=self.lf)
        # make sure the owner is correct
        if b'u%d\n'%uid not in out:
            err, msg = test_descr[code]
            raise err(msg)

    def assert_ca_is_trusted(self, code):
        script = b'''awk -v cmd='openssl x509 -noout -subject' '/BEGIN/{close(cmd)};{print | cmd}' < %s'''%(self.ca_list.encode('utf8'))
        rex(self.client, 'sh', fail_code=code, lf=self.lf)

    def assert_ca_not_trusted(self, code):
        script = b'''awk -v cmd='openssl x509 -noout -subject' '/BEGIN/{close(cmd)};{print | cmd}' < %s'''%(self.ca_list.encode('utf8'))
        rex(self.client, 'sh', fail_code=code, lf=self.lf)

    def assert_service_stopped(self, code):
        cmd = ('lsof', '-i', 'TCP:1995', '-F', 'cu')
        rex(self.client, cmd, expect_exit=1, fail_code=code, lf=self.lf)


class ArchTestSuite(LinuxTestSuite):
    def __init__(self, client, builder):
        self.client = client
        self.ca_list = '/etc/ssl/cert.pem'
        super().__init__(client, builder)

    def run_pkg_linter(self, code):
        # TODO implement this
        pass

    def install(self, code):
        # update package manager cache
        cmd = ('pacman', '-Sy')
        rex(self.client, cmd, fail_code=code, lf=self.lf)
        # install the package
        cmd = ('echo', 'y', '|', 'pacman', '-U', self.installer)
        rex(self.client, cmd, fail_code=code, lf=self.lf)

    def soft_uninstall(self, code):
        self.has_soft_uninstall = False

    def uninstall(self, code):
        # update package manager cache
        cmd = ('echo', 'y', '|', 'pacman', '-R', 'splintermail')
        rex(self.client, cmd, fail_code=code, lf=self.lf)

    def rageclean(self, code):
        # OK, technically archlinux does have this but it doesn't
        # work sequentially after the install step
        self.has_rageclean = False

    def start_non_automatic_service(self, code):
        cmd = ('systemctl', 'start', 'splintermail.service')
        rex(self.client, cmd, fail_code=code, lf=self.lf)

    def stop_non_automatic_service(self, code):
        cmd = ('systemctl', 'stop', 'splintermail.service')
        rex(self.client, cmd, fail_code=code, lf=self.lf)


class DebianTestSuite(LinuxTestSuite):
    def __init__(self, client, builder):
        self.client = client
        self.ca_list = '/etc/ssl/certs/ca-certificates.crt'
        super().__init__(client, builder)

    def run_pkg_linter(self, code):
        # TODO implement this
        pass

    def install(self, code):
        # update package manager cache
        cmd = ('apt-get', 'update')
        rex(self.client, cmd, fail_code=code, lf=self.lf)
        # since openssl is a pre-dependency, it has to be manually installed
        cmd = ('apt-get', '-y', 'install', 'openssl')
        rex(self.client, cmd, fail_code=code, lf=self.lf)
        # this will fail due to dependency on ca-certificates
        rex(self.client, ('dpkg', '-i', self.installer),
                expect_exit=1, lf=self.lf)
        # this will install splintermail and dependencies
        cmd = ('apt-get', '-fy', 'install')
        rex(self.client, cmd, fail_code=code, lf=self.lf)

    def soft_uninstall(self, code):
        cmd = ('apt-get', '-y', 'remove', 'splintermail')
        rex(self.client, cmd, fail_code=code, lf=self.lf)

    def uninstall(self, code):
        self.has_uninstall = False

    def rageclean(self, code):
        cmd = ('apt-get', '-y', 'purge', 'splintermail')
        rex(self.client, cmd, fail_code=code, lf=self.lf)


class UbuntuTestSuite(DebianTestSuite):
    def install(self, code):
        # wait until apt is done with all lock files
        # (not even sure why this is necessary here)
        cmd = ('while lsof | grep -q apt ; do echo waiting; done')
        rex(self.client, cmd, fail_code=code, lf=self.lf)
        # update package manager cache
        cmd = ('apt-get', 'update')
        rex(self.client, cmd, fail_code=code, lf=self.lf)
        # wait until apt is done with all lock files
        cmd = ('while lsof | grep -q apt ; do echo waiting; done')
        rex(self.client, cmd, fail_code=code, lf=self.lf)
        # ubuntu has ca-certificates and openssl installed by default
        cmd = ('dpkg', '-i', self.installer)
        rex(self.client, cmd, expect_exit=0, lf=self.lf)


class FedoraTestSuite(LinuxTestSuite):
    def __init__(self, client, builder):
        self.client = client
        self.ca_list = '/etc/ssl/certs/ca-bundle.crt'
        super().__init__(client, builder)

    def run_pkg_linter(self, code):
        # TODO implement this
        pass

    def install(self, code):
        # update package manager cache
        cmd = ('dnf', 'check-update', '||', 'true')
        rex(self.client, cmd, fail_code=code, lf=self.lf)
        # install package
        cmd = ('dnf', 'install', '-y', self.installer)
        rex(self.client, cmd, fail_code=code, lf=self.lf)

    def soft_uninstall(self, code):
        self.has_soft_uninstall = False

    def uninstall(self, code):
        cmd = ('dnf', 'remove', '-y', 'splintermail')
        rex(self.client, cmd, fail_code=code, lf=self.lf)

    def rageclean(self, code):
        self.has_rageclean = False

    def start_non_automatic_service(self, code):
        cmd = ('systemctl', 'start', 'splintermail.service')
        rex(self.client, cmd, fail_code=code, lf=self.lf)

    def stop_non_automatic_service(self, code):
        cmd = ('systemctl', 'stop', 'splintermail.service')
        rex(self.client, cmd, fail_code=code, lf=self.lf)

if __name__ == '__main__':

    if "--update" in argv:
        exit(update_containers())

    do_install = "--install" in argv

    v = '0.2.0'

    win_b = WindowsBuilder('windowsdev', v)
    deb_b = DebianBuilder('debian9-amd64-d', v)
    fed_b = FedoraBuilder('fedora27-amd64-d', v)
    osx_b = OSXBuilder('osxdev', v)
    arch_b = ArchBuilder('arch-amd64-b', v)
    # 32bitters
    deb32_b = DebianBuilder('debian9-x86-d', v)
    fed32_b = FedoraBuilder('fedora27-x86-d', v)


    builders = [deb_b, fed_b, osx_b, arch_b, win_b, deb32_b, fed32_b]

    for builder in builders:
        builder.start()

    for builder in builders:
        builder.join()
        print('builder %s took %.2fs'%(builder.dev, builder.buildtime))

    # find out if the builds succeeded
    build_succeed = True
    for builder in builders:
        if builder.success is not True:
            print('failed to build %s'%(builder.dev))
            build_succeed = False

    suites = []
    if do_install:
        # suites.append( FedoraTestSuite('fedora27-amd64-c', fed_b) )
        # suites.append( FedoraTestSuite('fedora28-amd64-c', fed_b) )
        # suites.append( DebianTestSuite('debian9-amd64-c', deb_b) )
        suites.append( OSXTestSuite('osxclient', osx_b) )
        #suites.append( ArchTestSuite('arch-amd64-c', arch_b) )
        #suites.append( UbuntuTestSuite('ubuntu1804-amd64-c', deb_b) )
        # 32bitters
        #suites.append( DebianTestSuite('debian9-x86-c', deb32_b) )
        #suites.append( FedoraTestSuite('fedora27-x86-c', fed32_b) )
        #suites.append( FedoraTestSuite('fedora28-x86-c', fed32_b) )
        #suites.append( UbuntuTestSuite('ubuntu1804-x86-c', deb32_b) )
        # windows
        #suites.append( WindowsTestSuite('windowsclient', win_b) )
        #suites.append( WindowsTestSuite('windows10client', win_b) )

    if do_install:
        print('launching installer tests')

    for suite in suites:
        suite.start()
        # weird failures if I launch too many LXC containers / VMs at once
        sleep(1)

    for suite in suites:
        suite.join()
        print('test %s took %.2fs'%(suite.client, suite.testtime))

    # find out if the installs succeeded
    install_succeed = True
    for suite in suites:
        if suite.succeeded is not True:
            print('failed to install to %s'%suite.client, file=stderr)
            install_succeed = False

    if build_succeed and install_succeed:
        print('PASS')
    else:
        print('FAIL')

