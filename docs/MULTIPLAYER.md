# Online multiplayer

**Build105 / 1.0.5 development source** supports public Internet server browsing
and joining for retail Halo PC 1.10, with working multiplayer Start menus.
Build the current source using the
[setup guide](SETUP.md) or [manual build guide](BUILDING.md). The published
v1.0.3 / build 103 download does not contain these networking changes.

## Join a public game

1. Connect the Vision Pro to the Internet and start the new build with your
   registered retail Halo PC installation and a paired controller.
2. Open **Multiplayer → Join Game → Internet**. If focus rests on the
   "Join Game" heading, move down to select Internet.
3. Choose **Get List**, wait for the server rows to populate, select a server
   with a free slot, and choose **Join Game**.
4. Enter the server's password if it requires one. Choose an open server if
   you do not have its password.

You do not need a friend's server address to use the public browser. Server
availability, player counts and rules change. The message-of-the-day panel
can remain loading while the list and joining work.

The **Direct IP** option can join a known retail PC 1.10 server by its address
and port. Use the host's actual game port; it may differ from its query port.
For a local server, allow Local Network access if visionOS requests it.

Your own valid game registration remains required. Custom Edition, MCC,
Anniversary and Xbox multiplayer are not supported by this port. There is
no matchmaking account, relay service or automatic router configuration in
Master Chef.

## In-game menu

Press **Start/Options** to open the menu. Use the D-pad or left stick to
navigate, **A/Cross** to select and **B/Circle** to go back. Select **Resume
Game** to return or **Leave Game** to disconnect. Pointer/pinch selection
uses the same menu route. Multiplayer continues running while the menu is open.

Build105 fixes Build104 treating this non-pausing menu as gameplay, which
prevented navigation and selection. Update to Build105 if joining works but
the Start menu ignores your input.

## Test status

The Mac runtime retrieved 71 public servers and joined an existing public
Timberland CTF server on October 4, 2026. Movement and firing worked in that
session. Direct-IP joining also passed against an unmodified local dedicated
server. Build105 also passed Start-menu navigation, opening/backing out of
settings, pointer-click resume and Leave Game against that local server.
The full visionOS Release app compiled with its restored layered app icon.

Build104 was installed and startup-verified on a Vision Pro; its owner
reported successful multiplayer joining and the menu failure addressed in
Build105. Build105 is installed, and its device-rendered icon was checked.
**Build105 headset menu acceptance remains pending.** Desktop smoke
tests do not establish headset performance, combat with other players,
long-session stability, or hosting across different networks. LAN broadcast discovery is
also unqualified; the app does not request Apple's multicast entitlement.
See [validation details](VALIDATION.md#build105-multiplayer-menus-and-app-icon).

## Troubleshooting

- **No servers:** confirm Internet access, select Get List again, and check
  that filters are not excluding the available games. If you have a known
  server address, try Direct IP to distinguish listing from connectivity.
- **One server fails to join:** try another open retail PC 1.10 server. A
  full server, password, version mismatch, registration rejection or server
  policy can reject a connection. The port preserves those checks.
- **A LAN address fails:** allow Local Network access in Settings, verify
  the host's game port and firewall, and try Direct IP. Public Internet
  browsing does not establish LAN broadcast support.
- **Testing client and server on one Mac:** give them distinct UDP ports.
  The dedicated server may also bind a query socket. For example, the
  development smoke test used server game port 2399 and client arguments
  `-port 2400 -cport 2401 -connect 127.0.0.1:2399`.
- **Stuck message of the day:** continue with Get List. The legacy MOTD
  service is separate from the working server browser.

When reporting a failure, include the app build, connection stage, server
edition, whether it also occurs on another server, and the exact on-screen
error. Review diagnostics before sharing; omit registration, device IDs,
private addresses and signing information.

## Implementation and regression checks

`native/EngineHost/winsock.c` translates the synchronous IPv4 Winsock calls
used by Halo to native sockets, including UDP, TCP, DNS, nonblocking I/O,
select, error codes and the different WSOCK32/WS2_32 ordinal layouts. It
owns native descriptors behind guest handles and releases them at shutdown.
This is a game-specific compatibility layer, not a complete Winsock library;
overlapped I/O, IPv6 and the full Windows socket-option surface are absent.
UDP FIONREAD reports the next datagram's payload size on Darwin.

The Internet menu's obsolete Windows patch lookup is replaced by the
original game's no-update result. This port requires a hash-verified 1.10
executable and updates through native app builds. The original browser,
join protocol, registration and server checks continue to run.

Menu detection reads the original engine's active widget root as well as its
front-end and pause state. Multiplayer menus can own input without pausing
simulation. `test_dinput_lifecycle.c` checks that distinction, gameplay input
restoration, preserved pointer clicks and suppression of gameplay joystick
actions while a menu is active.

`test_winsock.c` exercises the guest ABI against real native loopback peers.
`test_multiplayer_update.c` checks the native return and, when generated
engine code is available locally, compares its memory effects with the
original no-update callback. Both run through
`python3 tools/run_source_checks.py --portable-only`; add `--sanitize` for
ASan/UBSan. Game code and registrations are not needed for the source-only
cases and are never included in these tests.
