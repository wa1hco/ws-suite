"""Print the identifying records of TrueType fonts from their 'name' table, plus a SHA-256.

Name IDs: 0 copyright, 1 family, 2 subfamily, 4 full name, 5 version,
6 PostScript name, 7 trademark, 8 manufacturer, 9 designer, 11 vendor URL,
13 licence description, 14 licence URL.
"""
import hashlib
import json
import struct
import sys

IDS = {0: "copyright", 1: "family", 2: "subfamily", 4: "full_name", 5: "version", 6: "postscript_name",
       7: "trademark", 8: "manufacturer", 9: "designer", 11: "vendor_url", 13: "license", 14: "license_url"}


def name_records(path):
    data = open(path, "rb").read()
    n = struct.unpack(">H", data[4:6])[0]
    tables = {}
    for i in range(n):
        tag, _, off, length = struct.unpack(">4sIII", data[12 + 16 * i:28 + 16 * i])
        tables[tag] = (off, length)
    off = tables[b"name"][0]
    count, string_off = struct.unpack(">HH", data[off + 2:off + 6])
    found = {}
    for i in range(count):
        pid, eid, lid, nid, length, soff = struct.unpack(">HHHHHH", data[off + 6 + 12 * i:off + 18 + 12 * i])
        if nid not in IDS:
            continue
        raw = data[off + string_off + soff:off + string_off + soff + length]
        if pid == 3 and eid in (1, 10) and lid == 0x409:
            text = raw.decode("utf-16-be")
        elif pid == 1 and eid == 0:
            text = raw.decode("mac_roman")
        else:
            continue
        # prefer the Windows English record
        if IDS[nid] not in found or pid == 3:
            found[IDS[nid]] = text.strip()
    found["sha256"] = hashlib.sha256(data).hexdigest()
    found["file"] = path.replace("\\", "/").split("/")[-1]
    found["bytes"] = len(data)
    return found


if __name__ == "__main__":
    print(json.dumps([name_records(p) for p in sys.argv[1:]], indent=2, ensure_ascii=False))
