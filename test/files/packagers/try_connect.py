#!/usr/bin/env python3

from sys import argv, exit, stderr
from socket import socket

if len(argv) < 3:
    print('usage: %s HOST PORT'%argv[0], file=stderr)

host = argv[1]
port = int(argv[2])

try:
    s = socket()
    s.connect((host, port))
    s.close()
except:
    exit(1)
