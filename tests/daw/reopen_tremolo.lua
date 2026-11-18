-- Reopen the saved Tremolo project with no server running and report what the
-- host actually restored: the plugin's own state chunk and the host parameter
-- values. Proves offline reopen, not a file hash.
local root = assert(os.getenv('OPENVST_TEST_ROOT'), 'OPENVST_TEST_ROOT required')
local function write(name, text)
  local f = assert(io.open(root .. '/' .. name, 'w')); f:write(text); f:close()
end
local function main()
  reaper.Main_openProject('noprompt:' .. root .. '/tremolo.rpp')
  assert(reaper.CountTracks(0) == 1, 'tremolo.rpp did not open: ' .. reaper.CountTracks(0) .. ' tracks')
  local track = assert(reaper.GetTrack(0, 0), 'track missing after reopen')
  assert(reaper.TrackFX_GetCount(track) == 1, 'expected exactly one FX after reopen')
  local _, name = reaper.TrackFX_GetFXName(track, 0, '')
  assert(name:find('OpenVST FX', 1, true), 'plugin identity lost on reopen: ' .. tostring(name))
  local ok, chunk = reaper.TrackFX_GetNamedConfigParm(track, 0, 'vst_chunk')
  assert(ok and chunk and #chunk > 0, 'no chunk after reopen')
  write('tremolo-chunk-reopened.txt', chunk)
  local lines = {}
  for p = 0, reaper.TrackFX_GetNumParams(track, 0) - 1 do
    local _, pname = reaper.TrackFX_GetParamName(track, 0, p, '')
    lines[#lines + 1] = p .. '\t' .. pname .. '\t' ..
      string.format('%.9f', reaper.TrackFX_GetParamNormalized(track, 0, p))
  end
  write('tremolo-params-reopened.txt', table.concat(lines, '\n'))
  write('tremolo-reopened.txt', 'opened ' .. tostring(name) .. '\n')
end
local ok, err = xpcall(main, debug.traceback)
if not ok then write('error.txt', err) end
