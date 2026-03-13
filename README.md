## Overview
This project converts PlayStation 1 game images into native executables by translating the PS1 MIPS machine code into C source code that can be compiled on any platform. Input must be a `.bin` file with an accompanying `.cue` file in the same directory.

## Requirements
- MSYS2 (open the UCRT64 shell)
- CMake
- A cloned copy of this repository
- The target PS1 image: `game.bin` plus `game.cue`

## Quick build instructions
1. Open MSYS2 UCRT64.
2. In the terminal, go to the cloned repository directory.
3. Run:
```
mkdir build && cd build
cmake ..
make
```
NOTE: If you're using Ninja with Cmake. Run `ninja` instead of `make`

## Usage
To use `ps1recomp.exe` with a .bin file. Simply run:
```
./ps1recomp game.bin
```
Replace `game.bin` with the actual path/name of your `.bin` file.

If the output is successful. You should get
```
Successfully recompiled to recompiled_game.c
```

## Notes
- Ensure the `.bin` has a matching `.cue` file; the tool relies on the `.cue` metadata.
- The recompiler is still incomplete. So the generated C code of the game may contain a lot of missing opcodes. (As we have already tried it with Crash Bandicoot)
- The runtime is still unfinished.
- The project itself is still experimental and a lot of features still need to be added.
