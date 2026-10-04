# Third-party notices

## Xidi (bundled in the release zip, optional)

Xidi translates XInput controllers to DirectInput, which the game's own gamepad code needs when Steam Input
is used. The release zip contains the unmodified binaries `dinput.dll` and `Xidi.32.dll` from
[Xidi v5.0.0](https://github.com/samuelgr/Xidi/releases/tag/v5.0.0) and its license text.
The build script downloads that release and checks its SHA-256 before packaging.

Copyright (c) Samuel Grossman. Released under the BSD 3-Clause license (see `files/xidi/LICENSE` in the zip).

## Research this project builds on

- [user-grinch/Cheat-Menu-Mafia](https://github.com/user-grinch/Cheat-Menu-Mafia) for the layout of the game's
  world, player and camera structures (`CWorld`, `CPlayer`, `CCamera`) on the GOG build.
- [samuelgr/Xidi](https://github.com/samuelgr/Xidi) for the XInput to DirectInput layer.
- The Mafia modding community for documenting that the game's `LS3DF.dll` reads input through DirectInput 8.
