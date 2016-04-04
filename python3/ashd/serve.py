import sys, os, threading, time, logging, select, queue, collections
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
    def __init__(self, *, handler):
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
        while len(req.buffer) > 0:
            rls, wls, els = select.select([], [req], [req])
            req.flush()
    def close(self):
        pass

    @classmethod
    def parseargs(cls, **args):
        if len(args) > 0:
            raise ValueError("unknown handler argument: " + next(iter(args)))
        return {}

class single(handler):
    cname = "single"

    def handle(self, req):
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

def dbg(*a):
    f = True
    for o in a:
        if not f:
            sys.stderr.write(" ")
        sys.stderr.write(str(a))
        f = False
    sys.stderr.write("\n")
    sys.stderr.flush()

class freethread(handler):
    cname = "free"

    def __init__(self, *, max=None, timeout=None, **kw):
        super().__init__(**kw)
        self.current = set()
        self.lk = threading.Lock()
        self.tcond = threading.Condition(self.lk)
        self.max = max
        self.timeout = timeout

    @classmethod
    def parseargs(cls, *, max=None, abort=None, **args):
        ret = super().parseargs(**args)
        if max:
            ret["max"] = int(max)
        if abort:
            ret["timeout"] = int(abort)
        return ret

    def handle(self, req):
        with self.lk:
            if self.max is not None:
                if self.timeout is not None:
                    now = start = time.time()
                    while len(self.current) >= self.max:
                        self.tcond.wait(start + self.timeout - now)
                        now = time.time()
                        if now - start > self.timeout:
                            os.abort()
                else:
                    while len(self.current) >= self.max:
                        self.tcond.wait()
            th = reqthread(target=self.run, args=[req])
            th.registered = False
            th.start()
            while not th.registered:
                self.tcond.wait()

    def run(self, req):
        try:
            th = threading.current_thread()
            with self.lk:
                self.current.add(th)
                th.registered = True
                self.tcond.notify_all()
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
                    self.tcond.notify_all()
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
    cname = "pool"

    def __init__(self, *, max=25, qsz=100, timeout=None, **kw):
        super().__init__(**kw)
        self.current = set()
        self.clk = threading.Lock()
        self.ccond = threading.Condition(self.clk)
        self.queue = collections.deque()
        self.waiting = set()
        self.waitlimit = 5
        self.wlstart = 0.0
        self.qlk = threading.Lock()
        self.qcond = threading.Condition(self.qlk)
        self.max = max
        self.qsz = qsz
        self.timeout = timeout

    @classmethod
    def parseargs(cls, *, max=None, queue=None, abort=None, **args):
        ret = super().parseargs(**args)
        if max:
            ret["max"] = int(max)
        if queue:
            ret["qsz"] = int(queue)
        if abort:
            ret["timeout"] = int(abort)
        return ret

    def handle(self, req):
        spawn = False
        with self.qlk:
            if self.timeout is not None:
                now = start = time.time()
                while len(self.queue) >= self.qsz:
                    self.qcond.wait(start + self.timeout - now)
                    now = time.time()
                    if now - start > self.timeout:
                        os.abort()
            else:
                while len(self.current) >= self.qsz:
                    self.qcond.wait()
            self.queue.append(req)
            self.qcond.notify()
            if len(self.waiting) < 1:
                spawn = True
        if spawn:
            with self.clk:
                if len(self.current) < self.max:
                    th = reqthread(target=self.run)
                    th.registered = False
                    th.start()
                    while not th.registered:
                        self.ccond.wait()

    def handle1(self, req):
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

    def run(self):
        timeout = 10.0
        th = threading.current_thread()
        with self.clk:
            self.current.add(th)
            th.registered = True
            self.ccond.notify_all()
        try:
            while True:
                start = now = time.time()
                with self.qlk:
                    while len(self.queue) < 1:
                        if len(self.waiting) >= self.waitlimit and now - self.wlstart >= timeout:
                            return
                        self.waiting.add(th)
                        try:
                            if len(self.waiting) == self.waitlimit:
                                self.wlstart = now
                            self.qcond.wait(start + timeout - now)
                        finally:
                            self.waiting.remove(th)
                        now = time.time()
                        if now - start > timeout:
                            return
                    req = self.queue.popleft()
                try:
                    self.handle1(req)
                finally:
                    req.close()
        finally:
            with self.clk:
                self.current.remove(th)

    def close(self):
        while True:
            with self.clk:
                if len(self.current) > 0:
                    th = next(iter(self.current))
                else:
                    return
            th.join()

