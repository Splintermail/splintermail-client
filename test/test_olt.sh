#!/bin/sh

# get path to olt binary
if [ -z "$1" ] ; then
    echo "usage: $0 PATH/TO/OLT_BINARY"
    exit 1
fi
OLT="$1"

set_test_perms1(){
cat > test_perms << EOF
a:root:root:0644
b:root:root:0644
c:root:root:0644
d/:root:root:0755
    da:root:root:0644
    db:root:root:0644
    dc:root:root:0644
    dd:root:root:0644
l:root:root:0777
EOF
}

set_test_perms2(){
cat > test_perms << EOF
a:root:root:0644
b:root:root:0644
c:root:root:0644
d/:root:root:0755
    da:root:root:0644
    db:root:root:0644
    dc:root:root:0644
    dd:root:root:0644
e/:root:root:0755
    ee/:root:root:0755
        x:root:root:0755
        y:root:root:0755
        z:root:root:0755
l:root:root:0777
EOF
}

clean_files(){
    rm -f test_perms
    rm -f test_perms.old
    rm -rf test_overlay
    rm -rf test_dest
}

# start clean
clean_files

mkdir -p test_overlay
touch test_overlay/a
touch test_overlay/b
touch test_overlay/c
mkdir -p test_overlay/d
touch test_overlay/d/da
touch test_overlay/d/db
touch test_overlay/d/dc
touch test_overlay/d/dd
ln -s fake_file test_overlay/l

testname="test check mode against valid settings" ; echo "--- $testname ---"
set_test_perms1
if ! "$OLT" -d test_perms test_overlay ; then
    echo "failed $testname"
    exit 1
fi

testname="test check mode: perms missing in overlay" ; echo "--- $testname ---"
set_test_perms2
if "$OLT" -d test_perms test_overlay ; then
    echo "failed $testname"
    exit 1
fi

testname="test check mode: bad symbolic link mode" ; echo "--- $testname ---"
set_test_perms1
sed -i -e 's/^l:root:root:0777$/l:root:root:0666/' test_perms
if "$OLT" -d test_perms test_overlay ; then
    echo "failed $testname"
    exit 1
fi

testname="test check mode: overlay missing in perms" ; echo "--- $testname ---"
sed -i -e '/^l:/d' test_perms
if "$OLT" -d test_perms test_overlay ; then
    echo "failed $testname"
    exit 1
fi

testname="check install succeeds" ; echo "--- $testname ---"
set_test_perms1
mkdir -p test_dest
if ! "$OLT" -d test_perms test_overlay test_dest ; then
    echo "failed $testname"
    exit 1
fi

testname="check set mode" ; echo "--- $testname ---"
sed -i -e 's/^a:root:root:0644$/a:root:root:0600/' test_perms
if ! "$OLT" -d test_perms test_overlay test_dest ; then
    echo "failed $testname"
    exit 1
fi
if [ "`stat -c '%a' test_dest/a`" != "600" ] ; then
    echo "failed $testname"
    exit 1
fi

testname="check set owner" ; echo "--- $testname ---"
sed -i -e 's/^b:root:root:0644$/b:bin:bin:0644/' test_perms
if ! "$OLT" -d test_perms test_overlay test_dest ; then
    echo "failed $testname"
    exit 1
fi
if [ "`stat -c '%G:%U' test_dest/b`" != "bin:bin" ] ; then
    echo "failed $testname"
    exit 1
fi

testname="check install updated files" ; echo "--- $testname ---"
echo "update" > test_overlay/c
if ! "$OLT" -d test_perms test_overlay test_dest ; then
    echo "failed $testname"
    exit 1
fi
if [ "`cat test_dest/c`" != "update" ] ; then
    echo "failed $testname"
    exit 1
fi

testname="check install updated links" ; echo "--- $testname ---"
ln -sf update test_overlay/l
if ! "$OLT" -d test_perms test_overlay test_dest ; then
    echo "failed $testname"
    exit 1
fi
if [ "`readlink test_dest/l`" != "update" ] ; then
    echo "failed $testname"
    exit 1
fi

testname="check no non-update install files" ; echo "--- $testname ---"
echo "nonupdate" > test_dest/c
if ! "$OLT" -d test_perms test_overlay test_dest ; then
    echo "failed $testname"
    exit 1
fi
if [ "`cat test_dest/c`" != "nonupdate" ] ; then
    echo "failed $testname"
    exit 1
