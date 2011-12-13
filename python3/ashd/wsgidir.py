"""WSGI handler for serving chained WSGI modules from physical files

The WSGI handler in this module ensures that the SCRIPT_FILENAME
variable is properly set in every request and points out a file that
exists and is readable. It then dispatches the request in one of two
ways: If the header X-Ash-Python-Handler is set in the request, its
value is used as the name of a handler object to dispatch the request
to; otherwise, the file extension of the SCRIPT_FILENAME is used to
determine the handler object.

The name of a handler object is specified as a string, which is split
along its last constituent dot. The part left of the dot is the name
of a module, which is imported; and the part right of the dot is the
name of an object in that module, which should be a callable adhering
to the WSGI specification. Alternatively, the module part may be
omitted (such that the name is a string with no dots), in which case
the handler object is looked up from this module.

By default, this module will handle files with the extensions `.wsgi'
or `.wsgi3' using the `chain' handler, which chainloads such files and
runs them as independent WSGI applications. See its documentation for
details.

This module itself contains both an `application' and a `wmain'
object. If this module is used by ashd-wsgi(1) or scgi-wsgi(1) so that
its wmain function is called, arguments can be specified to it to
install handlers for other file extensions. Such arguments take the
form `.EXT=HANDLER', where EXT is the file extension to be handled,
and HANDLER is a handler name, as described above. For example, the
argument `.fpy=my.module.foohandler' can be given to pass requests for
`.fpy' files to the function `foohandler' in the module `my.module'
(which must, of course, be importable). When writing such handler
functions, you may want to use the getmod() function in this module.
"""

import os, threading, types, importlib
from . import wsgiutil

__all__ = ["application", "wmain", "getmod", "cachedmod"]

class cachedmod(object):
    """Cache entry for modules loaded by getmod()

    Instances of this class are returned by the getmod()
    function. They contain three data attributes:
     * mod - The loaded module
     * lock - A threading.Lock object, which can be used for
       manipulating this instance in a thread-safe manner
     * mtime - The time the file was last modified

    Additional data attributes can be arbitrarily added for recording
    any meta-data about the module.
    """
    def __init__(self, mod = None, mtime = -1):
        self.lock = threading.Lock()
        self.mod = mod
        self.mtime = mtime

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
    """Load the given file as a module, caching it appropriately

    The given file is loaded and compiled into a Python module. The
    compiled module is cached and returned upon subsequent requests
    for the same file, unless the file has changed (as determined by
    its mtime), in which case the cached module is discarded and the
    new file contents are reloaded in its place.

    The return value is an instance of the cachedmod class, which can
    be used for locking purposes and for storing arbitrary meta-data
    about the module. See its documentation for details.
    """
    sb = os.stat(path)
    with cachelock:
        if path in modcache:
            entry = modcache[path]
        else:
            entry = cachedmod()
            modcache[path] = entry
    with entry.lock:
        if entry.mod is None or sb.st_mtime > entry.mtime:
            with open(path, "rb") as f:
                text = f.read()
            code = compile(text, path, "exec")
            mod = types.ModuleType(mangle(path))
            mod.__file__ = path
            exec(code, mod.__dict__)
            entry.mod = mod
            entry.mtime = sb.st_mtime
        return entry

class handler(object):
    def __init__(self):
        self.lock = threading.Lock()
        self.handlers = {}
        self.exts = {}
        self.addext("wsgi", "chain")
        self.addext("wsgi3", "chain")

    def resolve(self, name):
        with self.lock:
            if name in self.handlers:
                return self.handlers[name]
            p = name.rfind('.')
            if p < 0:
                return globals()[name]
            mname = name[:p]
            hname = name[p + 1:]
            mod = importlib.import_module(mname)
            ret = getattr(mod, hname)
            self.handlers[name] = ret
            return ret
        
    def addext(self, ext, handler):
        self.exts[ext] = self.resolve(handler)

    def handle(self, env, startreq):
        if not "SCRIPT_FILENAME" in env:
            return wsgiutil.simpleerror(env, startreq, 500, "Internal Error", "The server is erroneously configured.")
        path = env["SCRIPT_FILENAME"]
        if not os.access(path, os.R_OK):
            return wsgiutil.simpleerror(env, startreq, 500, "Internal Error", "The server is erroneously configured.")
        if "HTTP_X_ASH_PYTHON_HANDLER" in env:
            handler = self.resolve(env["HTTP_X_ASH_PYTHON_HANDLER"])
        else:
            base = os.path.basename(path)
            p = base.rfind('.')
            if p < 0:
                return wsgiutil.simpleerror(env, startreq, 500, "Internal Error", "The server is erroneously configured.")
            ext = base[p + 1:]
            if not ext in self.exts:
                return wsgiutil.simpleerror(env, startreq, 500, "Internal Error", "The server is erroneously configured.")
            handler = self.exts[ext]
        return handler(env, startreq)

def wmain(*argv):
    """Main function for ashd(7)-compatible WSGI handlers

    Returns the `application' function. If any arguments are given,
    they are parsed according to the module documentation.
    """
    ret = handler()
    for arg in argv:
        if arg[0] == '.':
            p = arg.index('=')
            ret.addext(arg[1:p], arg[p + 1:])
    return ret.handle

def chain(env, startreq):
    """Chain-loading WSGI handler
    
    This handler loads requested files, compiles them and loads them
    into their own modules. The compiled modules are cached and reused
    until the file is modified, in which case the previous module is
    discarded and the new file contents are loaded into a new module
    in its place. When chaining such modules, an object named `wmain'
    is first looked for and called with no arguments if found. The
    object it returns is then used as the WSGI application object for
    that module, which is reused until the module is reloaded. If
    `wmain' is not found, an object named `application' is looked for
    instead. If found, it is used directly as the WSGI application
    object.
    """
    path = env["SCRIPT_FILENAME"]
    mod = getmod(path)
    entry = None
    if mod is not None:
        with mod.lock:
            if hasattr(mod, "entry"):
                entry = mod.entry
            else:
                if hasattr(mod.mod, "wmain"):
                    entry = mod.mod.wmain()
                elif hasattr(mod.mod, "application"):
                    entry = mod.mod.application
                mod.entry = entry
    if entry is not None:
        return entry(env, startreq)
    return wsgiutil.simpleerror(env, startreq, 500, "Internal Error", "Invalid WSGI handler.")

application = handler().handle
