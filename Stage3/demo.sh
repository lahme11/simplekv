#!/bin/bash
set -u
cd "$(dirname "$0")/../Stage2" || exit 1
make -s || exit 1

DIR=$(mktemp -d /tmp/skv-demo.XXXXXX)
DB="$DIR/demo.skv"
trap 'case "$DIR" in /tmp/skv-demo.*) rm -r -- "$DIR" ;; esac' EXIT

step()
{
    echo
    echo "== $1"
    printf '%s\n' "$2" | sed 's/^/simplekv> /'
    printf '%s\n' "$2" | ./bin/simplekv -f "$DB" -s never
    if [ "${DEMO_NOPAUSE:-0}" != 1 ]; then
        read -r -p "(press Enter) " _ < /dev/tty
    fi
}

step "Store and read values" 'SET name Aoife
SET "full name" "Aoife Byrne"
GET "full name"
GET nobody'
step "Counters" 'INCR visits
INCR visits 41'
step "Keys that expire" 'SETEX session 60 abc123
TTL session
TTL name'
step "List keys by prefix" 'SET user:1 Aoife
SET user:2 Brian
KEYS user:'
step "It survives a restart (this is a new process)" 'GET name
COUNT'

echo
echo "== Simulate a crash in the middle of a write: cut 4 bytes off the end of the file"
ls -l "$DB" | awk '{print $5 " bytes"}'
truncate -s -4 "$DB"
step "Recovery on the next start (the damaged last record is removed)" 'KEYS'

for i in $(seq 200); do echo "SET counter $i"; done | ./bin/simplekv -f "$DB" -s never > /dev/null
step "Compaction rewrites only the live data" 'STATS
COMPACT
STATS'

echo
echo "== A second program can't open the same database"
(sleep 1; echo QUIT) | ./bin/simplekv -f "$DB" -s never &
sleep 0.3
echo 'GET name' | ./bin/simplekv -f "$DB" -s never
wait
