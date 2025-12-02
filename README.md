# Calm Flight (C / Win32)

Small, soft-looking dodge-and-collect game written in pure C with the Win32 API and GDI.

## Build

Open a Developer Command Prompt (with `cl` in `PATH`) and run:

```
cl main.c /Fe:CalmFlight.exe /Zi user32.lib gdi32.lib msimg32.lib
```

## Play

- Move: `WASD` or arrow keys  
- Restart after a crash: `R`  
- Quit: `Esc`

Glide through the glowing shards, collect the cyan/green pickups, avoid the warm-colored obstacles, and chase a higher score.
