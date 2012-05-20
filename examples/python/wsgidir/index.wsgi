import sys, ashd.wsgiutil

n = 0

def application(env, startreq):
    global n
    startreq("200 OK", [("Content-Type", "text/html")])
    yield "<html>\n"
    yield "<head><title>Hello world from Python!</title></head>\n"
    yield "<body>\n"
    yield "<h1>Hello world from Python/WSGI!</h1>\n"
    yield "<p>Do note how the single-process nature of <code>ashd-wsgi</code> "
    yield "allows data to be kept in memory persistently across requests, and "
    yield "how the following counter increases by one for each request: " + str(n) + "</p>\n"
    n += 1
    yield "<p>This script is a very raw WSGI example, using no glue code "
    yield "whatever, but you should be able to use whatever WSGI middleware "
    yield "you like in order to make it easier to code WSGI. If you have no "
    yield "particular preference, I might recommend "
    yield "<a href=\"http://www.dolda2000.com/gitweb/?p=wrw.git;a=summary\">WRW</a>, "
    yield "my WSGI Request Wrapper library, which is particularly made for "
    yield "<code>ashd-wsgi</code> and therefore, for instance, allows "
    yield "non-pickleble objects (like sockets) to be stored in sessions.</p>"
    yield "<p>If you have installed the Python3 utilities as well, there is "
    yield "also a <a href=\"py3\">Python3 script</a> to demonstrate that "
    yield "Python3 is supported as well.\n"
    yield "<p>The current Python interpreter is <code>" + ashd.wsgiutil.htmlquote(sys.version) + "</code>.</p>"
    yield "</body>\n"
    yield "</html>\n"
