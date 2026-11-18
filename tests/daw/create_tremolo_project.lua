-- Load the catalogue module's saved state into a REAPER FX instance and save
-- the project. The chunk comes from tests/daw/tremolo_chunk.py; setting it goes
-- through the same host entry point as opening a saved project.
local root = assert(os.getenv('OPENVST_TEST_ROOT'), 'OPENVST_TEST_ROOT required')
local function write(name, text)
  local f = assert(io.open(root .. '/' .. name, 'w')); f:write(text); f:close()
end
local function read(name)
  local f = assert(io.open(root .. '/' .. name, 'r')); local text = f:read('*a'); f:close()
  return (text:gsub('%s+', ''))
end
local function main()
  local names = {}
  local i = 0
  while true do
    local found, name = reaper.EnumInstalledFX(i)
    if not found then break end
    if name:find('OpenVST FX', 1, true) then names.fx = name end
    i = i + 1
  end
  assert(names.fx, 'OpenVST FX not discovered')
  local chunk = read('tremolo-chunk.b64')
  assert(#chunk > 0, 'empty tremolo chunk')

  for key, value in pairs({PROJECT_SRATE=48000, PROJECT_SRATE_USE=1,
    RENDER_SETTINGS=0, RENDER_BOUNDSFLAG=0, RENDER_STARTPOS=0, RENDER_ENDPOS=1,
    RENDER_CHANNELS=2, RENDER_SRATE=48000, RENDER_TAILFLAG=0,
    RENDER_ADDTOPROJ=0, RENDER_DITHER=16, RENDER_NORMALIZE=0}) do
    reaper.GetSetProjectInfo(0, key, value, true)
  end
  for key, value in pairs({RENDER_FILE=root, RENDER_PATTERN='tremolo-render', RENDER_FORMAT='evaw', RENDER_FORMAT2=''}) do
    assert(reaper.GetSetProjectInfo_String(0, key, value, true))
  end
  reaper.InsertTrackAtIndex(0, true)
  local track = assert(reaper.GetTrack(0, 0))
  assert(reaper.InsertMedia(root .. '/input.wav', 0) ~= 0, 'input insertion failed')
  local item = assert(reaper.GetTrackMediaItem(track, 0))
  reaper.SetMediaItemInfo_Value(item, 'D_FADEINLEN', 0)
  reaper.SetMediaItemInfo_Value(item, 'D_FADEOUTLEN', 0)
  local fx = reaper.TrackFX_AddByName(track, names.fx, false, -1)
  assert(fx >= 0, 'FX insertion failed')
  assert(reaper.TrackFX_SetNamedConfigParm(track, fx, 'vst_chunk', chunk), 'host refused the module state')
  -- Read back what the host now holds: this is the plugin's own save path.
  local ok, saved = reaper.TrackFX_GetNamedConfigParm(track, fx, 'vst_chunk')
  assert(ok and saved and #saved > 0, 'no chunk after injection')
  write('tremolo-chunk-saved.txt', saved)
  -- The module maps its parameters onto the permanent generic slots. Report the
  -- host-side values so parameter exposure is checked, not assumed.
  local lines = {}
  for p = 0, reaper.TrackFX_GetNumParams(track, fx) - 1 do
    local _, pname = reaper.TrackFX_GetParamName(track, fx, p, '')
    lines[#lines + 1] = p .. '\t' .. pname .. '\t' ..
      string.format('%.9f', reaper.TrackFX_GetParamNormalized(track, fx, p))
  end
  write('tremolo-params.txt', table.concat(lines, '\n'))
  reaper.TrackFX_Show(track, fx, 2) -- keep the UI closed; DSP must stay active.
  reaper.Main_SaveProjectEx(0, root .. '/tremolo.rpp', 8)
  assert(io.open(root .. '/tremolo.rpp', 'r'), 'tremolo.rpp was not written')
  write('tremolo-created.txt', 'tremolo.rpp\n')
end
local ok, err = xpcall(main, debug.traceback)
if not ok then write('error.txt', err) end
