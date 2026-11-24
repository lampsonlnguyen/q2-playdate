local gfx = playdate.graphics
local position = 200
local height = 0

function playdate.update()
    local distance = playdate.getCrankChange()
    if playdate.buttonIsPressed(playdate.kButtonA) then distance = distance * 2 end
    if playdate.buttonIsPressed(playdate.kButtonLeft) then distance = distance - 3 end
    if playdate.buttonIsPressed(playdate.kButtonRight) then distance = distance + 3 end
    if playdate.buttonIsPressed(playdate.kButtonUp) and height > -55 then height = height - 2 end
    if playdate.buttonIsPressed(playdate.kButtonDown) and height < 55 then height = height + 2 end
    if playdate.buttonJustPressed(playdate.kButtonB) then
        position, height = 200, 0
    end
    position = 30 + (position - 30 + distance) % 340
    local x = position // 1

    gfx.clear()
    gfx.setColor(gfx.kColorBlack)
    gfx.fillRect(29, 119, 343, 2)
    for i = 0, 12 do
        gfx.fillRect(32 + i * 28, 114, 2, 12)
    end
    gfx.fillRect(x - 12, 101 + height, 24, 38)
    gfx.fillRect(x - 19, 108 + height, 38, 24)
    gfx.setColor(gfx.kColorWhite)
    gfx.fillRect(x - 4, 112 + height, 8, 16)
end
