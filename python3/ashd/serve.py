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

    @classmethod
    def parseargs(cls, **args):
        if len(args) > 0:
            raise ValueError("unknown handler argument: " + next(iter(args)))
        return {}

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
                    return
            th.join()

class threadpool(handler):
    def __init__(self, *, min=0, max=20, live=300, **kw):
        super().__init__(**kw)
        self.current = set()
        self.free = set()
        self.lk = threading.RLock()
        self.pcond = threading.Condition(self.lk)
        self.rcond = threading.Condition(self.lk)
        self.wreq = None
        self.min = min
        self.max = max
        self.live = live
        for i in range(self.min):
            self.newthread()

    @classmethod
    def parseargs(cls, *, min=None, max=None, live=None, **args):
        ret = super().parseargs(**args)
        if min:
            ret["min"] = int(min)
        if max:
            ret["max"] = int(max)
        if live:
            ret["live"] = int(live)
        return ret

    def newthread(self):
        with self.lk:
            th = reqthread(target=self.loop)
            th.start()
            while not th in self.current:
                self.pcond.wait()

    def ckflush(self, req):
        while len(req.buffer) > 0:
            rls, wls, els = select.select([], [req], [req])
            req.flush()

    def _handle(self, req):
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
            req.close()

    def loop(self):
        th = threading.current_thread()
        with self.lk:
            self.current.add(th)
        try:
            while True:
                with self.lk:
                    self.free.add(th)
                    try:
                        self.pcond.notify_all()
                        now = start = time.time()
                        while self.wreq is None:
                            self.rcond.wait(start + self.live - now)
                            now = time.time()
                            if now - start > self.live:
                                if len(self.current) > self.min:
                                    self.current.remove(th)
                                    return
                                else:
                                    start = now
                        req, self.wreq = self.wreq, None
                        self.pcond.notify_all()
                    finally:
                        self.free.remove(th)
                self._handle(req)
                req = None
        finally:
            with self.lk:
                try:
                    self.current.remove(th)
                except KeyError:
                    pass
                self.pcond.notify_all()

    def handle(self, req):
        while True:
            with self.lk:
                if len(self.free) < 1 and len(self.current) < self.max:
                    self.newthread()
                while self.wreq is not None:
                    self.pcond.wait()
                if self.wreq is None:
                    self.wreq = req
                    self.rcond.notify(1)
                    return

    def close(self):
        self.live = 0
        self.min = 0
        with self.lk:
            while len(self.current) > 0:
                self.rcond.notify_all()
                self.pcond.wait(1)

names = {"free": freethread,
         "pool": threadpool}

def parsehspec(spec):
    if ":" not in spec:
        return spec, {}
    nm, spec = spec.split(":", 1)
    args = {}
    while spec:
        if "," in spec:
            part, spec = spec.split(",", 1)
        else:
            part, spec = spec, None
        if "=" in part:
            key, val = part.split("=", 1)
        else:
            key, val = part, ""
        args[key] = val
    return nm, args
