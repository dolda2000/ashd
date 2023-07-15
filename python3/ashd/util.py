"""High-level utility module for ashd(7)

This module implements a rather convenient interface for writing ashd
handlers, wrapping the low-level ashd.proto module.
"""

import os, socket, collections
from . import proto

__all__ = ["stdfork", "pchild", "respond", "serveloop"]

def stdfork(argv, chinit = None):
    """Fork a persistent handler process using the `argv' argument
    list, as per the standard ashd(7) calling convention. For an
    easier-to-use interface, see the `pchild' class.

    If a callable object of no arguments is provided in the `chinit'
    argument, it will be called in the child process before exec()'ing
    the handler program, and can be used to set parameters for the new
    process, such as working directory, nice level or ulimits.

    Returns the file descriptor of the socket for sending requests to
    the new child.
    """
    csk, psk = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    pid = os.fork()
    if pid == 0:
        try:
            os.dup2(csk.fileno(), 0)
            for fd in range(3, 1024):
                try:
                    os.close(fd)
                except:
                    pass
            if chinit is not None:
                chinit()
            os.execvp(argv[0], argv)
        finally:
            os._exit(127)
    csk.close()
    fd = os.dup(psk.fileno())
    psk.close()
    return fd

class pchild(object):
    """class pchild(argv, autorespawn=False, chinit=None)

    Represents a persistent child handler process, started as per the
    standard ashd(7) calling convention. It will be called with the
    `argv' argument lest, which should be a list (or other iterable)
    of strings.

    If `autorespawn' is specified as True, the child process will be
    automatically restarted if a request cannot be successfully sent
    to it.

    For a description of the `chinit' argument, see `stdfork'.

    When this child handler should be disposed of, care should be
    taken to call the close() method to release its socket and let it
    exit. This class also implements the resource-manager interface,
    so that it can be used in `with' statements.
    """
    
    def __init__(self, argv, autorespawn = False, chinit = None):
        self.argv = argv
        self.chinit = chinit
        self.fd = -1
        self.respawn = autorespawn
        self.spawn()

    def spawn(self):
        """Start the child handler, or restart it if it is already
        running. You should not have to call this method manually
        unless you explicitly want to manage the process' lifecycle.
        """
        self.close()
        self.fd = stdfork(self.argv, self.chinit)

    def close(self):
        """Close this child handler's socket. For normal child
        handlers, this will make the program terminate normally.
        """
        if self.fd >= 0:
            os.close(self.fd)
            self.fd = -1

    def __del__(self):
        self.close()

    def passreq(self, req):
        """Pass the specified request (which should be an instance of
        the ashd.proto.req class) to this child handler. If the child
        handler fails for some reason, and `autorespawn' was specified
        as True when creating this handler, one attempt will be made
        to restart it.

        Note: You still need to close the request normally.

        This method may raise an OSError if the request fails and
        autorespawning was either not requested, or if the
        autorespawning fails.
        """
        try:
            proto.sendreq(self.fd, req)
        except OSError:
            if self.respawn:
                self.spawn()
                proto.sendreq(self.fd, req)

    def __enter__(self):
        return self

    def __exit__(self, *excinfo):
        self.close()
        return False

def respond(req, body, status = ("200 OK"), ctype = "text/html"):
    """Simple function for conveniently responding to a request.

    Sends the specified body text to the request's response socket,
    prepending an HTTP header with the appropriate Content-Type and
    Content-Length headers, and then closes the response socket.

    The `status' argument can be used to specify a non-200 response,
    and the `ctype' argument can be used to specify a non-HTML
    MIME-type.

    If `body' is not a byte string, its string representation will be
    encoded as UTF-8.

    For example:
        respond(req, "Not found", status = "404 Not Found", ctype = "text/plain")
    """
    if isinstance(body, collections.ByteString):
        body = bytes(body)
    else:
        body = str(body)
        body = body.encode("utf-8")
        if ctype[:5] == "text/" and ctype.find(';') < 0:
            ctype = ctype + "; charset=utf-8"
    try:
        head = ""
        head += "HTTP/1.1 %s\n" % status
        head += "Content-Type: %s\n" % ctype
        head += "Content-Length: %i\n" % len(body)
        head += "\n"
        req.sk.write(head.encode("ascii"))
        req.sk.write(body)
    finally:
        req.close()

def serveloop(handler, sock = 0):
    """Implements a simple loop for serving requests sequentially, by
    receiving requests from standard input (or the specified socket),
    passing them to the specified handler function, and finally making
    sure to close them. Returns when end-of-file is received on the
    incoming socket.

    The handler function should be a callable object of one argument,
    and is called once for each received request.
    """
    while True:
        try:
            req = proto.recvreq(sock)
        except InterruptedError:
            continue
        if req is None:
            break
        try:
            handler(req)
        finally:
            req.close()
