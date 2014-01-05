import sys, os, threading, time, logging, select
from . import perf

log = logging.getLogger("ashd.serve")
seq = 1
seqlk = threading.Lock()

def reqseq():
    global seq
    with seqlk:
        s = seq
        seq += 1
        return s

class closed(IOError):
    def __init__(self):
        super().__init__("The client has closed the connection.")

class reqthread(threading.Thread):
    def __init__(self, *, name=None, **kw):
        if name is None:
            name = "Request handler %i" % reqseq()
        super().__init__(name=name, **kw)

class wsgirequest(object):
    def __init__(self, handler):
        self.status = None
        self.headers = []
        self.respsent = False
        self.handler = handler
        self.buffer = bytearray()

    def handlewsgi(self):
        raise Exception()
    def fileno(self):
        raise Exception()
    def writehead(self, status, headers):
        raise Exception()
    def flush(self):
        raise Exception()
    def close(self):
        pass
    def writedata(self, data):
        self.buffer.extend(data)

    def flushreq(self):
        if not self.respsent:
            if not self.status:
                raise Exception("Cannot send response body before starting response.")
            self.respsent = True
            self.writehead(self.status, self.headers)

    def write(self, data):
        if not data:
            return
        self.flushreq()
        self.writedata(data)
        self.handler.ckflush(self)

    def startreq(self, status, headers, exc_info=None):
        if self.status:
            if exc_info:
                try:
                    if self.respsent:
                        raise exc_info[1]
                finally:
                    exc_info = None
            else:
                raise Exception("Can only start responding once.")
        self.status = status
        self.headers = headers
        return self.write

class handler(object):
    def handle(self, request):
        raise Exception()
    def ckflush(self, req):
        raise Exception()
    def close(self):
        pass

class freethread(handler):
    def __init__(self, **kw):
        super().__init__(**kw)
        self.current = set()
        self.lk = threading.Lock()

    def handle(self, req):
        reqthread(target=self.run, args=[req]).start()

    def ckflush(self, req):
        while len(req.buffer) > 0:
            rls, wls, els = select.select([], [req], [req])
            req.flush()

    def run(self, req):
        try:
            th = threading.current_thread()
            with self.lk:
                self.current.add(th)
            try:
                env = req.mkenv()
                with perf.request(env) as reqevent:
                    respiter = req.handlewsgi(env, req.startreq)
                    for data in respiter:
                        req.write(data)
                    if req.status:
                        reqevent.response([req.status, req.headers])
                        req.flushreq()
                    self.ckflush(req)
            except closed:
                pass
            except:
                log.error("exception occurred when handling request", exc_info=True)
            finally:
                with self.lk:
                    self.current.remove(th)
        finally:
            req.close()

    def close(self):
        while True:
            with self.lk:
                if len(self.current) > 0:
                    th = next(iter(self.current))
                else:
                    th = None
            th.join()

names = {"free": freethread}
