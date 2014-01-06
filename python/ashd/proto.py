"""Low-level protocol module for ashd(7)

This module provides primitive functions that speak the raw ashd(7)
protocol. Primarily, it implements the `req' class that is used to
represent ashd requests. The functions it provides can also be used to
create ashd handlers, but unless you require very precise control, the
ashd.util module provides an easier-to-use interface.
"""

import os, socket
import htlib

__all__ = ["req", "recvreq", "sendreq"]

class protoerr(Exception):
    pass

class req(object):
    """Represents a single ashd request. Normally, you would not
    create instances of this class manually, but receive them from the
    recvreq function.

    For the abstract structure of ashd requests, please see the
    ashd(7) manual page. This class provides access to the HTTP
    method, raw URL, HTTP version and rest string via the `method',
    `url', `ver' and `rest' variables respectively. It also implements
    a dict-like interface for case-independent access to the HTTP
    headers. The raw headers are available as a list of (name, value)
    tuples in the `headers' variable.

    For responding, the response socket is available as a standard
    Python stream object in the `sk' variable. Again, see the ashd(7)
    manpage for what to receive and transmit on the response socket.

    Note that instances of this class contain a reference to the live
    socket used for responding to requests, which should be closed
    when you are done with the request. The socket can be closed
    manually by calling the close() method on this
    object. Alternatively, this class implements the resource-manager
    interface, so that it can be used in `with' statements.
    """
    
    def __init__(self, method, url, ver, rest, headers, fd):
        self.method = method
        self.url = url
        self.ver = ver
        self.rest = rest
        self.headers = headers
        self.bsk = socket.fromfd(fd, socket.AF_UNIX, socket.SOCK_STREAM)
        self.sk = self.bsk.makefile('r+')
        os.close(fd)

    def close(self):
        "Close this request's response socket."
        self.sk.close()
        self.bsk.close()

    def __getitem__(self, header):
        """Find a HTTP header case-insensitively. For example,
        req["Content-Type"] returns the value of the content-type
        header regardlessly of whether the client specified it as
        "Content-Type", "content-type" or "Content-type".
        """
        header = header.lower()
        for key, val in self.headers:
            if key.lower() == header:
                return val
        raise KeyError(header)

    def __contains__(self, header):
        """Works analogously to the __getitem__ method for checking
        header presence case-insensitively.
        """
        header = header.lower()
        for key, val in self.headers:
            if key.lower() == header:
                return True
        return False

    def dup(self):
        """Creates a duplicate of this request, referring to a
        duplicate of the response socket.
        """
        return req(self.method, self.url, self.ver, self.rest, self.headers, os.dup(self.bsk.fileno()))

    def match(self, match):
        """If the `match' argument matches exactly the leading part of
        the rest string, this method strips that part of the rest
        string off and returns True. Otherwise, it returns False
        without doing anything.

        This can be used for simple dispatching. For example:
        if req.match("foo/"):
            handle(req)
        elif req.match("bar/"):
            handle_otherwise(req)
        else:
            util.respond(req, "Not found", status = "404 Not Found", ctype = "text/plain")
        """
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
    """Receive a single ashd request on the specified socket file
    descriptor (or standard input if unspecified).

    The returned value is an instance of the `req' class. As per its
    description, care should be taken to close() the request when
    done, to avoid leaking response sockets. If end-of-file is
    received on the socket, None is returned.

    This function may either raise an OSError if an error occurs on
    the socket, or an ashd.proto.protoerr if the incoming request is
    invalidly encoded.
    """
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
    """Encode and send a single request to the specified socket file
    descriptor using the ashd protocol. The request should be an
    instance of the `req' class.

    This function may raise an OSError if an error occurs on the
    socket.
    """
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
