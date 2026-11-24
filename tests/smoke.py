import pathlib
import struct
import subprocess
import sys
import tempfile
import zlib


def entry(name, flags, data):
    header = struct.pack("<I", (len(data) << 8) | flags) + name + b"\0"
    return header + bytes(-len(header) % 4) + data


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
        (bundle / "pdex.bin").touch()
        check(header + entry(b"main", 1, chunk), "native Playdate bundles")
        print("PDX smoke tests passed")


if __name__ == "__main__":
    main()
