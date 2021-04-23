import os
import sys
import base64
import subprocess
import argparse

import mariadb

pysm_path = "./server/pysm"
sys.path.append(pysm_path)
import pysm

parent_path = os.path.dirname(__file__)
test_files = os.path.join(parent_path, "files")
migrations = os.path.join(parent_path, "..", "server", "migrations")
migmysql_path = "./server/migmysql"

class SMPHP:
    """
    A wrapper for running php scripts and getting text outputs.
    """

    def __init__(self, bin, sock):
        self.bin = bin
        self.sock = sock

    def run(self, script):
        cmd = [
            self.bin,
            "-n",
            "-d", "extension_dir=server/smphp",
            "-d", "extension=smphp.so",
            "-d", f"smphp.sql_sock={self.sock}",
            "-d", "date.timezone=GMT",
            "-d", "max_execution_time=30",
            "-d", "memory_limit=128M",
            "-d", "error_reporting=E_ALL | E_STRICT",
            "-d", "display_errors=1",
            "-d", "log_errors=1",
            "-r", script
        ]
        p = subprocess.run(cmd, stdout=subprocess.PIPE, check=True)
        return p.stdout

    def valid_email(self, email):
        script = r"""
            $error = smphp_valid_email("%s");
            if($error === NULL){
                print("internal server error");
            }else{
                print($error);
            }
        """%email
        return self.run(script).decode('utf8')

    def valid_password(self, password):
        script = r"""
            $error = smphp_valid_password("%s");
            if($error === NULL){
                print("internal server error");
            }else{
                print($error);
            }
        """%password
        return self.run(script).decode('utf8')

    def create_account(self, email, password):
        script = r"""
            list($uuid, $error) = smphp_create_account("%s", "%s");
            print(base64_encode($uuid));
            print("\n");
            if($error === NULL){
                print("internal server error");
            }else{
                print($error);
            }
        """%(email, password)
        lines = self.run(script).split(b"\n", 2)
        assert len(lines) == 2, f"expected 2 lines but got {len(lines)}"
        uuid = base64.b64decode(lines[0])
        error = lines[1].decode('utf8')
        return uuid, error

    def login(self, email, password):
        script = r"""
            list($uuid, $error) = smphp_login("%s", "%s");
            print(base64_encode($uuid));
            print("\n");
            if($error === NULL){
                print("internal server error");
            }else{
                print($error);
            }
        """%(email, password)
        lines = self.run(script).split(b"\n", 2)
        assert len(lines) == 2, f"expected 2 lines but got {len(lines)}"
        uuid = base64.b64decode(lines[0])
        error = lines[1].decode('utf8')
        return uuid, error

def run_tests(php, smsql):
    """
    Compare the behavior of the smphp plugin against the behavior of the pysm
    plugin, since the python docs are far clearer on how to write a correct
    plugin.
    """

    email = "asdf@splintermail.com"
    password = "passwordpassword"

    # login in to an account that does not exist
    _, error = php.login(email, password)
    assert error == "bad credentials", error

    # create a new account without conflict
    uuid, error = php.create_account(email, password)
    assert error == "", error
    uuid_exp = smsql.get_uuid(email)
    assert uuid == uuid_exp, "account did not get created properly"

    # login in to an account correctly
    uuid, error = php.login(email, password)
    assert error == "", f"got error={error}"
    assert uuid == uuid_exp, \
        f"uid mismatch: {pysm.to_fsid(uuid)} == {pysm.to_fsid(uuid_exp)}"

    # login in to an account with the wrong password
    _, error = php.login(email, "")
    assert error == "bad credentials", error

    # create an account with a username conflict
    _, error = php.create_account(email, password)
    assert error == "username not available", error

    # create an account with an alias conflict
    alias = smsql.add_random_alias(uuid)
    assert alias is not None
    _, error = php.create_account(alias, password)
    assert error == "username not available", error

    # create an account with invalid email
    _, error = php.create_account("a"*200 + "@splintermail.com", password)
    assert error == "email too long", error
    _, error = php.create_account("asdf", password)
    assert error == "email must end in @splintermail.com", error
    _, error = php.create_account("@splintermail.com", password)
    assert error == "empty username", error
    _, error = php.create_account("ásdf@splintermail.com", password)
    assert error == "invalid characters in email", error

    # create an account with invalid password
    _, error = php.create_account(email, "a"*73)
    assert error == "password must not exceed 72 characters in length", error
    _, error = php.create_account(email, "a"*15)
    assert error == "password must be at least 16 characters in length", error
    _, error = php.create_account(email, "á"*16)
    assert error == "invalid characters in password", error
    _, error = php.create_account(email, " " + password)
    assert error == "no leading or trailing spaces in password", error
    _, error = php.create_account(email, password + " ")
    assert error == "no leading or trailing spaces in password", error

    # make smphp_valid_email works
    error = php.valid_email("a"*200 + "@splintermail.com")
    assert error == "email too long", error

    # make smphp_valid_password works
    error = php.valid_password("a"*73)
    assert error == "password must not exceed 72 characters in length", error

def main(php_bin):
    with mariadb.mariadb(None, migrations, migmysql_path) as script_runner:
        sock = script_runner.sockpath
        php = SMPHP(php_bin, sock)
        with pysm.SMSQL(sock=sock) as smsql:
            run_tests(php, smsql)
    print("PASS")

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--php-bin", default="php")
    args = parser.parse_args()
    main(args.php_bin)
