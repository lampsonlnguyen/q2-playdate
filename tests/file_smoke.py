"""Lua API smoke test for playdate.file / playdate.metadata.

Builds a bundle with data files and a pdxinfo, then runs the host
runner on a Lua chunk that asserts the file API behaviour, including
the Frogger high-score round-trip (exists -> open read -> readline,
and write-then-read-back when missing) and the XTRIS /Shared/ wrapper
paths (leading "/" maps to the saves root).

Usage: python3 tests/file_smoke.py build/luac build/host/q2-playdate
"""
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile


def entry(name, flags, data, offset=0):
    import struct
    header = struct.pack("<I", (len(data) << 8) | flags) + name + b"\0"
    return header + bytes(-(offset + len(header)) % 4) + data


TEST_LUA = r"""
local file = playdate.file
assert(file.kFileRead == 1 and file.kFileReadData == 2, "read constants")
assert(file.kFileWrite == 4 and file.kFileAppend == 8, "write constants")
assert(file.kSeekSet == 0 and file.kSeekFromCurrent == 1 and file.kSeekFromEnd == 2, "seek constants")
assert(playdate.metadata.bundleID == "com.test.filesmoke", "metadata bundleID")
assert(playdate.metadata.version == "1.0", "metadata version")
assert(playdate.metadata.buildNumber == "7", "metadata buildNumber")

-- bundle reads
assert(file.exists("data.txt"), "bundle exists")
assert(not file.exists("nope.txt"), "missing exists false")
assert(not file.exists("../evil"), "dotdot rejected")
assert(not file.exists("/../evil"), "rooted dotdot rejected")
assert(file.getType("data.txt") == "file", "getType file")
assert(file.getType("sub") == "directory", "getType dir")
assert(file.getType("nope") == nil, "getType missing nil")
assert(file.isdir("sub") and not file.isdir("data.txt"), "isdir")
assert(file.getSize("data.txt") == 12, "getSize bundle")
local mt = file.modtime("data.txt")
assert(type(mt.year) == "number" and mt.year > 2020, "modtime year")

local f = file.open("data.txt")
assert(f, "open bundle read")
local head, n = f:read(5)
assert(head == "hello" and n == 5, "read 5")
assert(f:tell() == 5, "tell after read")
f:seek(0)
assert(f:readline() == "hello", "readline 1")
assert(f:readline() == "world", "readline 2")
assert(f:readline() == nil, "readline EOF nil")
f:seek(0, file.kSeekSet)
assert(f:read() == "hello\nworld\n", "read rest")
f:close()
f:close()
assert(f:read(1) == nil, "closed read nil")
local missing, err = file.open("nope.txt")
assert(missing == nil and type(err) == "string", "open missing nil+err")

-- Frogger high-score round-trip (saves dir, hidden dotfile)
assert(not file.exists(".config.dat"), "config absent at first")
local w = file.open(".config.dat", file.kFileWrite)
assert(w, "open write config")
assert(w:write("100") == 3, "write 100")
w:close()
assert(file.exists(".config.dat"), "config exists after write")
local r = file.open(".config.dat")
assert(tonumber(r:readline()) == 100, "read back 100")
r:close()
local a = file.open(".config.dat", file.kFileAppend)
a:write("\n200")
a:close()
local r2 = file.open(".config.dat")
assert(r2:readline() == "100", "append line 1")
assert(r2:readline() == "200", "append line 2")
r2:close()

-- XTRIS /Shared/ paths (leading "/" maps to the saves root)
assert(file.mkdir("/Shared/deep/nest"), "mkdir nested")
assert(file.isdir("/Shared/deep/nest"), "isdir nested")
assert(file.getType("/Shared") == "directory", "Shared is dir")
local s = file.open("/Shared/x.txt", file.kFileWrite)
s:write("shared-data")
s:close()
assert(file.getSize("/Shared/x.txt") == 11, "getSize saves")
local listed = file.listFiles("/Shared")
assert(#listed == 2, "listFiles Shared count")
local seen = {}
for _, name in ipairs(listed) do seen[name] = true end
assert(seen["deep/"] and seen["x.txt"], "listFiles Shared names")
assert(file.rename("/Shared/x.txt", "/Shared/y.txt"), "rename")
assert(file.exists("/Shared/y.txt") and not file.exists("/Shared/x.txt"), "rename effect")
assert(not file.rename("/Shared/y.txt", "/Nope/y.txt"), "rename no mkdir")
assert(file.delete("/Shared/y.txt"), "delete file")
assert(not file.delete("/Shared/y.txt"), "delete twice false")
assert(file.delete("/Shared", true), "delete recursive")
assert(not file.isdir("/Shared"), "Shared gone")

-- merged root listing (bundle + saves), hidden files filtered
local root = file.listFiles()
local rseen = {}
for _, name in ipairs(root) do rseen[name] = true end
assert(rseen["data.txt"] and rseen["sub/"], "root lists bundle")
assert(not rseen[".hidden.txt"] and not rseen[".config.dat"], "root hides dotfiles")
local rootAll = file.listFiles(nil, true)
local aseen = {}
for _, name in ipairs(rootAll) do aseen[name] = true end
assert(aseen[".hidden.txt"] and aseen[".config.dat"], "showhidden lists dotfiles")
assert(#file.listFiles("/Nope") == 0, "listFiles missing empty")
"""


def main():
    _, luac, runner, *wrapper = sys.argv
    with tempfile.TemporaryDirectory() as temp:
        temp = pathlib.Path(temp)
        card_root = temp / "card-root"
        bundle = temp / "file-smoke.pdx"
        bundle.mkdir()
        (bundle / "data.txt").write_bytes(b"hello\nworld\n")
        (bundle / "sub").mkdir()
        (bundle / "sub" / "seed.txt").write_bytes(b"42\n")
        (bundle / ".hidden.txt").write_bytes(b"x")
        (bundle / "pdxinfo").write_text(
            "name=File Smoke\nbundleID=com.test.filesmoke\nversion=1.0\nbuildNumber=7\n"
        )
        source = bundle / "main.lua"
        bytecode = bundle / "main.luac"
        source.write_text(TEST_LUA)
        subprocess.run([luac, "-o", bytecode, source], check=True)
        chunk = bytecode.read_bytes()
        (bundle / "main.pdz").write_bytes(b"Playdate PDZ" + bytes(4) + entry(b"main", 1, chunk))
        env = dict(os.environ)
        env["Q2_CARD_ROOT"] = str(card_root)
        result = subprocess.run([*wrapper, runner, str(bundle)], capture_output=True, text=True,
                                timeout=30, env=env)
        assert result.returncode == 0, result
        assert "Executed main Lua chunk" in result.stdout + result.stderr, result
        saves = card_root / "saves" / "game-file-smoke.pdx"
        assert (saves / ".config.dat").read_bytes() == b"100\n200", saves
        assert not (saves / "Shared").exists(), saves


if __name__ == "__main__":
    main()
