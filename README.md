# Mafia Aim Assist

Lock-on aiming and full Xbox controller support for **Mafia: The City of Lost Heaven** (2002, PC).

Hold the **left trigger** (or the `O` key) and the camera glides onto the nearest person in front of you, similar to
the aim assist in Red Dead Redemption 2. The right stick also looks around like a mouse, so the game plays well on a
controller through Steam Input, on a laptop or on a handheld.

- Works with the **Steam** and **GOG** releases (patched version 1.3, see [Supported versions](#supported-versions)).
- Single-player only. No game files are modified or distributed; the mod adds a few DLLs next to `Game.exe`.
- Hot-reloadable logic, tiny and dependency free (no Visual C++ runtime needed).

## Install

1. Download the latest `MafiaAimAssist-x.y.z.zip` from the Releases page and extract it.
2. Find the folder that contains `Game.exe` and `LS3DF.dll`
   (Steam: `...\steamapps\common\Mafia\Mafia`, GOG: the folder you installed to).
3. Copy these files from the zip's `files` folder into that folder:
   - `dinput8.dll`, `MafiaAimLogic.dll`, `MafiaAimAssist.ini`
   - Controller with Steam Input on Windows: also everything from `files\xidi` (`dinput.dll`, `Xidi.32.dll`,
     `Xidi.ini`)
4. Set up the in-game controls once, as described below.

If the game is under `Program Files` and Windows refuses to copy, run the copy as administrator.

`dinput8.dll` is a proxy: the game loads it instead of the system DirectInput 8 library, and it forwards everything to
the real one. If the folder already has a `dinput8.dll` from another mod (for example an ASI loader or the Widescreen
Fix), the two cannot be used together; keep a backup of the original before overwriting it.

To remove the mod, delete the files you copied (and restore any `dinput8.dll` you backed up).

## Required: in-game controls

The mod feeds its camera movement through the game's mouse input, so the **Aim** control must listen to the mouse.
In the game go to **Options > Controls > character controls** (Polish: *Sterowanie postacią*) and set:

| Action (Polish) | Primary (*Podstawowe*) | Secondary (*Dodatkowe*) |
| --- | --- | --- |
| Aim (*Celowanie*) | `Mouse - Y axis` | `Mouse - X axis` |

To assign an axis, select the field and move the mouse along the wanted axis (up and down for Y, left and right for X).

### Controller bindings (Xbox controller)

Bind movement to the **left stick**; the game reads it as Joy0. The right stick and the left trigger are handled by the
mod, so leave them unassigned.

| Action | Binding |
| --- | --- |
| Forward / Backward | `Joy0 / Y Axis / -` and `Joy0 / Y Axis / +` |
| Left / Right | `Joy0 / X Axis / -` and `Joy0 / X Axis / +` |
| Look around | right stick (handled by the mod) |
| Lock-on aim | hold the left trigger (handled by the mod) |

Buttons are numbered by Xidi like this: A = 1, B = 2, X = 3, Y = 4, LB = 5, RB = 6, LT = 7, RT = 8, Back = 9,
Start = 10, left stick click = 11, right stick click = 12. A comfortable layout is Fire = `Joy0 / Button 8` (RT),
Action = Button 3 (X), Jump = Button 1 (A), Crouch = Button 2 (B), Reload = Button 4 (Y).

## Steam and Xbox controller on Windows

1. Connect the controller to Windows (USB or Bluetooth) **before** starting the game.
2. In Steam open the game's **Properties > Controller** and set Steam Input to **Enable Steam Input**.
3. Open the game's controller configuration and pick the plain **Gamepad** layout (not "Keyboard and mouse").
4. Make sure the Xidi files from `files\xidi` were copied next to `Game.exe`.
5. Launch the game **from Steam**, set the controls from the sections above, and play.

The game only knows old DirectInput controllers. Steam Input hides the pad from DirectInput and shows an XInput
controller instead; Xidi translates it back for the left stick and buttons, and the mod reads XInput directly for
the right stick and the trigger.

## Steam Deck, Legion Go and other SteamOS devices

Proton's own DirectInput already sees the controller, so Xidi is **not** needed.

1. Copy `dinput8.dll`, `MafiaAimLogic.dll` and `MafiaAimAssist.ini` from the zip's `files` folder into the game folder
   (in desktop mode: `~/.local/share/Steam/steamapps/common/Mafia/Mafia/`).
2. In Steam open the game's **Properties > General > Launch options** and enter:

   ```
   WINEDLLOVERRIDES="dinput8=n,b" %command%
   ```
3. Set the in-game controls as described above and start the game.

This path is expected to work but has had less testing than Windows. Please report what you see, including the
logs listed under [Troubleshooting](#troubleshooting).

## Settings

`MafiaAimAssist.ini` (next to `Game.exe`) is re-read every second, so you can change it while playing.

| Key | Default | Meaning |
| --- | --- | --- |
| `aim_height_cm` | `95` | Aim point above the target's feet. The default hits the torso of a standing person and the chest of a crouching one. Use about `120` to aim at the head of standing targets. |
| `aim_key` | `O` | Keyboard key for lock-on. Set one letter/digit, `F1`-`F12`, `SPACE`, `ENTER`, `TAB`, `ESC`, `SHIFT`, `CTRL`, `ALT`, `CAPSLOCK` or `BACKSPACE`. The left trigger remains enabled. |
| `aim_response_percent` | `70` | Lock-on strength. Lower it for a slower approach; accepted range is `25` to `150`. Changes are read while the game runs. |
| `target_switch_stick` | `2` | While locked on, flick this stick sideways to switch to the nearest person on that side: `2` right stick (it does not look around while a target is locked), `1` left stick (also moves the character), `0` off. |
| `right_stick_look` | `1` | Right stick moves the camera. Set `0` to disable. |
| `look_x_speed`, `look_y_speed` | `1100`, `1000` | Camera speed at full deflection, mouse counts per second. |
| `invert_y` | `0` | `1` inverts the vertical look direction. |
| `stick_deadzone` | `15` | Stick drift filter in percent of the travel. |

## Troubleshooting

- **Nothing happens when holding the trigger:** check that the Aim control is `Mouse - Y axis` / `Mouse - X axis`
  and that a person is within about 80 metres in front of you. Aim assist is off while driving.
- **The controller does nothing at all:** connect it before launching, enable Steam Input with the Gamepad layout,
  and confirm the Xidi files are installed. Without Steam Input, Windows exposes the controller to the game directly.
- **Camera spins or aim drifts the wrong way:** delete `%TEMP%\MafiaAimGain.cal` and try again; the mod relearns
  your mouse sensitivity in a few seconds of play.
- **Logs:** `%TEMP%\MafiaAimLogic.log` and `%TEMP%\MafiaAimHost.log`. On SteamOS they are in the Proton prefix
  (`.../compatdata/<appid>/pfx/drive_c/users/steamuser/Temp/`).

## Supported versions

The mod reads the game's memory at fixed addresses, so it only supports one executable: `Game.exe` with
SHA-256 `303eb95ee2de3433511ce0cb518921dcb96b62b0f64ebfcc436723ff5083f298`. This is the Steam release and the GOG
release, both version 1.3. Older retail builds are not supported.

## Known limitations

- It locks onto the nearest living pedestrian in front of the camera. It does not know friend from foe, so it may
  pick a civilian.
- It does not currently test line of sight. A pedestrian behind a wall can still be selected; reliable wall blocking
  needs a game collision/raycast query that has not yet been identified for this build.
- Disabled while driving.
- It replaces `dinput8.dll`, so it cannot be combined with other mods that use the same file name.

## How it works

`dinput8.dll` is loaded by the game's engine and hooks the DirectInput mouse device. Each time the game reads the
mouse, the mod adds the right stick's movement and, while the lock-on is held, a correction toward the target. The
correction is a closed loop: it reads the camera direction and the target's position from the game, and learns how many
radians of camera turn one mouse count produces so it works at any sensitivity. Because it only adds mouse input, the
game's own camera and aiming code does all the real work. `MafiaAimLogic.dll` is loaded by the proxy as a copy, which
lets the logic be replaced while the game runs.

## Build from source

Requires Visual Studio with the C++ desktop tools. Run `build.bat`; it builds the two DLLs, downloads Xidi
(verifying its SHA-256) and writes `dist\MafiaAimAssist-<version>.zip`.

```
src\MafiaAimLogic.c     aim logic, right stick, sensitivity learning (MafiaAimLogic.dll)
src\MafiaAimHost.c      DirectInput proxy and hot reload host (dinput8.dll)
config\                 default MafiaAimAssist.ini and Xidi.ini
```

## License

MIT, see `LICENSE`. Bundled Xidi is BSD 3-Clause, see `THIRD_PARTY_NOTICES.md`. This project is not affiliated with
2K Games, Hangar 13 or Illusion Softworks.
