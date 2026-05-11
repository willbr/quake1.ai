# Resolution Scaling Menu Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a Video Options menu to the SDL3 platform layer with 1x/2x/3x/4x window scale options that persist via the `vid_scale` cvar.

**Architecture:** All changes live in `sdlquake/platform/vid_sdl.c`. A `vid_scale` archived cvar (0=auto, 1–4=explicit) is registered in `VID_Init` and applied before the window is created. `VID_MenuDraw` and `VID_MenuKey` implement the classic Quake-style video screen. Both are wired to `vid_menudrawfn`/`vid_menukeyfn` at the end of `VID_Init`, which causes "Video Options" to appear in the engine's Options menu.

**Tech Stack:** C (gnu89), SDL3, Quake engine menu system (`M_Print`, `M_DrawPic`, `M_DrawCharacter` from `menu.c`)

---

### Task 1: Add vid_scale cvar and vid_scale_active; use them in VID_Init window sizing

**Files:**
- Modify: `sdlquake/platform/vid_sdl.c`

- [ ] **Step 1: Add state variables after the existing SDL globals (around line 53)**

After `static SDL_Texture *sdl_texture = NULL;` insert:

```c
static int vid_scale_active = 3;               // scale actually applied; set in VID_Init
static cvar_t vid_scale = {"vid_scale", "0", true}; // 0=auto, 1-4=explicit; archived
```

- [ ] **Step 2: Register the cvar in VID_Init before any window creation**

`VID_Init` currently starts with `if (!sys_headless)`. Insert the cvar registration **before** that block:

```c
Cvar_RegisterVariable(&vid_scale);
```

- [ ] **Step 3: Replace the auto-detect scale block in VID_Init**

Find this block inside the `if (!sys_headless)` block:

```c
// Pick the largest integer scale that fits the usable desktop area
int scale = 3; // fallback
{
    SDL_DisplayID display = SDL_GetPrimaryDisplay();
    SDL_Rect usable;
    if (SDL_GetDisplayUsableBounds(display, &usable))
    {
        int sx = usable.w / VID_WIDTH;
        int sy = usable.h / VID_HEIGHT;
        scale = sx < sy ? sx : sy;
        if (scale < 1) scale = 1;
    }
}
```

Replace it with:

```c
int scale;
if ((int)vid_scale.value >= 1 && (int)vid_scale.value <= 4)
{
    scale = (int)vid_scale.value;
}
else
{
    scale = 3; // fallback
    SDL_DisplayID display = SDL_GetPrimaryDisplay();
    SDL_Rect usable;
    if (SDL_GetDisplayUsableBounds(display, &usable))
    {
        int sx = usable.w / VID_WIDTH;
        int sy = usable.h / VID_HEIGHT;
        scale = sx < sy ? sx : sy;
        if (scale < 1) scale = 1;
        if (scale > 4) scale = 4;
    }
}
vid_scale_active = scale;
```

- [ ] **Step 4: Build and run**

```
zig build run -- +map e1m1
```

Expected: game launches normally at the same window size as before (cvar defaults to 0 = auto). No regression.

- [ ] **Step 5: Commit**

```
git add sdlquake/platform/vid_sdl.c
git commit -m "feat(video): vid_scale cvar for persistent window scale"
```

---

### Task 2: Implement VID_MenuDraw, VID_MenuKey, and wire to menu system

**Files:**
- Modify: `sdlquake/platform/vid_sdl.c`

- [ ] **Step 1: Add forward declarations for menu.c helpers**

`M_Print`, `M_DrawPic`, `M_DrawCharacter`, and `M_Menu_Options_f` are defined in `menu.c` but not in any header that `vid_sdl.c` currently includes. Add these declarations after the existing `#include` block (after line 11):

```c
// Menu helpers from menu.c (no shared header)
extern void M_Print(int cx, int cy, char *str);
extern void M_DrawPic(int x, int y, qpic_t *pic);
extern void M_DrawCharacter(int cx, int line, int num);
extern void M_Menu_Options_f(void);
```

