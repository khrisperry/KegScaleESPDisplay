"""Generate the embedded gzip page; runs automatically when HTML changes."""
import gzip
import pathlib
import sys

source, target = map(pathlib.Path, sys.argv[1:])
target.write_bytes(gzip.compress(source.read_bytes(), compresslevel=9, mtime=0))
