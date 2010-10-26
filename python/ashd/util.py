import os, socket
import proto

def stdfork(argv, chinit = None):
    csk, psk = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    pid = os.fork()
    if pid == 0:
        try:
            os.dup2(csk.fileno(), 0)
            for fd in xrange(3, 1024):
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

def respond(req, body, status = ("200 OK"), ctype = "text/html"):
    if type(body) == unicode:
        body = body.decode("utf-8")
        if ctype[:5] == "text/" and ctype.find(';') < 0:
            ctype = ctype + "; charset=utf-8"
    else:
        body = str(body)
    try:
        req.sk.write("HTTP/1.1 %s\n" % status)
        req.sk.write("Content-Type: %s\n" % ctype)
        req.sk.write("Content-Length: %i\n" % len(body))
        req.sk.write("\n")
        req.sk.write(body)
    finally:
        req.close()

def serveloop(handler, sock = 0):
    while True:
        req = proto.recvreq(sock)
        if req is None:
            break
        try:
            handler(req)
        finally:
            req.close()
