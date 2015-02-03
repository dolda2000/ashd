import time, sys, io

def htmlquote(text):
    ret = ""
    for c in text:
        if c == '&':
            ret += "&amp;"
        elif c == '<':
            ret += "&lt;"
        elif c == '>':
            ret += "&gt;"
        elif c == '"':
            ret += "&quot;"
        else:
            ret += c
    return ret

def simpleerror(env, startreq, code, title, msg):
    buf = """<?xml version="1.0" encoding="US-ASCII"?>
<!DOCTYPE html PUBLIC "-//W3C//DTD XHTML 1.1//EN" "http://www.w3.org/TR/xhtml11/DTD/xhtml11.dtd">
<html xmlns="http://www.w3.org/1999/xhtml" xml:lang="en-US">
<head>
<title>%s</title>
</head>
<body>
<h1>%s</h1>
<p>%s</p>
</body>
</html>""" % (title, title, htmlquote(msg))
    buf = buf.encode("ascii")
    startreq("%i %s" % (code, title), [("Content-Type", "text/html"), ("Content-Length", str(len(buf)))])
    return [buf]

def httpdate(ts):
    return time.strftime("%a, %d %b %Y %H:%M:%S +0000", time.gmtime(ts))

def phttpdate(dstr):
    tz = dstr[-6:]
    dstr = dstr[:-6]
    if tz[0] != " " or (tz[1] != "+" and tz[1] != "-") or not tz[2:].isdigit():
        return None
    tz = int(tz[1:])
    tz = (((tz / 100) * 60) + (tz % 100)) * 60
    return time.mktime(time.strptime(dstr, "%a, %d %b %Y %H:%M:%S")) - tz - time.altzone

def testenviron(uri, qs="", pi="", method=None, filename=None, host="localhost", data=None, ctype=None, head={}):
    if method is None:
        method = "GET" if data is None else "POST"
    if ctype is None and data is not None:
        ctype = "application/x-www-form-urlencoded"
    ret = {}
    ret["wsgi.version"] = 1, 0
    ret["SERVER_SOFTWARE"] = "ashd-test/1"
    ret["GATEWAY_INTERFACE"] = "CGI/1.1"
    ret["SERVER_PROTOCOL"] = "HTTP/1.1"
    ret["REQUEST_METHOD"] = method
    ret["wsgi.uri_encoding"] = "utf-8"
    ret["SCRIPT_NAME"] = uri
    ret["PATH_INFO"] = pi
    ret["QUERY_STRING"] = qs
    full = uri + pi
    if qs:
        full = full + "?" + qs
    ret["REQUEST_URI"] = full
    if filename is not None:
        ret["SCRIPT_FILENAME"] = filename
    ret["HTTP_HOST"] = ret["SERVER_NAME"] = host
    ret["wsgi.url_scheme"] = "http"
    ret["SERVER_ADDR"] = "127.0.0.1"
    ret["SERVER_PORT"] = "80"
    ret["REMOTE_ADDR"] = "127.0.0.1"
    ret["REMOTE_PORT"] = "12345"
    if data is not None:
        ret["CONTENT_TYPE"] = ctype
        ret["CONTENT_LENGTH"] = len(data)
        ret["wsgi.input"] = io.BytesIO(data)
    else:
        ret["wsgi.input"] = io.BytesIO(b"")
    ret["wsgi.errors"] = sys.stderr
    ret["wsgi.multithread"] = True
    ret["wsgi.multiprocess"] = False
    ret["wsgi.run_once"] = False
    for key, val in head.items():
        ret["HTTP_" + key.upper().replace("-", "_")] = val
    return ret

class testrequest(object):
    def __init__(self):
        self.wbuf = io.BytesIO()
        self.headers = None
        self.status = None

    def __call__(self, status, headers):
        self.status = status
        self.headers = headers
        return self.wbuf.write

    def __repr__(self):
        return "<ashd.wsgiutil.testrequest %r %s %s>" % (self.status,
                                                         "None" if self.headers is None else ("[%i]" % len(self.headers)),
                                                         "(no data)" if len(self.wbuf.getvalue()) == 0 else "(with data)")

    def __str__(self):
        return repr(self)
