"""Lua API smoke test for graphics.image / imagetable / sprite / display.
Builds a bundle with PDI/PDT fixture files and PDZ image entries, then
runs the host runner on a Lua chunk that asserts the API behaviour.

Usage: python3 tests/image_smoke.py build/luac build/host/q2-playdate
"""
import pathlib
import struct
import subprocess
import sys
import tempfile
import zlib

from image_check import pdi_file, pdt_file, pdi_payload, pdt_payload, pad4


def entry(name, flags, data, offset=0):
    header = struct.pack("<I", (len(data) << 8) | flags) + name + b"\0"
    return header + bytes(-(offset + len(header)) % 4) + data


TEST_LUA = r"""
local gfx = playdate.graphics
local w, h = gfx.image.new("Assets/checker"):getSize()
assert(w == 4 and h == 4, "checker size")
local checker = gfx.image.new("Assets/checker")
assert(checker:getPixel(0, 0) == 1, "checker white")
assert(checker:getPixel(1, 0) == 0, "checker black")
assert(checker:getPixel(0, 1) == 0, "checker black row 2")
local alpha = gfx.image.new("Assets/alpha")
assert(alpha:getPixel(0, 0) == 2, "alpha transparent")
assert(alpha:getPixel(1, 0) == 1, "alpha white")
local blank = gfx.image.new(8, 8)
local bw, bh = blank:getSize()
assert(bw == 8 and bh == 8, "blank size")
assert(blank:getPixel(0, 0) == 2, "blank defaults to clear")
local target = gfx.image.new(4, 4, gfx.kColorClear)
gfx.pushContext(target)
gfx.setColor(gfx.kColorBlack)
gfx.fillRect(0, 0, 2, 4)
gfx.popContext()
assert(target:getPixel(0, 0) == 0, "pushContext fill black")
assert(target:getPixel(3, 0) == 2, "pushContext clear stays clear")
local img2 = gfx.image.new(4, 4, gfx.kColorClear)
gfx.pushContext(img2)
checker:draw(0, 0)
gfx.popContext()
assert(img2:getPixel(0, 0) == 1, "image draw into context")
assert(img2:getPixel(1, 0) == 0, "image draw black")
local table = gfx.imagetable.new("Assets/trimmed")
assert(table:getLength() == 2, "imagetable length")
local cw, ch = table:getImage(1):getSize()
assert(cw == 10 and ch == 10, "imagetable logical size")
assert(table:getImage(1):getPixel(0, 0) == 2, "trimmed cell transparent edge")
assert(table:getImage(1):getPixel(1, 0) == 1, "trimmed cell white")
local map = gfx.tilemap.new()
map:setTiles({1, 0, 2, 0}, 2)
map:setImageTable(table)
local mw, mh = map:getSize()
assert(mw == 2 and mh == 2, "tilemap size")
local rects = map:getCollisionRects({0})
assert(#rects == 2, "collision rects")
assert(rects[1].x == 0 and rects[1].y == 0 and rects[1].width == 1, "rect 1")
local r = playdate.geometry.rect.new(1, 2, 3, 4)
assert(r.x == 1 and r.y == 2 and r.width == 3 and r.w == 3 and r.height == 4, "rect fields")
local r2 = r:offsetBy(1, 1)
assert(r2.x == 2 and r2.y == 3, "rect offsetBy")
assert(r:intersects(playdate.geometry.rect.new(2, 3, 5, 5)), "rect intersects")
assert(not r:intersects(playdate.geometry.rect.new(100, 100, 5, 5)), "rect disjoint")
assert(r:containsPoint(2, 3), "rect containsPoint")
assert(r:containsRect(playdate.geometry.rect.new(2, 3, 1, 1)), "rect containsRect")
local u = r:union(playdate.geometry.rect.new(0, 0, 1, 1))
assert(u.x == 0 and u.width == 4, "rect union")
assert(r:copy().x == 1, "rect copy")
assert(not r:isEmpty(), "rect isEmpty false")
assert(playdate.geometry.rect.new(0, 0, 0, 5):isEmpty(), "rect isEmpty true")
assert(r:isEqual(r:copy()), "rect isEqual")
assert(r:unpack() == 1, "rect unpack")
local ix, iy, iw, ih = playdate.geometry.rect.fast_intersection(0, 0, 10, 10, 5, 5, 10, 10)
assert(ix == 5 and iy == 5 and iw == 5 and ih == 5, "rect fast_intersection")
local ux, uy, uw, uh = playdate.geometry.rect.fast_union(0, 0, 10, 10, 5, 5, 10, 10)
assert(ux == 0 and uy == 0 and uw == 15 and uh == 15, "rect fast_union")
local sprite = gfx.sprite.new()
sprite:setImage(checker)
sprite:moveTo(10, 10)
sprite:add()
local wall = gfx.sprite.new()
wall:setBounds(0, 0, 10, 10)
wall:setCollideRect(0, 0, 10, 10)
wall:add()
local wall2 = gfx.sprite.new()
wall2:setBounds(20, 20, 10, 10)
wall2:setCollideRect(playdate.geometry.rect.new(1, 1, 8, 8))
wall2:add()
local crx, cry, crw, crh = wall2:getCollideRect()
assert(crx == 1 and cry == 1 and crw == 8 and crh == 8, "setCollideRect rect form")
local found = gfx.sprite.querySpritesInRect(5, 5, 10, 10)
assert(#found == 2, "querySpritesInRect")
local foundRect = gfx.sprite.querySpritesInRect(playdate.geometry.rect.new(5, 5, 10, 10))
assert(#foundRect == 2, "querySpritesInRect rect form")
gfx.sprite.update()
playdate.display.setScale(2)
assert(playdate.display.getWidth() == 200 and playdate.display.getHeight() == 120, "scale 2")
playdate.display.setScale(1)
local font = gfx.font.new("Assets/pico")
assert(font:getHeight() == 16, "font height")
gfx.setFont(font)
assert(gfx.getFont() == font, "setFont/getFont")
assert(gfx.drawText("hi", 0, 0) == 0, "drawText stub")
local menu = playdate.getSystemMenu()
menu:addMenuItem("Test", function() end)
menu:addCheckmarkMenuItem("Check", true, function() end)
local player = playdate.sound.fileplayer.new("Sounds/music/40")
player:setStopOnUnderrun(false)
player:play(0)
assert(player:isPlaying() == false, "fileplayer silent")
player:stop()
local fx = playdate.sound.sampleplayer.new(playdate.sound.sample.new("Sounds/sfx"))
fx:setVolume(0.125)
fx:play()
assert(playdate.getSecondsSinceEpoch() > 0, "epoch")
local t = playdate.getTime()
assert(t.year and t.month and t.day and t.hour ~= nil, "getTime")
assert(playdate.epochFromTime(t) > 0, "epochFromTime")
local pdzImage = import("Assets/pdz-image")
assert(getmetatable(pdzImage) == gfx.image, "imported image metatable")
local iw, ih = pdzImage:getSize()
assert(iw == 4 and ih == 4, "imported image size")
local pdzTable = import("Assets/pdz-table")
assert(getmetatable(pdzTable) == gfx.imagetable, "imported imagetable metatable")
assert(pdzTable:getLength() == 2, "imported imagetable length")
local font = assert(import("Assets/pdz-font"))
assert(font:getHeight() == 24)
assert(import("Assets/pdz-font") == font)
font:setLeading(3)
assert(font:getLeading() == 3)
function playdate.update() end
"""


