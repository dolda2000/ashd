import sys, os, threading, time, logging, select, Queue
import perf

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
        super(closed, self).__init__("The client has closed the connection.")

class reqthread(threading.Thread):
    def __init__(self, name=None, **kw):
        if name is None:
            name = "Request handler %i" % reqseq()
        super(reqthread, self).__init__(name=name, **kw)

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
        p = select.poll()
        p.register(req, select.POLLOUT)
        while len(req.buffer) > 0:
            p.poll()
            req.flush()
    def close(self):
        pass

    @classmethod
    def parseargs(cls, **args):
        if len(args) > 0:
            raise ValueError("unknown handler argument: " + iter(args).next())
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

class freethread(handler):
    cname = "free"

    def __init__(self, max=None, timeout=None, **kw):
        super(freethread, self).__init__(**kw)
        self.current = set()
        self.lk = threading.Lock()
        self.tcond = threading.Condition(self.lk)
        self.max = max
        self.timeout = timeout

    @classmethod
    def parseargs(cls, max=None, abort=None, **args):
        ret = super(freethread, cls).parseargs(**args)
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
                    th = iter(self.current).next()
                else:
                    return
            th.join()

class resplex(handler):
    cname = "rplex"

    def __init__(self, max=None, **kw):
        super(resplex, self).__init__(**kw)
        self.current = set()
        self.lk = threading.Lock()
        self.tcond = threading.Condition(self.lk)
        self.max = max
        self.cqueue = Queue.Queue(5)
        self.cnpipe = os.pipe()
        self.rthread = reqthread(name="Response thread", target=self.handle2)
        self.rthread.start()

    @classmethod
    def parseargs(cls, max=None, **args):
        ret = super(resplex, cls).parseargs(**args)
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
                    os.write(self.cnpipe[1], " ")
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
                        data = respiter.next()
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
                bufl = list(req for req in current.iterkeys() if req.buffer)
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
                    except Queue.Empty:
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
                    th = iter(self.current).next()
                else:
                    break
            th.join()
        os.close(self.cnpipe[1])
        self.rthread.join()

names = dict((cls.cname, cls) for cls in globals().itervalues() if
             isinstance(cls, type) and
             issubclass(cls, handler) and
             hasattr(cls, "cname"))

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
