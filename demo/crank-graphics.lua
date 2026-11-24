local gfx = playdate.graphics

function drawCrankMarker(x, height)
    gfx.clear()
    gfx.setColor(gfx.kColorBlack)
    gfx.drawLine(29, 119, 371, 119)
    gfx.drawRect(22, 91, 356, 57)
    for i = 0, 12 do
        gfx.fillRect(32 + i * 28, 114, 2, 12)
    end
    gfx.fillRect(x - 12, 101 + height, 24, 38)
    gfx.fillRect(x - 19, 108 + height, 38, 24)
    gfx.setColor(gfx.kColorWhite)
    gfx.fillRect(x - 4, 112 + height, 8, 16)
end
