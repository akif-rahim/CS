#!/bin/sh
#
# A simple test suite for cs.
#
# Copyright (C) 2002-2007 Andrew Tridgell
# Copyright (C) 2009-2012 Joel Rosdahl
#
# This program is free software; you can redistribute it and/or modify it under
# the terms of the GNU General Public License as published by the Free Software
# Foundation; either version 3 of the License, or (at your option) any later
# version.
#
# This program is distributed in the hope that it will be useful, but WITHOUT
# ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
# FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
# details.
#
# You should have received a copy of the GNU General Public License along with
# this program; if not, write to the Free Software Foundation, Inc., 51
# Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

unset CS_BASEDIR
unset CS_CC
unset CS_COMPILERCHECK
unset CS_COMPRESS
unset CS_CPP2
unset CS_CACHE_DIR
unset CS_DISABLE
unset CS_EXTENSION
unset CS_EXTRAFILES
unset CS_HARDLINK
unset CS_HASHDIR
unset CS_LOGFILE
unset CS_NLEVELS
unset CS_NODIRECT
unset CS_NOSTATS
unset CS_PATH
unset CS_PREFIX
unset CS_READONLY
unset CS_RECACHE
unset CS_SLOPPINESS
unset CS_TEMPDIR
unset CS_UMASK
unset CS_UNIFY

export CS_CLOUD_MODE=offline

test_failed() {
    echo "SUITE: \"$testsuite\", TEST: \"$testname\" - $1"
    $CS -s
    cd ..
    echo TEST FAILED
    echo "Test data and log file have been left in $TESTDIR"
    exit 1
}

randcode() {
    outfile="$1"
    nlines=$2
    i=0
    (
    while [ $i -lt $nlines ]; do
        echo "int foo$nlines$i(int x) { return x; }"
        i=`expr $i + 1`
    done
    ) >> "$outfile"
}


getstat() {
    stat="$1"
    value=`$CS -s | grep "$stat" | cut -c34-`
    echo $value
}

checkstat() {
    stat="$1"
    expected_value="$2"
    value=`getstat "$stat"`
    if [ "$expected_value" != "$value" ]; then
        test_failed "Expected \"$stat\" to be $expected_value, got $value"
    fi
}

compare_file() {
    if ! cmp -s "$1" "$2"; then
        test_failed "Files differ: $1 != $2"
    fi
}

checkfile() {
    if [ ! -f $1 ]; then
        test_failed "$1 not found"
    fi
    if [ "`cat $1`" != "$2" ]; then
        test_failed "Bad content of $1.\nExpected: $2\nActual: `cat $1`"
    fi
}

checkfilecount() {
    expected=$1
    pattern=$2
    dir=$3
    actual=`find $dir -name "$pattern" | wc -l`
    if [ $actual -ne $expected ]; then
        test_failed "Found $actual (expected $expected) $pattern files in $dir"
    fi
}

sed_in_place() {
    expr=$1
    shift
    for file in $*; do
        sed "$expr" > ${file}.sed < $file
        mv ${file}.sed $file
    done
}

backdate() {
    touch -t 199901010000 "$@"
}

run_suite() {
    rm -rf $CS_CACHE_DIR
    CS_NODIRECT=1
    export CS_NODIRECT

    echo "starting testsuite $1"
    testsuite=$1

    ${1}_suite

    testname="the tmp directory should be empty"
    if [ -d $CS_CACHE_DIR/tmp ] && [ "`find $CS_CACHE_DIR/tmp -type f | wc -l`" -gt 0 ]; then
        test_failed "$CS_CACHE_DIR/tmp is not empty"
    fi
}

