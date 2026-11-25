import pathlib
import struct
import subprocess
import sys
import tempfile
import zlib


def entry(name, flags, data, offset=0):
    header = struct.pack("<I", (len(data) << 8) | flags) + name + b"\0"
    return header + bytes(-(offset + len(header)) % 4) + data


def main():
    _, luac, runner, *wrapper = sys.argv
    with tempfile.TemporaryDirectory() as temp:
        bundle = pathlib.Path(temp) / "smoke.pdx"
        bundle.mkdir()
        source = bundle / "main.lua"
        bytecode = bundle / "main.luac"
        source.write_text("function playdate.update() end\n")
        subprocess.run([luac, "-o", bytecode, source], check=True)
        chunk = bytecode.read_bytes()
        header = b"Playdate PDZ" + bytes(4)

        def check(data, expected, success=False):
            (bundle / "main.pdz").write_bytes(data)
            result = subprocess.run(
                [*wrapper, runner, bundle], capture_output=True, text=True, timeout=10
            )
            assert (result.returncode == 0) == success, result
            assert expected in result.stdout + result.stderr, result

        check(header + entry(b"main", 1, chunk), "Executed main Lua chunk", True)
        compressed = struct.pack("<I", len(chunk)) + zlib.compress(chunk)
        check(header + entry(b"main", 129, compressed), "Executed main Lua chunk", True)
        check(header + entry(b"main", 1, chunk)[:-1], "truncated PDZ entry")
        check(header + entry(b"other", 1, chunk), "no main Lua chunk")
        check(header[:12] + struct.pack("<I", 0x40000000), "encrypted PDZ")
        def compile_lua(code):
            source.write_text(code)
            subprocess.run([luac, "-o", bytecode, source], check=True)
            return bytecode.read_bytes()

        module = compile_lua("count = (count or 0) + 1")
        imported = compile_lua(
            'import("lib/counter"); import("lib/counter.lua"); '
            'assert(count == 1 and math.floor(1.9) == 1)'
        )
        modules = entry(b"lib/counter", 129,
                        struct.pack("<I", len(module)) + zlib.compress(module))
        check(header + modules + entry(b"main", 1, imported, len(header) + len(modules)),
              "Executed main Lua chunk", True)
        check(header + entry(b"main", 1, imported), "missing Lua import: lib/counter")
        # Original synthetic assets; no commercial game data in the repository.
        cell = struct.pack("<8H", 8, 1, 1, 0, 0, 0, 0, 7) + b"\x80\xc0"
        def asset(magic, data, count):
            return magic + struct.pack("<5I", 0x80000000, len(data), 8, 1, count) + zlib.compress(data)
        (bundle / "test.pdi").write_bytes(asset(b"Playdate IMG", cell, 0))
        table = struct.pack("<HHII", 2, 2, len(cell), 2 * len(cell)) + cell * 2
        (bundle / "test.pdt").write_bytes(asset(b"Playdate IMT", table, 2))
        graphics = compile_lua("""
local g = playdate.graphics
local im = assert(g.image.new("test"))
local w,h = im:getSize()
assert(w == 8 and h == 1)
assert(im:sample(0,0) == g.kColorWhite)
assert(im:sample(1,0) == g.kColorBlack)
assert(im:sample(2,0) == g.kColorClear)
local copy = im:copy()
copy:clear(g.kColorBlack)
assert(copy:sample(0,0) == g.kColorBlack)
assert(im:sample(0,0) == g.kColorWhite)
im:draw(-1,0)
im:drawScaled(399,239,2)
local t = assert(g.imagetable.new("test"))
assert(t:getLength() == 2 and t:getImage(2):sample(1,0) == g.kColorBlack)
t:drawImage(1,0,0)
assert(g.image.new("../test") == nil)
""")
        (bundle / "main.pdz").write_bytes(header + entry(b"main", 1, graphics))
        result = subprocess.run([*wrapper, runner, "--check-game", bundle],
                                capture_output=True, text=True, timeout=10)
        assert result.returncode == 0, result
        assert "Initialized game with card APIs" in result.stdout, result
        (bundle / "pdex.bin").touch()
        check(header + entry(b"main", 1, chunk), "native Playdate bundles")
        print("PDX smoke tests passed")


if __name__ == "__main__":
    main()
