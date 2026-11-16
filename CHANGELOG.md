# Changelog

Each version's section is that release's notes: `tools/build/notes.py` puts it into `docs/release-template.md` (install,
updating, known issues, reporting) for the GitHub release. A section has a summary paragraph, `### Changes` (each with
its issue, #N), `### Tested` (what was run from the packages before publishing) and optionally `### Known issues`.

## 0.1.13

Terrain, roads and trees that drew black on Intel Arc cards, the game's own sharpening and anti-aliasing passes now
getting their values (#423 by @AngeloAscheri, #485 by @FIocker), Automatic frame rate picking 60 or 30 frame by frame
again, race frames recorded in parallel by default (split screen too), a 4 GB card that no longer draws a dark band, a
memory leak that lost the GPU in the title's demo loop, and the split-screen view that drew with the other player's
culling. New: Vert+ for tall windows by @simonwjackson, Discord
Rich Presence by @jony25, Classic VS by @andreknieriem, and a Windows release built on Windows itself by @Elenedeath.
Thanks to @AngeloAscheri, @FIocker, @VictorMerk, @SavageCore, @simonwjackson, @jony25, @andreknieriem, @Elenedeath
and @guillo1990.

### Changes

- **Intel Arc: ground, roads and trees no longer draw black** (#14, #64, #181, #183, #276, #337, #355, #368, #475,
  #476): Intel's Windows driver reports the sparse block of a compressed format as 64x64 blocks, where NVIDIA and
  Mesa report 256x256 texels (the same 64 KiB). Read as texels, every block took 16 times the tile slots: an Arc B580
  left 1 048 576 terrain tiles without memory against 32 768 on an RTX 3050. The size is now converted to texels.
  Not run on an Intel card here: the reports' logs are the evidence.
- **SMAA's search steps again, 13 ms a frame less** (#485 by @FIocker): the game pushes its constants (`1/w, 1/h, w,
  h`) once, naming the vertex stage, and 1.0.0's anti-aliasing chain (edge detection, blend weights, neighbourhood
  blending) reads them in its fragment shaders too, which saw zeros: the blend-weight pass then searched every edge
  pixel to its end, 13.1 ms of GPU per frame at 2560x1440 on an RTX 5090 and a game at 0.69-0.89x. A graphics push now
  reaches every graphics stage: 0.99 ms and 1.00x, with smoothed edges as on the console. Photo mode's compute push is
  unchanged.
- **The upscale pass gets its constants: sharp, uncropped picture** (#423 by @AngeloAscheri): the calls with which the
  game hands constants to the driver's bank c24 were dropped, so five programs read zeros. The fullscreen upscale pass
  sampled a cropped region, and every docked-mode frame on 1.8.0 was enlarged and blurred; `MKW_PUSH_CONST=0` brings
  the zeros back for comparing.
- **Automatic frame rate picks 60 or 30 frame by frame again** (#442, #446, #436, #448, #427, discussion 432): 0.1.12
  held one whole interval, so a PC at 40-55 fps sat at 30 and came back to 60 only after two seconds under 14.2 ms,
  or ran slowly at a fixed 60. The game's own choice (1 or 2 per frame, as on the console and in 0.1.11) is back while
  30 fps fits; 20 and 15 apply only below 30 and in races.
- **Automatic keeps its interval through stalls** (#461 by @andreknieriem): a stall (a frame past 133 ms after frames
  that fitted) sent Automatic to 15 fps for up to a minute, as on a Steam Deck with two players (interval 3 at full
  speed); a stall now moves nothing for up to 20 s.
- **A full 4 GB card no longer draws a dark band** (#443): a chunk of the game's physical memory that found no video
  memory stayed empty, and what the game placed there drew wrong (a dark band on the road that follows the kart on an
  RTX 3050). After freeing memory and retrying, it now takes any memory type, as sparse tiles do.
- **The title's demo loop no longer loses the GPU** (#468 by @AngeloAscheri): 1.8.0 makes its shadow map again at a
  new place in the buffer pool at each scene, and every released copy overlapped the next and was kept: 10.3 GB held
  until amdgpu ran out of memory and the device was lost after 190 s (7.3 GB after 60 s in a two-player race with
  default settings). A released texture a later image was written over now goes; the demo loop ran its full 415 s.
- **Split screen: each view draws with its own culling** (#430): both players' culling blocks, which share buffers,
  ran before the first view, so every view used the last one's: a parked player 1 lost the crowd and grass the far
  player 2 no longer saw. The two queues' work is ordered by their event waits by default (on a GTX 1050 Ti, 14.8
  against 13.8 fps; `MKW_QUEUE_WAITS=0` turns it off, +3.1 ms a frame on a Radeon 660M without it).
- **Race frames are recorded in parallel by default, split screen included** (#449): with 6 or more physical cores
  (`MKW_REPLAY_PAR`; `=0` turns it off, `=1` forces it) the replay records the frame's big submit in chunks on several
  threads. The replay thread records the first 256 draws itself while the chunks are cut, so the GPU starts 1.5-2.4 ms
  after the game's last submit, as without chunks, instead of 5.4-6.3 ms (+0.28 ms a frame left in one player on an
  i5-8400 with a GTX 1050 Ti, within noise). Where the recording holds the frame it is about a quarter shorter, and
  two-player frames, which used to stay serial, are chunked again: 33-40 to 25-27 ms of recording. Below 6 physical
  cores it stays off (a Steam Deck lost 25.4 to 22 fps in two players: the chunks took the game's CPU).
- **Water courses bounce and trick again** (#441 by @SavageCore, #410, #163, #433): a compute pass wrote the water's
  collision height map to the image only, never to the game's memory, so the CPU physics read a flat road while the
  GPU showed waves. Its writes now reach the game's memory at the fence.
- **Photo mode on game version 1.8.0**: its last step, a compute program that scales the picture to 3840x2160, read
  zeros for its pushed constants and wrote black at 1440 lines (#384, #423 and the compute push by @AngeloAscheri);
  and leaving photo mode in a race crashed (#440) because the second buffer pool had no room for its own shadow map,
  which it now counts (about 96 MiB more RAM; not yet confirmed in a race).
- **The setup survives more machines**: zig's caches go inside the game folder and are kept between setups, where a
  `/home` on one file system still failed with a cross-device rename (#439); a generated C file with NUL bytes is
  generated again instead of failing every retry (#434); the exefs step runs again when its copy has no `main`
  (#419); a Windows certificate store Python cannot read no longer stops the downloads (#456); and a new patch set no
  longer recompiles the game's 359 chunks (9 min on a 6-thread Windows PC).
- **Window mode fills screens taller than 16:9 with Vert+** (#450 by @simonwjackson, 1.8.0 by us): the 16:9
  horizontal field of view is kept and more shows vertically, controls fit without stretching and full-canvas
  backdrops cover the added height. On 1.8.0, where the game's output ignored the window at original resolution, the
  output now follows it and keeps the picture's shape.
- **Discord Rich Presence** (#455 by @jony25): while the game runs, the player's Discord profile shows it, the time
  played and the phase (menus, a race, Grand Prix, Time Trial, Battle...) with the game's own course names in English,
  through Discord's local pipe with no library; without Discord the game never waits. Off by default, as the player's friends see it:
  Settings > General.
- **Classic VS** (#452 by @andreknieriem): Settings > Hacks, off. Every VS race laps on one course, as Classic Grand
  Prix does for cups.
- **The replay thread pins itself to a fast core on hybrid CPUs** (#480 by @AngeloAscheri): the thread that records the
  race frame's big submit sat alone on the last physical core, which on an i7-14700F is an efficiency core (4.2 GHz
  against 5.4): +4.0 ms a frame. It now takes the last of the fastest cores (-2.5 ms against unpinned), and the other
  threads get every processor but that core's (AMD's SMT siblings are not numbered together on Linux).
- **UI images without a faint box on GPUs without ASTC** (#464 by @AngeloAscheri, #397): alpha 0 comes back exact when
  textures are re-encoded as BC7, so the lighter rectangle behind the Grand Prix "OK" banner and the cc badges is gone.
- **Radeon RX 570 and older AMD: black paving, grass and buildings** (#458 by @VictorMerk): texture-array sampling,
  integer image types, cube compatibility and wide-subgroup semantics fixed on the tested RX 570.
- **Vulkan teardown and sparse mips** (#459 by @VictorMerk): private sparse mip backing starts zeroed (it showed old
  GPU bytes), and the Vulkan objects are released in order before shutdown (zero validation errors on the RX 570).
- **Replays keep what the screen showed** (#422 by @AngeloAscheri, #447 by @AngeloAscheri): captures saved for testing keep RGB10A2
  presents and decoded ASTC images.
- **The controller reaches every button of the start screen again** (#418 by @guillo1990): since the dark start
  screen, a controller or the arrow keys only reached Play and the profile chip.
- **Windows release built on Windows itself** (#396 by @Elenedeath): `build.py`, `windeps.py` and `dist.py --target
  windows` run on Windows, without Linux, WSL or Wine.
- **CI actions at their latest majors, Dependabot grouped** (#444 by @SavageCore, #453 by @app/dependabot): fewer PRs,
  merged once every CI job is green.
- **Spanish (Latin America) corrected by a native speaker** (#421 by @AngeloAscheri).
- **Smaller**: `ASTC_BC7`, `ASTC_BC1` and `QUEUE_WAITS` are no longer logged as unknown keys of `mkw.ini` (they worked);
  a link to the project's GitHub page on the start screen (#420); `vkCreateInstance failed (-9)` is a
  known error with its wiki section (#426); the setup's warm-up writes `logs/warm.log`; frame captures work on Windows.

### Tested

From the packages Actions built for this tag, before publishing:

- The whole setup from the Linux AppImage (12 threads, game 1.0.0) in about 16 minutes, its warm-up to the end without a
  fault.
- The whole setup from the Windows zip (the 6-thread Windows PC, game 1.0.0) in about 25 minutes, its warm-up to the
  end without a fault.
- #485 replayed on a race capture (RADV): 0 validation errors, the images the same as without it (largest difference
  0.0003). The 1.0.0 SMAA cost is the author's measurement on an RTX 5090, not repeated here.

## 0.1.12

A new start screen and settings by @ViniciusStanula; 20 and 15 fps at the race's real speed, Low latency and frame
generation that splits the screen's refreshes evenly; a second fix for NVIDIA's "instruction pointer invalid" crashes,
with one part by @Elenedeath; Classic Grand Prix, faster audio and Steam Deck fixes by @andreknieriem; the Windows
setup sized by the PC's real memory by @fraustiz; and fixes for the setup's shaders step (cause found by @Talaric0),
the minimap on AMD (diagnosed by @elenotrindade) and game version 1.8.0 (photo mode, Thai, LAN play, button icons).
Thanks to @ViniciusStanula, @andreknieriem, @Elenedeath, @fraustiz, @SavageCore, @Talaric0 and @elenotrindade.

### Changes

- **The start screen's main page, with the update at the top right** (#357 by @ViniciusStanula): the profile that
  plays is a chip beside Play (it opens Profiles) instead of a menu row, the first controller SDL sees sits under
  Play — with how many more, and a click through to Controls, refreshed when a pad is connected or removed — and the
  update is offered as a card in the upper right with the release, the size in use, Update and What's new. The
  desktop shortcut is offered once after setup instead of taking a menu entry, and the game files' state at the
  lower left names the version the last setup installed.
- **Settings in five tabs plus Advanced and Hacks** (#358 by @ViniciusStanula): the eight tabs become Screen,
  Graphics, Sound, Controls and General, with Game and Menu folded into General (both languages, LAN play, the update
  check) — every row kept its setting, only where it lives changed. Graphics opens with the Quality preset and the
  four rows it sets hanging under it, then a Resolution group and a Performance group, and the FSR sharpness row is
  greyed out unless the scaling is AMD FSR 1. The row described on the right is the one the player used last, the
  mouse's or the cursor's, never both; the start screen's rows show a changed value with a dot instead of the
  default and "after restart" notes (nothing there needs a restart), and Graphics gains **Auto-set**, the best preset,
  resolution and FSR for the card and screen in use, only when the player asks for it — the defaults stay the
  console's — with an Undo.
- **The Game files page follows what is installed** (#359 by @ViniciusStanula): with nothing set up it is three
  numbered steps — the player's copy, how its RomFS is kept as two cards saying what each costs, then Set up with the
  folder it installs to — and once installed a card of what is there (game version, the port version that set it up,
  how the RomFS is kept, the copy and the folder) with the steps folded under Set up again. A failed setup gets a red
  card above the steps with the reason, Try again (the steps already done are kept) and the log folder.
- **A Tools page on the start screen** (#360 by @ViniciusStanula): Back up saves zips the profile into the game
  folder's `backups/`, Add to Steam puts the game in every Steam user's `shortcuts.vdf` (the same executable
  replaced, never doubled), Undo update brings back the version the last update replaced (the files it replaced are
  kept for it), and Report a problem copies the system's details to the clipboard and opens the bug form. The
  scripts run off the UI thread, one job at a time. The start screen's 64 new texts come in all 20 menu languages.
- **The Windows setup sizes its jobs by the PC's memory** (#416 by @fraustiz): it read the memory size from
  `/proc/meminfo`, which only Linux has, so every Windows PC was taken for one with 8 GB (a 24-thread PC with 31 GB
  logged `machine: 24 threads, 8.0 GB, 9 compile and 6 generate jobs, win32`). The size now comes from Windows
  (`GlobalMemoryStatusEx`). With it alone, 24 generate workers held 26.6 GB (1.1 GB each, 0.42 on Linux) and left
  0.13 GB free, so a Windows generate worker now counts 1.2 GB, and on every system the jobs also fit in the memory
  free when the setup starts: 17 compiles and 7 generate workers on that PC, the setup in 222 s instead of 300 s.
- **NVIDIA "instruction pointer invalid": the particles' integer abs** (#395 by @Elenedeath, #22): NVIDIA's shader
  compiler mis-compiles an integer `abs` of a small select result followed by an add or xor, and the GPU jumps to an
  invalid address; in 1.8.0 it hits the title screen's and the Souk's particle programs on their first draw. The
  translator now writes that `abs` as `x * ((x >> 31) | 1)`, the one form the driver does not turn back into it
  (bit-exact for every 32-bit input). It goes with the second fix below.
- **LAN play on game version 1.8.0** (#394 by @Elenedeath, #309): 1.8.0's patch table had no `lan` set, so its title
  menu kept Wireless Play (the console's radio, which a PC does not have) and picking it ended in 2203-0022, the game
  never asking for a local network. The set is ported, and with LAN play on the menu opens with LAN Play chosen, as
  on 1.0.0.
- **Classic Grand Prix** (#402 by @andreknieriem): a new switch in Settings > Hacks (`MKW_CLASSIC_GP`, off). With it
  on, every race of a Grand Prix cup is laps on one course: the intermissions, one lap on the road to the next
  course, become that course for its laps (Mushroom Cup: Mario Bros. Circuit, then Crown City for 3 laps, and so on).
  Knockout Tour is left as it is. Turned on during play, it applies to races started after its scan (a few seconds).
- **Audio decodes each sample once** (#331 by @andreknieriem): a DSP-ADPCM voice decoded each sample about three
  times; now about once, 24 % fewer cycles in the mixer. Two errors in the history carried from one buffer to the next
  (at a buffer change, and at a pitch above 1) are fixed along the way, checked against an exact decode.
- **Dynamic state recorded only when it changes** (#349 by @andreknieriem): the 12 to 18 dynamic-state commands of a
  draw were recorded again after every pipeline bind; now only the ones whose value changed, 10.6 % fewer Vulkan calls
  on a Steam Deck's Grand Prix (no measurable change in frame time there, which the replay thread bounds).
- **Out of video memory for a pool chunk, memory is freed first** (#326 by @andreknieriem): on the Steam Deck (1 GB
  carve-out) a pool chunk that found no video memory failed outright; it now frees idle textures and waits for the
  GPU, as an image already did, and tries again (a wait of 1.9 ms at the median and 17.5 ms at most on the Deck).
- **The GPU hang report comes before amdgpu resets the GPU** (#327 by @andreknieriem): on the Steam Deck the kernel
  resets a GPU job after 5 s (SteamOS's `amdgpu.lockup_timeout`), as soon as the report of #297 was due, so three hangs
  ended with no line in `mkw.log`. On amdgpu the report now comes after two fifths of that timeout (2 s on the Deck);
  an unset or unavailable kernel timeout uses 1 s (the documented default has been as short as 2 s), elsewhere it stays
  at 5 s, and `MKW_GPU_HANG_MS` sets it. Each stuck episode reports once, not only the first, and one the GPU finishes
  after all says so ("it was slow, not hung").
- **"Straight to" a Grand Prix** (#348 by @andreknieriem): Settings > Hacks > Straight to gains Grand Prix (24 karts,
  through the menus by phase, skipping the course's flyover), beside time trial and free roam.
- **Makefile shortcuts** (#362 by @SavageCore): for building from source, `make build`, `check`, `dist`, `appimage`,
  `windows` and `clean` run the commands `tools/build/README.md` documents (`make help` lists them).
- **The Steam Deck renders the race at 720 lines** (#317 by @andreknieriem): the Deck's picture is 1280x720 on its
  screen, but the 3D scene still rendered at the console's docked 2560x1440. @andreknieriem measured it on a Deck: those
  passes took 16 of the GPU's 42 ms a frame. The Deck's profile now sets Resolution to 720, and races went from about
  36 to 24 ms a frame. The menu or `mkw.ini` still pick another one (a Deck on a TV, for instance).
- **The start screen says how the last game ended**: when it stopped with a known problem, it names the issue that
  follows it (no need to report it again) or the release that fixes it; an unknown error gets a button to the bug
  form, asking for `logs/mkw.log`.
- **20 and 15 fps, at the race's real speed, and Low latency**: Settings > Graphics > Frame rate gains 20 and 15, and
  Automatic now drops to them (it stopped at 30) when frames take too long, so a slow graphics card no longer plays
  the race in slow motion: at 15 the time trial's clock runs at 0.998x real time and the karts drive as at 60. With
  Frame generation at 2x, 15 fps shows as 30. New **Low latency** (on): the game used to prepare up to two frames
  ahead of the screen; now it starts each one only as early as its work needs, measured frame by frame. On a Radeon
  660M at 15 fps a controller read reached its frame's display time in about 115 ms instead of 200. 20 and 15 apply
  only in races: menus and loading screens go at 30 at most, as on the console, and a cup's opening movie plays at
  60 from its first frame, so it can always be skipped (opened at 30 or 15 fps it was not skipped in 7 of 9 runs).
  Frame generation pauses while a movie plays: the movie keeps its own rate instead of repeated frames. At 15 fps
  with 2x the made frame and the game's now go out evenly, 33 ms apart (they went 25 and 42 ms apart), and the full
  performance overlay shows the game's frames and the screen's ("20/40 fps") instead of only the game's.
  Frame generation's **Automatic** now makes only as many frames as split the screen's refreshes evenly: on 60 Hz,
  2x at 30 fps, 3x at 20 and 4x at 15, all 60, where 2x at 20 (40 fps) held frames 1, 2, 1, 2 refreshes and
  juddered. A 75 Hz screen, where no game rate fits, shows the game's own frames. New **Variable refresh screen**
  (off): with FreeSync or G-Sync on, Automatic makes as many as fit in time instead. A fixed 2x, 3x or 4x that
  does not fit the screen shows a note under it.
- **Scaling to the window is bilinear by default**: AMD FSR 1 is on only when picked in Settings > Graphics or by
  Auto-set.
- **Less copying of game memory on dedicated graphics cards**: with the memory in `mirror` (what Automatic picks on a
  dedicated card), a page only the GPU wrote was uploaded again from the game's copy, and the shadow took back in what
  the GPU had just written out; neither does now. On a GTX 1050 Ti, 2.1 ms less per frame on average.
- **Updating keeps the setup's choices**: the setup that runs by itself after an update copied the whole RomFS
  (23 GB) even when the player had chosen to link it, and an update that found a leftover of the one before in
  `update-previous/` stopped with an error instead of retrying.
- **Issues**: a report of a known error is closed at once as its duplicate (a reply from its author reopens it), and
  a report on an older release, quiet for 3 days, is asked to try the latest and closed after 7 days without a reply.
- **Motion controls for every player** (discussion 302): the gyroscope and accelerometer were read from player 1's
  controller only, so in local multiplayer players 2 to 8 had no motion; each player's controller now feeds its own.
- **Thai no longer crashes on game version 1.8.0** (discussion 193): with the game in Thai, loading closed the game
  (`no function at guest pc main+0x1bb0b34`). Its text code leaves an error with the C library's `longjmp`, which
  goes back to where `setjmp` saved its place: in translated code that place is the middle of a function. A function
  that calls `setjmp` now keeps a host return point that the `longjmp` comes back to.
- **Xbox, PlayStation and keyboard button icons on game version 1.8.0** (#19): 1.8.0's button sheet has two more
  icons, so the runtime did not recognise it and the prompts kept Nintendo's buttons. The other icons sit where
  1.0.0 has them, and both sheets are redrawn now.
- **LAN play picks the home network's address** (discussion 148): with several network adapters, the game could take
  one that leads nowhere, such as a VirtualBox host-only adapter (192.168.56.1) or one without a DHCP server
  (169.254.x.x), and two PCs on the same network never saw each other. The address on the network with the default
  route now comes first, then other local networks, then 169.254.x.x, and VPN /32 addresses last. The log lists the
  candidates (`nifm: LAN candidate`); `lan_ip` in `mkw.ini` still picks one by hand.
- **"Choose game folder" in every menu language**: its four texts came in English only in 0.1.11.
- **Photo mode no longer crashes on game version 1.8.0** (#305): pressing Minus in free roam at 1440 lines closed the
  game (`main+0xa66e0`): 1.8.0's photo mode, with its stickers, needs render targets the game's buffer pool has no room
  for. The pool gets a second part of the runtime's own memory for them, as it did at 2160 lines (#255).
- **No more silent exits in the C runtime on Windows** (#310): a date past the year 3000 given to the game's time
  service ended the process with no message (code 0xC0000409 in `ucrtbase.dll`); it is now refused as on Linux, and any
  other call the Windows C runtime rejects writes a crash report with the game's frames to `logs/mkw.log`.
- **The setup refuses `exefs` and `romfs` from different game versions** (#315): 1.0.0's program with 1.8.0's game
  files passed the checks and the game crashed while warming up; the setup now says which version each one is.
- **Unlock everything opens Mirror** (discussion 292): Settings > Hacks > Unlock everything now also offers Mirror in
  the class select, as the game does after its "Mirror Mode has been unlocked!" notice.
- **NVIDIA "instruction pointer invalid" crashes, second fix** (#22, #231): 0.1.11's change did not cover Moo Moo
  Meadows. Every logged case was a pass stuck for 1-1.5 s whose draw has a fragment shader that discards pixels
  midway (8 of 8, against 30 % of the game's fragment shaders). A discarded pixel now stays as a helper to the end of
  the shader (Vulkan's demote, as Switch emulators and DXVK do) instead of ending in the middle of it. Verified on the
  captures (same images) and not on an NVIDIA card.
- **A setup that failed once in the shaders step no longer fails every time after** (#325, cause and fix by
  @Talaric0): when NVIDIA's disassembler gave a garbled capture, the rejected spec was already in `sm86/`, and every
  later setup reused it and stopped in `translate.py` (`KeyError: 'BITS_…'` or `classes carry no Opcode constant`). A
  capture is now parsed aside and kept only when accepted, and a spec left broken by an earlier release is built
  again. Before 0.1.12: delete the game folder's `sm86` folder and run Set up again.
- **The minimap without garbage on AMD** (#365, diagnosed by @elenotrindade): AMD lays out some depth buffers larger
  than the Switch 2 does (the minimap's 676x676 one takes 0x2e0170 bytes for 0x204000), and bound at the game's address
  its extra bytes overwrote the minimap's colour target right after it. A depth or stencil buffer that does not fit its
  space now gets its own memory (`MKW_POOL_SPILL=1` restores the old binding). Same picture on the race and title
  captures on a Radeon 660M, where 7 depth buffers did not fit.
- **A processor without AVX2 gets a message instead of nothing** (#262): the game is still built for AVX2 (a build
  without it is planned in discussion 82), and on an AMD FX or an Intel Core older than the 4th generation `mkw.exe`
  closed before writing any log. It now says so in a box before anything else runs.

### Tested

From these packages, before publishing:

- The whole setup from the Linux AppImage (Radeon 660M, RADV, game 1.0.0) in 16 minutes (generate 57 s, compile 8 min,
  shaders 6 min), its warm-up to the title without a fault.
- The whole setup from the Windows zip (Windows 11, GTX 1050 Ti, 6 threads) in 23 minutes, its warm-up to the end
  without a fault; its log now names the PC's real memory (`machine: 6 threads, 15.9 GB, 10.9 GB free`, where 0.1.11
  said 8.0 GB).
- Before the tag, on `main`: the race and title captures replayed on the Radeon 660M with the minimap's depth change
  (the same images as without it: 0.053 and 0.0015 against the game), the shaders step's spec built and rejected on
  purpose (a rejected capture leaves no spec behind), and the setup's tests (40 passed).

### Known issues

- NVIDIA "instruction pointer invalid" (#22): the second fix is checked on the captures (same images), not on an
  NVIDIA card yet; `skip_programs = 1c7a765b` in `mkw.ini` still avoids the Shell Cup's crash if it comes back.
- Some AMD cards on Linux still hang later on, in a race or a menu, with the compute path (#50).
- Black ground on Intel Arc and AMD Polaris (#319, #314): the terrain's texture tiles past the 768 MiB budget stay
  empty there.
- A processor without AVX2 cannot run the game yet (discussion 82); it now says so instead of closing silently.

## 0.1.11

Choose where the game is installed, by @innateessence; @EdouardSence's fixes for the menu labels, the Linux taskbar
icon and LAN Play over a VPN; @guillo1990's free roam hitches and GPU hang reports; the Shy Guy Bazaar triangles by
@flavio-silva; @tibor-stick's AMD GPU resets on Linux; @andreknieriem's Steam Deck memory; @SavageCore's game folder
updates; handheld resolution by @titooo7; and a faster start on AMD cards on Linux.

### Changes

- **Choose where the game is installed** (#224 by @innateessence, #284): the game files always went beside the
  launcher (on Linux `~/.local/share/mkw-recomp`), and a folder moved elsewhere was set up again in the old place.
  @innateessence added "Choose game folder" to the files page: the game files, saves, mods, logs and shader caches go
  to the folder you pick (a new one is set up there; an existing installation is used as it is), the launcher's own
  `mkw.ini` only remembers which folder, and `MKW_GAME_DIR` overrides it. With no folder picked everything stays
  where it was.
- **Frame hitches in free roam from a pass rendered one layer at a time** (#296 by @guillo1990): a draw whose
  pipeline is still being built is skipped when its target was rendered recently, and the frame waits for it when it
  was not (a volume or table drawn only once). @guillo1990 found a free roam pass that renders one of its 12 layers
  each frame, so every layer looked like a one-shot draw and the frame waited 36 to 43 ms for the pipeline. Rendered
  within the last 16 frames now counts as recent.
- **GPU hangs that the driver never reports now leave a report in the log** (#297 by @guillo1990): on NVIDIA 620.70.06
  (Linux) the GPU stopped with Xid 109 while the game stayed open and the log said nothing (#13 and #206 looked the
  same). @guillo1990 added a check: when the GPU has not finished a submission in 5 s, the log names what it had
  started and not finished, with every step between them and the shader programs involved.
- **GPU memory left empty when video memory runs low** (#295 by @andreknieriem): on a Steam Deck (1 GB of video
  memory, low preset, 1.8.0 time trial) the game's GPU memory pools got their memory in 4 MiB pieces, and a piece
  that did not fit was left empty while released render targets still held 2 601 MiB. @andreknieriem found that this
  was the only allocation that gave up at once; it now drops released and idle textures first and tries again, as
  textures and upload buffers already did.
- **AMD GPU resets with "Illegal opcode in command stream" on Linux** (#281 by @tibor-stick; #43, #50): the thread
  that records the GPU commands told the submitting thread it had finished while its last commands could still be in
  the CPU's write-combining buffers, so the GPU sometimes ran leftovers of an earlier recording and the driver reset
  it. @tibor-stick found it with a patched RADV that checks every command buffer when it is recorded and again when
  it is submitted (108 broken in a 106 s run); the recorder now flushes those buffers before it says it is done.
- **About 6 fps at the title on Intel Meteor Lake** (#283, #289): the Intel driver there (Mesa 25.3 and 26.0) has no
  memory that is both cached and coherent, so the copies of game memory the GPU reads, kept in sync every frame, went
  into uncached memory and the CPU read them at about 0.25 GB/s. They now use the cached memory and write it out and
  read it back explicitly (`MKW_SHADOW_CACHED=0` restores the old choice). Not yet confirmed on that hardware.
- **Profiles in the launcher** (discussion #288): the name the game showed was always "Local". The start screen now
  has a Profiles page, opened from the "Profile: name" row under Play: each profile has its own name (up to 10
  characters, as on the console; also what LAN players see) and its own save, and the one marked Playing is the one
  the next start plays. The save you already have is the first profile, named "Player" until you rename it; removing
  a profile takes it off the list and keeps its save in `profile/players/deleted`.
- **Crash at 2160p on game version 1.8.0** (#255, #210): with the internal resolution at 2160 the game's render-buffer
  pool (250 MiB, sized for the console's 1440-line targets) ran out at the title screen and the game died in
  `ShadowPrePass`. The second pool now comes from memory the runtime reserves beside the game's heap, sized by the
  resolution and the shadow map, so it fits at any resolution without taking memory from the game. The game logic is
  checked; the 2160 picture itself still needs a GPU with memory for it (the developer's Radeon 660M runs out).
- **Play no longer hangs after many mod changes**: before the first start with a new set of mods the launcher copies
  the save to `save-before-mods` and keeps the newest 10 copies, but it was not allowed to delete the older ones and
  tried again forever, so the 11th change of mods froze the launcher on Play. The old copies are now deleted (only
  those dated folders), and the launcher goes on if one cannot be.
- **Reset all settings keeps your mods**: it also emptied the list of enabled mods.
- **Giant triangles over Shy Guy Bazaar** (#291 by @flavio-silva, #72): for single frames the 3D scene was covered by
  huge triangles there. MKW fills its instanced characters' animations on the GPU into pitch textures of 70-190 KiB
  and its CPU code reads their bytes in place every frame; only targets up to 64 KiB went back to memory at a fence
  wait, so the bones read zeros or stale bytes. @flavio-silva traced it down to the two game functions and the
  textures; pitch textures now go back at the fence whatever their size: 0 wrong frames in his Flower Cup, from 16-75
  per 600.
- **NVIDIA: the device loss with "instruction pointer invalid"** (#22, also #112 #133 #137 #142 #166 #189 #203 #208
  #212 #219 #221 #230 #231 and discussion #139): the Shell Cup after its video, Star High Sundae's lap 2, Moo Moo
  Meadows after the cows and the Special Cup's unlock animation stopped the GPU on drivers 616-621, Turing to
  Blackwell. The draws that fault use vertex shaders that read a constant bank at an address computed from a vertex
  attribute; the shader that faults and its twin that does not differ only in one such read (`c74a0d4e` and
  `852f48d6`). Those reads went through a second uniform block declared on the same binding as the bank, the only
  aliased binding of the translation; they now go through a read-only storage view of their own binding, bounded by
  its range, as the console's bank. Verified on the captures (same images) and not on an NVIDIA card: the
  `skip_programs` workaround stays, and this is the version to try without it.
- **Internal resolution in handheld mode** (#248 by @titooo7, #195): with Settings > Graphics > Operation mode on
  Handheld, the 3D scene rendered well above the chosen resolution (1476x828 for 540p), as the game draws all of its
  3D lines there and only 65 % of them docked. @titooo7 measured both modes on an AYN Odin 2 and the setting now
  gives the chosen lines in either; the log line names the mode.
- **Menu labels no longer mix with other labels' letters** (#250 by @EdouardSence, #61): on AMD cards on Linux,
  menu texts drew another frame's glyphs for a frame. @EdouardSence traced it, on an RX 9070 XT, to pages of game
  memory the GPU copy took from a shadow that did not have them yet, and measured each step: 2 947 stale pages before
  the fix, 8 after, and no swapped labels on screen.
- **The game's icon in the Linux taskbar** (#252 by @EdouardSence): on Wayland (KDE Plasma and other compositors with
  window icons) the game's and the start screen's windows showed a generic icon; they now carry the chequered flag.
- **Updating a game folder to another version no longer keeps the old version's files** (#261 by @SavageCore): a
  folder set up from 1.0.0 and then from 1.8.0 kept 1.0.0's files next to 1.8.0's, and the internal resolution was
  built from 1.0.0's `general_format` archive. @SavageCore found it on a 23 GB install, where the setup now removed
  203 files 1.8.0 does not have; the same cleanup also runs for `.nup` and `.nx` copies, and never in a linked folder.
- A setup that stops before doing anything leaves a log now (#251): `logs/setup.log` is opened first, and while the
  setup runs it writes there every two minutes where it is waiting.
- The setup compiles with more jobs where memory held it back: a compile was given 1 GB and takes at most 0.4 (a
  12-thread PC with 13 GB went from 8 to 12 jobs, 330 s to 302 s for the game's code; an 8 GB PC with 8 threads from
  4 to 8). `logs/setup.log` now says how long each step took.
- **LAN Play with a VPN such as Tailscale** (#245 by @EdouardSence, discussion #148): entering LAN Play went straight
  back to the menu (error 2318-0007) when the VPN's adapter came first, as the game took its /32 address. @EdouardSence
  found it in the reporter's log and reproduced it on Linux; the port now takes an address with a real network first,
  and says in the log when only a /32 one is left.
- 48:9 for three screens side by side (discussion #264): Settings > Graphics > Aspect ratio now offers 48:9 (5760x1080,
  7680x1440 on NVIDIA Surround or AMD Eyefinity) beside 21:9 and 32:9. Races show more at the sides and the menus and
  HUD keep their shape; "window" already took that shape from a full-screen window.
- The start screen in Thai, Japanese, Korean or Chinese (discussion #193): its title, Play button and page names showed
  `?` for every letter, as only the small text took the system font with that script; they all do now. The log also
  says which language the game was given and whether the setting or the system's locale chose it.
- With Settings > Game > Language on Automatic, the game follows the language chosen in Settings > Menu > Language
  when the game has it (discussion #193): picking Thai there gives the game in Thai too after a restart, instead of
  the system's language. A menu language the game lacks (Turkish, Ukrainian) still leaves it to the system.
- **Terrain on GPUs whose sparse textures take other memory than buffers** (#215, GTX 750 Ti): the terrain's texture
  tiles came from memory shared with a buffer used to clear them, and on that card no memory type serves both, so not
  one tile could be made and the driver then lost the device at the loading screen. Those tiles now take the
  texture's own memory type, without the clearing buffer. Checked on our cards by forcing that path (same images).
- **The picture fills the window when the driver's surface is larger** (#140, ROG Ally with Windows at 1280x720): the
  driver gave a 1920x1080 surface for the 1280x720 window, the picture was fitted to 1920x1080 and Windows showed its
  top-left part, off-centre and cut. The picture now goes in the part the window shows.
- **The HDR setting is kept** (#168): Settings > Screen > HDR was written to `mkw.ini` but dropped at the next start
  ("unknown key 'HDR'" in the log).
- **A faster start, and a smoother loading screen, on AMD cards on Linux**: the terrain's sparse textures were bound to
  their memory one 64 KiB block at a time, 11 471 driver calls in one frame of the logo (a hitch of 1.5 s) and about
  20 000 more while the loading screen ran at about 10 fps. The blocks of a row now bind as one; on a Radeon 660M the
  logo hitch is 0.5 s shorter, the loading screen goes from 6-7 s to 5 s and the title comes 1.7 s earlier.

### Tested

From these packages, before publishing:

- The whole setup on Linux (Radeon 660M, RADV) in 15 minutes, its warm-up 1 087 frames without a fault, and on
  Windows 11 (GTX 1050 Ti) in 25 minutes, its warm-up to the end without a fault.
- Before the tag, on `main`: the sparse bind change against the title capture (same images as the reference), and
  boot runs to the title on the Radeon 660M.

## 0.1.10

Mods in the start screen, from @SavageCore's work, with GameBanana's mods a click away; Unlock everything on 1.8.0 by
@IgorLomba; @flavio-silva's fix for the slowdown of 8 GB cards and the video memory diagnostics that found it; Switch 2
controllers over Bluetooth, AMD cards on Linux past the title, Intel GPUs past the warm-up, and less memory on Windows.

### Changes

- **Mods** (#218 by @SavageCore): the start screen's Mods page, by @SavageCore, installs a mod from a `.zip` or folder
  and enables it for the next start, merges enabled mods that change the same game archive, and builds code mods into
  the game with patches that skip a region of code, not only replace an instruction. On top of it: `.rar` and `.7z`, a
  GameBanana tab that searches and browses the game's mods there, with their pictures, and installs one with a click
  (or with GameBanana's 1-Click button), and GameBanana's code mods (`.ips`, such as 200cc or Classic Grand Prix)
  taken as they are published.
  Before the first install the page says what a mod can do (crashes, broken saves, graphics mods that cost frame rate),
  the save is copied to `profile/save-before-mods` before a start with new mods, and the log names the mods in use.
  Mods are their authors' work; this project is not affiliated with GameBanana or with any mod (README, Mods).
- **Unlock everything on game version 1.8.0** (#211 by @IgorLomba): Settings > Hacks > Unlock everything did nothing on
  1.8.0, whose save has another layout than 1.0.0's; @IgorLomba's fix finds the lists by their key in the save itself.
  It also sets the Special Cup as seen, so the cup select skips its opening animation, where NVIDIA cards faulted after
  unlocking (#142, not yet confirmed on one).
- **8 GB cards no longer slow down after a few menus** (#200 by @flavio-silva): textures the game released were capped
  in count only and could hold 1.6 GB; Windows then moved memory out of the GPU and the game fell to 0.07x speed on the
  trophy screen of an 8 GB RTX 4070 laptop. Their memory is now bounded too (a twelfth of the video memory budget,
  `tex_released_mib`), and @flavio-silva's two cups in a row on that laptop stayed at full speed.
- **Video memory diagnostics** (#199 by @flavio-silva): `vram_log = <frames>` logs the video memory census without the
  other traces, and the commands `vram` and `trim [MiB]` show it, or free memory as a failed allocation would, in a
  running game. This is how the slowdown above was found.
- **Switch 2 controllers over Bluetooth** (discussion #113): Settings > Controls > Switch 2 controllers (Bluetooth),
  off by default, pairs a Pro Controller or Joy-Con 2 without other software, on Linux (BlueZ) and Windows. On
  Windows 11 it asks for the shortest Bluetooth connection interval, against the input lag a first tester saw, and the
  log says how many reports a second the controller sends. Its Bluetooth LE protocol is from mkw-neo.
- **Less memory on Windows** (#198): the game's 8 GB heap is committed as the game touches it instead of at start, so
  Windows no longer charges it all against RAM and the page file (a race at 10.6 GB private instead of 15.7), which
  is what a 16 GB PC ran out of at the intro loading screen.
- Below 3 GB of video memory the texture tiles take a quarter of it instead of 768 MB (#215), and integrated GPUs may
  use 40 % of the RAM instead of a quarter (#174).
- On game version 1.8.0: the internal resolution setting takes effect (#195), and so do the ultrawide menus, the shadow
  map size and the fixed seed, and the game's own texts of 1.8.0 are in Catalan, Galician and Basque as well.
- In a container that refuses `userfaultfd` (Docker's and Podman's default), textures are no longer uploaded again
  every frame: ~20 fps there instead of ~6 (#241); the log says how to allow it for full speed (README).
- **Polish and Thai in the game** (discussion #193): the languages the game's own updates added (Polish in 1.5.0, Thai
  in 1.7.0) are listed in Settings > Game > Language when the dump has them (1.8.0), and a system set to one of them
  picks it; a 1.0.0 dump stays as before. Thai is also a menu and launcher language (the 20th; our first translation,
  corrections welcome).
- Settings > Sound > Mute in background: no sound while the game window is not in front (off by default).
- Settings > Controls > Stick dead zone (8 % by default): round instead of per axis, and the tilt past it starts from
  zero, so small moves just outside it are no longer lost; raise it if the kart drifts with the stick at rest.
- The setup no longer fails at "generate" with "Module use of python312.dll conflicts with this version of Python"
  (#177) when another Python's `PYTHONPATH` or `PYTHONHOME` is set on the machine.
- An update whose changes only touch how the game code is compiled no longer stops the setup at "compile" with
  `generated/layout.json` missing: the code is generated again first.
- The setup no longer fails at the shaders on Windows PCs with more than 61 CPU threads ("max_workers must be <= 61",
  #222): it uses up to 61 there.
- Moving or renaming the folder of your game files no longer stops the update's setup with "no exefs/main and romfs/"
  (#188): the start screen says the files are no longer there and asks for the folder again, and choosing it does not
  redo the game code and the shaders, since the setup now recognises the same game by its version, not its folder.
- 21:9 at 2160p (5040x2160) no longer gives a black screen with sound (#213): the game's output pool did not fit its
  memory at that size, and the port now gives it its own.
- Intel GPUs no longer hang at the title or in the setup's warm-up (#174, #184, #223): in the game's compute programs
  with barriers, a thread that leaves early now goes on through the barriers without writing anything, as on the
  console, instead of leaving the rest of its group waiting for it forever.
- Images whose usages the driver refuses together are created without the optional ones (#174: an `R8G8_UNORM` image
  on Intel's Linux driver was made with a combination it does not support).
- The setup also takes a folder with a loose `exefs/` and `romfs` as a `.nup` package (or a link to one).
- NVIDIA's shader tools are downloaded again when they arrive cut or damaged (#207), and if they keep arriving damaged
  the setup says so (an antivirus, proxy, VPN or unstable connection) instead of only showing two checksums.
- AMD GPUs on Linux (RADV) no longer lose the device at the title as soon as the game draws its first task and mesh
  programs (#4, #43, #50, #225): they are drawn as compute passes there by default, as `mesh_compute = 1` in `mkw.ini`
  did (`mesh_compute = 0` goes back to the mesh shaders). A later hang some of these cards still have is in #50.
- For NVIDIA's "instruction pointer invalid" (#22): `skip_programs = <key>[,<key>...]` in `mkw.ini` leaves out the
  draws of the programs whose key starts with those hex digits (the `vs`/`fs` keys of the breadcrumb lines), so a
  reporter can tell whether the program named at the fault is the one that faults. A diagnosis: those draws are missing.

### Tested

From these packages, before publishing:

- The whole setup on Linux (Radeon 660M, RADV) in 16 minutes, its warm-up 993 frames with the task and mesh programs
  drawn as compute passes (the new default on RADV), and on Windows 11 (GTX 1050 Ti) in 31 minutes, its warm-up to
  the end without a fault.
- Before the tag, on `main`: a mod of each kind on Windows, a skin from GameBanana's tab and Classic Grand Prix
  (`.ips`), built into the game and raced; the GameBanana tab and its 1-Click link on Windows.
- The AArch64 AppImage on an AYN Odin 2 (Adreno 740, Turnip) by @titooo7, with 0.1.9 (#176).

### Known issues

- NVIDIA cards can still lose the device in some races and menus ("instruction pointer invalid", #22); `skip_programs`
  above helps us find the program at fault.
- Some AMD cards on Linux still hang later on, in a race or a menu, with the compute path (#50).
- Switch 2 controllers over Bluetooth: tested by one player on Windows, where they worked with noticeable input lag;
  the shorter connection interval above is not confirmed yet to remove it, and no one has tried them on Linux yet
  (discussion #113). Over USB-C they are not recognised on Windows.

## 0.1.9

Mario Kart World 1.8.0 as well as 1.0.0, HDR on HDR screens, the menus in 19 languages and the game itself in Catalan,
Galician and Basque, 5.1 sound, and fixes for crashes and freezes reported since 0.1.8.

### Changes

- **Game version 1.8.0** (#19, #172 by @flavio-silva): the setup takes a 1.8.0 dump as well as 1.0.0, recognises it by
  its `exefs/main` and builds that version. Tested by its author on Windows up to the title and a race in Toad Harbor;
  on 1.8.0 the ultrawide menus, the shadow map size and the fixed seed have no effect yet.
- **A dump as one file** (discussion #8, #167 by @Robert-Hoffman04): Game files > Choose .nup or .nx file takes a
  `.nup` package or a `.nx` (NXFormat) archive where the setup took the `exefs/` and `romfs/` folders; with "Use it
  where it is" the game reads its files straight from the package, without the 23 GB copy.
- **HDR** (#168): Settings > Screen > HDR: Automatic (default) or Off. On a display in HDR the game shows its own HDR
  picture, as the console does, with the system's peak and paper white; it follows the system's HDR setting live.
- **Languages**: the menu (F1), the start screen and the setup page in English, Japanese, French, German, Italian,
  Spanish (Spain and Latin America), Dutch, Portuguese (Portugal and Brazil), Russian, Simplified and Traditional
  Chinese, Korean, Catalan, Galician, Basque, Polish, Turkish and Ukrainian; Automatic follows the system's language.
  These first translations are ours: corrections from native speakers are very welcome (`i18n/*.po`, by pull request).
  The setup's errors (a folder without the game, an encrypted or other version, an incomplete copy) are shown in the
  menu language.
- **The game in Catalan, Galician and Basque**: Settings > Game > Language. The game's own texts are rebuilt from your
  copy at setup (`i18n/game/`); the rest of the game stays in Spanish. A system set to one of them picks it. On 1.8.0,
  81 texts that changed since 1.0.0 stay in Spanish (#178 by @flavio-silva: the setup of a 1.8.0 dump stopped there).
- Brazilian Portuguese: a Brazilian system picks the game's Brazilian Portuguese (discussion #42, #134 by
  @GabrielAndrade419).
- **5.1 surround sound** when the system's output has 6 channels (discussion #90).
- **LAN play** (discussion #148, #144 by @EdouardSence): Settings > Game > LAN play turns the title menu's Wireless
  Play into the game's hidden LAN Play, which joins games on the same local network over UDP (ports 49152 and 35000).
  It only reaches local network addresses, never the internet. Off by default: so far it has been played between two
  instances on one Linux PC, not yet between two PCs or on Windows. If you have two PCs on one network, please try it
  and tell us in discussion #148.
- A new start screen: dark, with the port's logo and no game art (#70, by @ViniciusStanula); the F1 menu keeps its look.
- A notice on screen when the players chosen lack controllers (#106, #160).
- The game stopped when swapping two controllers' players (#123).
- 16 GB machines keep their pipeline workers (#132).
- The game stopped on some error screens (#147): they are logged instead of opening the console's error applet.
- Long freezes on the title or at start (#111 and others): shaders the translator now structures instead of running
  them as a slow switch (after updating, the setup's shader step runs again); a start that died building pipelines, or a failed present, no longer breaks every next start.
- Starting on Windows takes 20 s less (#128): the game files are indexed per folder.
- Game files set up by another version are set up again, also after updating by hand (#50).
- Saves survive an antivirus or OneDrive holding the file, and only one copy of the game runs per profile.
- Windows with the window minimized or hidden keeps its timing (no slowdown in the background).
- Fixes in threads and fibers inherited from the Switch 1 port (core masks, waits on 64-bit values, fibers resumed on
  another thread).
- From contributors: the window's picture takes the lines it is shown at (#130), the start no longer waits forever for
  a pipeline the driver never returns (#131), and a faster shader lookup (#129), by @guillo1990; replay chunks ended on
  their own thread (#141, by @riyadhHDA); development runs capped and stoppable (#145, by @EdouardSence); Adreno
  GPUs (Turnip) reach the menus and races without blue lines, and Linux on AArch64 (#170, #171, #173, #176, by
  @titooo7): this release has its own AppImage, `mkw-recomp-0.1.9-aarch64.AppImage` (ARMv8.2, Snapdragon 8 Gen 1 and
  newer).
- Diagnostics: a log line when the game waits too long for its frame, saying what the submission was doing (#138), and
  GPU breadcrumbs on more commands (#43); `MKW_RTSTATS` and `MKW_DUMP_DRAWS` for rendering bugs that log no error (#80,
  by @guillo1990).

### Tested

From these packages, before publishing:

- The whole setup on Linux (Radeon 660M, RADV) and on Windows 11 (GTX 1050 Ti), with its one-minute warm-up: about 16
  minutes on Linux and 31 on Windows, where the shader step took 10 minutes (14 with 0.1.8).
- The game from the Linux package reading its files straight from a `.nup` and from a `.nx` dump.
- The AArch64 AppImage is built by the same release workflow but has not been run yet: we have no ARM machine. If you
  play on one, please tell us how far it gets (#176).

## 0.1.8

Frame generation, races that render again on Intel, Adreno and (likely) AMD's Windows driver, a first take on
ultrawide screens, and a faster setup.

### Changes

- **Ultrawide** (#77, #40): Settings > Screen > Aspect ratio: 16:9 (the console's, default), the window's, 21:9 or
  32:9. Races and the title's 3D show more at the sides instead of stretching; the menus and the HUD keep their shape
  in the middle 16:9, and full-screen backdrops and fades reach the edges. What is still missing is under Known issues.
- **Frame generation** (#101, by @guillo1990): Settings > Screen > Frame generation: Off (default), Automatic, 2x, 3x,
  4x. Frames made between the game's own from the pictures (optical flow), so a race at 30 fps is shown at 60; the
  FPS overlay shows game / shown frames. It makes the picture smoother, not the game faster.
- Races black, and menus cut along a diagonal, on Intel (ANV) and Adreno (Turnip) (#26, found and fixed by
  @titooo7): full-screen passes left the position's w undefined. The black 3D with the HUD visible on AMD's Windows
  driver 26.10 (#105) looks like the same cause. After updating, the setup's shader step runs again
  (it now does whenever the translator changes).
- Black terrain on cards whose own memory filled up (#63): sparse tiles fall back to system memory.
- The game stopped when it started its network (#63, after a while in free roam): its sockets now answer as a
  console with no network, so it stays offline.
- A crash pressing HOME or switching windows during the startup logo (#83, #89, #92).
- AMD on Windows: the race's mesh draws run as compute passes, where AMD's driver lost the device (#88).
- Intel Arc drew the race black (#64): integer images and integer colour targets are read and written as integers.
- Big AMD cards ran out of video memory on the title (#43, #68): the texture pool starts at 8 blocks.
- **Faster setup**: the shader step compiles each shader inside its own worker instead of starting a compiler for
  each of the ~11 600 (3.4x faster for that part, same output), and decodes the console's shader code 40 % faster:
  the whole step takes about 43 % less processor time. Generating the game code is 13 % faster. The compiled game
  code now stays in `setup-work/obj`, so from the next update on only the parts that changed are compiled again
  (seconds instead of 8-12 minutes).
- Integrated GPUs: the memory cap counts the firmware carve-out (#50).
- The setup's downloads failed certificate checks on systems without the usual CA file (#102, NixOS).
- Diagnostics: GPU breadcrumbs around the present (#43, #50).

### Tested

From these packages, before publishing:

- The whole setup on Linux (Radeon 660M, RADV) and on Windows 11 (GTX 1050 Ti), with its one-minute warm-up and no
  shader left behind: about 15.5 minutes on Linux (22 with 0.1.7) and 30 on Windows (45), where the shader step went
  from 23 to 14 minutes.

### Known issues

- Ultrawide (#77, #40): during the cup movie the "Skip" prompt is not shown (pressing the button still skips it); in
  two-player split screen the coin and lap numbers sit beside their frames; three and four players are not checked
  yet. 16:9, the default, is not affected.

## 0.1.7

The game starts on AMD graphics cards under Windows, where the window did not open or the title froze.

### Changes

- On AMD's Windows drivers the window did not open after Play, or opened minutes later and froze on the title (#36,
  #38, #54, #20). Some of the game's shaders hold a `switch`, and our translation gave every instruction of them an
  entry point; AMD's compiler took minutes on those (with AMD's own offline compiler, 50 of 81 did not finish in 30 s;
  now none, 84 s for the 81). The translation now takes the switch's own cases. After updating, the setup's shader
  step runs again. Also: a shader the driver does not return holds the game at most 30 s, once, and the start no
  longer waits minutes for the whole prewarm. `GPL=0` in `mkw.ini` is not needed: remove it.
- The window opens black at once instead of after the first image (#36, #20).
- A crash in a race, `no function at guest pc main+0x35bdf8` (#40): the recompiler took an address loaded into a
  register that was overwritten in between, and split a game function in 67 pieces.
- A crash after choosing the character with a 4096 shadow map at 1440p (#48): when that map does not fit, it keeps
  the game's size.
- 2160p in Graphics > Resolution (#55).
- A 64-bit integer compare in 6 shaders was done on 32 bits.
- Diagnostics for graphics-card crashes: the log names the clear, copy or blit after the frame's last pass (#43), and
  `GPU_CRUMBS_DRAW=1` in `mkw.ini` names the shader of each draw (#37, #40, NVIDIA's "instruction pointer invalid").

### Tested

From these packages, before publishing:

- The whole setup on Linux (Radeon 660M, RADV) and on Windows 11 (GTX 1050 Ti), with its one-minute warm-up and no
  shader left behind. We have no AMD card on Windows: the shader fix was checked with AMD's own offline compiler.

## 0.1.6

Graphics cards with more video memory now get more of it for the course's textures.

### Changes

- On a graphics card with lots of video memory the course's ground came out flat and some textures plain (#31): the
  memory for the large tiled textures was capped at 768 MiB on every PC, the figure of the integrated GPU we develop
  on. It now comes from the card: a quarter of its video memory, from 768 MiB to 3 GiB (an 8 GB card gets 1792 MiB;
  uncapped, the title screen took 1472 MiB in our tests). Integrated GPUs keep 768 MiB. `SPARSE_MIB` in `mkw.ini` still sets it by hand.
- `GPU_MEM_LIMIT` in `mkw.ini` on a dedicated graphics card capped the video memory at a small share of it (#28: 6400
  left an 8 GB card 1040 MiB), since it was split with the system memory: it now caps the video memory alone. Without
  it, an integrated GPU on a PC with more than 16 GB of RAM may use a quarter of it instead of 4 GiB.
- `GPU_MEM_LIMIT`, `SPARSE_MIB` and `VRAM_MARGIN` in `mkw.ini` no longer log "unknown key".

### Tested

From these packages, before publishing:

- The whole setup on Linux (Radeon 660M, RADV) and the warm-up step on Windows 11 (GTX 1050 Ti), where the tile memory
  stays at 768 MiB as before (a 4 GB card). The caps for larger cards are checked by the tests on the GPUs players
  reported; we have no card of 6 GB or more to run them on.

## 0.1.5

Local multiplayer, the game no longer runs in slow motion below 60 fps, and the start screen now updates itself.

### Changes

- Local multiplayer with one controller per player (#23): each connected controller is its own player (the first one
  and the keyboard are player 1), the console's controller screen gets the number connected, and rumble goes to each
  player's own controller. Before, Multiplayer went back to the home screen.
- Below 60 fps the race ran in slow motion with Frame rate on Automatic: the game picks 30 or 60 fps from the time the
  GPU reports for each frame, and ours reported the time the work was sent, so it kept choosing 60. It now reports when
  the GPU finishes, and the game stays at 30 (on a Radeon 660M: 110 switches between 30 and 60 in 20 s of race to 0).
- The start screen offers a newer release and installs it (Settings > Menu > Check for updates turns it off).
- "Translate the shaders" failed with `FileNotFoundError` when the game folder's path had a space (#29): the compiler's
  path was split at the space on its way to the shader step.
- The 4096 shadow map (Settings > Graphics) no longer crashes at the kart screen (#9).
- On scaled displays the mouse no longer lands off its place on the start screen (#27).

### Tested

From these packages, before publishing:

- The whole setup on Linux (Radeon 660M, RADV) and on Windows 11 (GTX 1050 Ti), and the update from the start screen on
  both.

## 0.1.4

Fixes the setup that stopped at 359/360 on many Windows PCs, and the shader step failing when a worker runs out of
memory.

### Changes

- "Compile the game code" stopped at 359/360 on Windows (#2, #12): zig, the C compiler in the package, builds its own C
  runtime the first time it links, and on those PCs that build never ended (`setup.log` had no `link probe` line). The
  package now carries that runtime already built and the setup links with it, so zig builds nothing of its own on the
  player's PC, neither for the game library nor for the shader step's helper DLL, where #24 stopped after 360/360.
- The shader step failed with `BrokenProcessPool` when the system ended a worker, usually when memory ran out (#24): it
  now goes on with half the workers, down to one, and `setup.log` says so.

### Tested

From these packages, before publishing:

- The whole setup (generate, compile, translate the shaders, the first run of the game) on Linux (Radeon 660M, RADV)
  with no system tools on PATH, and on Windows 11 (GTX 1050 Ti) with zig's cache emptied first.

## 0.1.3

Fixes two ways the setup could fail or look stuck and a GPU crash on NVIDIA under Linux, keeps a log of the setup, and
makes a GPU crash say where it happened.

### Changes

- "Compile the game code" sat at 359/359 with no sign of life (#2): after the last file the game library is linked,
  and on its first link zig also builds its C runtime for the target (76 s on a 6-thread Windows PC, more on a slow
  disk or with an antivirus scanning). That now runs while the files compile, the link counts in the progress bar,
  and a link that takes over 30 minutes stops with an error instead of waiting forever.
- `unable to find zig installation directory`: zig now gets its `lib/` folder named, so it no longer depends on the
  path a launcher or sandbox gives its executable.
- The setup writes `logs/setup.log`: every tool's output and how long each step took; a failed setup points to it.
- NVIDIA on Linux lost the device as soon as sparse textures were in use (at boot or when a race started): a texture
  was destroyed while a bind of its memory was still queued (#7, thanks to @guillo1990; likely #6).
- When the GPU is lost (#4, #5: AMD discrete cards that went black after the startup logo), `logs/mkw.log` now says
  what the driver knows (the faulting address, `VK_EXT_device_fault`) and which upload batch, render pass or dispatch
  the GPU had started and finished last (`VK_AMD_buffer_marker` marks). Nothing to set: attach the log.

### Tested

From these packages, before publishing:

- The whole setup (generate, compile, translate the 11 561 shaders, the first run of the game) on Linux (Radeon 660M,
  RADV) with no system tools on PATH, and on Windows 11 (GTX 1050 Ti).
- Races on both with the GPU marks on and off (no measurable cost), and #7's fix on AMD (a 60 s race, no GPU
  faults).

## 0.1.2

Fixes the setup, which could not build the game in 0.1.0 and 0.1.1.

### Changes

- It stopped right after "Generate the game code" with `fatal error: 'gamelib.h' file not found` (#1), on Linux and
  Windows.
- Translating the shaders needed `glslc` and `spirv-val` installed on the system.

### Tested

The whole setup (generate, compile, shaders and the first run of the game) from these packages on Linux, with no
system tools on PATH, and on Windows.

## 0.1.1

The Windows zip; its setup builds the shader spec too.

### Changes

- Windows 10/11 x64 package.

### Tested

The start screen on Linux and Windows.

## 0.1.0

First release: the start screen for Linux as an AppImage.

### Changes

- Linux AppImage of the start screen and its setup tools.

### Tested

The start screen on Linux.
