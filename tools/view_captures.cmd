@echo off
rem SPDX-License-Identifier: MIT
rem Opens the viewer for the frame-generation captures (F9 while playing):
rem each capture in out\Glasir\Captures in order, real and generated frames,
rem tone-mapped for the screen. Needs Python with Pillow, and ffmpeg on PATH.
rem   tools\view_captures.cmd            the captures of out\Glasir
rem   tools\view_captures.cmd FOLDER     another folder of captures
start "" pythonw "%~dp0..\src\framegen\eval\view_captures.py" %*