def main():
    _, luac, runner, *wrapper = sys.argv
    with tempfile.TemporaryDirectory() as temp:
        bundle = pathlib.Path(temp) / "image.pdx"
        assets = bundle / "Assets"
        assets.mkdir(parents=True)
        (assets / "checker.pdi").write_bytes(
            pdi_file(4, 4, [0xa0, 0x50, 0xa0, 0x50]))
        (assets / "alpha.pdi").write_bytes(
            pdi_file(4, 4, [0x70, 0xf0, 0xf0, 0xf0], mask=[0x70, 0xf0, 0xf0, 0xf0]))
        cell0 = pad4(pdi_payload(9, 10, [0xff, 0xff] * 10, xoff=1, yoff=0))
        cell1 = pad4(pdi_payload(10, 8, [0x00, 0x00] * 8, xoff=0, yoff=1))
        (assets / "trimmed.pdt").write_bytes(pdt_file(10, 10, [cell0, cell1]))
        (assets / "pico.fnt").write_bytes(b"")
        source = bundle / "main.lua"
        bytecode = bundle / "main.luac"
        source.write_text(TEST_LUA)
        subprocess.run([luac, "-o", bytecode, source], check=True)
        chunk = bytecode.read_bytes()
        header = b"Playdate PDZ" + bytes(4)
        image_payload = pdi_payload(4, 4, [0xf0] * 4)
        table_payload = pdt_payload([pad4(pdi_payload(4, 4, [0xf0] * 4)),
                                     pad4(pdi_payload(4, 4, [0x00] * 4))])
        image_entry = entry(b"Assets/pdz-image", 130,
                            struct.pack("<I", len(image_payload)) +
                            zlib.compress(image_payload))
        table_entry = entry(b"Assets/pdz-table", 131,
                            struct.pack("<I", len(table_payload)) +
                            zlib.compress(table_payload))
        font_payload = bytes([12, 24, 0, 0]) + bytes(64)
        font_entry = entry(b"Assets/pdz-font", 135,
                           struct.pack("<I", len(font_payload)) + zlib.compress(font_payload))
        prefix = header + image_entry + table_entry
        # Entry alignment depends on the start of the entry.
        font_entry = entry(b"Assets/pdz-font", 135,
                           struct.pack("<I", len(font_payload)) + zlib.compress(font_payload), len(prefix))
        prefix += font_entry
        pdz = prefix + entry(b"main", 1, chunk, len(prefix))
        (bundle / "main.pdz").write_bytes(pdz)
        result = subprocess.run([*wrapper, runner, bundle], capture_output=True, text=True, timeout=10)
        assert result.returncode == 0, result
        assert "Executed main Lua chunk" in result.stdout, result
        print("Image Lua API smoke test passed")


if __name__ == "__main__":
    main()
