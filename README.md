# Remy, toy remote file access

To build and run:
```
cmake -S . -B build [options]
cmake --build build
build/net/remy-server ...
build/net/remy-client ...
```
Boost 1.83 is used (with `coroutine` and `program_options` libraries).
Working checked on clang++ 17 and g++ 13 (both with libstdc++).

TUI controls support arrows, hjkl, mouse/scroll (press F1 or help button for more details)

## Usage

```
$ build/net/remy-server -h
Usage:
Remy server:
  -h [ --help ]                  Help screen
  -a [ --addr ] arg (=127.0.0.1) Addr
  -p [ --port ] arg (=7312)      Port
  -o [ --log-file ] arg          Log file
$ build/net/remy-client -h
Usage:
Remy client:
  -h [ --help ]                  Help screen
  -a [ --addr ] arg (=127.0.0.1) Addr
  -p [ --port ] arg (=7312)      Port
  -C [ --dir ] arg               Directory
  -o [ --log-file ] arg          Log file
```
