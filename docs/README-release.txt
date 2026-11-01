Glasir - modern PC graphics for game consoles
https://github.com/Abhito/Glasir

GETTING STARTED

1. Unzip this folder somewhere you can write to, for example
   C:\Games\Glasir. Not "Program Files": Glasir keeps its settings, logs
   and screenshots in its own folder.
2. Plug in your capture card and turn the console on.
3. Start Glasir.exe. The first time, it asks to download the programs it
   runs on (mpv and ReShade, plus DLSS 5 on an NVIDIA RTX 50 series GPU).
   They come straight from their authors' sites; THIRD-PARTY-NOTICES.md
   lists them and their licences.

Settings are in glasir.ini (written on the first start, every setting
explained). Volume, boost and audio delay you change while playing are kept
there.

KEYS

  double-click, f          full screen
  Up / Down, mouse wheel   volume
  m                        mute
  Ctrl+Up / Ctrl+Down      audio boost (up to 400 %)
  [ / ]                    audio delay -/+ 10 ms (lip sync)
  Ctrl+r                   restart the sound
  F6                       DLSS 5 on/off
  F8                       frame generation on/off
  F9                       save the frames around now for a bug report
  s                        screenshot (in Screenshots\)
  q                        quit
  Home                     ReShade's overlay (DLSS 5 and effect settings)

DLSS 5 starts with its authors' default settings. To tune it, open
ReShade's overlay (Home): RenoDX-DLSS5's settings are on the Add-ons tab;
DLSS5_Feed's on the Home tab (untick "Performance Mode" there to edit them).
Your changes are kept in engine\ReShade.ini and engine\ReShadePreset.ini.

UPDATING THE DOWNLOADED PROGRAMS

  Glasir.exe --setup       asks again and downloads the newest versions

To remove Glasir, delete its folder.

Logs: logs\glasir.log, logs\audio.log, engine\ReShade.log.
