import os, threading, types
import wsgiutil

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
            mod, mtime = modcache[path]
            if sb.st_mtime <= mtime:
                return mod
        f = open(path)
        try:
            text = f.read()
        finally:
            f.close()
        code = compile(text, path, "exec")
        mod = types.ModuleType(mangle(path))
        mod.__file__ = path
        exec code in mod.__dict__
        modcache[path] = mod, sb.st_mtime
        return mod
    finally:
        cachelock.release()

def chain(path, env, startreq):
    mod = getmod(path)
    if hasattr(mod, "wmain"):
        return (mod.wmain())(env, startreq)
    elif hasattr(mod, "application"):
        return mod.application(env, startreq)
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
