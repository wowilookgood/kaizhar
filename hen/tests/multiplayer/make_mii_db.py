#!/usr/bin/env python3
"""Puts one Mii (the emulator's standard one, core/hle/applets/mii_selector.cpp) into the Mii
Maker database the game created in a test user folder on its first start: make_mii_db.py USERDIR NAME"""
import struct, sys, os
def crc16(data):
    crc = 0
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc
def utf16(s, n):
    return s.encode("utf-16-le").ljust(n * 2, b"\0")
def mii(name, mii_id):
    return (bytes([0x03, 0x00, 0x10, 0x30]) + struct.pack(">Q", 0xD285B6B300C8850A) + struct.pack(">I", mii_id) +
            bytes([0x40, 0xF4, 0x07, 0xB7, 0x37, 0x10]) + b"\0\0" + struct.pack(">H", 0xA600) + utf16(name, 10) +
            bytes([0x40, 0x40, 0x00, 0x00, 0x21, 0x01]) + struct.pack(">II", 0x02684418, 0x26344614) +
            struct.pack(">HHHHHH", 0x8112, 0x1768, 0x0D00, 0x0029, 0x0052, 0x4850) + utf16("flTobi", 10))
user, name = sys.argv[1], sys.argv[2]
m = mii(name, 0x98391EE4 + (sum(name.encode()) & 0xFF))
assert len(m) == 0x5C, hex(len(m))
path = os.path.join(user, "Library/Application Support/Azahar/nand/data/00000000000000000000000000000000/extdata/00048000/F000000B/user/CFL_DB.dat")
db = bytearray(open(path, "rb").read())  # (the database the game itself made on its first start)
assert db[0:4] == b"CFOG" and len(db) == 0x4BD20
db[8:8 + 0x5C] = m
db[0xC81E:0xC820] = struct.pack(">H", crc16(bytes(db[:0xC81E])))
open(path, "wb").write(db)
print("one Mii in", path)
