#!/usr/bin/env python3
"""List the strings in one of FH2's string tables, with their keys.

    python dump_strings.py <path to EN.zip> [Table.str] [search text]

With just the zip, lists the tables in it. With a table, prints every
string as   0xKEY  IDS_Name  text   so you can pass the key to set_string().
A third argument only shows strings containing that text (any case).

Example:
    python dump_strings.py "D:/Games/Azimuth/FH2/Mount/media/Stripped/StringTables/EN.zip" Main.str start
"""
import struct
import sys
import zipfile


def parse(b):
    if b[:4] != b"LSB2":
        raise ValueError("not an LSB2 string table")
    nsec = struct.unpack_from("<I", b, 0x10)[0]
    offs = struct.unpack_from("<%dI" % nsec, b, 0x14)
    s0 = offs[0]
    _, count = struct.unpack_from("<II", b, s0)
    tab = s0 + 8
    strs = tab + (count + 1) * 6
    entries = [struct.unpack_from("<HI", b, tab + i * 6) for i in range(count + 1)]
    texts = {}
    for key, off in entries[:-1]:
        p = strs + 2 * off
        e = p
        while b[e:e + 2] != b"\0\0":
            e += 2
        texts[key] = b[p:e].decode("utf-16le", "replace")
    ids = {}
    if nsec > 1 and offs[1]:
        s1 = offs[1]
        _, c1 = struct.unpack_from("<II", b, s1)
        names = s1 + 8 + c1 * 8
        for i in range(c1):
            key, _, noff = struct.unpack_from("<HHI", b, s1 + 8 + i * 8)
            end = b.index(b"\0", names + noff)
            ids[key] = b[names + noff:end].decode("ascii", "replace")
    return [(k, ids.get(k, ""), texts[k]) for k, _ in entries[:-1]]


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    z = zipfile.ZipFile(sys.argv[1])
    if len(sys.argv) < 3:
        for n in sorted(z.namelist()):
            print(n)
        return 0
    rows = parse(z.read(sys.argv[2]))
    needle = sys.argv[3].lower() if len(sys.argv) > 3 else None
    for key, name, text in rows:
        if needle and needle not in text.lower() and needle not in name.lower():
            continue
        print("0x%04X  %-32s %s" % (key, name, text.replace("\n", "\\n")))
    return 0


if __name__ == "__main__":
    sys.exit(main())
