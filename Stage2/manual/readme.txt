SimpleKV User Manual - Stage 2

USAGE
-----
  ./bin/simplekv [-f file] [-s always|batch|never] [-q] [-h]

  -f file   database file (default data.skv)
  -s mode   when to fsync: always (default), batch (every 100
            writes and on exit) or never (leave it to the OS)
  -q        no banner or prompt
  -h        show usage

Type commands at the simplekv> prompt, or pipe a script in:

  ./bin/simplekv -f shop.skv < commands.txt

When input is not a terminal, the exit status is 1 if any command
printed an error, so scripts can check it.

COMMANDS
--------
  SET key value            OK
  SETEX key seconds value  OK (the key disappears after seconds)
  GET key                  the value, or (nil)
  DEL key                  (integer) 1 or 0
  EXISTS key               (integer) 1 or 0
  KEYS [prefix]            1) key  2) key ...  or (empty)
  COUNT                    (integer) n
  EXPIRE key seconds       (integer) 1, or 0 if no such key
  TTL key                  seconds left, -1 = never, -2 = no key
  INCR key [by]            the new number (a missing key is 0)
  COMPACT                  OK (reclaimed N bytes)
  STATS                    keys, records, sizes, sync mode
  HELP                     list the commands
  QUIT                     exit (end of input works too)

Command names can be in any case. Put "double quotes" around keys
or values with spaces. Inside quotes: \" \\ \n \t \r and \xNN.
GET prints values the same way, so its output can be pasted back.

Keys are 1 to 1024 bytes; values are at most 1 MiB.

ERRORS
------
Errors start with ERR, for example:

  ERR unknown command 'GTE'
  ERR wrong number of arguments for 'set'
  ERR value is not an integer or out of range
  ERR line too long

STORAGE
-------
Every change is appended to the file as a record with a CRC-32
checksum. On start-up SimpleKV reads the records and builds an
index of where each value is. If the end of the file is damaged
(the program was killed or the power failed mid-write), the
damaged bytes are cut off and a "recovered" message is shown.

Only one program can use a database file at a time; a second one
gets "database is locked by another process".

COMPACT rewrites the live data into a new file and swaps it in
with an atomic rename, so the old file is replaced in one step.
This also happens automatically when more than half of a file
over 1 MiB is old data.
