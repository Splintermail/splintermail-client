#!/usr/bin/env python3

from sys import argv, exit, stderr
from hashlib import md5

if len(argv) < 2:
    print('usage: %s FILE'%argv[0], file=stderr)

with open(argv[1], 'rb') as f:
    print(md5(f.read()).hexdigest())
