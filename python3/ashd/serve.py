import os, threading, time, logging

log = logging.getLogger("ashd.serve")
seq = 1
seqlk = threading.Lock()

def reqseq():
    global seq
    with seqlk:
        s = seq
        seq += 1
        return s

class reqthread(threading.Thread):
    def __init__(self, name=None):
        if name is None:
            name = "Request handler %i" % reqseq()
        super().__init__(name=name)

    def handle(self):
        raise Exception()

    def run(self):
        try:
            self.handle()
        except:
            log.error("exception occurred when handling request", exc_info=True)

class closed(IOError):
    def __init__(self):
        super().__init__("The client has closed the connection.")

class wsgithread(reqthread):
    def __init__(self, **kwargs):
        super().__init__(**kwargs)
        self.status = None
        self.headers = []
        self.respsent = False

    def handlewsgi(self):
        raise Exception()
    def writehead(self, status, headers):
        raise Exception()
    def writedata(self, data):
        raise Exception()

    def write(self, data):
        if not data:
            return
        self.flushreq()
        self.writedata(data)

    def flushreq(self):
        if not self.respsent:
            if not self.status:
                raise Exception("Cannot send response body before starting response.")
            self.respsent = True
            self.writehead(self.status, self.headers)

    def startreq(self, status, headers, exc_info=None):
        if self.status:
            if exc_info:                # Nice calling convetion ^^
                try:
                    if self.respsent:
                        raise exc_info[1]
                finally:
                    exc_info = None     # CPython GC bug?
            else:
                raise Exception("Can only start responding once.")
        self.status = status
        self.headers = headers
        return self.write
 
    def handle(self):
        try:
            respiter = self.handlewsgi()
            try:
                for data in respiter:
                    self.write(data)
                if self.status:
                    self.flushreq()
            finally:
                if hasattr(respiter, "close"):
                    respiter.close()
        except closed:
            pass

class calllimiter(object):
    def __init__(self, limit):
        self.limit = limit
        self.lock = threading.Condition()
        self.inflight = 0

    def waited(self, time):
        if time > 10:
            raise RuntimeError("Waited too long")

    def __enter__(self):
        with self.lock:
            start = time.time()
            while self.inflight >= self.limit:
                self.lock.wait(10)
                self.waited(time.time() - start)
            self.inflight += 1
            return self

    def __exit__(self, *excinfo):
        with self.lock:
            self.inflight -= 1
            self.lock.notify()
        return False

    def call(self, target):
        with self:
            return target()

class abortlimiter(calllimiter):
    def waited(self, time):
        if time > 10:
            os.abort()
