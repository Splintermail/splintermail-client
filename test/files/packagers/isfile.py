#!/usr/bin/env python3

from sys import argv, exit, stderr
from os.path import isfile

if len(argv) < 2:
    print('usage: %s FILE'%argv[0], file=stderr)

exists = isfile(argv[1])
if not exists:
    print('not found:', argv[1], file=stderr)

exit(0 if exists else 1)
