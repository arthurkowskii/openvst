-- Phase 3: prove host-side state recall, then build an automation render.
-- Opens the project created by create_projects.lua; nothing of Arthur's is used.
local root = assert(os.getenv('OPENVST_TEST_ROOT'), 'OPENVST_TEST_ROOT required')
local function write(name, text)
  local f = assert(io.open(root .. '/' .. name, 'w')); f:write(text); f:close()
end
local function main()
  reaper.Main_openProject('noprompt:' .. root .. '/fx.rpp') -- returns no value
  local opened, project = reaper.GetProjectName(0, '')
  write('opened.txt', tostring(opened) .. '|' .. tostring(project))
  assert(reaper.CountTracks(0) == 1, 'fx.rpp did not open: ' .. reaper.CountTracks(0) .. ' tracks')
  local track = assert(reaper.GetTrack(0,0), 'track missing after reopen')
  local count = reaper.TrackFX_GetCount(track)
  assert(count == 1, 'expected exactly one FX after reopen, got ' .. count)
  local _, name = reaper.TrackFX_GetFXName(track,0,'')
  local restored = reaper.TrackFX_GetParamNormalized(track,0,0)
  write('recall.txt', name .. '\nnormalized=' .. string.format('%.9f',restored) .. '\n')
  assert(name:find('OpenVST FX',1,true), 'plugin identity lost on reopen')
  assert(math.abs(restored - 0.25) < 1e-6, 'gain not restored from project chunk')
  -- Automation: gain 0 -> 2 linearly over the first second of the project.
  local envelope = assert(reaper.GetFXEnvelope(track,0,0,true), 'cannot create FX envelope')
  reaper.InsertEnvelopePoint(envelope, 0.0, 0.0, 0, 0, false, true)
  reaper.InsertEnvelopePoint(envelope, 1.0, 1.0, 0, 0, false, true)
  reaper.Envelope_SortPoints(envelope)
  reaper.GetSetProjectInfo_String(0,'RENDER_PATTERN','automation-render',true)
  reaper.Main_SaveProjectEx(0,root .. '/automation.rpp',8) -- returns no value
  assert(io.open(root .. '/automation.rpp','r'), 'automation.rpp was not written')
  write('automation.txt','created\n')
end
local ok, err = xpcall(main,debug.traceback)
if not ok then write('error.txt',err) end
