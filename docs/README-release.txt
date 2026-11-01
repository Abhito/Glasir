Glasir - modern PC graphics for game consoles
https://github.com/Abhito/Glasir

GETTING STARTED

1. Unzip this folder somewhere you can write to, for example
   C:\Games\Glasir. Not "Program Files": Glasir keeps its settings, logs
   and screenshots in its own folder.
2. Plug in your capture card and turn the console on.
3. Start Glasir.exe, pick your card and what you want in its window, and
   press Play. The first time, it asks to download the programs it runs on
   (mpv and ReShade, plus DLSS 5 on an NVIDIA RTX 50 series GPU). They come
   straight from their authors' sites; THIRD-PARTY-NOTICES.md lists them and
   their licences.
4. While playing, Tab opens the menu.

Settings are in glasir.ini (written on the first start, every setting
explained). Volume, boost and audio delay you change while playing are kept
there, and so is what you change in the menu (Tab).

KEYS

  Tab                      the menu (Esc or Tab closes it): DLSS 5, frame
                           generation, scaling, sound, full screen, SDR/HDR
  double-click, f          full screen
  Up / Down, mouse wheel   volume
  m                        mute
  Ctrl+Up / Ctrl+Down      audio boost (up to 400 %)
  [ / ]                    audio delay -/+ 10 ms (lip sync)
  Ctrl+r                   restart the sound
  F6                       DLSS 5 on/off
  F8                       frame generation on/off
  F9                       save a bug report, with the frames on screen
  s                        screenshot (in Screenshots\)
  q, Esc                   quit (Esc closes the menu first if it is open)
  Home                     ReShade's overlay (DLSS 5 and effect settings)

DLSS 5 starts with its authors' default settings. To tune it, open
ReShade's overlay (Home): RenoDX-DLSS5's settings are on the Add-ons tab;
DLSS5_Feed's on the Home tab (untick "Performance Mode" there to edit them).
Your changes are kept in engine\ReShade.ini and engine\ReShadePreset.ini.

UPDATING

When a newer Glasir is out, its window says so under Play: "Update Glasir"
installs it (your settings stay). [update] check = 0 in glasir.ini stops
Glasir from asking GitHub.

  Update (in the window)   downloads the newest mpv, ReShade and DLSS 5
  Glasir.exe --setup       the same, from a command line
  Glasir.exe --update      installs the newest Glasir

REPORTING A PROBLEM

Press F9 while it happens: a zip with the logs, settings, your GPUs and
screens (and the frames on screen, with frame generation on) is saved in
Bug reports\. Your Windows user name is left out. Attach it to an issue at
https://github.com/Abhito/Glasir/issues and say what you saw.

To remove Glasir, delete its folder.

Logs: logs\glasir.log, logs\audio.log, engine\ReShade.log.