base_tests() {
    checkstat 'cache hit (preprocessed)' 0
    checkstat 'cache miss' 0
    checkstat 'files in cache' 0

    j=1
    rm -f *.c
    while [ $j -lt 32 ]; do
        randcode test$j.c $j
        j=`expr $j + 1`
    done

    CS_DISABLE=1 $COMPILER -c -o reference_test1.o test1.c

    testname="BASIC"
    $CS_COMPILE -c test1.c
    checkstat 'cache hit (preprocessed)' 0
    checkstat 'cache miss' 1
    checkstat 'files in cache' 2
    compare_file reference_test1.o test1.o

    testname="BASIC2"
    $CS_COMPILE -c test1.c
    checkstat 'cache hit (preprocessed)' 1
    checkstat 'cache miss' 1
    checkstat 'files in cache' 2
    compare_file reference_test1.o test1.o

    testname="debug"
    $CS_COMPILE -c test1.c -g
    checkstat 'cache hit (preprocessed)' 1
    checkstat 'cache miss' 2
    checkstat 'files in cache' 3

    testname="debug2"
    $CS_COMPILE -c test1.c -g
    checkstat 'cache hit (preprocessed)' 2
    checkstat 'cache miss' 2

    testname="output"
    $CS_COMPILE -c test1.c -o foo.o
    checkstat 'cache hit (preprocessed)' 3
    checkstat 'cache miss' 2
    compare_file reference_test1.o foo.o

    testname="link"
    $CS_COMPILE test1.c -o test 2> /dev/null
    checkstat 'called for link' 1

    testname="linkobj"
    $CS_COMPILE foo.o -o test 2> /dev/null
    checkstat 'called for link' 2

    testname="preprocessing"
    $CS_COMPILE -E -c test1.c > /dev/null 2>&1
    checkstat 'called for preprocessing' 1

    testname="multiple"
    $CS_COMPILE -c test1.c test2.c
    checkstat 'multiple source files' 1

    testname="find"
    $CS blahblah -c test1.c 2> /dev/null
    checkstat "couldn't find the compiler" 1

    testname="bad"
    $CS_COMPILE -c test1.c -I 2> /dev/null
    checkstat 'bad compiler arguments' 1

    testname="unsupported source language"
    ln -f test1.c test1.ccc
    $CS_COMPILE -c test1.ccc 2> /dev/null
    checkstat 'unsupported source language' 1

    testname="unsupported"
    $CS_COMPILE -M foo -c test1.c > /dev/null 2>&1
    checkstat 'unsupported compiler option' 1

    testname="stdout"
    $CS echo foo -c test1.c > /dev/null
    checkstat 'compiler produced stdout' 1

    testname="non-regular"
    mkdir testd
    $CS_COMPILE -o testd -c test1.c > /dev/null 2>&1
    rmdir testd > /dev/null 2>&1
    checkstat 'output to a non-regular file' 1

    testname="no-input"
    $CS_COMPILE -c -O2 2> /dev/null
    checkstat 'no input file' 1

    testname="CS_DISABLE"
    CS_DISABLE=1 $CS_COMPILE -c test1.c 2> /dev/null
    checkstat 'cache hit (preprocessed)' 3
    $CS_COMPILE -c test1.c
    checkstat 'cache hit (preprocessed)' 4

    testname="CS_CPP2"
    CS_CPP2=1 $CS_COMPILE -c test1.c -O -O
    checkstat 'cache hit (preprocessed)' 4
    checkstat 'cache miss' 3
    CS_DISABLE=1 $COMPILER -c test1.c -o reference_test1.o -O -O
    compare_file reference_test1.o test1.o

    CS_CPP2=1 $CS_COMPILE -c test1.c -O -O
    checkstat 'cache hit (preprocessed)' 5
    checkstat 'cache miss' 3
    compare_file reference_test1.o test1.o

    testname="CS_NOSTATS"
    CS_NOSTATS=1 $CS_COMPILE -c test1.c -O -O
    checkstat 'cache hit (preprocessed)' 5
    checkstat 'cache miss' 3

    testname="CS_RECACHE"
    CS_RECACHE=1 $CS_COMPILE -c test1.c -O -O
    checkstat 'cache hit (preprocessed)' 5
    checkstat 'cache miss' 4
    compare_file reference_test1.o test1.o

    # strictly speaking should be 5 - RECACHE causes a double counting!
    checkstat 'files in cache' 6
    $CS -c > /dev/null
    checkstat 'files in cache' 5

    testname="CS_HASHDIR"
    CS_HASHDIR=1 $CS_COMPILE -c test1.c -O -O
    checkstat 'cache hit (preprocessed)' 5
    checkstat 'cache miss' 5
    compare_file reference_test1.o test1.o

    CS_HASHDIR=1 $CS_COMPILE -c test1.c -O -O
    checkstat 'cache hit (preprocessed)' 6
    checkstat 'cache miss' 5
    checkstat 'files in cache' 6
    compare_file reference_test1.o test1.o

    testname="comments"
    echo '/* a silly comment */' > test1-comment.c
    cat test1.c >> test1-comment.c
    $CS_COMPILE -c test1-comment.c
    rm -f test1-comment*
    checkstat 'cache hit (preprocessed)' 6
    checkstat 'cache miss' 6

    testname="CS_UNIFY"
    CS_UNIFY=1 $CS_COMPILE -c test1.c
    checkstat 'cache hit (preprocessed)' 6
    checkstat 'cache miss' 7
    mv test1.c test1-saved.c
    echo '/* another comment */' > test1.c
    cat test1-saved.c >> test1.c
    CS_UNIFY=1 $CS_COMPILE -c test1.c
    mv test1-saved.c test1.c
    checkstat 'cache hit (preprocessed)' 7
    checkstat 'cache miss' 7
    CS_DISABLE=1 $COMPILER -c test1.c -o reference_test1.o
    compare_file reference_test1.o test1.o


    testname="cache-size"
    for f in *.c; do
        $CS_COMPILE -c $f
    done
    checkstat 'cache hit (preprocessed)' 8
    checkstat 'cache miss' 37
    checkstat 'files in cache' 38

    $CS -C >/dev/null

    testname="cpp call"
    $CS_COMPILE -c test1.c -E > test1.i
    checkstat 'cache hit (preprocessed)' 8
    checkstat 'cache miss' 37

    testname="direct .i compile"
    $CS_COMPILE -c test1.c
    checkstat 'cache hit (preprocessed)' 8
    checkstat 'cache miss' 38

    $CS_COMPILE -c test1.i
    checkstat 'cache hit (preprocessed)' 9
    checkstat 'cache miss' 38

    $CS_COMPILE -c test1.i
    checkstat 'cache hit (preprocessed)' 10
    checkstat 'cache miss' 38

    testname="-x c"
    $CS_COMPILE -x c -c test1.ccc
    checkstat 'cache hit (preprocessed)' 10
    checkstat 'cache miss' 39
    $CS_COMPILE -x c -c test1.ccc
    checkstat 'cache hit (preprocessed)' 11
    checkstat 'cache miss' 39

    testname="-xc"
    $CS_COMPILE -xc -c test1.ccc
    checkstat 'cache hit (preprocessed)' 12
    checkstat 'cache miss' 39

    testname="-x none"
    $CS_COMPILE -x assembler -x none -c test1.c
    checkstat 'cache hit (preprocessed)' 13
    checkstat 'cache miss' 39

    testname="-x unknown"
    $CS_COMPILE -x unknown -c test1.c 2>/dev/null
    checkstat 'cache hit (preprocessed)' 13
    checkstat 'cache miss' 39
    checkstat 'unsupported source language' 2

    testname="-D not hashed"
    $CS_COMPILE -DNOT_AFFECTING=1 -c test1.c 2>/dev/null
    checkstat 'cache hit (preprocessed)' 14
    checkstat 'cache miss' 39

    if [ -x /usr/bin/printf ]; then
        /usr/bin/printf '#include <wchar.h>\nwchar_t foo[] = L"\xbf";\n' >latin1.c
        if CS_DISABLE=1 $COMPILER -c -finput-charset=latin1 latin1.c >/dev/null 2>&1; then
            testname="-finput-charset"
            CS_CPP2=1 $CS_COMPILE -c -finput-charset=latin1 latin1.c
            checkstat 'cache hit (preprocessed)' 14
            checkstat 'cache miss' 40
            $CS_COMPILE -c -finput-charset=latin1 latin1.c
            checkstat 'cache hit (preprocessed)' 15
            checkstat 'cache miss' 40
        fi
    fi

    testname="override path"
    $CS -Cz >/dev/null
    override_path=`pwd`/override_path
    rm -rf $override_path
    mkdir $override_path
    cat >$override_path/cc <<EOF
#!/bin/sh
touch override_path_compiler_executed
EOF
    chmod +x $override_path/cc
    CS_PATH=$override_path $CS cc -c test1.c
    if [ ! -f override_path_compiler_executed ]; then
        test_failed "CS_PATH had no effect"
    fi

    testname="compilercheck=mtime"
    $CS -Cz >/dev/null
    cat >compiler.sh <<EOF
#!/bin/sh
CS_DISABLE=1 # If $COMPILER happens to be a cs symlink...
export CS_DISABLE
exec $COMPILER "\$@"
# A comment
EOF
    chmod +x compiler.sh
    backdate compiler.sh
    CS_COMPILERCHECK=mtime $CS ./compiler.sh -c test1.c
    checkstat 'cache hit (preprocessed)' 0
    checkstat 'cache miss' 1
    sed_in_place 's/comment/yoghurt/' compiler.sh # Don't change the size
    chmod +x compiler.sh
    backdate compiler.sh
    CS_COMPILERCHECK=mtime $CS ./compiler.sh -c test1.c
    checkstat 'cache hit (preprocessed)' 1
    checkstat 'cache miss' 1
    touch compiler.sh
    CS_COMPILERCHECK=mtime $CS ./compiler.sh -c test1.c
    checkstat 'cache hit (preprocessed)' 1
    checkstat 'cache miss' 2

    testname="compilercheck=content"
    # ccache only needs '-z' here, but cs needs '-Cz' because it's just better at cache hits. :)
    $CS -Cz >/dev/null
    CS_COMPILERCHECK=content $CS ./compiler.sh -c test1.c
    checkstat 'cache hit (preprocessed)' 0
    checkstat 'cache miss' 1
    backdate compiler.sh
    CS_COMPILERCHECK=content $CS ./compiler.sh -c test1.c
    checkstat 'cache hit (preprocessed)' 1
    checkstat 'cache miss' 1
    echo "# Compiler upgrade" >>compiler.sh
    backdate compiler.sh
    CS_COMPILERCHECK=content $CS ./compiler.sh -c test1.c
    checkstat 'cache hit (preprocessed)' 1
    checkstat 'cache miss' 2

    testname="compilercheck=none"
    $CS -z >/dev/null
    backdate compiler.sh
    CS_COMPILERCHECK=none $CS ./compiler.sh -c test1.c
    checkstat 'cache hit (preprocessed)' 0
    checkstat 'cache miss' 1
    CS_COMPILERCHECK=none $CS ./compiler.sh -c test1.c
    checkstat 'cache hit (preprocessed)' 1
    checkstat 'cache miss' 1
    echo "# Compiler upgrade" >>compiler.sh
    CS_COMPILERCHECK=none $CS ./compiler.sh -c test1.c
    checkstat 'cache hit (preprocessed)' 2
    checkstat 'cache miss' 1

    testname="compilercheck=command"
    $CS -z >/dev/null
    backdate compiler.sh
    CS_COMPILERCHECK='echo %compiler%' $CS ./compiler.sh -c test1.c
    checkstat 'cache hit (preprocessed)' 0
    checkstat 'cache miss' 1
    echo "# Compiler upgrade" >>compiler.sh
    CS_COMPILERCHECK="echo ./compiler.sh" $CS ./compiler.sh -c test1.c
    checkstat 'cache hit (preprocessed)' 1
    checkstat 'cache miss' 1
    cat <<EOF >foobar.sh
#!/bin/sh
echo foo
echo bar
EOF
    chmod +x foobar.sh
    CS_COMPILERCHECK='./foobar.sh' $CS ./compiler.sh -c test1.c
    checkstat 'cache hit (preprocessed)' 1
    checkstat 'cache miss' 2
    CS_COMPILERCHECK='echo foo; echo bar' $CS ./compiler.sh -c test1.c
    checkstat 'cache hit (preprocessed)' 2
    checkstat 'cache miss' 2

    testname="compilercheck=unknown_command"
    $CS -z >/dev/null
    backdate compiler.sh
    CS_COMPILERCHECK="unknown_command" $CS ./compiler.sh -c test1.c 2>/dev/null
    if [ "$?" -eq 0 ]; then
        test_failed "Expected failure running unknown_command to verify compiler but was success"
    fi
    checkstat 'compiler check failed' 1

    testname="recache should remove previous .stderr"
    $CS -Cz >/dev/null
    $CS_COMPILE -c test1.c
    checkstat 'cache hit (preprocessed)' 0
    checkstat 'cache miss' 1
    num=`find $CS_CACHE_DIR -name '*.stderr' | wc -l`
    if [ $num -ne 0 ]; then
        test_failed "$num stderr files found, expected 0 (#1)"
    fi
    obj_file=`find $CS_CACHE_DIR -name '*.o'`
    stderr_file=`echo $obj_file | sed 's/..$/.stderr/'`
    echo "Warning: foo" >$stderr_file
    CS_RECACHE=1 $CS_COMPILE -c test1.c
    num=`find $CS_CACHE_DIR -name '*.stderr' | wc -l`
    if [ $num -ne 0 ]; then
        test_failed "$num stderr files found, expected 0 (#2)"
    fi

    testname="no object file"
    $CS -Cz >/dev/null
    cat <<'EOF' >test_no_obj.c
int test_no_obj;
EOF
    cat <<'EOF' >prefix-remove.sh
#!/bin/sh
"$@"
[ x$3 = x-o ] && rm $4
EOF
    chmod +x prefix-remove.sh
    CS_PREFIX=`pwd`/prefix-remove.sh $CS_COMPILE -c test_no_obj.c
    checkstat 'compiler produced no output' 1

    testname="empty object file"
    cat <<'EOF' >test_empty_obj.c
int test_empty_obj;
EOF
    cat <<'EOF' >prefix-empty.sh
#!/bin/sh
"$@"
[ x$3 = x-o ] && cp /dev/null $4
EOF
    chmod +x prefix-empty.sh
    CS_PREFIX=`pwd`/prefix-empty.sh $CS_COMPILE -c test_empty_obj.c
    checkstat 'compiler produced empty output' 1

    testname="stderr-files"
    $CS -Cz >/dev/null
    num=`find $CS_CACHE_DIR -name '*.stderr' | wc -l`
    if [ $num -ne 0 ]; then
        test_failed "$num stderr files found, expected 0"
    fi
    cat <<EOF >stderr.c
int stderr(void)
{
	/* Trigger warning by having no return statement. */
}
EOF
    checkstat 'files in cache' 0
    $CS_COMPILE -Wall -W -c stderr.c 2>/dev/null
    num=`find $CS_CACHE_DIR -name '*.stderr' | wc -l`
    if [ $num -ne 1 ]; then
        test_failed "$num stderr files found, expected 1"
    fi
    checkstat 'files in cache' 3

    testname="zero-stats"
    $CS -z > /dev/null
    checkstat 'cache hit (preprocessed)' 0
    checkstat 'cache miss' 0
    checkstat 'files in cache' 3

    testname="clear"
    $CS -C > /dev/null
    checkstat 'files in cache' 0

    # the profile options do not seem to work correctly with clang or gcc-llvm
    # on darwin machines
    darwin_llvm=0
    if [ $HOST_OS_APPLE -eq 1 ] && \
        [ $COMPILER_USES_LLVM -eq 1 ]; then
        darwin_llvm=1
    fi

    $COMPILER -c -fprofile-generate test1.c 2>/dev/null
    if [ $? -eq 0 ] && [ $darwin_llvm -eq 0 ]; then
        testname="profile-generate"
        $CS -Cz > /dev/null
        $CS_COMPILE -c -fprofile-generate test1.c
        checkstat 'cache hit (preprocessed)' 0
        checkstat 'cache miss' 1
        checkstat 'files in cache' 2
        $CS_COMPILE -c -fprofile-generate test1.c
        checkstat 'cache hit (preprocessed)' 1
        checkstat 'cache miss' 1
        checkstat 'files in cache' 2

        testname="profile-arcs"
        $CS_COMPILE -c -fprofile-arcs test1.c
        checkstat 'cache hit (preprocessed)' 1
        checkstat 'cache miss' 2
        checkstat 'files in cache' 3
        $CS_COMPILE -c -fprofile-arcs test1.c
        checkstat 'cache hit (preprocessed)' 2
        checkstat 'cache miss' 2
        checkstat 'files in cache' 3

        testname="profile-use"
        $CS_COMPILE -c -fprofile-use test1.c 2> /dev/null
        checkstat 'cache hit (preprocessed)' 2
        checkstat 'cache miss' 3
        checkstat 'files in cache' 5
        $CS_COMPILE -c -fprofile-use test1.c 2> /dev/null
        checkstat 'cache hit (preprocessed)' 3
        checkstat 'cache miss' 3
        checkstat 'files in cache' 5
    fi

    ##################################################################
    # Check that -Wp,-P disables ccache. (-P removes preprocessor information
    # in such a way that the object file from compiling the preprocessed file
    # will not be equal to the object file produced when compiling without
    # ccache.)
    testname="-Wp,-P"
    $CS -Cz >/dev/null
    $CS $COMPILER -c -Wp,-P test1.c
    checkstat 'cache hit (direct)' 0
    checkstat 'cache hit (preprocessed)' 0
    checkstat 'cache miss' 0
    checkstat 'unsupported compiler option' 1
    $CS $COMPILER -c -Wp,-P test1.c
    checkstat 'cache hit (direct)' 0
    checkstat 'cache hit (preprocessed)' 0
    checkstat 'cache miss' 0
    checkstat 'unsupported compiler option' 2
    $CS $COMPILER -c -Wp,-DFOO,-P,-DGOO test1.c
    checkstat 'cache hit (direct)' 0
    checkstat 'cache hit (preprocessed)' 0
    checkstat 'cache miss' 0
    checkstat 'unsupported compiler option' 3
    $CS $COMPILER -c -Wp,-DFOO,-P,-DGOO test1.c
    checkstat 'cache hit (direct)' 0
    checkstat 'cache hit (preprocessed)' 0
    checkstat 'cache miss' 0
    checkstat 'unsupported compiler option' 4

    ##################################################################

    rm -f test1.c
}

