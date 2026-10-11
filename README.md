# LinMTR

LinMTR is a native Linux port of [WinMTR](https://github.com/WinMTR/WinMTR-Official),
with a Qt 6 GUI. Like WinMTR and [mtr](https://github.com/traviscross/mtr), it combines
traceroute and ping in one tool. It probes every hop on the path to a host continuously
and shows per-hop packet loss and round-trip times in a live table.

The window copies the WinMTR layout, labels, defaults and report formats closely. All
probing is unprivileged: LinMTR needs no root, no `sudo` and no `setcap`.

Binary `linmtr` · licence GPL-2.0-only

## Features

- Live hop table with the WinMTR columns: `Hostname, Nr, Loss %, Sent, Recv, Best, Avrg, Worst, Last`.
- Text and HTML reports in the WinMTR format. You can copy them to the clipboard or export
  them to a file, also while a trace runs.
- Host history (most recently used list) in the host box.
- The WinMTR **Options** dialog (interval, ping size, LRU size, name resolution). It adds
  **Show IP next to hostname** and a **Probing** group: protocol, TCP port, IP version,
  reply timeout and max hops.
- Host name and IP address together, for example `one.one.one.one (1.1.1.1)`, in the
  table and in the reports (on by default in the GUI; `-b` on the command line).
- Copy one hop: **Ctrl+C** in the hop table, or the right-click menu (**Copy hostname**,
  **Copy IP address**).
- **Host properties** dialog: double-click a hop.
- ICMP echo probes through unprivileged ping sockets, with automatic fallback to UDP probes.
  UDP and TCP probes are also available on request.
- IPv4 and IPv6 (WinMTR supports IPv4 only).
- Headless `--report` mode. It prints the text, HTML or JSON report to stdout and needs no
  display.

## How probing works without root

Classic traceroute tools need raw sockets, so they need root or `CAP_NET_RAW`. LinMTR
uses two Linux socket features that unprivileged users can open instead. The
unprivileged `ping` from iputils uses the first one, and iputils `tracepath` uses the
second one.

### ICMP ping sockets (default)

LinMTR opens an ICMP *datagram* socket (`socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP)`, and
the `IPPROTO_ICMPV6` equivalent for IPv6). The kernel fills in the ICMP identifier and
checksum, and it delivers only the echo replies to the requests of LinMTR. For each probe,
LinMTR sets the TTL (hop limit) with `setsockopt()` before it sends. The socket also has
`IP_RECVERR` set, so "time exceeded" and "destination unreachable" messages from routers
arrive on the error queue of the socket. Each message has the address of the router and a
copy of the original ICMP header. LinMTR matches it to the probe by sequence number.

The kernel permits ping sockets only for users whose group ID is in the range of the
`net.ipv4.ping_group_range` sysctl. The same sysctl also controls ICMPv6 ping sockets.
The kernel default (`1 0`) permits nobody. systemd changes it to `0 2147483647`
(all groups) in `/usr/lib/sysctl.d/50-default.conf`. Thus it works without changes on
Arch Linux and most other systemd distributions. To examine the setting:

```sh
sysctl net.ipv4.ping_group_range
```

To enable it on a system where it is off (as root):

```sh
sysctl -w net.ipv4.ping_group_range="0 2147483647"                         # until reboot
echo 'net.ipv4.ping_group_range = 0 2147483647' > /etc/sysctl.d/60-ping.conf  # persistent
```

### UDP fallback

If LinMTR cannot open an ICMP socket (`EACCES`, `EPERM` or `EPROTONOSUPPORT`), it changes
to UDP probes automatically and shows a message that refers to `net.ipv4.ping_group_range`.
You can also select UDP with `--udp` or with **Options → Probing → Protocol**.

In UDP mode, an ordinary UDP socket with `IP_RECVERR` sends datagrams to port
`33434 + (TTL - 1)`. The kernel queues the ICMP replies from routers on the error queue
of the socket. A router quotes only a short part of the original datagram. Thus LinMTR
matches each reply by the original destination port and by the socket it arrived on.

Each round uses the next socket of a pool. The pool size makes sure that a hop uses a
socket again only after the earlier probe there timed out. Thus a reply always belongs
to exactly one probe, and its round-trip time is correct. The pool has at most 128
sockets, which covers all settings of the GUI and the command line.

When the target replies "port unreachable", the trace has reached it. A firewall that
drops these datagrams, or the ICMP replies, can prevent detection of the target. In that
case, ICMP mode usually gives better results.

### TCP probes

Select TCP with `--tcp` or with **Options → Probing → Protocol**. The destination port
is 80 by default; change it with `-P`/`--port` or **TCP port**. TCP probes follow the
connect method of `mtr --tcp`, and they can get through firewalls that drop ICMP or UDP.

Each probe is a non-blocking `connect()` on its own ordinary TCP socket, with the TTL set
and `IP_RECVERR` on. Routers on the path reply with ICMP "time exceeded", which the
kernel puts into the error queue of the socket. The trace has reached the target when the
target accepts the connection or refuses it with a TCP reset.

As in UDP mode, an ICMP "port unreachable" also counts as reaching the target. It means
that a firewall at the target, or in front of it, rejected the connection request. The
hop then shows the address that sent the ICMP message. Other "destination unreachable"
messages show as an error on the hop. LinMTR closes each socket when its probe has a
result or times out. TCP probes carry no payload, so **Ping size** and `-s` have no
effect on them.

When a connection request has no answer after about 1 second, the kernel sends it
again. LinMTR counts an answer that arrives after this resend as lost: it cannot know
which of the two requests the answer is for (Karn's rule). Thus in TCP mode, a reply
that is slower than about 1 second shows as loss, not as a long round-trip time.

Each probe is a new connection with a new source port. Routers that balance traffic over
parallel paths by flow (ECMP) can thus send the probes for one TTL along different paths.
A hop can move to a different row from round to round. The target row can show fewer
**Sent** probes than the number of rounds. `mtr --tcp` has the same limitation.

## Building

Requirements:

- Linux (LinMTR uses Linux-specific socket extensions)
- CMake 3.21 or later
- Qt 6.4 or later (Widgets and Network modules)
- A C++20 compiler (GCC or Clang)

On Arch Linux: `sudo pacman -S --needed base-devel cmake qt6-base`

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/linmtr
```

To run the tests, use `ctest --test-dir build`.

To install system-wide without a package (the prefix is `/usr/local` by default):

```sh
sudo cmake --install build
```

This installs the `linmtr` binary, the desktop entry, the icon and the man page.

## Installing on Arch Linux (PKGBUILD)

The PKGBUILD in `packaging/arch/linmtr/` downloads the tagged release archive from
GitHub, builds it, runs the tests and packages the result. The `linmtr-git` PKGBUILD in
`packaging/arch/linmtr-git/` builds the latest `main` commit instead; it provides and
conflicts with `linmtr`.

```sh
cd packaging/arch/linmtr && makepkg -si
```

The package installs:

| File | Path |
|---|---|
| Binary | `/usr/bin/linmtr` |
| Desktop entry | `/usr/share/applications/linmtr.desktop` |
| Icon | `/usr/share/icons/hicolor/scalable/apps/linmtr.svg` |
| Man page | `/usr/share/man/man1/linmtr.1.gz` |

To remove it: `sudo pacman -R linmtr`. To delete the makepkg work directories next to
the PKGBUILD, use `makepkg -c` or `makepkg -C`.

The package depends on `qt6-base`, `glibc`, `libgcc`, `libstdc++` and
`hicolor-icon-theme` (which owns the icon directories).

## Usage

### GUI

Start **LinMTR** from the application menu, or run `linmtr`. Type a host name or IP
address and push **Start** (or press Enter). Push **Stop** to end the trace.
Double-click a hop to see its details. The status bar shows `Ready`,
`Resolving host <host>...`, `Tracing <host>` or the last error.

`linmtr HOST` opens the window with `HOST` in the host box and starts the trace
immediately.

The host box keeps a history of recent hosts, the newest first. A host goes into the
history when its trace starts, that is, after its name resolved. A host name that does not
resolve does not go into the history.

The last entry of the list is always **Clear History**. Select it to delete the history;
LinMTR does not ask for confirmation. The text `Clear History` (in any case) in the host
box also deletes the history when you push **Start** or press Enter. LinMTR then starts no
trace. A `HOST` given on the command line is always a host to trace, also when it is
`Clear History`.

To copy one hop, select its row and press **Ctrl+C**. This copies the text of the
**Hostname** column. Right-click a row for a menu with **Copy hostname** (the same text)
and **Copy IP address**. **Copy IP address** is not available for a hop without an
address. Ctrl+C copies a hop only while the table has the keyboard focus. In the host box,
it copies the selected text as usual.

Changes in **Options** apply to the next trace (as in WinMTR). **Resolve names** and
**Show IP next to hostname** are exceptions: they apply immediately to the table and to
the reports.

### Command line

`linmtr [options] [HOST]`. `linmtr --help` lists the options with their ranges, and
`man linmtr` describes each one.

| Option | Effect |
|---|---|
| `--report` | Headless report mode: run `-c` rounds, print the report (text by default) to stdout, exit |
| `--html` | Report mode with the HTML report, as **Export HTML** |
| `--json` | Report mode with a JSON report |
| `-c`, `--cycles N` | Rounds in report mode (default 10; `0` = until a signal stops the trace) |
| `-n`, `--no-dns` | No reverse DNS |
| `-b`, `--show-ips` | Host name and IP address for each hop, as `mtr -b` |
| `-4`, `-6` | IPv4 only, IPv6 only (default: the address that the resolver lists first) |
| `--udp`, `--tcp` | UDP or TCP probes instead of ICMP |
| `-P`, `--port N` | TCP destination port (default 80) |
| `-s`, `-i`, `--timeout`, `-m` | Ping size, interval, reply timeout and max hops, as in **Options** |

In report mode, LinMTR sends exactly `--cycles` rounds and waits until each probe has a
reply or has timed out. A hop that LinMTR probes in every round shows `Sent` equal to
`--cycles`, and no probe is still in flight when the report prints. Report mode
ignores the saved GUI settings, so host names show without IP addresses unless you
give `-b`. In GUI mode, `-n`, `-b`, `-4`, `-6`, `--udp`, `--tcp`, `-P`, `-s`, `-i`,
`--timeout` and `-m` replace the saved **Options** values for that session only.

`--html` and `--json` each select report mode, also without `--report`. With `--html`,
LinMTR writes the HTML report in the format of **Export HTML**. With `--json`, it writes a
JSON object with two keys: `linmtr` (version, target, target address, protocol and completed
rounds) and `hops` (one object per hop, with times in milliseconds). `DESIGN.md` gives the
full schema. You cannot give both options.

With `-c 0`, the trace in report mode continues until LinMTR gets a signal. On SIGINT
(Ctrl+C), SIGTERM or SIGHUP, LinMTR writes the report for the rounds done so far and exits
with status 0. A signal during a `-c N` trace has the same effect. A second signal ends
the process at once.

Examples:

```sh
linmtr example.com                              # GUI, start tracing at once
linmtr --report example.com                     # 10 rounds, text report to stdout
linmtr --report -c 30 -n example.com            # 30 rounds, no reverse DNS
linmtr --report -6 --udp example.com > hops.txt # IPv6, UDP probes, save the report
linmtr --report --tcp -P 443 -b example.com     # TCP to port 443, name and IP per hop
linmtr --report -c 0 example.com > hops.txt     # until Ctrl+C
linmtr --json -c 20 example.com | jq '.hops[-1]' # JSON report, last hop only
linmtr --html example.com > hops.html           # HTML report to a file
```

Exit status in report mode: `0` success (also when a signal stops the trace), `1` trace
failure, `2` bad arguments. `man linmtr` gives the full reference after you install the
package.

### Configuration file

LinMTR saves its settings with `QSettings` in `~/.config/LinMTR/LinMTR.conf`
(`$XDG_CONFIG_HOME/LinMTR/LinMTR.conf` if `XDG_CONFIG_HOME` is set):

| Key | Meaning |
|---|---|
| `options/interval`, `options/pingSize`, `options/maxLru`, `options/resolveNames` | WinMTR options |
| `options/showIps` | **Show IP next to hostname** (`true` if the key is missing) |
| `options/timeout`, `options/maxHops`, `options/ipVersion` | LinMTR probing options |
| `options/protocol` | `icmp`, `udp` or `tcp` |
| `options/tcpPort` | TCP probe port (`80` if the key is missing) |
| `ui/geometry` | window size and position |
| `lru/hosts` | host history |

Delete the file to restore the defaults.

## Differences from WinMTR

LinMTR replicates the visible behaviour of WinMTR: labels, mnemonics, defaults, column
order and report layout. Its internals are new and designed for Linux.

| Topic | LinMTR | WinMTR |
|---|---|---|
| Privileges | Unprivileged ICMP datagram sockets (`net.ipv4.ping_group_range`); no root, no setcap | Windows ICMP API (`IcmpSendEcho`) |
| Fallback | UDP probes with the `IP_RECVERR` error queue (tracepath method), automatic or user-selected | None |
| Protocols | ICMP, UDP and TCP (`connect()` probes, port 80 by default) | ICMP only |
| Host display | Name, IP address, or both (`name (IP)`, on by default) | Name or IP address |
| Copy one hop | Ctrl+C in the table, right-click menu | None |
| IPv6 | Supported. **IP version** `Auto` takes the address that the resolver lists first: a dual-stack name traces over IPv6 when this computer has IPv6 connectivity, as with mtr | IPv4 only |
| Threading | One GUI thread, non-blocking sockets in the Qt event loop | One blocking thread per TTL |
| Probe scheduling | Rounds, as in mtr: one probe per TTL per round, one round every `interval` seconds | Each TTL thread pings in a loop |
| Loss % | The WinMTR integer formula `100 - 100*recv/sent`, but only over probes that got a reply or timed out | Same formula; probes still in flight count as lost |
| Reply timeout | 3.0 s by default, configurable | Fixed 5 s |
| Defaults | Interval 1.0 s, ping size 64, LRU 128, DNS on, max hops 30 | Same |
| New options | **Max hops**, **Protocol**, **TCP port**, **IP version**, **Reply timeout**, **Show IP next to hostname** | None |
| About | An about label in **Options** | **License and Credits** dialog |
| Host properties | A hop without an address shows its real statistics and `No response from host`. For an ICMP error, the error text is the comment and **Name** shows the host name or address. | Zeros and an empty comment; the error text is **Name** and the comment is `Host alive.` |
| Host history | Newest first; a host that you trace again moves to the top. A host goes in only after its name resolved, as in WinMTR. | New hosts at the end, no reordering |
| Reports | Same layout; title `LinMTR statistics` (same length as `WinMTR statistics`, so columns align); `\n` line endings | `\r\n` line endings |
| Settings | `~/.config/LinMTR/LinMTR.conf` | Windows registry |

## Upstream feature requests implemented

- [#3 Feature Request: UDP support](https://github.com/WinMTR/WinMTR-Official/issues/3)
  (and TCP, from a comment): `--udp`, `--tcp`, `-P`; **Protocol** and **TCP port**.
- [#4 Feature: Show hostname & IP](https://github.com/WinMTR/WinMTR-Official/issues/4):
  `-b`/`--show-ips`; **Show IP next to hostname**.
- [#10 Feature: Ability to copy selected hostname](https://github.com/WinMTR/WinMTR-Official/issues/10):
  **Ctrl+C**, **Copy hostname**, **Copy IP address**.

## Troubleshooting

- **A message mentions `net.ipv4.ping_group_range` and LinMTR uses UDP.** Refer to
  [ICMP ping sockets](#icmp-ping-sockets-default).
- **Many hops show `No response from host`.** Some routers do not send ICMP "time
  exceeded" messages, or they rate-limit them. This is normal for single hops. If every
  hop is silent, a local firewall possibly drops the ICMP replies.
- **The trace never reaches the target in UDP mode.** The target, or a firewall in front of it,
  possibly drops the UDP datagrams or does not send "port unreachable". Use ICMP mode.
- **The trace never reaches the target in TCP mode.** A firewall possibly drops the connection
  requests to that port without a reply. Try a port that the target serves, for example
  `-P 443` for a web server.

## Project layout

| Path | Contents |
|---|---|
| `src/core/` | Probing engine: `TraceSession`, ICMP, UDP and TCP probers, asynchronous reverse DNS |
| `src/gui/` | Qt Widgets main window, table model, dialogs, report builder |
| `src/cli/` | Headless `--report` runner |
| `tests/` | QtTest unit tests and a loopback trace test |
| `packaging/` | Arch PKGBUILD, desktop entry, icon, man page |
| `DESIGN.md` | Architecture reference |

## Credits

- **WinMTR**: Vasile Laurentiu Stanimir started it in 2000 as a Windows clone of mtr.
  Appnor MSP released it under the GPL v2, and Dragos Manac maintains it.
  Website [winmtr.net](https://winmtr.net), source
  [WinMTR/WinMTR-Official](https://github.com/WinMTR/WinMTR-Official).
  The UI strings, defaults and report formats of LinMTR come from WinMTR.
- **mtr** (Matt's traceroute) by Matt Kimball and Roger Wolff. WinMTR is a clone of mtr,
  and the round-based scheduling of LinMTR follows mtr.
- **tracepath** and **ping** from [iputils](https://github.com/iputils/iputils). The UDP
  and `IP_RECVERR` probing method comes from tracepath. iputils `ping` is the reference
  user of unprivileged ICMP ping sockets.
- **[Qt](https://www.qt.io)** supplies the GUI toolkit, event loop and DNS lookups.

## Licence

Copyright (C) 2026 Wyn

This program is free software; you can redistribute it and/or modify it under the terms
of the GNU General Public License version 2 as published by the Free Software Foundation
(`GPL-2.0-only`).

This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
See the GNU General Public License for more details.

The full text of the GPL v2 is in [`LICENSE`](LICENSE). This notice applies to all files
in this source tree, also to files that do not have their own notice.
