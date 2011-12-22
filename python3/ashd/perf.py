import collections
try:
    import pdm.perf
except:
    pdm = None

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
            self.remoteaddr = env.get("REMOTE_ADDR")
            self.remoteport = env.get("REMOTE_PORT")

    class reqfinish(pdm.perf.finishevent):
        def __init__(self, start, aborted, status):
            super().__init__(start, aborted)
            self.status = status

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
                if isinstance(status, collections.ByteString):
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
