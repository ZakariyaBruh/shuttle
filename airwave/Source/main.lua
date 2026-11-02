-- Airwave: the app itself is C (src/main.c). Lua only drives the frame loop and the system keyboard,
-- which the Playdate only offers to Lua (CoreLibs/keyboard).
import "CoreLibs/keyboard"

local kb = playdate.keyboard

-- the keyboard reports every change; the C side keeps the text
kb.textChangedCallback = function() airwave.kbText(kb.text) end
kb.keyboardWillHideCallback = function(ok) airwave.kbDone(ok) end

function playdate.update()
	-- C asks for the keyboard by returning the starting text (and the capitalisation it wants)
	local text, caps = airwave.kbRequest()
	if text ~= nil and not kb.isVisible() then
		kb.setCapitalizationBehavior(caps == 1 and kb.kCapitalizationSentences or kb.kCapitalizationNormal)
		kb.show(text)
	end
	-- the keyboard slides in from the right; its left edge lets the C UI shrink to the free space, frame by frame
	local visible = kb.isVisible()
	airwave.update(visible and kb.left() or 400, visible)
end
