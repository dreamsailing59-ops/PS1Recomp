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
## Usage
To use `ps1recomp.exe` with a .bin file. Simply run:
```
ps1recomp.exe game.bin
```
Replace `game.bin` with the actual name of your `.bin` file.

## Notes
- Ensure the `.bin` has a matching `.cue` file; the tool relies on the `.cue` metadata.
- Generated C code is intended for portability but may require platform-specific toolchain adjustments for best results.
- For large images the conversion and build can take significant time and disk space.
