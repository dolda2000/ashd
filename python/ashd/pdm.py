"""Management for daemon processes

This module contains a utility to listen for management commands on a
socket, lending itself to managing daemon processes.
"""

import os, sys, socket, threading, grp, select
import types, pprint, traceback

class repl(object):
    def __init__(self, cl):
        self.cl = cl
        self.mod = types.ModuleType("repl")
        self.mod.echo = self.echo
        self.printer = pprint.PrettyPrinter(indent = 4, depth = 6)
        cl.sk.send("+REPL\n")

    def sendlines(self, text):
        for line in text.split("\n"):
            self.cl.sk.send(" " + line + "\n")

    def echo(self, ob):
        self.sendlines(self.printer.pformat(ob))

    def command(self, cmd):
        try:
            try:
                ccode = compile(cmd, "PDM Input", "eval")
            except SyntaxError:
                ccode = compile(cmd, "PDM Input", "exec")
                exec ccode in self.mod.__dict__
                self.cl.sk.send("+OK\n")
            else:
                self.echo(eval(ccode, self.mod.__dict__))
                self.cl.sk.send("+OK\n")
        except:
            for line in traceback.format_exception(*sys.exc_info()):
                self.cl.sk.send(" " + line)
            self.cl.sk.send("+EXC\n")

    def handle(self, buf):
        p = buf.find("\n\n")
        if p < 0:
            return buf
        cmd = buf[:p + 1]
        self.command(cmd)
        return buf[p + 2:]

class client(threading.Thread):
    def __init__(self, sk):
        super(client, self).__init__(name = "Management client")
        self.setDaemon(True)
        self.sk = sk
        self.handler = self

    def choose(self, proto):
        if proto == "repl":
            self.handler = repl(self)
        else:
            self.sk.send("-ERR Unknown protocol: %s\n" % proto)
            raise Exception()

    def handle(self, buf):
        p = buf.find("\n")
        if p >= 0:
            proto = buf[:p]
            buf = buf[p + 1:]
            self.choose(proto)
        return buf

    def run(self):
        try:
            buf = ""
            self.sk.send("+PDM1\n")
            while True:
                ret = self.sk.recv(1024)
                if ret == "":
                    return
                buf += ret
                while True:
                    try:
                        nbuf = self.handler.handle(buf)
                    except:
                        return
                    if nbuf == buf:
                        break
                    buf = nbuf
        finally:
            self.sk.close()

class listener(threading.Thread):
    def __init__(self):
        super(listener, self).__init__(name = "Management listener")
        self.setDaemon(True)

    def listen(self, sk):
        self.running = True
        while self.running:
            rfd, wfd, efd = select.select([sk], [], [sk], 1)
            for fd in rfd:
                if fd == sk:
                    nsk, addr = sk.accept()
                    self.accept(nsk, addr)

    def stop(self):
        self.running = False
        self.join()

    def accept(self, sk, addr):
        cl = client(sk)
        cl.start()

class unixlistener(listener):
    def __init__(self, name, mode = 0600, group = None):
        super(unixlistener, self).__init__()
        self.name = name
        self.mode = mode
        self.group = group

    def run(self):
        sk = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        ul = False
        try:
            if os.path.exists(self.name) and os.path.stat.S_ISSOCK(os.stat(self.name).st_mode):
                os.unlink(self.name)
            sk.bind(self.name)
            ul = True
            os.chmod(self.name, self.mode)
            if self.group is not None:
                os.chown(self.name, os.getuid(), grp.getgrnam(self.group).gr_gid)
            sk.listen(16)
            self.listen(sk)
        finally:
            sk.close()
            if ul:
                os.unlink(self.name)

class tcplistener(listener):
    def __init__(self, port, bindaddr = "127.0.0.1"):
        super(tcplistener, self).__init__()
        self.port = port
        self.bindaddr = bindaddr

    def run(self):
        sk = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        try:
            sk.bind((self.bindaddr, self.port))
            sk.listen(16)
            self.listen(sk)
        finally:
            sk.close()