base_suite() {
    CS_COMPILE="$CS $COMPILER"
    base_tests
}

link_suite() {
    if [ `dirname $COMPILER` = . ]; then
        ln -s "$CS" $COMPILER
        CS_COMPILE="./$COMPILER"
        base_tests
        rm -f $COMPILER
    else
        echo "Compiler ($COMPILER) not taken from PATH -- not running link test"
    fi
}

hardlink_suite() {
    CS_COMPILE="$CS $COMPILER"
    CS_HARDLINK=1
    export CS_HARDLINK
    base_tests
    unset CS_HARDLINK
}

cpp2_suite() {
    CS_COMPILE="$CS $COMPILER"
    CS_CPP2=1
    export CS_CPP2
    base_tests
    unset CS_CPP2
}

nlevels4_suite() {
    CS_COMPILE="$CS $COMPILER"
    CS_NLEVELS=4
    export CS_NLEVELS
    base_tests
    unset CS_NLEVELS
}

nlevels1_suite() {
    CS_COMPILE="$CS $COMPILER"
    CS_NLEVELS=1
    export CS_NLEVELS
    base_tests
    unset CS_NLEVELS
}

direct_suite() {
    unset CS_NODIRECT

    ##################################################################
    # Create some code to compile.
    cat <<EOF >test.c
/* test.c */
#include "test1.h"
#include "test2.h"
EOF
    cat <<EOF >test1.h
#include "test3.h"
int test1;
EOF
    cat <<EOF >test2.h
int test2;
EOF
    cat <<EOF >test3.h
int test3;
EOF
    backdate test1.h test2.h test3.h

    ##################################################################
    # First compilation is a miss.
    testname="first compilation"
    $CS -z >/dev/null
    $CS $COMPILER -c test.c
    checkstat 'cache hit (direct)' 0
    checkstat 'cache hit (preprocessed)' 0
    checkstat 'cache miss' 1

    ##################################################################
    # Another compilation should now generate a direct hit.
    testname="direct hit"
    $CS -z >/dev/null
    $CS $COMPILER -c test.c
    checkstat 'cache hit (direct)' 1
    checkstat 'cache hit (preprocessed)' 0
    checkstat 'cache miss' 0

    ##################################################################
    # Check that corrupt manifest files are handled and rewritten.
    testname="corrupt manifest file"
    $CS -z >/dev/null
    manifest_file=`find $CS_CACHE_DIR -name '*.manifest'`
    rm $manifest_file
    touch $manifest_file
    $CS $COMPILER -c test.c
    checkstat 'cache hit (direct)' 0
    checkstat 'cache hit (preprocessed)' 1
    checkstat 'cache miss' 0
    $CS $COMPILER -c test.c
    checkstat 'cache hit (direct)' 1
    checkstat 'cache hit (preprocessed)' 1
    checkstat 'cache miss' 0

    ##################################################################
    # Compiling with CS_NODIRECT set should generate a preprocessed hit.
    testname="preprocessed hit"
    $CS -z >/dev/null
    CS_NODIRECT=1 $CS $COMPILER -c test.c
    checkstat 'cache hit (direct)' 0
    checkstat 'cache hit (preprocessed)' 1
    checkstat 'cache miss' 0

    ##################################################################
    # Test compilation of a modified include file.
    testname="modified include file"
    $CS -z >/dev/null
    echo "int test3_2;" >>test3.h
    backdate test3.h
    $CS $COMPILER -c test.c
    checkstat 'cache hit (direct)' 0
    checkstat 'cache hit (preprocessed)' 0
    checkstat 'cache miss' 1

    $CS $COMPILER -c test.c
    checkstat 'cache hit (direct)' 1
    checkstat 'cache hit (preprocessed)' 0
    checkstat 'cache miss' 1

    ##################################################################
    # A removed but previously compiled header file should be handled
    # gracefully.
    testname="missing header file"
    $CS -z >/dev/null

    mv test1.h test1.h.saved
    mv test3.h test3.h.saved
    cat <<EOF >test1.h
/* No more include of test3.h */
int test1;
EOF
    backdate test1.h

    $CS $COMPILER -c test.c
    checkstat 'cache hit (direct)' 0
    checkstat 'cache hit (preprocessed)' 0
    checkstat 'cache miss' 1

    $CS $COMPILER -c test.c
    checkstat 'cache hit (direct)' 1
    checkstat 'cache hit (preprocessed)' 0
    checkstat 'cache miss' 1

    # Restore
    mv test1.h.saved test1.h
    mv test3.h.saved test3.h

    rm -f other.d

    ##################################################################
    # Check calculation of dependency file names.
    $CS -Cz >/dev/null
    checkstat 'files in cache' 0
    mkdir test.dir
    for ext in .obj "" . .foo.bar; do
        testname="dependency file calculation from object file 'test$ext'"
        dep_file=test.dir/`echo test$ext | sed 's/\.[^.]*\$//'`.d
        $CS $COMPILER -MD -c test.c -o test.dir/test$ext
        rm -f $dep_file
        $CS $COMPILER -MD -c test.c -o test.dir/test$ext
        if [ ! -f $dep_file ]; then
            test_failed "$dep_file missing"
        fi
    done
    rm -rf test.dir
    checkstat 'files in cache' 13

    ##################################################################
    # Check that -Wp,-MD,file.d works.
    testname="-Wp,-MD"
    $CS -z >/dev/null
    $CS $COMPILER -c -Wp,-MD,other.d test.c
    checkstat 'cache hit (direct)' 0
    checkstat 'cache hit (preprocessed)' 0
    checkstat 'cache miss' 1
    checkfile other.d "test.o: test.c test1.h test3.h test2.h"
    CS_DISABLE=1 $COMPILER -c -Wp,-MD,other.d test.c -o reference_test.o
    compare_file reference_test.o test.o

    rm -f other.d

    $CS $COMPILER -c -Wp,-MD,other.d test.c
    checkstat 'cache hit (direct)' 1
    checkstat 'cache hit (preprocessed)' 0
    checkstat 'cache miss' 1
    checkfile other.d "test.o: test.c test1.h test3.h test2.h"
    compare_file reference_test.o test.o

    rm -f other.d

    ##################################################################
    # Check that -Wp,-MMD,file.d works.
    testname="-Wp,-MMD"
    $CS -C >/dev/null
    $CS -z >/dev/null
    $CS $COMPILER -c -Wp,-MMD,other.d test.c
    checkstat 'cache hit (direct)' 0
    checkstat 'cache hit (preprocessed)' 0
    checkstat 'cache miss' 1
    checkfile other.d "test.o: test.c test1.h test3.h test2.h"
    CS_DISABLE=1 $COMPILER -c -Wp,-MMD,other.d test.c -o reference_test.o
    compare_file reference_test.o test.o

    rm -f other.d

    $CS $COMPILER -c -Wp,-MMD,other.d test.c
    checkstat 'cache hit (direct)' 1
    checkstat 'cache hit (preprocessed)' 0
    checkstat 'cache miss' 1
    checkfile other.d "test.o: test.c test1.h test3.h test2.h"
    compare_file reference_test.o test.o

    rm -f other.d

    ##################################################################
    # Test some header modifications to get multiple objects in the manifest.
    testname="several objects"
    $CS -z >/dev/null
    for i in 0 1 2 3 4; do
        echo "int test1_$i;" >>test1.h
        backdate test1.h
        $CS $COMPILER -c test.c
        $CS $COMPILER -c test.c
    done
    checkstat 'cache hit (direct)' 5
    checkstat 'cache hit (preprocessed)' 0
    checkstat 'cache miss' 5

    ##################################################################
    # Check that -MD works.
    testname="-MD"
    $CS -z >/dev/null
    $CS $COMPILER -c -MD test.c
    checkstat 'cache hit (direct)' 0
    checkstat 'cache hit (preprocessed)' 0
    checkstat 'cache miss' 1
    checkfile test.d "test.o: test.c test1.h test3.h test2.h"
    CS_DISABLE=1 $COMPILER -c -MD test.c -o reference_test.o
    compare_file reference_test.o test.o


    rm -f test.d

    $CS $COMPILER -c -MD test.c
    checkstat 'cache hit (direct)' 1
    checkstat 'cache hit (preprocessed)' 0
    checkstat 'cache miss' 1
    checkfile test.d "test.o: test.c test1.h test3.h test2.h"
    compare_file reference_test.o test.o

    ##################################################################
    # Check the scenario of running a cs with direct mode on a cache
    # built up by a cs without direct mode support.
    testname="direct mode on old cache"
    $CS -z >/dev/null
    $CS -C >/dev/null
    CS_NODIRECT=1 $CS $COMPILER -c -MD test.c
    checkstat 'cache hit (direct)' 0
    checkstat 'cache hit (preprocessed)' 0
    checkstat 'cache miss' 1
    checkfile test.d "test.o: test.c test1.h test3.h test2.h"
    CS_DISABLE=1 $COMPILER -c -MD test.c -o reference_test.o
    compare_file reference_test.o test.o

    rm -f test.d

    CS_NODIRECT=1 $CS $COMPILER -c -MD test.c
    checkstat 'cache hit (direct)' 0
    checkstat 'cache hit (preprocessed)' 1
    checkstat 'cache miss' 1
    checkfile test.d "test.o: test.c test1.h test3.h test2.h"
    compare_file reference_test.o test.o

    rm -f test.d

    $CS $COMPILER -c -MD test.c
    checkstat 'cache hit (direct)' 0
    checkstat 'cache hit (preprocessed)' 2
    checkstat 'cache miss' 1
    checkfile test.d "test.o: test.c test1.h test3.h test2.h"
    compare_file reference_test.o test.o

    rm -f test.d

    $CS $COMPILER -c -MD test.c
    checkstat 'cache hit (direct)' 1
    checkstat 'cache hit (preprocessed)' 2
    checkstat 'cache miss' 1
    checkfile test.d "test.o: test.c test1.h test3.h test2.h"
    compare_file reference_test.o test.o

    ##################################################################
    # Check that -MF works.
    testname="-MF"
    $CS -C >/dev/null
    $CS -z >/dev/null
    $CS $COMPILER -c -MD -MF other.d test.c
    checkstat 'cache hit (direct)' 0
    checkstat 'cache hit (preprocessed)' 0
    checkstat 'cache miss' 1
    checkfile other.d "test.o: test.c test1.h test3.h test2.h"
    CS_DISABLE=1 $COMPILER -c -MD -MF other.d test.c -o reference_test.o
    compare_file reference_test.o test.o

    rm -f other.d

    $CS $COMPILER -c -MD -MF other.d test.c
    checkstat 'cache hit (direct)' 1
    checkstat 'cache hit (preprocessed)' 0
    checkstat 'cache miss' 1
    checkfile other.d "test.o: test.c test1.h test3.h test2.h"
    compare_file reference_test.o test.o

    ##################################################################
    # Check that a missing .d file in the cache is handled correctly.
    testname="missing dependency file"
    $CS -z >/dev/null
    $CS -C >/dev/null

    $CS $COMPILER -c -MD test.c
    checkstat 'cache hit (direct)' 0
    checkstat 'cache hit (preprocessed)' 0
    checkstat 'cache miss' 1
    checkfile other.d "test.o: test.c test1.h test3.h test2.h"
    CS_DISABLE=1 $COMPILER -c -MD test.c -o reference_test.o
    compare_file reference_test.o test.o

    $CS $COMPILER -c -MD test.c
    checkstat 'cache hit (direct)' 1
    checkstat 'cache hit (preprocessed)' 0
    checkstat 'cache miss' 1
    checkfile other.d "test.o: test.c test1.h test3.h test2.h"
    compare_file reference_test.o test.o

    find $CS_CACHE_DIR -name '*.d' -exec rm -f '{}' \;

    $CS $COMPILER -c -MD test.c
    checkstat 'cache hit (direct)' 1
    checkstat 'cache hit (preprocessed)' 1
    checkstat 'cache miss' 1
    checkfile other.d "test.o: test.c test1.h test3.h test2.h"
    compare_file reference_test.o test.o

    ##################################################################
    # Check that stderr from both the preprocessor and the compiler is emitted
    # in direct mode too.
    testname="cpp stderr"
    $CS -z >/dev/null
    $CS -C >/dev/null
cat <<EOF >cpp-warning.c
#if FOO
/* Trigger preprocessor warning about extra token after #endif. */
#endif FOO
int stderr(void)
{
	/* Trigger compiler warning by having no return statement. */
}
EOF
    $CS $COMPILER -Wall -W -c cpp-warning.c 2>stderr-orig.txt
    checkstat 'cache hit (direct)' 0
    checkstat 'cache hit (preprocessed)' 0
    checkstat 'cache miss' 1

    CS_NODIRECT=1
    export CS_NODIRECT
    $CS $COMPILER -Wall -W -c cpp-warning.c 2>stderr-cpp.txt
    unset CS_NODIRECT
    checkstat 'cache hit (direct)' 0
    checkstat 'cache hit (preprocessed)' 1
    checkstat 'cache miss' 1
    checkfile stderr-cpp.txt "`cat stderr-orig.txt`"

    $CS $COMPILER -Wall -W -c cpp-warning.c 2>stderr-mf.txt
    checkstat 'cache hit (direct)' 1
    checkstat 'cache hit (preprocessed)' 1
    checkstat 'cache miss' 1
    checkfile stderr-mf.txt "`cat stderr-orig.txt`"

    ##################################################################
    # Check that it is possible to compile and cache an empty source code file.
    testname="empty source file"
    $CS -Cz >/dev/null
    cp /dev/null empty.c
    $CS $COMPILER -c empty.c
    checkstat 'cache hit (direct)' 0
    checkstat 'cache hit (preprocessed)' 0
    checkstat 'cache miss' 1
    $CS $COMPILER -c empty.c
    checkstat 'cache hit (direct)' 1
    checkstat 'cache hit (preprocessed)' 0
    checkstat 'cache miss' 1

    ##################################################################
    # Check that empty include files are handled as well.
    testname="empty include file"
    $CS -Cz >/dev/null
    cp /dev/null empty.h
    cat <<EOF >include_empty.c
#include "empty.h"
EOF
    backdate empty.h
    $CS $COMPILER -c include_empty.c
    checkstat 'cache hit (direct)' 0
    checkstat 'cache hit (preprocessed)' 0
    checkstat 'cache miss' 1
    $CS $COMPILER -c include_empty.c
    checkstat 'cache hit (direct)' 1
    checkstat 'cache hit (preprocessed)' 0
    checkstat 'cache miss' 1

    ##################################################################
    # Check that direct mode correctly detects file name/path changes.
    testname="__FILE__ in source file"
    $CS -Cz >/dev/null
    cat <<EOF >file.c
#define file __FILE__
int test;
EOF
    $CS $COMPILER -c file.c
    checkstat 'cache hit (direct)' 0
    checkstat 'cache hit (preprocessed)' 0
    checkstat 'cache miss' 1
    $CS $COMPILER -c file.c
    checkstat 'cache hit (direct)' 1
    checkstat 'cache hit (preprocessed)' 0
    checkstat 'cache miss' 1
    $CS $COMPILER -c `pwd`/file.c
    checkstat 'cache hit (direct)' 1
    checkstat 'cache hit (preprocessed)' 0
    checkstat 'cache miss' 2

    testname="__FILE__ in include file"
    $CS -Cz >/dev/null
    cat <<EOF >file.h
#define file __FILE__
int test;
EOF
    backdate file.h
    cat <<EOF >file_h.c
#include "file.h"
EOF
    $CS $COMPILER -c file_h.c
    checkstat 'cache hit (direct)' 0
    checkstat 'cache hit (preprocessed)' 0
    checkstat 'cache miss' 1
    $CS $COMPILER -c file_h.c
    checkstat 'cache hit (direct)' 1
    checkstat 'cache hit (preprocessed)' 0
    checkstat 'cache miss' 1
    mv file_h.c file2_h.c
    $CS $COMPILER -c `pwd`/file2_h.c
    checkstat 'cache hit (direct)' 1
    checkstat 'cache hit (preprocessed)' 0
    checkstat 'cache miss' 2

    ##################################################################
    # Check that direct mode ignores __FILE__ if sloppiness is specified.
    testname="__FILE__ in source file, sloppy"
    $CS -Cz >/dev/null
    cat <<EOF >file.c
#define file __FILE__
int test;
EOF
    CS_SLOPPINESS=file_macro $CS $COMPILER -c file.c
    checkstat 'cache hit (direct)' 0
    checkstat 'cache hit (preprocessed)' 0
    checkstat 'cache miss' 1
    CS_SLOPPINESS=file_macro $CS $COMPILER -c file.c
    checkstat 'cache hit (direct)' 1
    checkstat 'cache hit (preprocessed)' 0
    checkstat 'cache miss' 1
    CS_SLOPPINESS=file_macro $CS $COMPILER -c `pwd`/file.c
    checkstat 'cache hit (direct)' 2
    checkstat 'cache hit (preprocessed)' 0
    checkstat 'cache miss' 1

    testname="__FILE__ in include file, sloppy"
    $CS -Cz >/dev/null
    cat <<EOF >file.h
#define file __FILE__
int test;
EOF
    backdate file.h
    cat <<EOF >file_h.c
#include "file.h"
EOF
    CS_SLOPPINESS=file_macro $CS $COMPILER -c file_h.c
    checkstat 'cache hit (direct)' 0
    checkstat 'cache hit (preprocessed)' 0
    checkstat 'cache miss' 1
    CS_SLOPPINESS=file_macro $CS $COMPILER -c file_h.c
    checkstat 'cache hit (direct)' 1
    checkstat 'cache hit (preprocessed)' 0
    checkstat 'cache miss' 1
    mv file_h.c file2_h.c
    CS_SLOPPINESS=file_macro $CS $COMPILER -c `pwd`/file2_h.c
    checkstat 'cache hit (direct)' 2
    checkstat 'cache hit (preprocessed)' 0
    checkstat 'cache miss' 1

    ##################################################################
    # Check that we never get direct hits when __TIME__ is used.
    testname="__TIME__ in source file"
    $CS -Cz >/dev/null
    cat <<EOF >time.c
#define time __TIME__
int test;
EOF
    $CS $COMPILER -c time.c
    checkstat 'cache hit (direct)' 0
    checkstat 'cache hit (preprocessed)' 0
    checkstat 'cache miss' 1
    $CS $COMPILER -c time.c
    checkstat 'cache hit (direct)' 0
    checkstat 'cache hit (preprocessed)' 1
    checkstat 'cache miss' 1

    testname="__TIME__ in include time"
    $CS -Cz >/dev/null
    cat <<EOF >time.h
#define time __TIME__
int test;
EOF
    backdate time.h
    cat <<EOF >time_h.c
#include "time.h"
EOF
    $CS $COMPILER -c time_h.c
    checkstat 'cache hit (direct)' 0
    checkstat 'cache hit (preprocessed)' 0
    checkstat 'cache miss' 1
    $CS $COMPILER -c time_h.c
    checkstat 'cache hit (direct)' 0
    checkstat 'cache hit (preprocessed)' 1
    checkstat 'cache miss' 1

    ##################################################################
    # Check that direct mode ignores __TIME__ when sloppiness is specified.
    testname="__TIME__ in source file, sloppy"
    $CS -Cz >/dev/null
    cat <<EOF >time.c
#define time __TIME__
int test;
EOF
    CS_SLOPPINESS=time_macros $CS $COMPILER -c time.c
    checkstat 'cache hit (direct)' 0
    checkstat 'cache hit (preprocessed)' 0
    checkstat 'cache miss' 1
    CS_SLOPPINESS=time_macros $CS $COMPILER -c time.c
    checkstat 'cache hit (direct)' 1
    checkstat 'cache hit (preprocessed)' 0
    checkstat 'cache miss' 1

    testname="__TIME__ in include time, sloppy"
    $CS -Cz >/dev/null
    cat <<EOF >time.h
#define time __TIME__
int test;
EOF
    backdate time.h
    cat <<EOF >time_h.c
#include "time.h"
EOF
    CS_SLOPPINESS=time_macros $CS $COMPILER -c time_h.c
    checkstat 'cache hit (direct)' 0
    checkstat 'cache hit (preprocessed)' 0
    checkstat 'cache miss' 1
    CS_SLOPPINESS=time_macros $CS $COMPILER -c time_h.c
    checkstat 'cache hit (direct)' 1
    checkstat 'cache hit (preprocessed)' 0
    checkstat 'cache miss' 1

    ##################################################################
    # Check that a too new include file turns off direct mode.
    testname="too new include file"
    $CS -Cz >/dev/null
    cat <<EOF >new.c
#include "new.h"
EOF
    cat <<EOF >new.h
int test;
EOF
    touch -t 203801010000 new.h
    $CS $COMPILER -c new.c
    checkstat 'cache hit (direct)' 0
    checkstat 'cache hit (preprocessed)' 0
    checkstat 'cache miss' 1
    $CS $COMPILER -c new.c
    checkstat 'cache hit (direct)' 0
    checkstat 'cache hit (preprocessed)' 1
    checkstat 'cache miss' 1

    ##################################################################
    # Check that include file mtime is ignored when sloppiness is specified.
    testname="too new include file, sloppy"
    $CS -Cz >/dev/null
    cat <<EOF >new.c
#include "new.h"
EOF
    cat <<EOF >new.h
int test;
EOF
    touch -t 203801010000 new.h
    CS_SLOPPINESS=include_file_mtime $CS $COMPILER -c new.c
    checkstat 'cache hit (direct)' 0
    checkstat 'cache hit (preprocessed)' 0
    checkstat 'cache miss' 1
    CS_SLOPPINESS=include_file_mtime $CS $COMPILER -c new.c
    checkstat 'cache hit (direct)' 1
    checkstat 'cache hit (preprocessed)' 0
    checkstat 'cache miss' 1

    ##################################################################
    # Check that environment variables that affect the preprocessor are taken
    # into account.
    testname="environment variables"
    $CS -Cz >/dev/null
    rm -rf subdir1 subdir2
    mkdir subdir1 subdir2
    cat <<EOF >subdir1/foo.h
int foo;
EOF
    cat <<EOF >subdir2/foo.h
int foo;
EOF
    cat <<EOF >foo.c
#include <foo.h>
EOF
    backdate subdir1/foo.h subdir2/foo.h
    CPATH=subdir1 $CS $COMPILER -c foo.c
    checkstat 'cache hit (direct)' 0
    checkstat 'cache hit (preprocessed)' 0
    checkstat 'cache miss' 1
    CPATH=subdir1 $CS $COMPILER -c foo.c
    checkstat 'cache hit (direct)' 1
    checkstat 'cache hit (preprocessed)' 0
    checkstat 'cache miss' 1
    CPATH=subdir2 $CS $COMPILER -c foo.c
    checkstat 'cache hit (direct)' 1
    checkstat 'cache hit (preprocessed)' 0
    checkstat 'cache miss' 2 # subdir2 is part of the preprocessor output
    CPATH=subdir2 $CS $COMPILER -c foo.c
    checkstat 'cache hit (direct)' 2
    checkstat 'cache hit (preprocessed)' 0
    checkstat 'cache miss' 2

    #################################################################
    # Check that strange "#line" directives are handled.
    testname="#line directives with troublesome files"
    $CS -Cz >/dev/null
    cat <<EOF >strange.c
int foo;
EOF
    for x in stdout tty sda hda; do
        if [ -b /dev/$x ] || [ -c /dev/$x ]; then
            echo "#line 1 \"/dev/$x\"" >> strange.c
        fi
    done
    CS_SLOPPINESS=include_file_mtime $CS $COMPILER -c strange.c
    manifest=`find $CS_CACHE_DIR -name '*.manifest'`
    if [ -n "$manifest" ]; then
        data="`$CS --dump-manifest $manifest | egrep '/dev/(stdout|tty|sda|hda'`"
        if [ -n "$data" ]; then
            test_failed "$manifest contained troublesome file(s): $data"
        fi
    fi

    ##################################################################
    # Test --dump-manifest output.
    testname="--dump-manifest"
    $CS -Cz >/dev/null
    $CS $COMPILER test.c -c -o test.o
    manifest=`find $CS_CACHE_DIR -name '*.manifest'`
    $CS --dump-manifest $manifest |
        perl -ape 's/:.*/: normalized/ if $F[0] =~ "(Hash|Size):" and ++$n > 6' \
        >manifest.dump
    if [ $COMPILER_TYPE_CLANG -eq 1 ]; then
        cat <<EOF >expected.dump
Magic: cCmF
Version: 0
Hash size: 16
Reserved field: 0
File paths (3):
  0: ./test3.h
  1: ./test1.h
  2: ./test2.h
File infos (3):
  0:
    Path index: 0
    Hash: c2f5392dbc7e8ff6138d01608445240a
    Size: 24
  1:
    Path index: 1
    Hash: e6b009695d072974f2c4d1dd7e7ed4fc
    Size: 95
  2:
    Path index: 2
    Hash: e94ceb9f1b196c387d098a5f1f4fe862
    Size: 11
Results (1):
  0:
    File hash indexes: 0 1 2
    Hash: normalized
    Size: normalized
EOF
    else
        cat <<EOF >expected.dump
Magic: cCmF
Version: 0
Hash size: 16
Reserved field: 0
File paths (3):
  0: test2.h
  1: test3.h
  2: test1.h
File infos (3):
  0:
    Path index: 0
    Hash: e94ceb9f1b196c387d098a5f1f4fe862
    Size: 11
  1:
    Path index: 1
    Hash: c2f5392dbc7e8ff6138d01608445240a
    Size: 24
  2:
    Path index: 2
    Hash: e6b009695d072974f2c4d1dd7e7ed4fc
    Size: 95
Results (1):
  0:
    File hash indexes: 0 1 2
    Hash: normalized
    Size: normalized
EOF
    fi

    if diff expected.dump manifest.dump; then
        :
    else
        test_failed "unexpected output of --dump-manifest"
    fi
}

