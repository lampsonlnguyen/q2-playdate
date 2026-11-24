import "crank-graphics"
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

    drawCrankMarker(x, height)
end
