-- Isolated REAPER integration fixture. No user project or config is touched.
local root = assert(os.getenv('OPENVST_TEST_ROOT'), 'OPENVST_TEST_ROOT required')
local function write(name, text)
  local f = assert(io.open(root .. '/' .. name, 'w')); f:write(text); f:close()
end
write('entry.txt', 'resource=' .. reaper.GetResourcePath() .. '\nversion=' .. reaper.GetAppVersion())
local function main()
  local names = {}
  local inventory = {}
  local i = 0
  while true do
    local found, name, ident = reaper.EnumInstalledFX(i)
    if not found then break end
    if name:find('OpenVST', 1, true) then
      inventory[#inventory+1] = name .. '\t' .. ident
      if name:find('Instrument', 1, true) then names.synth = name
      elseif name:find('FX', 1, true) then names.fx = name end
    end
    i = i + 1
  end
  write('inventory.txt', table.concat(inventory, '\n'))
  assert(names.fx and names.synth, 'Both OpenVST variants must be discovered')
  local commands = {}
  for _, id in ipairs({40004, 40023}) do
    commands[#commands+1] = id .. '\t' .. reaper.kbd_getTextFromCmd(id, 0)
  end
  write('commands.txt', table.concat(commands, '\n'))
  assert(reaper.kbd_getTextFromCmd(40023,0):lower():find('new project',1,true), 'new project action mismatch')
  assert(reaper.kbd_getTextFromCmd(40004,0):lower():find('quit',1,true), 'quit action mismatch')
  local function setup(name)
    for key, value in pairs({PROJECT_SRATE=48000, PROJECT_SRATE_USE=1,
      RENDER_SETTINGS=0, RENDER_BOUNDSFLAG=0, RENDER_STARTPOS=0, RENDER_ENDPOS=1,
      RENDER_CHANNELS=2, RENDER_SRATE=48000, RENDER_TAILFLAG=0,
      RENDER_ADDTOPROJ=0, RENDER_DITHER=16, RENDER_NORMALIZE=0}) do
      reaper.GetSetProjectInfo(0,key,value,true)
    end
    for key,value in pairs({RENDER_FILE=root, RENDER_PATTERN=name, RENDER_FORMAT='evaw', RENDER_FORMAT2=''}) do
      assert(reaper.GetSetProjectInfo_String(0,key,value,true))
    end
    reaper.InsertTrackAtIndex(0,true)
    local track = assert(reaper.GetTrack(0,0))
    reaper.SetMediaTrackInfo_Value(track,'D_VOL',1)
    reaper.SetMediaTrackInfo_Value(track,'D_PAN',0)
    return track
  end
  local fxTrack = setup('fx-render')
  assert(reaper.InsertMedia(root .. '/input.wav',0) ~= 0, 'input insertion failed')
  local item = assert(reaper.GetTrackMediaItem(fxTrack,0))
  reaper.SetMediaItemInfo_Value(item,'D_FADEINLEN',0)
  reaper.SetMediaItemInfo_Value(item,'D_FADEOUTLEN',0)
  local fx = reaper.TrackFX_AddByName(fxTrack,names.fx,false,-1)
  assert(fx >= 0, 'FX insertion failed')
  assert(reaper.TrackFX_SetParamNormalized(fxTrack,fx,0,0.25))
  reaper.TrackFX_Show(fxTrack,fx,2) -- close any floating UI, DSP must remain active.
  -- The host's own view of the plugin chunk: a reference for the tail JUCE
  -- appends after the container's state document (host-level bypass data).
  local chunkOk, chunk = reaper.TrackFX_GetNamedConfigParm(fxTrack,fx,'vst_chunk')
  if chunkOk and chunk then write('fx-chunk.txt', chunk) end
  reaper.Main_SaveProjectEx(0,root .. '/fx.rpp',8)
  assert(math.abs(reaper.TrackFX_GetParamNormalized(fxTrack,fx,0)-0.25)<1e-6)
  reaper.Main_OnCommand(40023,0)
  local synthTrack = setup('synth-render')
  local synth = reaper.TrackFX_AddByName(synthTrack,names.synth,false,-1)
  assert(synth >= 0, 'Instrument insertion failed')
  local midiItem = assert(reaper.CreateNewMIDIItemInProj(synthTrack,0,1,false))
  local take = assert(reaper.GetActiveTake(midiItem))
  for _,note in ipairs({60,64,67}) do
    local start = reaper.MIDI_GetPPQPosFromProjTime(take,37/48000)
    local finish = reaper.MIDI_GetPPQPosFromProjTime(take,0.5)
    assert(reaper.MIDI_InsertNote(take,false,false,start,finish,0,note,100,true))
  end
  reaper.MIDI_Sort(take)
  reaper.TrackFX_Show(synthTrack,synth,2)
  reaper.Main_SaveProjectEx(0,root .. '/synth.rpp',8)
  write('created.txt','fx.rpp\nsynth.rpp\n')
  reaper.Main_OnCommand(40004,0)
end
local ok, err = xpcall(main,debug.traceback)
if not ok then write('error.txt',err) end