basedir_suite() {
    ##################################################################
    # Create some code to compile.
    mkdir -p dir1/src dir1/include
    cat <<EOF >dir1/src/test.c
#include <test.h>
EOF
    cat <<EOF >dir1/include/test.h
int test;
EOF
    cp -r dir1 dir2
    backdate dir1/include/test.h dir2/include/test.h

    cat <<EOF >stderr.h
int stderr(void)
{
	/* Trigger warning by having no return statement. */
}
EOF
    cat <<EOF >stderr.c
#include <stderr.h>
EOF
    backdate stderr.h

    ##################################################################
    # CS_BASEDIR="" and using absolute include path will result in a cache
    # miss.
    testname="empty CS_BASEDIR"
    $CS -z >/dev/null

    cd dir1
    CS_BASEDIR="" $CS $COMPILER -I`pwd`/include -c src/test.c
    checkstat 'cache hit (direct)' 0
    checkstat 'cache hit (preprocessed)' 0
    checkstat 'cache miss' 1
    cd ..

    cd dir2
    CS_BASEDIR="" $CS $COMPILER -I`pwd`/include -c src/test.c
    checkstat 'cache hit (direct)' 0
    checkstat 'cache hit (preprocessed)' 0
    checkstat 'cache miss' 2
    cd ..

    ##################################################################
    # Setting CS_BASEDIR will result in a cache hit because include paths
    # in the preprocessed output are rewritten.
    testname="set CS_BASEDIR"
    $CS -z >/dev/null
    $CS -C >/dev/null

    cd dir1
    CS_BASEDIR="`pwd`" $CS $COMPILER -I`pwd`/include -c src/test.c
    checkstat 'cache hit (direct)' 0
    checkstat 'cache hit (preprocessed)' 0
    checkstat 'cache miss' 1
    cd ..

    cd dir2
    # The space after -I is there to test an extra code path.
    CS_BASEDIR="`pwd`" $CS $COMPILER -I `pwd`/include -c src/test.c
    checkstat 'cache hit (direct)' 0
    checkstat 'cache hit (preprocessed)' 1
    checkstat 'cache miss' 1
    cd ..

    ##################################################################
    # Setting CS_BASEDIR will result in a cache hit because -I arguments
    # are rewritten, as are the paths stored in the manifest.
    testname="set CS_BASEDIR, direct lookup"
    $CS -z >/dev/null
    $CS -C >/dev/null
    unset CS_NODIRECT

    cd dir1
    CS_BASEDIR="`pwd`" $CS $COMPILER -I`pwd`/include -c src/test.c
    checkstat 'cache hit (direct)' 0
    checkstat 'cache hit (preprocessed)' 0
    checkstat 'cache miss' 1
    cd ..

    cd dir2
    CS_BASEDIR="`pwd`" $CS $COMPILER -I`pwd`/include -c src/test.c
    checkstat 'cache hit (direct)' 1
    checkstat 'cache hit (preprocessed)' 0
    checkstat 'cache miss' 1
    cd ..

    ##################################################################
    # CS_BASEDIR="" is the default.
    testname="default CS_BASEDIR"
    cd dir1
    $CS -z >/dev/null
    $CS $COMPILER -I`pwd`/include -c src/test.c
    checkstat 'cache hit (direct)' 0
    checkstat 'cache hit (preprocessed)' 0
    checkstat 'cache miss' 1
    cd ..

    ##################################################################
    # Rewriting triggered by CS_BASEDIR should handle paths with multiple
    # slashes correctly.
    testname="path normalization"
    cd dir1
    $CS -z >/dev/null
    CS_BASEDIR=`pwd` $CS $COMPILER -I`pwd`//include -c `pwd`//src/test.c
    checkstat 'cache hit (direct)' 1
    checkstat 'cache hit (preprocessed)' 0
    checkstat 'cache miss' 0
    cd ..

    ##################################################################
    # Check that rewriting triggered by CS_BASEDIR also affects stderr.
    testname="stderr"
    $CS -z >/dev/null
    CS_BASEDIR=`pwd` $CS $COMPILER -Wall -W -I`pwd` -c `pwd`/stderr.c -o `pwd`/stderr.o 2>stderr.txt
    checkstat 'cache hit (direct)' 0
    checkstat 'cache hit (preprocessed)' 0
    checkstat 'cache miss' 1
    if grep `pwd` stderr.txt >/dev/null 2>&1; then
        test_failed "Base dir (`pwd`) found in stderr:\n`cat stderr.txt`"
    fi

    CS_BASEDIR=`pwd` $CS $COMPILER -Wall -W -I`pwd` -c `pwd`/stderr.c -o `pwd`/stderr.o 2>stderr.txt
    checkstat 'cache hit (direct)' 1
    checkstat 'cache hit (preprocessed)' 0
    checkstat 'cache miss' 1
    if grep `pwd` stderr.txt >/dev/null 2>&1; then
        test_failed "Base dir (`pwd`) found in stderr:\n`cat stderr.txt`"
    fi

    ##################################################################
    # Check that -MF, -MQ and -MT arguments with absolute paths are rewritten
    # to relative.
    testname="-MF/-MQ/-MT with absolute paths"
    for option in MF "MF " MQ "MQ " MT "MT "; do
        $CS -Cz >/dev/null
        cd dir1
        CS_BASEDIR="`pwd`" $CS $COMPILER -I`pwd`/include -MD -${option}`pwd`/test.d -c src/test.c
        checkstat 'cache hit (direct)' 0
        checkstat 'cache hit (preprocessed)' 0
        checkstat 'cache miss' 1
        cd ..

        cd dir2
        CS_BASEDIR="`pwd`" $CS $COMPILER -I`pwd`/include -MD -${option}`pwd`/test.d -c src/test.c
        checkstat 'cache hit (direct)' 1
        checkstat 'cache hit (preprocessed)' 0
        checkstat 'cache miss' 1
        cd ..
    done
}

