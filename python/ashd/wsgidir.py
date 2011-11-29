"""WSGI handler for serving chained WSGI modules from physical files

The WSGI handler in this module examines the SCRIPT_FILENAME variable
of the requests it handles -- that is, the physical file corresponding
to the request, as determined by the webserver -- determining what to
do with the request based on the extension of that file.

By default, it handles files named `.wsgi' by compiling them into
Python modules and using them, in turn, as chained WSGI handlers, but
handlers for other extensions can be installed as well.

When handling `.wsgi' files, the compiled modules are cached and
reused until the file is modified, in which case the previous module
is discarded and the new file contents are loaded into a new module in
its place. When chaining such modules, an object named `wmain' is
first looked for and called with no arguments if found. The object it
returns is then used as the WSGI application object for that module,
which is reused until the module is reloaded. If `wmain' is not found,
an object named `application' is looked for instead. If found, it is
used directly as the WSGI application object.

This module itself contains both an `application' and a `wmain'
object. If this module is used by ashd-wsgi(1) or scgi-wsgi(1) so that
its wmain function is called, arguments can be specified to it to
install handlers for other file extensions. Such arguments take the
form `.EXT=MODULE.HANDLER', where EXT is the file extension to be
handled, and the MODULE.HANDLER string is treated by splitting it
along its last constituent dot. The part left of the dot is the name
of a module which is imported, and the part right of the dot is the
name of an object in that module, which should be a callable adhering
to the WSGI specification. When called, this module will have made
sure that the WSGI environment contains the SCRIPT_FILENAME parameter
and that it is properly working. For example, the argument
`.fpy=my.module.foohandler' can be given to pass requests for `.fpy'
files to the function `foohandler' in the module `my.module' (which
must, of course, be importable). When writing such handler functions,
you will probably want to use the getmod() function in this module.
"""

import os, threading, types
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
        exec(code, mod.__dict__)
        entry = cachedmod(mod, sb.st_mtime)
        modcache[path] = entry
        return entry
    finally:
        cachelock.release()

def chain(env, startreq):
    path = env["SCRIPT_FILENAME"]
    mod = getmod(path)
    entry = None
    if mod is not None:
        mod.lock.acquire()
        try:
            if hasattr(mod, "entry"):
                entry = mod.entry
            else:
                if hasattr(mod.mod, "wmain"):
                    entry = mod.mod.wmain()
                elif hasattr(mod.mod, "application"):
                    entry = mod.mod.application
                mod.entry = entry
        finally:
            mod.lock.release()
    if entry is not None:
        return entry(env, startreq)
    return wsgiutil.simpleerror(env, startreq, 500, "Internal Error", "Invalid WSGI handler.")
exts["wsgi"] = chain

def addext(ext, handler):
    p = handler.rindex('.')
    mname = handler[:p]
    hname = handler[p + 1:]
    mod = __import__(mname, fromlist = ["dummy"])
    exts[ext] = getattr(mod, hname)

def application(env, startreq):
    """WSGI handler function

    Handles WSGI requests as per the module documentation.
    """
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
    return(exts[ext](env, startreq))

def wmain(*argv):
    """Main function for ashd(7)-compatible WSGI handlers

    Returns the `application' function. If any arguments are given,
    they are parsed according to the module documentation.
    """
    for arg in argv:
        if arg[0] == '.':
            p = arg.index('=')
            addext(arg[1:p], arg[p + 1:])
    return application
