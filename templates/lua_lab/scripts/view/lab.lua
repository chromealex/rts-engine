-- View script (presentation only): notices for lab messages (fire smoke: the field view, rules fields.fire.view).

view.on("message", function(e)
  if e.text and (e.player == 0 or e.player == view.player()) then view.notice(e.text) end
end)
