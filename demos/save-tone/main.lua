local pd = playdate
local gfx = pd.graphics
local saved = pd.datastore.read()
local count = saved and saved.count or 0
local tone = pd.sound.synth.new(pd.sound.kWaveSquare)
local marker = gfx.sprite.new()
local lit = false
local saveFailed = false

marker:setSize(28, 28)
marker:moveTo(44 + (count % 10) * 34, 117)
marker:add()

function marker:draw(x, y, w, h)
    gfx.setColor(gfx.kColorBlack)
    gfx.fillRect(x, y, w, h)
    if lit then
        gfx.setColor(gfx.kColorWhite)
        gfx.fillRect(x + 6, y + 6, w - 12, h - 12)
    end
end

function pd.update()
    if pd.buttonJustPressed(pd.kButtonA) then
        count = count + 1
        saveFailed = not pd.datastore.write({count = count, version = {major = 1}})
        marker:moveTo(44 + (count % 10) * 34, 117)
        tone:playNote(300 + (count % 10) * 35, 0.4, 0.15)
        lit = true
        pd.timer.new(220, function() lit = false end)
    end
    pd.timer.updateTimers()
    gfx.clear()
    gfx.setColor(gfx.kColorBlack)
    for i = 0, 9 do
        gfx.fillRect(43 + i * 34, 150, 2, 12)
    end
    if saveFailed then gfx.fillRect(28, 180, 344, 4) end
    gfx.sprite.update()
end
