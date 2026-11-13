-- Second isolated phase: render the saved projects without any GUI session.
local root = assert(os.getenv('OPENVST_TEST_ROOT'), 'OPENVST_TEST_ROOT required')
local function write(name, text)
  local f = assert(io.open(root .. '/' .. name, 'w')); f:write(text); f:close()
end
local function main()
  assert(reaper.GetSetProjectInfo_String(0,'RENDER_FILE',root,true))
  assert(reaper.GetSetProjectInfo(0,'RENDER_BOUNDSFLAG',0,true))
  assert(reaper.GetSetProjectInfo(0,'RENDER_TAILFLAG',0,true))
  assert(reaper.GetSetProjectInfo(0,'RENDER_ADDTOPROJ',0,true))
  assert(reaper.GetSetProjectInfo(0,'RENDER_DITHER',16,true))
  assert(reaper.Main_openProject('noprompt:' .. root .. '/fx.rpp'))
  assert(reaper.GetSetProjectInfo_String(0,'RENDER_PATTERN','fx-render',true))
  reaper.Main_OnCommand(41824,0) -- File: Render project, using the most recent render settings
  assert(reaper.Main_openProject('noprompt:' .. root .. '/synth.rpp'))
  assert(reaper.GetSetProjectInfo_String(0,'RENDER_PATTERN','synth-render',true))
  reaper.Main_OnCommand(41824,0)
  write('rendered.txt','fx-render.wav\nsynth-render.wav\n')
end
local ok, err = xpcall(main,debug.traceback)
if not ok then write('error.txt',err) end
