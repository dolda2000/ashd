import sys, os, errno, threading, select, traceback

class epoller(object):
    exc_handler = None

    def __init__(self):
        self.registered = {}
        self.lock = threading.RLock()
        self.ep = None
        self.th = None
        self.stopped = False
        self._daemon = True

    @staticmethod
    def _evsfor(ch):
        return ((select.EPOLLIN if ch.readable else 0) |
                (select.EPOLLOUT if ch.writable else 0))

    def _ckrun(self):
        if self.registered and self.th is None:
            th = threading.Thread(target=self._run, name="Async epoll thread")
            th.daemon = self._daemon
            th.start()
            self.th = th

    def exception(self, ch, *exc):
        self.remove(ch)
        if self.exc_handler is None:
            traceback.print_exception(*exc)
        else:
            self.exc_handler(ch, *exc)

    def _cb(self, ch, nm):
        try:
            m = getattr(ch, nm, None)
            if m is None:
                raise AttributeError("%r has no %s method" % (ch, nm))
            m()
        except Exception as exc:
            self.exception(ch, *sys.exc_info())

    def _closeall(self):
        with self.lock:
            while self.registered:
                fd, (ch, evs) = next(iter(self.registered.items()))
                del self.registered[fd]
                self.ep.unregister(fd)
                self._cb(ch, "close")

    def _run(self):
        ep = select.epoll()
        try:
            with self.lock:
                for fd, (ob, evs) in self.registered.items():
                    ep.register(fd, evs)
                self.ep = ep

            while self.registered:
                if self.stopped:
                    self._closeall()
                    break
                try:
                    evlist = ep.poll(10)
                except IOError as exc:
                    if exc.errno == errno.EINTR:
                        continue
                    raise
                for fd, evs in evlist:
                    with self.lock:
                        if fd not in self.registered:
                            continue
                        ch, cevs = self.registered[fd]
                        if fd in self.registered and evs & (select.EPOLLIN | select.EPOLLHUP | select.EPOLLERR):
                            self._cb(ch, "read")
                        if fd in self.registered and evs & select.EPOLLOUT:
                            self._cb(ch, "write")
                        if fd in self.registered:
                            nevs = self._evsfor(ch)
                            if nevs == 0:
                                del self.registered[fd]
                                ep.unregister(fd)
                                self._cb(ch, "close")
                            elif nevs != cevs:
                                self.registered[fd] = ch, nevs
                                ep.modify(fd, nevs)

        finally:
            with self.lock:
                self.th = None
                self.ep = None
                self._ckrun()
            ep.close()

    @property
    def daemon(self): return self._daemon
    @daemon.setter
    def daemon(self, value):
        self._daemon = bool(value)
        with self.lock:
            if self.th is not None:
                self.th = daemon = self._daemon

    def add(self, ch):
        with self.lock:
            fd = ch.fileno()
            if fd in self.registered:
                raise KeyError("fd %i is already registered" % fd)
            evs = self._evsfor(ch)
            if evs == 0:
                ch.close()
                return
            ch.watcher = self
            self.registered[fd] = (ch, evs)
            if self.ep:
                self.ep.register(fd, evs)
            self._ckrun()

    def remove(self, ch, ignore=False):
        with self.lock:
            fd = ch.fileno()
            if fd not in self.registered:
                if ignore:
                    return
                raise KeyError("fd %i is not registered" % fd)
            pch, cevs = self.registered[fd]
            if pch is not ch:
                raise ValueError("fd %i registered via object %r, cannot remove with %r" % (pch, ch))
            del self.registered[fd]
            if self.ep:
                self.ep.unregister(fd)
            ch.close()

    def update(self, ch, ignore=False):
        with self.lock:
            fd = ch.fileno()
            if fd not in self.registered:
                if ignore:
                    return
                raise KeyError("fd %i is not registered" % fd)
            pch, cevs = self.registered[fd]
            if pch is not ch:
                raise ValueError("fd %i registered via object %r, cannot update with %r" % (pch, ch))
            evs = self._evsfor(ch)
            if evs == 0:
                del self.registered[fd]
                if self.ep:
                    self.ep.unregister(fd)
                ch.close()
            elif evs != cevs:
                self.registered[fd] = ch, evs
                if self.ep:
                    self.ep.modify(fd, evs)

    def stop(self):
        if threading.current_thread() == self.th:
            self.stopped = True
        else:
            def tgt():
                self.stopped = True
            cb = callbuffer()
            cb.call(tgt)
            cb.stop()
            self.add(cb)

def watcher():
    return epoller()

class channel(object):
    readable = False
    writable = False

    def __init__(self):
        self.watcher = None

    def fileno(self):
        raise NotImplementedError("fileno()")

    def close(self):
        pass

class sockbuffer(channel):
    def __init__(self, socket, **kwargs):
        super().__init__(**kwargs)
        self.sk = socket
        self.eof = False
        self.obuf = bytearray()

    def fileno(self):
        return self.sk.fileno()

    def close(self):
        self.sk.close()

    def gotdata(self, data):
        if data == b"":
            self.eof = True

    def send(self, data, eof=False):
        self.obuf.extend(data)
        if eof:
            self.eof = True
        if self.watcher is not None:
            self.watcher.update(self, True)

    @property
    def readable(self):
        return not self.eof
    def read(self):
        try:
            data = self.sk.recv(1024)
            self.gotdata(data)
        except IOError:
            self.obuf[:] = b""
            self.eof = True

    @property
    def writable(self):
        return bool(self.obuf);
    def write(self):
        try:
            ret = self.sk.send(self.obuf)
            self.obuf[:ret] = b""
        except IOError:
            self.obuf[:] = b""
            self.eof = True

class callbuffer(channel):
    def __init__(self, **kwargs):
        super().__init__(**kwargs)
        self.queue = []
        self.rp, self.wp = os.pipe()
        self.lock = threading.Lock()
        self.eof = False

    def fileno(self):
        return self.rp

    def close(self):
        with self.lock:
            try:
                if self.wp >= 0:
                    os.close(self.wp)
                self.wp = -1
            finally:
                if self.rp >= 0:
                    os.close(self.rp)
                self.rp = -1

    @property
    def readable(self):
        return not self.eof
    def read(self):
        with self.lock:
            try:
                data = os.read(self.rp, 1024)
                if data == b"":
                    self.eof = True
            except IOError:
                self.eof = True
            cbs = list(self.queue)
            self.queue[:] = []
        for cb in cbs:
            cb()

    writable = False

    def call(self, cb):
        with self.lock:
            if self.wp < 0:
                raise Exception("stopped")
            self.queue.append(cb)
            os.write(self.wp, b"a")

    def stop(self):
        with self.lock:
            if self.wp >= 0:
                os.close(self.wp)
                self.wp = -1

def currentwatcher(io, current):
    def run():
        while current:
            current.wait()
        io.stop()
    threading.Thread(target=run, name="Current watcher").start()
