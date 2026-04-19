# Kerzoix Terminal

A custom terminal emulator written in C using SDL2 and SDL_ttf.

## Overview

Kerzoix Terminal is a lightweight, stylized terminal-like application with a custom UI and command execution system. It supports asynchronous command execution, text selection, clipboard integration, and a fully custom-rendered interface (no native console UI used).

The project is focused on low-level rendering, input handling, and building a functional terminal experience from scratch.

## Features

- Custom UI rendered with SDL2
- Borderless window with draggable title bar
- Command execution via system shell (Windows / POSIX)
- Asynchronous command processing (separate thread)
- Real-time output streaming
- Text selection (mouse + keyboard)
- Clipboard support (Ctrl+C / selection copy)
- Input editing with cursor control
- Command history (Up/Down arrows)
- Smooth text rendering using font atlas
- Scrollable output buffer
- Animated input characters and cursor blinking
- Scrollbar visualization

## Controls

- `Enter` — execute command  
- `Ctrl + C` — copy selection / interrupt input  
- `Ctrl + A` — select all input  
- `Ctrl + L` — clear output  
- `Arrow keys` — move cursor / history navigation  
- `Shift + Arrows` — text selection  
- `Mouse drag` — select output text  
- `Mouse wheel` — scroll output  
- `ESC` — clear selection  
- Drag title bar — move window  

## Tech Stack

- C (C99)
- SDL2
- SDL_ttf
- WinAPI (optional, Windows-specific features)

## Notes

- Commands are executed through system shell (`cmd.exe` on Windows).
- Output is streamed in real time from a separate thread.
- Rendering is fully custom (no native terminal UI components).

## Status

Early-stage project. Core terminal functionality is implemented, including input, rendering, and command execution.
