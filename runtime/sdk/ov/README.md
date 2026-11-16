# runtime/sdk/ov

Replacements of `nn::*` functions by name: system, input, audio, video, graphics, network, time and touch.

| File | What it is |
|---|---|
| `ov_system.c` | Accounts, applets, time, system fonts, etc. |
| `ov_input.c`, `ov_media.c`, `ov_gfx.c`, `ov_zstd.c` | Input, Opus, the SDK's graphics (`nv::InitializeGraphics(0, 0)` is valid on Switch 2; `nv::InitCheck`), native zstd (its outputs go to `core/hacks/classic.c`, which patches the cups' archive) |
| `ov_codec.c` | MKW's video: the `nn::codec` video API (VP9 with libvpx, decoded in `Submit`) and the `nn::syncpt` events as the game's `SystemEvent` |
| `ov.h` | Header shared by the replacements and their test (`ov_probe.c`) |
| `ov_touch.c` | MKW's own: touch screen off (optional), reads without touches |
| `ov_oe2.c` | MKW's own: the `nn::oe` that totk-pc-recomp does not replace (version 1.0.0, graphics scale, notifications) |
| `ov_chat.c` | MKW 1.8.0's `nn::chat` (GameChat): a console where no chat is started; the room-info event is real and never signalled |
| `ov_net.c` | MKW's own: offline network (`nifm`, `friends`, `ldn`, `nsd`, `prepo`); with `MKW_LAN=1`, `nifm`'s local network mode (the LAN lobby) is connected with the host's address |
| `ov_lanaddr.c` | MKW's own: the address and mask `nifm` reports in LAN mode, chosen from the host's interfaces (or `MKW_LAN_IP`) |
| `ov_socket.c` | MKW's own: `nn::socket` as main's network-down stub by default; with `MKW_LAN=1`, over the host's sockets (datagrams, poll, eventfd), with the console's FreeBSD sockaddr, options and flags: the base of LAN play. Bind and SendTo take local network addresses only (private, link-local, multicast, broadcast), so LAN play cannot reach the internet. LAN tested on Linux only; on Windows it compiles, with ICMP resets off on datagram sockets and no eventfd (only gRPC, online, calls it) |
| `ov_time2.c` | MKW's own: the `nn::time` that totk-pc-recomp does not replace (exact network clock, host time zone, instant clock correction) |
