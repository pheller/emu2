#!/bin/bash
# Test script for emu2 -s (script mode)
#
# This tests the emu2sh scripting language features without requiring
# a DOS executable.

set -e

EMU2="./emu2"
PASS=0
FAIL=0

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
NC='\033[0m' # No Color

pass() {
    echo -e "${GREEN}PASS${NC}: $1"
    PASS=$((PASS + 1))
}

fail() {
    echo -e "${RED}FAIL${NC}: $1"
    FAIL=$((FAIL + 1))
}

# Create temp directory for test scripts
TMPDIR=$(mktemp -d)
trap "rm -rf $TMPDIR" EXIT

#############################################
# Test 1: Basic script execution
#############################################
cat > "$TMPDIR/test1.sh" << 'EOF'
#!/usr/bin/env emu2 -s
x = 42
print(x)
exit(0)
EOF

OUTPUT=$($EMU2 -s "$TMPDIR/test1.sh" 2>&1)
if [ "$OUTPUT" = "42" ]; then
    pass "Basic script execution"
else
    fail "Basic script execution (got: $OUTPUT)"
fi

#############################################
# Test 2: String operations
#############################################
cat > "$TMPDIR/test2.sh" << 'EOF'
#!/usr/bin/env emu2 -s
s = "hello" + " " + "world"
print(s)
exit(0)
EOF

OUTPUT=$($EMU2 -s "$TMPDIR/test2.sh" 2>&1)
if [ "$OUTPUT" = "hello world" ]; then
    pass "String concatenation"
else
    fail "String concatenation (got: $OUTPUT)"
fi

#############################################
# Test 3: Path functions
#############################################
cat > "$TMPDIR/test3.sh" << 'EOF'
#!/usr/bin/env emu2 -s
path = "/foo/bar/test.txt"
print(dirname(path))
print(basename(path))
print(extname(path))
print(stripext(path))
exit(0)
EOF

OUTPUT=$($EMU2 -s "$TMPDIR/test3.sh" 2>&1)
EXPECTED="/foo/bar
test.txt
.txt
/foo/bar/test"
if [ "$OUTPUT" = "$EXPECTED" ]; then
    pass "Path functions"
else
    fail "Path functions (got: $OUTPUT)"
fi

#############################################
# Test 4: Argument parsing
#############################################
cat > "$TMPDIR/test4.sh" << 'EOF'
#!/usr/bin/env emu2 -s
arg("-o", "--output", dest="outdir", default=".")
arg("-v", "--verbose", flag=true)
arg("input", positional=true)
opts = parse_args()
print(opts.outdir)
print(opts.verbose)
print(opts.input)
exit(0)
EOF

OUTPUT=$($EMU2 -s "$TMPDIR/test4.sh" -o /tmp -v myfile.txt 2>&1)
EXPECTED="/tmp
true
myfile.txt"
if [ "$OUTPUT" = "$EXPECTED" ]; then
    pass "Argument parsing"
else
    fail "Argument parsing (got: $OUTPUT)"
fi

#############################################
# Test 5: Control flow - if/else
#############################################
cat > "$TMPDIR/test5.sh" << 'EOF'
#!/usr/bin/env emu2 -s
x = 10
if x > 5:
    print("big")
else:
    print("small")
exit(0)
EOF

OUTPUT=$($EMU2 -s "$TMPDIR/test5.sh" 2>&1)
if [ "$OUTPUT" = "big" ]; then
    pass "If/else control flow"
else
    fail "If/else control flow (got: $OUTPUT)"
fi

#############################################
# Test 6: For loop
#############################################
cat > "$TMPDIR/test6.sh" << 'EOF'
#!/usr/bin/env emu2 -s
items = ["a", "b", "c"]
for item in items:
    print(item)
exit(0)
EOF

OUTPUT=$($EMU2 -s "$TMPDIR/test6.sh" 2>&1)
EXPECTED="a
b
c"
if [ "$OUTPUT" = "$EXPECTED" ]; then
    pass "For loop"
else
    fail "For loop (got: $OUTPUT)"
fi

#############################################
# Test 7: Functions
#############################################
cat > "$TMPDIR/test7.sh" << 'EOF'
#!/usr/bin/env emu2 -s
def greet(name):
    return "Hello, " + name

print(greet("World"))
exit(0)
EOF

OUTPUT=$($EMU2 -s "$TMPDIR/test7.sh" 2>&1)
if [ "$OUTPUT" = "Hello, World" ]; then
    pass "Function definition and call"
else
    fail "Function definition and call (got: $OUTPUT)"
fi

#############################################
# Test 8: int() builtin
#############################################
cat > "$TMPDIR/test8.sh" << 'EOF'
#!/usr/bin/env emu2 -s
s = "42"
n = int(s)
print(n + 8)
exit(0)
EOF

OUTPUT=$($EMU2 -s "$TMPDIR/test8.sh" 2>&1)
if [ "$OUTPUT" = "50" ]; then
    pass "int() builtin"
else
    fail "int() builtin (got: $OUTPUT)"
fi

#############################################
# Test 9: Exit code
#############################################
cat > "$TMPDIR/test9.sh" << 'EOF'
#!/usr/bin/env emu2 -s
exit(42)
EOF

set +e
$EMU2 -s "$TMPDIR/test9.sh" 2>&1
EXIT_CODE=$?
set -e
if [ "$EXIT_CODE" = "42" ]; then
    pass "Exit code"
else
    fail "Exit code (got: $EXIT_CODE)"
fi

#############################################
# Test 10: exists() and cwd()
#############################################
cat > "$TMPDIR/test10.sh" << 'EOF'
#!/usr/bin/env emu2 -s
if exists(cwd()):
    print("cwd exists")
else:
    print("cwd missing")
exit(0)
EOF

OUTPUT=$($EMU2 -s "$TMPDIR/test10.sh" 2>&1)
if [ "$OUTPUT" = "cwd exists" ]; then
    pass "exists() and cwd()"
else
    fail "exists() and cwd() (got: $OUTPUT)"
fi

#############################################
# Test 11: split() function
#############################################
cat > "$TMPDIR/test11.sh" << 'EOF'
#!/usr/bin/env emu2 -s
s = "a:b:c"
parts = split(s, ":")
for p in parts:
    print(p)
exit(0)
EOF

OUTPUT=$($EMU2 -s "$TMPDIR/test11.sh" 2>&1)
EXPECTED="a
b
c"
if [ "$OUTPUT" = "$EXPECTED" ]; then
    pass "split() function"
else
    fail "split() function (got: $OUTPUT)"
fi

#############################################
# Test 12: len() function
#############################################
cat > "$TMPDIR/test12.sh" << 'EOF'
#!/usr/bin/env emu2 -s
s = "hello"
l = [1, 2, 3, 4]
print(len(s))
print(len(l))
exit(0)
EOF

OUTPUT=$($EMU2 -s "$TMPDIR/test12.sh" 2>&1)
EXPECTED="5
4"
if [ "$OUTPUT" = "$EXPECTED" ]; then
    pass "len() function"
else
    fail "len() function (got: $OUTPUT)"
fi

#############################################
# Summary
#############################################
echo ""
echo "================================"
echo "Tests passed: $PASS"
echo "Tests failed: $FAIL"
echo "================================"

if [ $FAIL -gt 0 ]; then
    exit 1
fi
exit 0
