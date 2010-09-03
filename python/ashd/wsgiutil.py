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
    startreq("%i %s" % (code, title), [("Content-Type", "text/html"), ("Content-Length", str(len(buf)))])
    return [buf]