compression_suite() {
    ##################################################################
    # Create some code to compile.
    cat <<EOF >test.c
int test;
EOF

    ##################################################################
    # Check that compressed and uncompressed files get the same hash sum.
    testname="compression hash sum"
    CS_COMPRESS=1 $CS $COMPILER -c test.c
    checkstat 'cache hit (direct)' 0
    checkstat 'cache hit (preprocessed)' 0
    checkstat 'cache miss' 1

    CS_COMPRESS=1 $CS $COMPILER -c test.c
    checkstat 'cache hit (direct)' 0
    checkstat 'cache hit (preprocessed)' 1
    checkstat 'cache miss' 1

    $CS $COMPILER -c test.c
    checkstat 'cache hit (direct)' 0
    checkstat 'cache hit (preprocessed)' 2
    checkstat 'cache miss' 1
}

readonly_suite() {
    ##################################################################
    # Create some code to compile.
    echo "int test;" >test.c
    echo "int test2;" >test2.c

    # Cache a compilation.
    $CS $COMPILER -c test.c -o test.o
    checkstat 'cache hit (preprocessed)' 0
    checkstat 'cache miss' 1

    # Make the cache readonly
    # Check that readonly mode finds the result.
    testname="cache hit"
    rm -f test.o
    chmod -R a-w $CS_CACHE_DIR
    CS_READONLY=1 CS_TEMPDIR=/tmp CS_PREFIX=false $CS $COMPILER -c test.c -o test.o
    status=$?
    chmod -R a+w $CS_CACHE_DIR
    if [ $status -ne 0 ]; then
        test_failed "failure when compiling test.c readonly"
    fi
    if [ ! -f test.o ]; then
        test_failed "test.o missing"
    fi

    # Check that readonly mode doesn't try to store new results.
    testname="cache miss"
    files_before=`find $CS_CACHE_DIR -type f | wc -l`
    CS_READONLY=1 CS_TEMPDIR=/tmp $CS $COMPILER -c test2.c -o test2.o
    if [ $? -ne 0 ]; then
        test_failed "failure when compiling test2.c readonly"
    fi
    if [ ! -f test2.o ]; then
        test_failed "test2.o missing"
    fi
    files_after=`find $CS_CACHE_DIR -type f | wc -l`
    if [ $files_before -ne $files_after ]; then
        test_failed "readonly mode stored files in the cache"
    fi

    # Check that readonly mode and direct mode works.
    unset CS_NODIRECT
    files_before=`find $CS_CACHE_DIR -type f | wc -l`
    CS_READONLY=1 CS_TEMPDIR=/tmp $CS $COMPILER -c test.c -o test.o
    CS_NODIRECT=1
    export CS_NODIRECT
    if [ $? -ne 0 ]; then
        test_failed "failure when compiling test2.c readonly"
    fi
    files_after=`find $CS_CACHE_DIR -type f | wc -l`
    if [ $files_before -ne $files_after ]; then
        test_failed "readonly mode + direct mode stored files in the cache"
    fi

    ##################################################################
}

