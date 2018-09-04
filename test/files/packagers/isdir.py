#!/usr/bin/env python3

from sys import argv, exit, stderr
from os.path import isdir

if len(argv) < 2:
    print('usage: %s DIRECTORY'%argv[0], file=stderr)

exists = isdir(argv[1])
if not exists:
    print('not found:', argv[1], file=stderr)

exit(0 if exists else 1)
