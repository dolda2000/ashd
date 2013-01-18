class protoerr(Exception):
    pass

def readns(sk):
    hln = 0
    while True:
        c = sk.read(1)
        if c == b':':
            break
        elif c >= b'0' or c <= b'9':
            hln = (hln * 10) + (ord(c) - ord(b'0'))
        else:
            raise protoerr("Invalid netstring length byte: " + c)
    ret = sk.read(hln)
    if sk.read(1) != b',':
        raise protoerr("Non-terminated netstring")
    return ret

def readhead(sk):
    parts = readns(sk).split(b'\0')[:-1]
    if len(parts) % 2 != 0:
        raise protoerr("Malformed headers")
    ret = {}
    i = 0
    while i < len(parts):
        ret[parts[i]] = parts[i + 1]
        i += 2
    return ret

def decodehead(head, coding):
    return {k.decode(coding): v.decode(coding) for k, v in head.items()}