extrafiles_suite() {
    ##################################################################
    # Create some code to compile.
    cat <<EOF >test.c
int test;
EOF
    echo a >a
    echo b >b

    ##################################################################
    # Test the CS_EXTRAFILES feature.

    testname="cache hit"
    $CS $COMPILER -c test.c
    checkstat 'cache hit (preprocessed)' 0
    checkstat 'cache miss' 1

    testname="cache miss"
    $CS $COMPILER -c test.c
    checkstat 'cache hit (preprocessed)' 1
    checkstat 'cache miss' 1

    testname="cache miss a b"
    CS_EXTRAFILES="a${PATH_DELIM}b" $CS $COMPILER -c test.c
    checkstat 'cache hit (preprocessed)' 1
    checkstat 'cache miss' 2

    testname="cache hit a b"
    CS_EXTRAFILES="a${PATH_DELIM}b" $CS $COMPILER -c test.c
    checkstat 'cache hit (preprocessed)' 2
    checkstat 'cache miss' 2

    testname="cache miss a b2"
    echo b2 >b
    CS_EXTRAFILES="a${PATH_DELIM}b" $CS $COMPILER -c test.c
    checkstat 'cache hit (preprocessed)' 2
    checkstat 'cache miss' 3

    testname="cache hit a b2"
    CS_EXTRAFILES="a${PATH_DELIM}b" $CS $COMPILER -c test.c
    checkstat 'cache hit (preprocessed)' 3
    checkstat 'cache miss' 3

    testname="cache miss doesntexist"
    CS_EXTRAFILES="doesntexist" $CS $COMPILER -c test.c
    checkstat 'cache hit (preprocessed)' 3
    checkstat 'cache miss' 3
    checkstat 'error hashing extra file' 1
}

