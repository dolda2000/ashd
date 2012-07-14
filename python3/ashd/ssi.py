"""Module for handling server-side-include formatted files

This module is quite incomplete. I might complete it with more
features as I need them. It will probably never be entirely compliant
with Apache's version due to architectural differences.
"""

import sys, os, io, time, logging, functools
from . import wsgiutil

log = logging.getLogger("ssi")

def parsecmd(text, p):
    try:
        while text[p].isspace(): p += 1
        cmd = ""
        while not text[p].isspace():
            cmd += text[p]
            p += 1
        pars = {}
        while True:
            while text[p].isspace(): p += 1
            if text[p:p + 3] == "-->":
                return cmd, pars, p + 3
            key = ""
            while text[p].isalnum():
                key += text[p]
                p += 1
            if key == "":
                return None, {}, p
            while text[p].isspace(): p += 1
            if text[p] != '=':
                continue
            p += 1
            while text[p].isspace(): p += 1
            q = text[p]
            if q != '"' and q != "'" and q != '`':
                continue
            val = ""
            p += 1
            while text[p] != q:
                val += text[p]
                p += 1
            p += 1
            pars[key] = val
    except IndexError:
        return None, {}, len(text)

class context(object):
    def __init__(self, out, root):
        self.out = out
        self.vars = {}
        now = time.time()
        self.vars["DOCUMENT_NAME"] = os.path.basename(root.path)
        self.vars["DATE_GMT"] = time.asctime(time.gmtime(now))
        self.vars["DATE_LOCAL"] = time.asctime(time.localtime(now))
        self.vars["LAST_MODIFIED"] = time.asctime(time.localtime(root.mtime))

class ssifile(object):
    def __init__(self, path):
        self.path = path
        self.mtime = os.stat(self.path).st_mtime
        with open(path) as fp:
            self.parts = self.parse(fp.read())

    def text(self, text, ctx):
        ctx.out.write(text)

    def echo(self, var, enc, ctx):
        if var in ctx.vars:
            ctx.out.write(enc(ctx.vars[var]))

    def include(self, path, ctx):
        try:
            nest = getfile(os.path.join(os.path.dirname(self.path), path))
        except Exception:
            log.warning("%s: could not find included file %s" % (self.path, path))
            return
        nest.process(ctx)

    def process(self, ctx):
        for part in self.parts:
            part(ctx)

    def resolvecmd(self, cmd, pars):
        if cmd == "include":
            if "file" in pars:
                return functools.partial(self.include, pars["file"])
            elif "virtual" in pars:
                # XXX: For now, just include the file as-is. Change
                # when necessary.
                return functools.partial(self.include, pars["virtual"])
            else:
                log.warning("%s: invalid `include' directive" % self.path)
                return None
        elif cmd == "echo":
            if not "var" in pars:
                log.warning("%s: invalid `echo' directive" % self.path)
                return None
            enc = wsgiutil.htmlquote
            if "encoding" in pars:
                if pars["encoding"] == "entity":
                    enc = wsgiutil.htmlquote
            return functools.partial(self.echo, pars["var"], enc)
        else:
            log.warning("%s: unknown SSI command `%s'" % (self.path, cmd))
            return None

    def parse(self, text):
        ret = []
        p = 0
        while True:
            p2 = text.find("<!--#", p)
            if p2 < 0:
                ret.append(functools.partial(self.text, text[p:]))
                return ret
            ret.append(functools.partial(self.text, text[p:p2]))
            cmd, pars, p = parsecmd(text, p2 + 5)
            if cmd is not None:
                cmd = self.resolvecmd(cmd, pars)
                if cmd is not None:
                    ret.append(cmd)

filecache = {}

def getfile(path):
    path = os.path.normpath(path)
    cf = filecache.get(path)
    if not cf:
        cf = filecache[path] = ssifile(path)
    elif os.stat(path).st_mtime != cf.mtime:
        cf = filecache[path] = ssifile(path)
    return cf

def wsgi(env, startreq):
    try:
        if env["PATH_INFO"] != "":
            return wsgiutil.simpleerror(env, startreq, 404, "Not Found", "The resource specified by the URL does not exist.")
        root = getfile(env["SCRIPT_FILENAME"])
        buf = io.StringIO()
        root.process(context(buf, root))
    except Exception:
        return wsgituil.simpleerror(env, startreq, 500, "Internal Error", "The server encountered an unpexpected error while handling SSI.")
    ret = buf.getvalue().encode("utf8")
    startreq("200 OK", [("Content-Type", "text/html; charset=UTF-8"), ("Content-Length", str(len(ret)))])
    return [ret]
