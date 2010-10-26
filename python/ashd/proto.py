import os, socket
import htlib

class protoerr(Exception):
    pass

class req(object):
    def __init__(self, method, url, ver, rest, headers, fd):
        self.method = method
        self.url = url
        self.ver = ver
        self.rest = rest
        self.headers = headers
        self.sk = socket.fromfd(fd, socket.AF_UNIX, socket.SOCK_STREAM).makefile('r+')
        os.close(fd)

    def close(self):
        self.sk.close()

    def __getitem__(self, header):
        header = header.lower()
        for key, val in self.headers:
            if key.lower() == header:
                return val
        raise KeyError(header)

    def __contains__(self, header):
        header = header.lower()
        for key, val in self.headers:
            if key.lower() == header:
                return True
        return False

    def dup(self):
        return req(self.method, self.url, self.ver, self.rest, self.headers, os.dup(self.sk.fileno()))

    def match(self, match):
        if self.rest[:len(match)] == match:
            self.rest = self.rest[len(match):]
            return True
        return False

    def __str__(self):
        return "\"%s %s %s\"" % (self.method, self.url, self.ver)

    def __enter__(self):
        return self

    def __exit__(self, *excinfo):
        self.sk.close()
        return False

def recvreq(sock = 0):
    data, fd = htlib.recvfd(sock)
    if fd is None:
        return None
    try:
        parts = data.split('\0')[:-1]
        if len(parts) < 5:
            raise protoerr("Truncated request")
        method, url, ver, rest = parts[:4]
        headers = []
        i = 4
        while True:
            if parts[i] == "": break
            if len(parts) - i < 3:
                raise protoerr("Truncated request")
            headers.append((parts[i], parts[i + 1]))
            i += 2
        return req(method, url, ver, rest, headers, os.dup(fd))
    finally:
        os.close(fd)

def sendreq(sock, req):
    data = ""
    data += req.method + '\0'
    data += req.url + '\0'
    data += req.ver + '\0'
    data += req.rest + '\0'
    for key, val in req.headers:
        data += key + '\0'
        data += val + '\0'
    data += '\0'
    htlib.sendfd(sock, req.sk.fileno(), data)