prepare_cleanup_test() {
    dir=$1
    rm -rf $dir
    mkdir -p $dir
    i=0
    while [ $i -lt 10 ]; do
        perl -e 'print "A" x 4017' >$dir/result$i-4017.o
        touch $dir/result$i-4017.stderr
        touch $dir/result$i-4017.d
        if [ $i -gt 5 ]; then
            backdate $dir/result$i-4017.stderr
        fi
        i=`expr $i + 1`
    done
    # NUMFILES: 30, TOTALSIZE: 40 KiB, MAXFILES: 0, MAXSIZE: 0
    echo "0 0 0 0 0 0 0 0 0 0 0 30 40 0 0" >$dir/stats
}

cleanup_suite() {
    testname="clear"
    prepare_cleanup_test $CS_CACHE_DIR/a
    $CS -C >/dev/null
    checkfilecount 0 '*.o' $CS_CACHE_DIR
    checkfilecount 0 '*.d' $CS_CACHE_DIR
    checkfilecount 0 '*.stderr' $CS_CACHE_DIR
    checkstat 'files in cache' 0

    testname="forced cleanup, no limits"
    $CS -C >/dev/null
    prepare_cleanup_test $CS_CACHE_DIR/a
    $CS -F 0 -M 0 >/dev/null
    $CS -c >/dev/null
    checkfilecount 10 '*.o' $CS_CACHE_DIR
    checkfilecount 10 '*.d' $CS_CACHE_DIR
    checkfilecount 10 '*.stderr' $CS_CACHE_DIR
    checkstat 'files in cache' 30

    testname="forced cleanup, file limit"
    $CS -C >/dev/null
    prepare_cleanup_test $CS_CACHE_DIR/a
    # (9/10) * 30 * 16 = 432
    $CS -F 432 -M 0 >/dev/null
    $CS -c >/dev/null
    # floor(0.8 * 9) = 7
    checkfilecount 7 '*.o' $CS_CACHE_DIR
    checkfilecount 7 '*.d' $CS_CACHE_DIR
    checkfilecount 7 '*.stderr' $CS_CACHE_DIR
    checkstat 'files in cache' 21
    for i in 0 1 2 3 4 5 9; do
        file=$CS_CACHE_DIR/a/result$i-4017.o
        if [ ! -f $file ]; then
            test_failed "File $file removed when it shouldn't"
        fi
    done
    for i in 6 7 8; do
        file=$CS_CACHE_DIR/a/result$i-4017.o
        if [ -f $file ]; then
            test_failed "File $file not removed when it should"
        fi
    done

    # Warning: this test is known to fail on filesystems that have
    # unusual block sizes, including ecryptfs.  The workaround is
    # to place the test directory elsewhere:
    #     cd /tmp
    #     CS=$DIR/cs $DIR/test.sh
    testname="forced cleanup, size limit"
    $CS -C >/dev/null
    prepare_cleanup_test $CS_CACHE_DIR/a
    # (4/10) * 10 * 4 * 16 = 256
    $CS -F 0 -M 256K >/dev/null
    $CS -c >/dev/null
    # floor(0.8 * 4) = 3
    checkfilecount 3 '*.o' $CS_CACHE_DIR
    checkfilecount 3 '*.d' $CS_CACHE_DIR
    checkfilecount 3 '*.stderr' $CS_CACHE_DIR
    checkstat 'files in cache' 9
    for i in 3 4 5; do
        file=$CS_CACHE_DIR/a/result$i-4017.o
        if [ ! -f $file ]; then
            test_failed "File $file removed when it shouldn't"
        fi
    done
    for i in 0 1 2 6 7 8 9; do
        file=$CS_CACHE_DIR/a/result$i-4017.o
        if [ -f $file ]; then
            test_failed "File $file not removed when it should"
        fi
    done

    testname="autocleanup"
    $CS -C >/dev/null
    for x in 0 1 2 3 4 5 6 7 8 9 a b c d e f; do
        prepare_cleanup_test $CS_CACHE_DIR/$x
    done
    # (9/10) * 30 * 16 = 432
    $CS -F 432 -M 0 >/dev/null
    touch empty.c
    checkfilecount 160 '*.o' $CS_CACHE_DIR
    checkfilecount 160 '*.d' $CS_CACHE_DIR
    checkfilecount 160 '*.stderr' $CS_CACHE_DIR
    checkstat 'files in cache' 480
    $CS $COMPILER -c empty.c -o empty.o
    # floor(0.8 * 9) = 7
    checkfilecount 157 '*.o' $CS_CACHE_DIR
    checkfilecount 156 '*.d' $CS_CACHE_DIR
    checkfilecount 156 '*.stderr' $CS_CACHE_DIR
    checkstat 'files in cache' 469

    testname="sibling cleanup"
    $CS -C >/dev/null
    prepare_cleanup_test $CS_CACHE_DIR/a
    # (9/10) * 30 * 16 = 432
    $CS -F 432 -M 0 >/dev/null
    backdate $CS_CACHE_DIR/a/result2-4017.stderr
    $CS -c >/dev/null
    # floor(0.8 * 9) = 7
    checkfilecount 7 '*.o' $CS_CACHE_DIR
    checkfilecount 7 '*.d' $CS_CACHE_DIR
    checkfilecount 7 '*.stderr' $CS_CACHE_DIR
    checkstat 'files in cache' 21
    for i in 0 1 3 4 5 8 9; do
        file=$CS_CACHE_DIR/a/result$i-4017.o
        if [ ! -f $file ]; then
            test_failed "File $file removed when it shouldn't"
        fi
    done
    for i in 2 6 7; do
        file=$CS_CACHE_DIR/a/result$i-4017.o
        if [ -f $file ]; then
            test_failed "File $file not removed when it should"
        fi
    done

    testname="new unknown file"
    $CS -C >/dev/null
    prepare_cleanup_test $CS_CACHE_DIR/a
    touch $CS_CACHE_DIR/a/abcd.unknown
    $CS -F 0 -M 0 -c >/dev/null # update counters
    checkstat 'files in cache' 31
    # (9/10) * 30 * 16 = 432
    $CS -F 432 -M 0 >/dev/null
    $CS -c >/dev/null
    if [ ! -f $CS_CACHE_DIR/a/abcd.unknown ]; then
        test_failed "$CS_CACHE_DIR/a/abcd.unknown removed"
    fi
    checkstat 'files in cache' 19

    testname="old unknown file"
    $CS -C >/dev/null
    prepare_cleanup_test $CS_CACHE_DIR/a
    # (9/10) * 30 * 16 = 432
    $CS -F 432 -M 0 >/dev/null
    touch $CS_CACHE_DIR/a/abcd.unknown
    backdate $CS_CACHE_DIR/a/abcd.unknown
    $CS -c >/dev/null
    if [ -f $CS_CACHE_DIR/a/abcd.unknown ]; then
        test_failed "$CS_CACHE_DIR/a/abcd.unknown not removed"
    fi

    testname="cleanup of tmp files"
    $CS -C >/dev/null
    touch $CS_CACHE_DIR/a/abcd.tmp.efgh
    $CS -c >/dev/null # update counters
    checkstat 'files in cache' 1
    backdate $CS_CACHE_DIR/a/abcd.tmp.efgh
    $CS -c >/dev/null
    if [ -f $CS_CACHE_DIR/a/abcd.tmp.efgh ]; then
        test_failed "$CS_CACHE_DIR/a/abcd.tmp.unknown not removed"
    fi
    checkstat 'files in cache' 0

    testname="ignore .nfs* files"
    prepare_cleanup_test $CS_CACHE_DIR/a
    touch $CS_CACHE_DIR/a/.nfs0123456789
    $CS -F 0 -M 0 >/dev/null
    $CS -c >/dev/null
    checkfilecount 1 '.nfs*' $CS_CACHE_DIR
    checkstat 'files in cache' 30
}

