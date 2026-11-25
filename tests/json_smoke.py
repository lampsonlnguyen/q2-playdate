"""Lua API smoke test for the global `json` library.

Builds a bundle with a default Achievements.json and a pdxinfo, then runs
the host runner on a Lua chunk that asserts encode/decode behaviour and
the XTRIS achievements round-trip (decodeFile from the bundle, then
encodeToFile to the saves dir and decodeFile again).

Usage: python3 tests/json_smoke.py build/luac build/host/q2-playdate
"""
import os
import pathlib
import subprocess
import sys
import tempfile


def entry(name, flags, data, offset=0):
    import struct
    header = struct.pack("<I", (len(data) << 8) | flags) + name + b"\0"
    return header + bytes(-(offset + len(header)) % 4) + data


TEST_LUA = r"""
-- encode basics (object keys sorted, deterministic output)
assert(json.encode({b = 2, a = 1}) == '{"a":1,"b":2}', "encode object sorted")
assert(json.encode({1, "x", true, json.null}) == '[1,"x",true,null]', "encode array")
assert(json.encode("a\"b\\c\nd") == '"a\\"b\\\\c\\nd"', "encode escapes")
assert(json.encode(1.5) == "1.5", "encode float")
assert(json.encode(100.0) == "100", "encode integral float")
assert(json.encode(nil) == "null", "encode nil")
assert(json.encode(json.null) == "null", "encode null")
assert(json.encode({}) == "{}", "encode empty object")
assert(json.encodePretty({a = 1}):find("\n"), "encodePretty newlines")
assert(tostring(json.null) == "null", "null tostring")

-- decode basics
local d = json.decode('{"a":[1,2.5,"x",true,false,null],"b":"y\\n","c":-3e2}')
assert(d.a[1] == 1 and d.a[2] == 2.5 and d.a[3] == "x", "decode array")
assert(d.a[4] == true and d.a[5] == false, "decode bools")
assert(d.a[6] == json.null, "decode null")
assert(d.b == "y\n", "decode escape")
assert(d.c == -300, "decode exponent")
assert(json.decode('"\\u0041\\u00e9"') == "A\195\169", "decode unicode")
assert(json.decode("  [ 1 , 2 ]  ")[2] == 2, "decode whitespace")
assert(json.decode("{}").x == nil, "decode empty object")
local ok = pcall(json.decode, "{bad")
assert(not ok, "decode invalid errors")

-- decodeFile from the bundle (default achievements), then overwrite
local ach = json.decodeFile("Achievements.json")
assert(ach and ach.version == 1 and ach.unlocked[1] == "first", "decodeFile bundle")
assert(ach.count == 1, "decodeFile bundle count")
assert(json.decodeFile("Missing.json") == nil, "decodeFile missing nil")

-- XTRIS shape: encodeToFile(path, pretty, table), then decodeFile round-trip
-- (SDK order: path first. XTRIS calls it as ("Achievements.json", false, t)
-- and (path, true, t); the 2-arg (path, table) form also works.)
ach.unlocked[#ach.unlocked + 1] = "second"
ach.count = 2
assert(json.encodeToFile("Achievements.json", false, ach), "encodeToFile pretty=false")
local back = json.decodeFile("Achievements.json")
assert(back.count == 2 and back.unlocked[2] == "second", "file round-trip")
assert(back.version == 1 and back.unlocked[1] == "first", "round-trip keeps fields")
assert(json.encodeToFile("Pretty.json", true, {a = 1}), "encodeToFile pretty=true")
assert(json.decodeFile("Pretty.json").a == 1, "pretty round-trip")
assert(json.encodeToFile("TwoArg.json", {b = 2}), "encodeToFile two-arg")
assert(json.decodeFile("TwoArg.json").b == 2, "two-arg round-trip")
assert(not json.encodeToFile("Bad.json", false, function() end), "encodeToFile bad value false")
-- final compact write so the byte-exact check below sees the round-trip file
assert(json.encodeToFile("Achievements.json", ach), "encodeToFile final")
"""


def main():
    _, luac, runner = sys.argv
    with tempfile.TemporaryDirectory() as temp:
        temp = pathlib.Path(temp)
        card_root = temp / "card-root"
        bundle = temp / "json-smoke.pdx"
        bundle.mkdir()
        (bundle / "Achievements.json").write_text(
            '{"version":1,"unlocked":["first"],"count":1}'
        )
        (bundle / "pdxinfo").write_text(
            "name=JSON Smoke\nbundleID=com.test.jsonsmoke\nversion=1.0\nbuildNumber=1\n"
        )
        source = bundle / "main.lua"
        bytecode = bundle / "main.luac"
        source.write_text(TEST_LUA)
        subprocess.run([luac, "-o", bytecode, source], check=True)
        chunk = bytecode.read_bytes()
        (bundle / "main.pdz").write_bytes(b"Playdate PDZ" + bytes(4) + entry(b"main", 1, chunk))
        env = dict(os.environ)
        env["Q2_CARD_ROOT"] = str(card_root)
        result = subprocess.run([runner, str(bundle)], capture_output=True, text=True,
                                timeout=30, env=env)
        assert result.returncode == 0, result
        assert "Executed main Lua chunk" in result.stdout + result.stderr, result
        saves = card_root / "saves" / "game-json-smoke.pdx"
        assert (saves / "Achievements.json").read_bytes() == (
            b'{"count":2,"unlocked":["first","second"],"version":1}'
        ), saves


if __name__ == "__main__":
    main()
