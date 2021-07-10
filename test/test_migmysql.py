import os
import sys
import mariadb

test_files = os.path.join(os.path.dirname(__file__), "files")
test_migrations = os.path.join(test_files, "migrations")


def main(migmysql_path):
    with mariadb.mariadb(None, None, None) as runner:

        def verify_level(level):
            if level == -1:
                # ensure no databases are configured at all
                out = runner.run("show databases;", None)
                assert b"\nsplintermail\n" not in out, out.decode('utf8')
                return

            if level == 0:
                out = runner.run("show databases;", None)
                assert b"\nsplintermail\n" in out, out.decode('utf8')
                out = runner.run("show tables;", "splintermail")
                assert b"\nasdf\n" not in out, out.decode('utf8')
                assert b"\nasdf2\n" not in out, out.decode('utf8')
                return

            if level == 1:
                out = runner.run("show databases;", None)
                assert b"\nsplintermail\n" in out, out.decode('utf8')
                out = runner.run("show tables;", "splintermail")
                assert b"\nasdf\n" in out, out.decode('utf8')
                assert b"\nasdf2\n" not in out, out.decode('utf8')
                out = runner.run("describe asdf;", "splintermail")
                assert b"\nqwer\t" not in out, out.decode('utf8')
                return

            if level == 2:
                out = runner.run("show databases;", None)
                assert b"\nsplintermail\n" in out, out.decode('utf8')
                out = runner.run("show tables;", "splintermail")
                assert b"\nasdf\n" in out, out.decode('utf8')
                assert b"\nasdf2\n" not in out, out.decode('utf8')
                out = runner.run("describe asdf;", "splintermail")
                assert b"\nqwer\t" in out, out.decode('utf8')
                return

            if level == 3:
                out = runner.run("show databases;", None)
                assert b"\nsplintermail\n" in out, out.decode('utf8')
                out = runner.run("show tables;", "splintermail")
                assert b"\nasdf\n" in out, out.decode('utf8')
                assert b"\nasdf2\n" in out, out.decode('utf8')
                out = runner.run("describe asdf;", "splintermail")
                assert b"\nqwer\t" in out, out.decode('utf8')
                return

            raise ValueError(f"unknown level {level}")

        def migmysql(level=None):
            return mariadb.migmysql(
                migmysql_path,
                test_migrations,
                level=level,
                socket=runner.sockpath,
            )

        # before running
        verify_level(-1)

        oldtgt = -1
        for tgt in [0, 1, 2, 3, 2, 1, 0, 2, 0, 1, 3, 1]:
            print(f"transitioning from {oldtgt} to {tgt}")
            oldtgt = tgt

            migmysql(tgt)
            verify_level(tgt)


if __name__ == "__main__":
    print(sys.argv)
    if len(sys.argv) > 1:
        migmysql_path = sys.argv[1]
    else:
        migmysql_path = "server/migmysql"
    main(migmysql_path)