pch_suite() {
    unset CS_NODIRECT

    cat <<EOF >pch.c
#include "pch.h"
int main()
{
  void *p = NULL;
  return 0;
}
EOF
    cat <<EOF >pch.h
#include <stdlib.h>
EOF
    cat <<EOF >pch2.c
int main()
{
  void *p = NULL;
  return 0;
}
EOF

    if $COMPILER -fpch-preprocess pch.h 2>/dev/null && [ -f pch.h.gch ] && $COMPILER pch.c -o pch; then
        :
    else
        echo "Compiler (`$COMPILER --version | head -1`) doesn't support precompiled headers -- not running pch test"
        return
    fi

    ##################################################################
    # Tests for creating a .gch.

    backdate pch.h

    testname="create .gch, -c, no -o"
    $CS -zC >/dev/null
    $CS $COMPILER -c pch.h
    checkstat 'cache hit (direct)' 0
    checkstat 'cache hit (preprocessed)' 0
    checkstat 'cache miss' 1
    rm -f pch.h.gch
    $CS $COMPILER -c pch.h
    checkstat 'cache hit (direct)' 1
    checkstat 'cache hit (preprocessed)' 0
    checkstat 'cache miss' 1
    if [ ! -f pch.h.gch ]; then
        test_failed "pch.h.gch missing"
    fi

    testname="create .gch, no -c, -o"
    $CS -Cz >/dev/null
    $CS $COMPILER pch.h -o pch.gch
    checkstat 'cache hit (direct)' 0
    checkstat 'cache hit (preprocessed)' 0
    checkstat 'cache miss' 1
    $CS $COMPILER pch.h -o pch.gch
    checkstat 'cache hit (direct)' 1
    checkstat 'cache hit (preprocessed)' 0
    checkstat 'cache miss' 1
    if [ ! -f pch.gch ]; then
        test_failed "pch.gch missing"
    fi

    ##################################################################
    # Tests for using a .gch.

    rm -f pch.h
    backdate pch.h.gch

    testname="no -fpch-preprocess, #include"
    $CS -Cz >/dev/null
    $CS $COMPILER -c pch.c 2>/dev/null
    checkstat 'cache hit (direct)' 0
    checkstat 'cache hit (preprocessed)' 0
    checkstat 'cache miss' 0
    # Preprocessor error because GCC can't find the real include file when
    # trying to preprocess:
    checkstat 'preprocessor error' 1

    testname="no -fpch-preprocess, -include, no sloppy time macros"
    $CS -Cz >/dev/null
    $CS $COMPILER -c -include pch.h pch2.c 2>/dev/null
    checkstat 'cache hit (direct)' 0
    checkstat 'cache hit (preprocessed)' 0
    checkstat 'cache miss' 0
    # Must enable sloppy time macros:
    checkstat "can't use precompiled header" 1

    testname="no -fpch-preprocess, -include"
    $CS -Cz >/dev/null
    CS_SLOPPINESS=time_macros $CS $COMPILER -c -include pch.h pch2.c 2>/dev/null
    checkstat 'cache hit (direct)' 0
    checkstat 'cache hit (preprocessed)' 0
    checkstat 'cache miss' 1
    CS_SLOPPINESS=time_macros $CS $COMPILER -c -include pch.h pch2.c 2>/dev/null
    checkstat 'cache hit (direct)' 1
    checkstat 'cache hit (preprocessed)' 0
    checkstat 'cache miss' 1

    testname="-fpch-preprocess, #include, no sloppy time macros"
    $CS -Cz >/dev/null
    $CS $COMPILER -c -fpch-preprocess pch.c
    checkstat 'cache hit (direct)' 0
    checkstat 'cache hit (preprocessed)' 0
    # Must enable sloppy time macros:
    checkstat "can't use precompiled header" 1

    testname="-fpch-preprocess, #include, sloppy time macros"
    $CS -Cz >/dev/null
    CS_SLOPPINESS=time_macros $CS $COMPILER -c -fpch-preprocess pch.c
    checkstat 'cache hit (direct)' 0
    checkstat 'cache hit (preprocessed)' 0
    checkstat 'cache miss' 1
    CS_SLOPPINESS=time_macros $CS $COMPILER -c -fpch-preprocess pch.c
    checkstat 'cache hit (direct)' 1
    checkstat 'cache hit (preprocessed)' 0
    checkstat 'cache miss' 1

    testname="-fpch-preprocess, #include, file changed"
    echo "updated" >>pch.h.gch # GCC seems to cope with this...
    backdate pch.h.gch
    CS_SLOPPINESS=time_macros $CS $COMPILER -c -fpch-preprocess pch.c
    checkstat 'cache hit (direct)' 1
    checkstat 'cache hit (preprocessed)' 0
    checkstat 'cache miss' 2

    testname="preprocessor mode"
    $CS -Cz >/dev/null
    CS_NODIRECT=1 CS_SLOPPINESS=time_macros $CS $COMPILER -c -fpch-preprocess pch.c
    checkstat 'cache hit (direct)' 0
    checkstat 'cache hit (preprocessed)' 0
    checkstat 'cache miss' 1
    CS_NODIRECT=1 CS_SLOPPINESS=time_macros $CS $COMPILER -c -fpch-preprocess pch.c
    checkstat 'cache hit (direct)' 0
    checkstat 'cache hit (preprocessed)' 1
    checkstat 'cache miss' 1

    testname="preprocessor mode, file changed"
    echo "updated" >>pch.h.gch # GCC seems to cope with this...
    backdate pch.h.gch
    CS_NODIRECT=1 CS_SLOPPINESS=time_macros $CS $COMPILER -c -fpch-preprocess pch.c
    checkstat 'cache hit (direct)' 0
    checkstat 'cache hit (preprocessed)' 1
    checkstat 'cache miss' 2
    CS_NODIRECT=1 CS_SLOPPINESS=time_macros $CS $COMPILER -c -fpch-preprocess pch.c
    checkstat 'cache hit (direct)' 0
    checkstat 'cache hit (preprocessed)' 2
    checkstat 'cache miss' 2
}

upgrade_suite() {
    testname="keep maxfiles and maxsize settings"
    rm -rf $CS_CACHE_DIR $CS_CONFIGPATH
    mkdir -p $CS_CACHE_DIR/0
    echo "0 0 0 0 0 0 0 0 0 0 0 0 0 2000 131072" >$CS_CACHE_DIR/0/stats
    checkstat 'max files' 32000
    checkstat 'max cache size' '2.1 GB'
}

prefix_suite() {
    testname="prefix"
    $CS -Cz >/dev/null
    rm -f prefix.result
    cat <<'EOF' >prefix-a
#!/bin/sh
echo a >>prefix.result
exec "$@"
EOF
    cat <<'EOF' >prefix-b
#!/bin/sh
echo b >>prefix.result
exec "$@"
EOF
    chmod +x prefix-a prefix-b
    cat <<'EOF' >file.c
int foo;
EOF
    PATH=.:$PATH CS_PREFIX="prefix-a prefix-b" $CS $COMPILER -c file.c
    checkstat 'cache hit (direct)' 0
    checkstat 'cache hit (preprocessed)' 0
    checkstat 'cache miss' 1
    checkfile prefix.result "a
b"
    PATH=.:$PATH CS_PREFIX="prefix-a prefix-b" $CS $COMPILER -c file.c
    checkstat 'cache hit (direct)' 0
    checkstat 'cache hit (preprocessed)' 1
    checkstat 'cache miss' 1
    checkfile prefix.result "a
b"
}

######################################################################
# main program

suites="$*"
if [ -n "$CC" ]; then
    COMPILER="$CC"
else
    COMPILER=gcc
fi
if [ -z "$CS" ]; then
    CS=`pwd`/cs
fi


# save the type of compiler because some test may not work on all compilers
COMPILER_TYPE_CLANG=0
COMPILER_TYPE_GCC=0

COMPILER_USES_LLVM=0
HOST_OS_APPLE=0

compiler_version="`$COMPILER --version 2>&1 | head -1`"
case $compiler_version in
    *gcc*|*g++*|2.95*)
        COMPILER_TYPE_GCC=1
        ;;
    *clang*)
        COMPILER_TYPE_CLANG=1
        ;;
    *)
        echo "WARNING: Compiler $COMPILER not supported (version: $compiler_version) -- not running tests" >&2
        exit 0
        ;;
esac

case $compiler_version in
    *llvm*|*LLVM*)
        COMPILER_USES_LLVM=1
        ;;
esac

host_os="`uname -s`"
case $host_os in
    *Darwin*)
        HOST_OS_APPLE=1
        ;;
esac


TESTDIR=testdir.$$
rm -rf $TESTDIR
mkdir $TESTDIR
cd $TESTDIR || exit 1

CS_CACHE_DIR=`pwd`/.cscache
export CS_CACHE_DIR
CS_LOGFILE=`pwd`/cs.log
export CS_LOGFILE
CS_CONFIGPATH=`pwd`/cs.conf
export CS_CONFIGPATH
touch $CS_CONFIGPATH

# ---------------------------------------

all_suites="
base
link          !win32
hardlink
cpp2
nlevels4
nlevels1
basedir       !win32
direct
compression
readonly
extrafiles
cleanup
pch
upgrade
prefix
"

case $host_os in
    *MINGW*|*mingw*)
        export CS_DETECT_SHEBANG
        CS_DETECT_SHEBANG=1
        DEVNULL=NUL
        PATH_DELIM=";"
        all_suites="`echo "$all_suites" | grep -v '!win32'`"
        ;;
    *)
        DEVNULL=/dev/null
        PATH_DELIM=":"
        all_suites="`echo "$all_suites" | cut -d' ' -f1`"
        ;;
esac

if [ -z "$suites" ]; then
    suites="$all_suites"
fi

for suite in $suites; do
    run_suite $suite
done

# ---------------------------------------

cd ..
rm -rf $TESTDIR
echo test done - OK
exit 0
