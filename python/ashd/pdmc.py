"""Management for daemon processes

This module provides some client support for the daemon management
provided in the ashd.pdm module.
"""

import socket

class protoerr(Exception):
    pass

def resolve(spec):
    if isinstance(spec, socket.socket):
        return spec
    sk = None
    try:
        if "/" in spec:
            sk = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            sk.connect(spec)
        elif spec.isdigit():
            sk = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sk.connect(("localhost", int(spec)))
        elif ":" in spec:
            sk = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            p = spec.rindex(":")
            sk.connect((spec[:p], int(spec[p + 1:])))
        else:
            raise Exception("Unknown target specification %r" % spec)
        rv = sk
        sk = None
    finally:
        if sk is not None: sk.close()
    return rv

class client(object):
    def __init__(self, sk, proto = None):
        self.sk = resolve(sk)
        self.buf = ""
        line = self.readline()
        if line != "+PDM1":
            raise protoerr("Illegal protocol signature")
        if proto is not None:
            self.select(proto)

    def close(self):
        self.sk.close()

    def readline(self):
        while True:
            p = self.buf.find("\n")
            if p >= 0:
                ret = self.buf[:p]
                self.buf = self.buf[p + 1:]
                return ret
            ret = self.sk.recv(1024)
            if ret == "":
                return None
            self.buf += ret

    def select(self, proto):
        if "\n" in proto:
            raise Exception("Illegal protocol specified: %r" % proto)
        self.sk.send(proto + "\n")
        rep = self.readline()
        if len(rep) < 1 or rep[0] != "+":
            raise protoerr("Error reply when selecting protocol %s: %s" % (proto, rep[1:]))

    def __enter__(self):
        return self

    def __exit__(self, *excinfo):
        self.close()
        return False

class replclient(client):
    def __init__(self, sk):
        super(replclient, self).__init__(sk, "repl")

    def run(self, code):
        while True:
            ncode = code.replace("\n\n", "\n")
            if ncode == code: break
            code = ncode
        while len(code) > 0 and code[-1] == "\n":
            code = code[:-1]
        self.sk.send(code + "\n\n")
        buf = ""
        while True:
            ln = self.readline()
            if ln[0] == " ":
                buf += ln[1:] + "\n"
            elif ln[0] == "+":
                return buf
            elif ln[0] == "-":
                raise protoerr("Error reply: %s" % ln[1:])
            else:
                raise protoerr("Illegal reply: %s" % ln)