(`Draw_CachePic`, `S_LocalSound`, and `realtime` are already declared via `quakedef.h` → `draw.h` / `sound.h`.)

- [ ] **Step 2: Add scale tables and cursor state**

After the `vid_scale_active` / `vid_scale` declarations added in Task 1, add:

```c
#define VID_NUM_SCALES 4
static const int    vid_scale_factors[VID_NUM_SCALES] = {1, 2, 3, 4};
static const char  *vid_scale_labels[VID_NUM_SCALES]  = {
    "1x  320x200",
    "2x  640x400",
    "3x  960x600",
    "4x  1280x800"
};
static int vid_menu_cursor = 0; // index into vid_scale_factors; set in VID_Init
```

- [ ] **Step 3: Implement VID_MenuDraw**

Add the following function before `VID_Init`:

```c
static void VID_MenuDraw(void)
{
    qpic_t *p = Draw_CachePic("gfx/p_option.lmp");
    M_DrawPic((320 - p->width) / 2, 4, p);

    int base_y = 40;
    for (int i = 0; i < VID_NUM_SCALES; i++)
    {
        M_Print(64, base_y + i * 16, vid_scale_labels[i]);
        if (vid_menu_cursor == i)
            M_DrawCharacter(56, base_y + i * 16, 12 + ((int)(realtime * 4) & 1));
    }
}
```

- [ ] **Step 4: Implement VID_MenuKey**

Add immediately after `VID_MenuDraw`:

```c
static void VID_MenuKey(int key)
{
    switch (key)
    {
    case K_ESCAPE:
        M_Menu_Options_f();
        break;

    case K_UPARROW:
        S_LocalSound("misc/menu1.wav");
        vid_menu_cursor = (vid_menu_cursor - 1 + VID_NUM_SCALES) % VID_NUM_SCALES;
        break;

    case K_DOWNARROW:
        S_LocalSound("misc/menu1.wav");
        vid_menu_cursor = (vid_menu_cursor + 1) % VID_NUM_SCALES;
        break;

    case K_ENTER:
    case K_SPACE:
        {
            int new_scale = vid_scale_factors[vid_menu_cursor];
            vid_scale_active = new_scale;
            Cvar_SetValue("vid_scale", (float)new_scale);
            SDL_SetWindowSize(sdl_window, VID_WIDTH * new_scale, VID_HEIGHT * new_scale);
            S_LocalSound("misc/menu2.wav");
        }
        break;

    default:
        break;
    }
}
```

- [ ] **Step 5: Wire menu functions and initialise cursor in VID_Init**

At the very end of `VID_Init` (just before the closing `}`), add:

```c
vid_menu_cursor = vid_scale_active - 1;
vid_menudrawfn  = VID_MenuDraw;
vid_menukeyfn   = VID_MenuKey;
```

- [ ] **Step 6: Build**

```
zig build
```

Expected: clean build, no errors or unexpected warnings.

- [ ] **Step 7: Run and verify the menu appears**

```
zig build run -- +map e1m1
```

1. Open main menu → Options.
2. Confirm "Video Options" appears at the bottom of the options list.
3. Press Enter on it.
4. Confirm the Video Options screen shows four entries (`1x`–`4x`) with an animated cursor beside the current scale.

- [ ] **Step 8: Verify scale change and persistence**

1. Navigate to `2x  640x400` and press Enter. Confirm the window immediately resizes to 640×400.
2. Quit the game. Inspect `id1/config.cfg` — it should contain `vid_scale "2"`.
3. Relaunch (`zig build run -- +map e1m1`). Confirm the window opens at 640×400.

- [ ] **Step 9: Commit**

```
git add sdlquake/platform/vid_sdl.c
git commit -m "feat(video): Video Options menu with 1x-4x window scale"
```
