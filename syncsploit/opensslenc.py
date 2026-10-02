#!/usr/bin/env python3
"""openssl 0.9.8 compatible `enc -aes-256-cbc -salt -md md5 -pass file:<f>`.

the unit's /proc/boot/openssl is 0.9.8g, so the .enc files the navi synctool check script
decrypts are the classic "Salted__" format: 8 byte salt, key/iv from EVP_BytesToKey with MD5,
PKCS#7 padding. this module reimplements encryption only, no dependencies (the host may have no
openssl binary), and can self check against libcrypto if it is around.

    ./opensslenc.py --selftest
    ./opensslenc.py --password-file key.bin plain.txt out.enc
"""
import argparse
import hashlib
import os
import sys

SBOX = bytes.fromhex(
    '637c777bf26b6fc53001672bfed7ab76'
    'ca82c97dfa5947f0add4a2af9ca472c0'
    'b7fd9326363ff7cc34a5e5f171d83115'
    '04c723c31896059a071280e2eb27b275'
    '09832c1a1b6e5aa0523bd6b329e32f84'
    '53d100ed20fcb15b6acbbe394a4c58cf'
    'd0efaafb434d338545f9027f503c9fa8'
    '51a3408f929d38f5bcb6da2110fff3d2'
    'cd0c13ec5f974417c4a77e3d645d1973'
    '60814fdc222a908846eeb814de5e0bdb'
    'e0323a0a4906245cc2d3ac629195e479'
    'e7c8376d8dd54ea96c56f4ea657aae08'
    'ba78252e1ca6b4c6e8dd741f4bbd8b8a'
    '703eb5664803f60e613557b986c11d9e'
    'e1f8981169d98e949b1e87e9ce5528df'
    '8ca1890dbfe6426841992d0fb054bb16')

RCON = (0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1b, 0x36)


def _xtime(a):
    a <<= 1
    return (a ^ 0x1b) & 0xff if a & 0x100 else a


def _mul(a, b):
    r = 0
    while b:
        if b & 1:
            r ^= a
        a = _xtime(a)
        b >>= 1
    return r


