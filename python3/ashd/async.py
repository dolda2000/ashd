import os, threading, select

class pool(object):
    def __init__(self):
        self.clients = set()
        self.lock = threading.RLock()
        self.th = None
        self.ipipe = -1

    def add(self, cl):
        with self.lock:
            self.clients.add(cl)
            self._ckrun()
            cl.registered = self
        self._interrupt()

    def __iter__(self):
        with self.lock:
            return iter([cl for cl in self.clients if not cl.closed])

    def broadcast(self, data, eof=False):
        with self.lock:
            for cl in self:
                cl.obuf.extend(data)
                if eof:
                    cl.closed = True
        self._interrupt()

    def _ckrun(self):
        if self.clients and self.th is None:
            th = threading.Thread(target=self._run, name="Async watcher thread")
            th.start()
            self.th = th

    def _interrupt(self):
        fd = self.ipipe
        if fd >= 0 and  threading.current_thread() != self.th:
            os.write(fd, b"a")

    def _remove(self, cl):
        self.clients.remove(cl)
        cl.registered = None
        cl._doclose()

    def _run(self):
        ipr, ipw = None, None
        try:
            ipr, ipw = os.pipe()
            self.ipipe = ipw
            while True:
                with self.lock:
                    for cl in list(self.clients):
                        if cl.closed and not cl.writable:
                            self._remove(cl)
                    if not self.clients:
                        break
                    rsk = [cl for cl in self.clients if not cl.closed] + [ipr]
                    wsk = [cl for cl in self.clients if cl.writable]
                # XXX: Switch to epoll.
                rsk, wsk, esk = select.select(rsk, wsk, [])
                for sk in rsk:
                    if sk == ipr:
                        os.read(ipr, 1024)
                    elif sk in self.clients:
                        sk._doread()
                for sk in wsk:
                    if sk in self.clients:
                        sk._dowrite()
        finally:
            with self.lock:
                self.th = None
                self.ipipe = -1
                self._ckrun()
            if ipr is not None:
                try: os.close(ipr)
                except: pass
            if ipw is not None:
                try: os.close(ipw)
                except: pass

class client(object):
    pool = None

    def __init__(self, sock):
        self.sk = sock
        self.obuf = bytearray()
        self.closed = False
        self.registered = None
        p = self.pool
        if p is not None:
            p.add(self)

    def fileno(self):
        return self.sk.fileno()

    def close(self):
        self.closed = True
        if self.registered:
            self.registered._interrupt()

    def write(self, data):
        self.obuf.extend(data)
        if self.registered:
            self.registered._interrupt()

    @property
    def writable(self):
        return bool(self.obuf)

    def gotdata(self, data):
        if data == b"":
            self.close()

    def _doread(self):
        try:
            ret = self.sk.recv(1024)
        except IOError:
            self.close()
        self.gotdata(ret)

    def _dowrite(self):
        try:
            if self.obuf:
                ret = self.sk.send(self.obuf)
                self.obuf[:ret] = b""
        except IOError:
            self.close()

    def _doclose(self):
        try:
            self.sk.close()
        except IOError:
            pass
