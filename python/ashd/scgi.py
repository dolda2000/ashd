class protoerr(Exception):
    pass

def readns(sk):
    hln = 0
    while True:
        c = sk.read(1)
        if c == ':':
            break
        elif c >= '0' or c <= '9':
            hln = (hln * 10) + (ord(c) - ord('0'))
        else:
            raise protoerr, "Invalid netstring length byte: " + c
    ret = sk.read(hln)
    if sk.read(1) != ',':
        raise protoerr, "Non-terminated netstring"
    return ret

def readhead(sk):
    parts = readns(sk).split('\0')[:-1]
    if len(parts) % 2 != 0:
        raise protoerr, "Malformed headers"
    ret = {}
    i = 0
    while i < len(parts):
        ret[parts[i]] = parts[i + 1]
        i += 2
    return ret