class resplex(handler):
    cname = "rplex"

    def __init__(self, *, max=None, **kw):
        super().__init__(**kw)
        self.current = set()
        self.lk = threading.Lock()
        self.tcond = threading.Condition(self.lk)
        self.max = max
        self.cqueue = queue.Queue(5)
        self.cnpipe = os.pipe()
        self.rthread = reqthread(name="Response thread", target=self.handle2)
        self.rthread.start()

    @classmethod
    def parseargs(cls, *, max=None, **args):
        ret = super().parseargs(**args)
        if max:
            ret["max"] = int(max)
        return ret

    def ckflush(self, req):
        raise Exception("resplex handler does not support the write() function")

    def handle(self, req):
        with self.lk:
            if self.max is not None:
                while len(self.current) >= self.max:
                    self.tcond.wait()
            th = reqthread(target=self.handle1, args=[req])
            th.registered = False
            th.start()
            while not th.registered:
                self.tcond.wait()

    def handle1(self, req):
        try:
            th = threading.current_thread()
            with self.lk:
                self.current.add(th)
                th.registered = True
                self.tcond.notify_all()
            try:
                env = req.mkenv()
                respobj = req.handlewsgi(env, req.startreq)
                respiter = iter(respobj)
                if not req.status:
                    log.error("request handler returned without calling start_request")
                    if hasattr(respiter, "close"):
                        respiter.close()
                    return
                else:
                    self.cqueue.put((req, respiter))
                    os.write(self.cnpipe[1], b" ")
                    req = None
            finally:
                with self.lk:
                    self.current.remove(th)
                    self.tcond.notify_all()
        except closed:
            pass
        except:
            log.error("exception occurred when handling request", exc_info=True)
        finally:
            if req is not None:
                req.close()

    def handle2(self):
        try:
            rp = self.cnpipe[0]
            current = {}

            def closereq(req):
                respiter = current[req]
                try:
                    if respiter is not None and hasattr(respiter, "close"):
                        respiter.close()
                except:
                    log.error("exception occurred when closing iterator", exc_info=True)
                try:
                    req.close()
                except:
                    log.error("exception occurred when closing request", exc_info=True)
                del current[req]
            def ckiter(req):
                respiter = current[req]
                if respiter is not None:
                    rem = False
                    try:
                        data = next(respiter)
                    except StopIteration:
                        rem = True
                        try:
                            req.flushreq()
                        except:
                            log.error("exception occurred when handling response data", exc_info=True)
                    except:
                        rem = True
                        log.error("exception occurred when iterating response", exc_info=True)
                    if not rem:
                        if data:
                            try:
                                req.flushreq()
                                req.writedata(data)
                            except:
                                log.error("exception occurred when handling response data", exc_info=True)
                                rem = True
                    if rem:
                        current[req] = None
                        try:
                            if hasattr(respiter, "close"):
                                respiter.close()
                        except:
                            log.error("exception occurred when closing iterator", exc_info=True)
                        respiter = None
                if respiter is None and not req.buffer:
                    closereq(req)

            while True:
                bufl = list(req for req in current.keys() if req.buffer)
                rls, wls, els = select.select([rp], bufl, [rp] + bufl)
                if rp in rls:
                    ret = os.read(rp, 1024)
                    if not ret:
                        os.close(rp)
                        return
                    try:
                        while True:
                            req, respiter = self.cqueue.get(False)
                            current[req] = respiter
                            ckiter(req)
                    except queue.Empty:
                        pass
                for req in wls:
                    try:
                        req.flush()
                    except closed:
                        closereq(req)
                    except:
                        log.error("exception occurred when writing response", exc_info=True)
                        closereq(req)
                    else:
                        if len(req.buffer) < 65536:
                            ckiter(req)
        except:
            log.critical("unexpected exception occurred in response handler thread", exc_info=True)
            os.abort()

    def close(self):
        while True:
            with self.lk:
                if len(self.current) > 0:
                    th = next(iter(self.current))
                else:
                    break
            th.join()
        os.close(self.cnpipe[1])
        self.rthread.join()

names = {cls.cname: cls for cls in globals().values() if
         isinstance(cls, type) and
         issubclass(cls, handler) and
         hasattr(cls, "cname")}

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
