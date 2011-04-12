import sys
import threading

class protoerr(Exception):
    pass

class closed(IOError):
    def __init__(self):
        super(closed, self).__init__("The client has closed the connection.")

def readns(sk):
    hln = 0
    while True:
        c = sk.read(1)
        if c == ':':
            break
        elif c >= '0' or c <= '9':
            hln = (hln * 10) + (ord(c) - ord('0'))
        else:
            raise protoerr, "Invalid netstring length byte: " + c
    ret = sk.read(hln)
    if sk.read(1) != ',':
        raise protoerr, "Non-terminated netstring"
    return ret

def readhead(sk):
    parts = readns(sk).split('\0')[:-1]
    if len(parts) % 2 != 0:
        raise protoerr, "Malformed headers"
    ret = {}
    i = 0
    while i < len(parts):
        ret[parts[i]] = parts[i + 1]
        i += 2
    return ret

class reqthread(threading.Thread):
    def __init__(self, sk, handler):
        super(reqthread, self).__init__(name = "SCGI request handler")
        self.sk = sk.dup().makefile("r+")
        self.handler = handler

    def run(self):
        try:
            head = readhead(self.sk)
            self.handler(head, self.sk)
        finally:
            self.sk.close()

def handlescgi(sk, handler):
    t = reqthread(sk, handler)
    t.start()

def servescgi(socket, handler):
    while True:
        nsk, addr = socket.accept()
        try:
            handlescgi(nsk, handler)
        finally:
            nsk.close()

def wrapwsgi(handler):
    def handle(head, sk):
        env = dict(head)
        env["wsgi.version"] = 1, 0
        if "HTTP_X_ASH_PROTOCOL" in env:
            env["wsgi.url_scheme"] = env["HTTP_X_ASH_PROTOCOL"]
        elif "HTTPS" in env:
            env["wsgi.url_scheme"] = "https"
        else:
            env["wsgi.url_scheme"] = "http"
        env["wsgi.input"] = sk
        env["wsgi.errors"] = sys.stderr
        env["wsgi.multithread"] = True
        env["wsgi.multiprocess"] = False
        env["wsgi.run_once"] = False

        resp = []
        respsent = []

        def flushreq():
            if not respsent:
                if not resp:
                    raise Exception, "Trying to write data before starting response."
                status, headers = resp
                respsent[:] = [True]
                sk.write("Status: %s\n" % status)
                for nm, val in headers:
                    sk.write("%s: %s\n" % (nm, val))
                sk.write("\n")

        def write(data):
            if not data:
                return
            try:
                flushreq()
                sk.write(data)
                sk.flush()
            except IOError:
                raise closed()

        def startreq(status, headers, exc_info = None):
            if resp:
                if exc_info:                # Interesting, this...
                    try:
                        if respsent:
                            raise exc_info[0], exc_info[1], exc_info[2]
                    finally:
                        exc_info = None     # CPython GC bug?
                else:
                    raise Exception, "Can only start responding once."
            resp[:] = status, headers
            return write

        respiter = handler(env, startreq)
        try:
            for data in respiter:
                write(data)
            if resp:
                flushreq()
        finally:
            if hasattr(respiter, "close"):
                respiter.close()
    return handle
