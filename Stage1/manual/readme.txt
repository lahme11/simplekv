SimpleKV User Manual - Stage 1

USAGE
-----
  ./bin/simplekv [logfile]          (default data.kvlog)

Type commands at the simplekv> prompt, or pipe a script in:

  ./bin/simplekv mydb.kvlog < commands.txt

COMMANDS
--------
  SET key value     store a value                       -> OK
  GET key           print a value                       -> value or (nil)
  DEL key           remove a key                        -> (integer) 1 or 0
  EXISTS key        is the key there?                   -> (integer) 1 or 0
  KEYS [prefix]     list keys in order                  -> 1) key ...
  COUNT             how many keys                       -> (integer) n
  HELP              list the commands
  QUIT              exit (end of input works too)

Put "double quotes" around keys or values with spaces. Inside
quotes, write \" for a quote and \\ for a backslash.

STORAGE
-------
Every SET and DEL is appended to the log file as one line, for
example:

  S 3 foo 11 hello world

The numbers are byte lengths, so values may contain spaces. When
SimpleKV starts it replays the log. If the last line is damaged
(for example the program was killed while writing it), that line
is ignored, a warning is printed, and the log is cut back to the
last good entry.

LIMITATIONS
-----------
The log keeps every change, so it only grows. The table has a
fixed number of buckets. See Stage 2 for the full engine with a
checked binary format, compaction and locking.
