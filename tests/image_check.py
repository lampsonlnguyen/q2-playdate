"""PDI/PDT decoder checks. Generates fixtures in the PDI/PDT file format
(as produced by pdc) and asserts the image-test binary's pixel dumps.

Usage: python3 tests/image_check.py build/host/image-test
"""
import pathlib
import struct
import subprocess
import sys
import tempfile
import zlib


def pad16(data):
    return data + bytes(-len(data) % 16)


def pad4(data):
    return data + bytes(-len(data) % 4)


def pdi_payload(width, height, rows, mask=None, xoff=0, yoff=0):
    rowbytes = (width + 7) // 8
    flags = 7 if mask is not None else 4
    header = struct.pack("<HHHHHHHH", width, height, rowbytes, 0, xoff, yoff, 0, flags)
    bitmap = bytes(rows)
    payload = header + bitmap
    if mask is not None:
        payload += bytes(mask)
    return payload


def pdi_file(width, height, rows, mask=None, xoff=0, yoff=0,
             logical_w=None, logical_h=None):
    payload = pad16(pdi_payload(width, height, rows, mask, xoff, yoff))
    body = (b"Playdate IMG" + struct.pack("<IIIII", 0x80000000, len(payload),
            logical_w if logical_w is not None else width,
            logical_h if logical_h is not None else height, 0)
            + zlib.compress(payload))
    return body


def pdt_payload(cells):
    """The raw PDT payload (no file header): count, columns, cumulative
    end offsets, then the cells."""
    offsets = []
    running = 0
    body = b""
    for payload in cells:
        running += len(payload)
        offsets.append(running)
        body += payload
    return (struct.pack("<HH", len(cells), 1) +
            struct.pack("<%dI" % len(cells), *offsets) + body)


def pdt_file(cell_w, cell_h, cells):
    """cells: list of (payload, ) PDI payloads (already padded to 4)."""
    payload = pad16(pdt_payload(cells))
    return (b"Playdate IMT" + struct.pack("<IIIII", 0x80000000, len(payload),
            cell_w, cell_h, len(cells)) + zlib.compress(payload))


def main():
    _, test_binary = sys.argv
    with tempfile.TemporaryDirectory() as temp:
        root = pathlib.Path(temp)
        # 4x4 checker: rows 0xa0, 0x50, 0xa0, 0x50 (1=white).
        (root / "checker.pdi").write_bytes(
            pdi_file(4, 4, [0xa0, 0x50, 0xa0, 0x50]))
        (root / "white.pdi").write_bytes(pdi_file(4, 4, [0xf0] * 4))
        (root / "black.pdi").write_bytes(pdi_file(4, 4, [0x00] * 4))
        # Same 4x4 with a transparent top-left pixel (mask bit 0).
        (root / "alpha.pdi").write_bytes(
            pdi_file(4, 4, [0x70, 0xf0, 0xf0, 0xf0], mask=[0x70, 0xf0, 0xf0, 0xf0]))
        # Trimmed 10x10 image: bitmap is 10x8 with yoff=1.
        (root / "trimmed.pdi").write_bytes(
            pdi_file(10, 8, [0xff, 0xff] * 8, xoff=0, yoff=1,
                     logical_w=10, logical_h=10))
        # Imagetable: logical 10x10, cell 0 is 9x10 with xoff=1 (trimmed
        # on the left, all white), cell 1 is 10x8 with yoff=1 (all black).
        cell0 = pad4(pdi_payload(9, 10, [0xff, 0xff] * 10, xoff=1, yoff=0))
        cell1 = pad4(pdi_payload(10, 8, [0x00, 0x00] * 8, xoff=0, yoff=1))
        (root / "trimmed.pdt").write_bytes(pdt_file(10, 10, [cell0, cell1]))

        result = subprocess.run(
            [test_binary, str(root / "checker.pdi"), str(root / "white.pdi"),
             str(root / "black.pdi"), str(root / "alpha.pdi"),
             str(root / "trimmed.pdi"), str(root / "trimmed.pdt")],
            capture_output=True, text=True, timeout=10)
        assert result.returncode == 0, result.stderr
        out = result.stdout

        def section(name):
            start = out.index("== " + name)
            end = out.find("== ", start + 3)
            return out[start:end if end != -1 else len(out)]

        checker = section("checker.pdi")
        assert "size 4x4 bitmap 4x4 offset 0,0 mask no" in checker, checker
        assert "#.#.\n.#.#\n#.#.\n.#.#\n" in checker, checker
        white = section("white.pdi")
        assert "####\n" * 4 in white, white
        black = section("black.pdi")
        assert "....\n" * 4 in black, black
        alpha = section("alpha.pdi")
        assert "size 4x4 bitmap 4x4 offset 0,0 mask yes" in alpha, alpha
        assert " ###\n####\n####\n####\n" in alpha, alpha
        trimmed = section("trimmed.pdi")
        assert "size 10x10 bitmap 10x8 offset 0,1 mask no" in trimmed, trimmed
        assert "          \n" + "##########\n" * 8 + "          \n" in trimmed, trimmed
        table = section("trimmed.pdt")
        assert "cells 2 size 10x10" in table, table
        assert "-- cell 1\n" + " #########\n" * 10 in table, table
        assert "-- cell 2\n" + "          \n" + "..........\n" * 8 + "          \n" in table, table
        print("PDI/PDT decoder checks passed")


if __name__ == "__main__":
    main()
