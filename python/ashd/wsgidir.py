import os, threading, types
import wsgiutil

class cachedmod:
    def __init__(self, mod, mtime):
        self.lock = threading.Lock()
        self.mod = mod
        self.mtime = mtime

exts = {}
modcache = {}
cachelock = threading.Lock()

def mangle(path):
    ret = ""
    for c in path:
        if c.isalnum():
            ret += c
        else:
            ret += "_"
    return ret

def getmod(path):
    sb = os.stat(path)
    cachelock.acquire()
    try:
        if path in modcache:
            entry = modcache[path]
            if sb.st_mtime <= entry.mtime:
                return entry
        
        f = open(path)
        try:
            text = f.read()
        finally:
            f.close()
        code = compile(text, path, "exec")
        mod = types.ModuleType(mangle(path))
        mod.__file__ = path
        exec code in mod.__dict__
        entry = cachedmod(mod, sb.st_mtime)
        modcache[path] = entry
        return entry
    finally:
        cachelock.release()

def chain(path, env, startreq):
    mod = getmod(path)
    entry = None
    if mod is not None:
        mod.lock.acquire()
        try:
            if hasattr(mod, "entry"):
                entry = mod.entry
            else:
                if hasattr(mod.mod, "wmain"):
                    entry = mod.mod.wmain([])
                elif hasattr(mod.mod, "application"):
                    entry = mod.mod.application
                mod.entry = entry
        finally:
            mod.lock.release()
    if entry is not None:
        return entry(env, startreq)
    return wsgiutil.simpleerror(env, startreq, 500, "Internal Error", "Invalid WSGI handler.")
exts["wsgi"] = chain

def application(env, startreq):
    if not "SCRIPT_FILENAME" in env:
        return wsgiutil.simpleerror(env, startreq, 500, "Internal Error", "The server is erroneously configured.")
    path = env["SCRIPT_FILENAME"]
    base = os.path.basename(path)
    p = base.rfind('.')
    if p < 0 or not os.access(path, os.R_OK):
        return wsgiutil.simpleerror(env, startreq, 500, "Internal Error", "The server is erroneously configured.")
    ext = base[p + 1:]
    if not ext in exts:
        return wsgiutil.simpleerror(env, startreq, 500, "Internal Error", "The server is erroneously configured.")
    return(exts[ext](path, env, startreq))

def wmain(argv):
    return application