fi

testname="test replace file with dir" ; echo "--- $testname ---"
rm test_overlay/a
mkdir test_overlay/a
sed -i -e 's/^a:.*/a\/:root:root:0700/' test_perms
if ! "$OLT" -d test_perms test_overlay test_dest ; then
    echo "failed $testname"
    exit 1
fi
if [ ! -d "test_dest/a" ] ; then
    echo "failed $testname"
    exit 1
fi

testname="test replace dir with file" ; echo "--- $testname ---"
rmdir test_overlay/a
touch test_overlay/a
sed -i -e 's/^a\/:.*/a:root:root:0644/' test_perms
if ! "$OLT" -d test_perms test_overlay test_dest ; then
    echo "failed $testname"
    exit 1
fi
if [ ! -f "test_dest/a" ] ; then
    echo "failed $testname"
    exit 1
fi

testname="check no non-update install links" ; echo "--- $testname ---"
ln -sf nonupdate test_dest/l
if ! "$OLT" -d test_perms test_overlay test_dest ; then
    echo "failed $testname"
    exit 1
fi
if [ "`readlink test_dest/l`" != "nonupdate" ] ; then
    echo "failed $testname"
    exit 1
fi

testname="check delete files" ; echo "--- $testname ---"
set_test_perms2
mkdir -p test_overlay/e/ee
touch test_overlay/e/ee/x
touch test_overlay/e/ee/y
touch test_overlay/e/ee/z
if ! "$OLT" -d test_perms test_overlay test_dest ; then
    echo "failed $testname"
    exit 1
fi
# make sure our filecount is what we think it is
if [ "`find test_dest | wc -l`" != 15 ]; then
    echo "failed $testname"
    exit 1
fi
# delete some files from perms and overlay
rm test_overlay/e/ee/x
sed -i -e '/^ *x:/d' test_perms
rm test_overlay/e/ee/y
sed -i -e '/^ *y:/d' test_perms
rm test_overlay/c
sed -i -e '/^c:/d' test_perms
rm test_overlay/d/dd
sed -i -e '/^ *dd:/d' test_perms
echo "--- ---"
if ! "$OLT" -d test_perms test_overlay test_dest ; then
    echo "failed $testname"
    exit 1
fi
# make sure we deleted what we think we deleted
if     [ -e "test_dest/e/ee/x" ] \
    || [ -e "test_dest/e/ee/y" ] \
    || [ -e "test_dest/c" ] \
    || [ -e "test_dest/d/dd" ] \
    || [ "`find test_dest | wc -l`" != 11 ]; then
    echo "failed $testname"
    exit 1
fi


testname="check trim empty dirs" ; echo "--- $testname ---"
# delete some files from perms and overlay
rm test_overlay/e/ee/z
sed -i -e '/^ *z:/d' test_perms
rmdir test_overlay/e/ee
sed -i -e '/^ *ee\/:/d' test_perms
if ! "$OLT" -d test_perms test_overlay test_dest ; then
    echo "failed $testname"
    exit 1
fi
# make sure we deleted what we think we deleted
if     [ -e "test_dest/e/ee/z" ] \
    || [ -e "test_dest/e/ee" ] \
    || [ "`find test_dest | wc -l`" != 9 ]; then
    echo "failed $testname"
    exit 1
fi

testname="check don't trim non-empty dirs" ; echo "--- $testname ---"
# delete some files from perms and overlay
touch test_dest/d/untracked
rm test_overlay/d/da
sed -i -e '/^ *da:/d' test_perms
rm test_overlay/d/db
sed -i -e '/^ *db:/d' test_perms
rm test_overlay/d/dc
sed -i -e '/^ *dc:/d' test_perms
rmdir test_overlay/d
sed -i -e '/^ *d\/:/d' test_perms
if ! "$OLT" -d test_perms test_overlay test_dest ; then
    echo "failed $testname"
    exit 1
fi
# make sure we deleted what we think we deleted
if     [ -e "test_dest/d/da" ] \
    || [ -e "test_dest/d/db" ] \
    || [ -e "test_dest/d/dc" ] \
    || [ ! -e "test_dest/d" ] \
    || [ ! -e "test_dest/d/untracked" ] \
    || [ "`find test_dest | wc -l`" != 7 ]; then
    echo "failed $testname"
    exit 1
fi

echo PASS
clean_files
exit 0
