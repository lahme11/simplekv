#!/bin/bash
set -u
cd "$(dirname "$0")/.." || exit 1

PASSED=0
FAILED=0
TMP=$(mktemp -d /tmp/skv-it.XXXXXX)
KV=bin/simplekv-test

cleanup()
{
    case "$TMP" in
        /tmp/skv-it.*) rm -r -- "$TMP" ;;
    esac
}
trap cleanup EXIT

check()
{
    if [ "$2" == "$3" ]; then
        PASSED=$((PASSED + 1))
        echo "  pass  $1"
    else
        FAILED=$((FAILED + 1))
        echo "  FAIL  $1: expected '$2', got '$3'"
    fi
}

kv()
{
    local db=$1
    shift
    "$KV" -f "$TMP/$db" -s never "$@"
}

echo "Building"
make -s -C ../Stage1 || exit 1
mkdir -p bin
gcc -Wall -Wextra -g -fsanitize=address,undefined -fno-sanitize-recover=all ../Stage2/src/*.c -o "$KV" || exit 1

echo "Stage 1"
S1=$(printf 'SET a 1\nSET "two words" "x y"\nGET "two words"\nDEL a\nCOUNT\n' | ../Stage1/bin/simplekv "$TMP/s1.kvlog")
check "s1 commands" 'OK|OK|"x y"|(integer) 1|(integer) 1' "$(tr '\n' '|' <<< "$S1" | sed 's/|$//')"
check "s1 replay" '"x y"' "$(printf 'GET "two words"\n' | ../Stage1/bin/simplekv "$TMP/s1.kvlog")"

echo "Stage 2 commands"
OUT=$(kv basic.skv 2>&1 <<'EOF'
SET name Aoife
GET name
GET missing
SET "full name" "Aoife Byrne"
GET "full name"
SET quote "say \"hi\"\nthen \\ go"
GET quote
SET empty ""
GET empty
EXISTS name
EXISTS nobody
DEL name
DEL name
KEYS
COUNT
INCR visits
INCR visits 41
INCR "full name"
SETEX session 100 token
TTL session
TTL visits
TTL nobody
EXPIRE visits 50
TTL visits
set lower case
get lower
EOF
)
EXPECTED='OK
Aoife
(nil)
OK
"Aoife Byrne"
OK
"say \"hi\"\nthen \\ go"
OK
""
(integer) 1
(integer) 0
(integer) 1
(integer) 0
1) empty
2) "full name"
3) quote
(integer) 3
(integer) 1
(integer) 42
ERR value is not an integer or out of range
OK
(integer) 100
(integer) -1
(integer) -2
(integer) 1
(integer) 50
OK
case'
check "command outputs" "$EXPECTED" "$OUT"

check "prefix keys" '1) visits' "$(printf 'KEYS vis\n' | kv basic.skv)"
check "empty keys" '(empty)' "$(printf 'KEYS zzz\n' | kv basic.skv)"
check "persists across runs" '"Aoife Byrne"' "$(printf 'GET "full name"\n' | kv basic.skv)"

ERRORS=$(printf 'BOGUS\nGET\nSET only-key\nSETEX k abc v\nSET "open\n' | kv errors.skv)
check "errors" "ERR unknown command 'BOGUS'|ERR wrong number of arguments for 'get'|ERR wrong number of arguments for 'set'|ERR invalid expire time|ERR unterminated quote" \
    "$(tr '\n' '|' <<< "$ERRORS" | sed 's/|$//')"
printf 'BOGUS\n' | kv errors.skv > /dev/null
check "exit status after an error" 1 "$?"
printf 'SET ok 1\n' | kv errors.skv > /dev/null
check "exit status when all is well" 0 "$?"

LONG=$( { printf 'SET big "'; head -c 1200000 /dev/zero | tr '\0' a; printf '"\nSET after 1\nGET after\n'; } | kv long.skv)
check "long line refused" 'ERR line too long|OK|1' "$(tr '\n' '|' <<< "$LONG" | sed 's/|$//')"

echo "Stage 2 storage"
printf 'SET a first\nSET b second\nSET c third\n' | kv torn.skv
truncate -s -3 "$TMP/torn.skv"
TORN=$(printf 'GET a\nGET b\nGET c\n' | kv torn.skv 2> "$TMP/torn.err")
check "recovery keeps earlier records" 'first|second|(nil)' "$(tr '\n' '|' <<< "$TORN" | sed 's/|$//')"
check "recovery is reported" yes "$(grep -q 'recovered: removed' "$TMP/torn.err" && echo yes)"
check "recovery happens once" "" "$(printf 'GET a\n' | kv torn.skv 2>&1 >/dev/null)"

echo "not a database" > "$TMP/junk.skv"
printf 'GET a\n' | kv junk.skv > /dev/null 2> "$TMP/junk.err"
check "foreign file refused" 1 "$?"
check "foreign file message" yes "$(grep -q 'not a SimpleKV file' "$TMP/junk.err" && echo yes)"

(sleep 2; echo QUIT) | kv locked.skv > /dev/null &
HOLDER=$!
sleep 0.5
printf 'GET a\n' | kv locked.skv > /dev/null 2> "$TMP/locked.err"
check "second process is locked out" 1 "$?"
check "lock message" yes "$(grep -q 'locked by another process' "$TMP/locked.err" && echo yes)"
wait "$HOLDER"
check "lock released after exit" OK "$(printf 'SET a 1\n' | kv locked.skv)"

VALUE=$(head -c 1000 /dev/zero | tr '\0' v)
for i in $(seq 3000); do echo "SET same \"$i $VALUE\""; done | kv churn.skv > /dev/null 2> "$TMP/churn.err"
check "auto-compaction runs" yes "$(grep -q 'auto-compacted' "$TMP/churn.err" && echo yes)"
SIZE=$(stat -c %s "$TMP/churn.skv")
check "file stays small" yes "$([ "$SIZE" -lt 2000000 ] && echo yes)"
check "latest value survives" "3000" "$(printf 'GET same\n' | kv churn.skv | cut -c2-5)"

STATS=$(printf 'SET a 1\nSET a 2\nDEL a\nSET b 3\nCOMPACT\nSTATS\n' | kv stats.skv)
check "compact output" yes "$(grep -Eq '^OK \(reclaimed [0-9]+ bytes\)$' <<< "$STATS" && echo yes)"
check "stats keys" 'keys: 1' "$(grep '^keys:' <<< "$STATS")"
check "stats dead bytes" 'dead_bytes: 0' "$(grep '^dead_bytes:' <<< "$STATS")"

for mode in always batch; do
    check "sync $mode" "OK|v" "$(printf 'SET k v\nGET k\n' | "$KV" -f "$TMP/sync-$mode.skv" -s "$mode" | tr '\n' '|' | sed 's/|$//')"
done
"$KV" -s sometimes -f "$TMP/x.skv" < /dev/null > /dev/null 2>&1
check "bad sync mode" 1 "$?"

echo
echo "$PASSED passed, $FAILED failed"
[ "$FAILED" -eq 0 ]
