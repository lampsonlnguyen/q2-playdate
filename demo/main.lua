local gfx = playdate.graphics
local position = 200

function playdate.update()
    position = 30 + (position - 30 + playdate.getCrankChange()) % 340
    local x = position // 1

    gfx.clear()
    gfx.setColor(gfx.kColorBlack)
    gfx.fillRect(29, 119, 343, 2)
    for i = 0, 12 do
        gfx.fillRect(32 + i * 28, 114, 2, 12)
    end
    gfx.fillRect(x - 12, 101, 24, 38)
    gfx.fillRect(x - 19, 108, 38, 24)
    gfx.setColor(gfx.kColorWhite)
    gfx.fillRect(x - 4, 112, 8, 16)
end