def _expand_key(key):
    w = [list(key[4 * i:4 * i + 4]) for i in range(8)]
    for i in range(8, 60):
        t = w[i - 1][:]
        if i % 8 == 0:
            t = t[1:] + t[:1]
            t = [SBOX[b] for b in t]
            t[0] ^= RCON[i // 8 - 1]
        elif i % 8 == 4:
            t = [SBOX[b] for b in t]
        w.append([w[i - 8][j] ^ t[j] for j in range(4)])
    return w


def _shift_rows(s):
    out = [0] * 16
    for r in range(4):
        for c in range(4):
            out[r + 4 * c] = s[r + 4 * ((c + r) % 4)]
    return out


def _mix_columns(s):
    out = []
    for c in range(4):
        a = s[4 * c:4 * c + 4]
        out += [
            _mul(a[0], 2) ^ _mul(a[1], 3) ^ a[2] ^ a[3],
            a[0] ^ _mul(a[1], 2) ^ _mul(a[2], 3) ^ a[3],
            a[0] ^ a[1] ^ _mul(a[2], 2) ^ _mul(a[3], 3),
            _mul(a[0], 3) ^ a[1] ^ a[2] ^ _mul(a[3], 2),
        ]
    return out


def _encrypt_block(block, w):
    s = [block[i] ^ w[i // 4][i % 4] for i in range(16)]
    for rnd in range(1, 14):
        s = [SBOX[b] for b in s]
        s = _shift_rows(s)
        s = _mix_columns(s)
        s = [s[i] ^ w[4 * rnd + i // 4][i % 4] for i in range(16)]
    s = [SBOX[b] for b in s]
    s = _shift_rows(s)
    s = [s[i] ^ w[56 + i // 4][i % 4] for i in range(16)]
    return bytes(s)


def evp_bytes_to_key(password, salt, key_len=32, iv_len=16):
    """OpenSSL EVP_BytesToKey(EVP_aes_256_cbc(), EVP_md5(), salt, password, 1)."""
    out = b''
    prev = b''
    while len(out) < key_len + iv_len:
        prev = hashlib.md5(prev + password + salt).digest()
        out += prev
    return out[:key_len], out[key_len:key_len + iv_len]


def passphrase_from_file(data):
    """what openssl 0.9.8 `-pass file:` ends up using: the first line, trailing CRLF gone,
    then a C string so anything from the first NUL on is dropped. BIO_gets takes at most
    1023 bytes."""
    line = data[:1024].split(b'\n', 1)[0][:1023]
    if line.endswith(b'\r'):
        line = line[:-1]
    nul = line.find(b'\0')
    if nul >= 0:
        line = line[:nul]
    return line


def encrypt(plaintext, passphrase, salt=None):
    salt = salt if salt is not None else os.urandom(8)
    key, iv = evp_bytes_to_key(passphrase, salt)
    pad = 16 - len(plaintext) % 16
    data = plaintext + bytes([pad]) * pad
    out = b''
    prev = iv
    for i in range(0, len(data), 16):
        block = bytes(a ^ b for a, b in zip(data[i:i + 16], prev))
        prev = _encrypt_block(block, _expand_key(key))
        out += prev
    return b'Salted__' + salt + out


def _selftest_libcrypto():
    import ctypes
    lib = ctypes.CDLL('libcrypto.so.3')
    lib.EVP_aes_256_cbc.restype = ctypes.c_void_p
    lib.EVP_md5.restype = ctypes.c_void_p
    lib.EVP_CIPHER_CTX_new.restype = ctypes.c_void_p
    lib.EVP_BytesToKey.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_char_p,
                                   ctypes.c_char_p, ctypes.c_int, ctypes.c_int,
                                   ctypes.c_char_p, ctypes.c_char_p]

    def ref(pt, pw, salt):
        key = ctypes.create_string_buffer(32)
        iv = ctypes.create_string_buffer(16)
        assert lib.EVP_BytesToKey(lib.EVP_aes_256_cbc(), lib.EVP_md5(), salt, pw, len(pw), 1,
                                  key, iv) == 32
        ctx = lib.EVP_CIPHER_CTX_new()
        out = ctypes.create_string_buffer(len(pt) + 32)
        n1 = ctypes.c_int()
        lib.EVP_EncryptInit_ex(ctypes.c_void_p(ctx), ctypes.c_void_p(lib.EVP_aes_256_cbc()),
                               None, key.raw, iv.raw)
        lib.EVP_EncryptUpdate(ctypes.c_void_p(ctx), out, ctypes.byref(n1), pt, len(pt))
        n2 = ctypes.c_int()
        lib.EVP_EncryptFinal_ex(ctypes.c_void_p(ctx),
                                ctypes.cast(ctypes.addressof(out) + n1.value, ctypes.c_char_p),
                                ctypes.byref(n2))
        lib.EVP_CIPHER_CTX_free(ctypes.c_void_p(ctx))
        return b'Salted__' + salt + out.raw[:n1.value + n2.value]

    for n in (0, 1, 15, 16, 17, 100):
        for _ in range(3):
            pt = os.urandom(n)
            pw = os.urandom(23)
            salt = os.urandom(8)
            assert encrypt(pt, pw, salt) == ref(pt, pw, salt), n
    print('aes-256-cbc matches libcrypto evp')


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--selftest', action='store_true')
    ap.add_argument('--password-file', metavar='FILE')
    ap.add_argument('--password', metavar='STR')
    ap.add_argument('--salt', metavar='HEX')
    ap.add_argument('input', nargs='?')
    ap.add_argument('output', nargs='?')
    a = ap.parse_args()
    if a.selftest:
        return _selftest_libcrypto()
    pw = None
    if a.password_file:
        pw = passphrase_from_file(open(a.password_file, 'rb').read())
    elif a.password is not None:
        pw = passphrase_from_file(a.password.encode())
    else:
        sys.exit('need --password-file or --password')
    salt = bytes.fromhex(a.salt) if a.salt else None
    data = open(a.input, 'rb').read() if a.input else sys.stdin.buffer.read()
    out = encrypt(data, pw, salt)
    if a.output:
        open(a.output, 'wb').write(out)
        print('wrote %s (%d bytes, password %d bytes)' % (a.output, len(out), len(pw)))
    else:
        sys.stdout.buffer.write(out)


if __name__ == '__main__':
    main()
