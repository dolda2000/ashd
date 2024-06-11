import collections.abc
try:
    import pdm.perf
except:
    pdm = None
try:
    import time
    clock_thread = time.CLOCK_THREAD_CPUTIME_ID
except:
    clock_thread = None

reqstat = {}

if pdm:
    statistics = pdm.perf.staticdir()
    statistics["req"] = pdm.perf.valueattr(reqstat)
    requests = pdm.perf.eventobj()

    class reqstart(pdm.perf.startevent):
        def __init__(self, env):
            super().__init__()
            self.method = env.get("REQUEST_METHOD")
            self.uri = env.get("REQUEST_URI")
            self.host = env.get("HTTP_HOST")
            self.script_uri = env.get("SCRIPT_NAME")
            self.script_path = env.get("SCRIPT_FILENAME")
            self.pathinfo = env.get("PATH_INFO")
            self.querystring = env.get("QUERY_STRING")
            self.remoteaddr = env.get("REMOTE_ADDR")
            self.remoteport = env.get("REMOTE_PORT")
            self.scheme = env.get("wsgi.url_scheme")
            if clock_thread is not None:
                self.icpu = time.clock_gettime(clock_thread)

    class reqfinish(pdm.perf.finishevent):
        def __init__(self, start, aborted, status):
            super().__init__(start, aborted)
            self.status = status
            self.cputime = 0
            if clock_thread is not None:
                self.cputime = time.clock_gettime(clock_thread) - start.icpu

class request(object):
    def __init__(self, env):
        self.resp = None
        if pdm:
            self.startev = reqstart(env)
            requests.notify(self.startev)

    def response(self, resp):
        self.resp = resp

    def finish(self, aborted):
        key = None
        status = None
        try:
            if len(self.resp) > 0:
                status = self.resp[0]
                if isinstance(status, collections.abc.ByteString):
                    status = status.decode("latin-1")
                else:
                    status = str(status)
                p = status.find(" ")
                if p < 0:
                    key = status
                else:
                    key = status[:p]
        except:
            pass
        reqstat[key] = reqstat.setdefault(key, 0) + 1
        if pdm:
            requests.notify(reqfinish(self.startev, aborted, status))

    def __enter__(self):
        return self
    
    def __exit__(self, *excinfo):
        self.finish(bool(excinfo[0]))
        return False
