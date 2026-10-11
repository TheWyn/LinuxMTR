# LinMTR design

LinMTR is a native Linux port of WinMTR (https://github.com/WinMTR/WinMTR-Official), written
with Qt 6 Widgets. Like mtr, it combines traceroute and ping and probes every hop to a target
continuously. All probing is unprivileged. LinMTR replicates only the externally visible
behaviour of WinMTR: strings, formats, defaults, column names and order, and semantics. It
contains no Windows code. The literal UI and report strings are copies on purpose.

- Binary: `linmtr`. Window title: `LinMTR v0.1.0 - WinMTR port for Linux`.
- Licence: GPL-2.0-only (rationale in README). Copyright (C) 2026 Wyn.

## 1. Decisions at a glance

| Topic | Decision |
|---|---|
| Probing privilege | Unprivileged ICMP datagram sockets (`SOCK_DGRAM`, `IPPROTO_ICMP[V6]`), which `net.ipv4.ping_group_range` enables on Arch by default. No root, no setcap. |
| Fallback | UDP probes and the `IP_RECVERR` error queue (tracepath method), automatic when ICMP sockets are not available, or when the user selects them. |
| TCP mode | Non-blocking `connect()` probes as in `mtr --tcp` (TTL per socket, `IP_RECVERR`); port 80 by default, configurable. |
| Upstream feature issues | #3 (UDP and TCP probing), #4 (host name and IP display, on by default in the GUI), #10 (copy of the selected hop). |
| IPv6 | Supported (upstream is IPv4 only). `Auto` follows the resolver's address order: IPv6 first on a host with IPv6 connectivity. |
| Threading | None. One GUI thread, non-blocking sockets and the Qt event loop. |
| Probe scheduling | Rounds, as in mtr: one probe per TTL per round, one round every `interval` seconds. Upstream uses one blocking thread per TTL. |
| Loss % | The upstream integer formula `100 - 100*recv/sent`, over resolved probes only (replied and timed out). A probe in flight does not count as lost. |
| Reply timeout | 3.0 s by default, configurable (upstream: fixed 5 s). |
| Defaults | Interval 1.0 s, ping size 64, LRU 128, DNS on, max hops 30 (all upstream). |
| Reports | The upstream layout byte for byte, with `WinMTR statistics` changed to `LinMTR statistics` (same length, so the alignment stays). `\n` line endings (upstream `\r\n`). |
| Settings | QSettings, organization and application `LinMTR` (`~/.config/LinMTR/LinMTR.conf`). |
| Graphs | `HopGraphWidget` draws `HopStats::history`: a live graph of one hop in Host properties, and an optional route graph of all hops under the table. Upstream has no graph. |

## 2. API contract

The GUI talks to the engine only through `TraceSession`, `HopStats` and `TraceOptions`; the
headers are authoritative. `start(target, options)` resolves, then probes continuously (a call
while active calls `stop()` first). `stop()` is a synchronous teardown to `Idle`. `hops()` is a
snapshot of the probed hops in TTL order, trimmed by the upstream `GetMax()` rule (§3.3).
Signals: `stateChanged`, `hopsChanged` (coalesced, §3.3), `roundCompleted(int)`,
`errorOccurred(QString)` (fatal; back to `Idle`) and `infoMessage(QString)` (non-fatal).
`HopStats::history` holds the newest `kHistoryRounds` (3600) probe results of the hop in
ascending round order, for the graphs (§4). The reports do not use it. Each `HopSample` has
the round number, the RTT in ms (a negative RTT means no reply) and the send time of the
probe in ms since the Unix epoch.

## 3. Engine (`src/core/`)

The techniques come from mtr and iputils `tracepath`. All received network bytes are hostile
input: the engine checks the bounds of every access and ignores malformed packets silently.

### 3.1 Sockets and matching

- ICMP (default): one `SOCK_DGRAM` ping socket with `IP_RECVERR`. The kernel fills in the
  echo id and checksum and delivers only the echo replies of the socket; the seq matches the
  probe. If the socket fails with `EACCES`, `EPERM` or `EPROTONOSUPPORT`, the session changes
  to UDP and emits an `infoMessage` that names `net.ipv4.ping_group_range`.
- UDP: the probe for TTL n goes to port `33434 + (n - 1)`, with `max(pingSizeBytes, 8)`
  payload bytes. ICMP answers arrive through the error queue. Routers can quote only the UDP
  header. Thus the destination port gives the TTL, and the receiving socket gives the probe.
  A quoted payload tag (seq, `LM`, TTL) gives an exact match. A port unreachable means target
  reached, also from another address of the target (anycast, multi-homed hosts).
- UDP socket pool: round n uses socket `(n - 1) % N`, `N = ceil(timeout / interval) + 2`, at
  most 128 (the widest GUI and CLI setting, 10 s / 0.1 s, needs 102). Invariant: at most one
  pending probe per TTL per socket, so a port-only match is exact. When a socket still has
  a probe of the TTL in flight, the session skips the probe (no `sent++`) and emits one
  `infoMessage` per trace.

### 3.2 Receive path and answers

Socket notifiers and each pump tick drain the normal queue and the error queue until
`EAGAIN`, at most 1024 messages per call. Qt wakes Read notifiers on `POLLERR`.
- Echo reply: the hop takes the source address, the RTT and `isTarget`.
- Error queue: time exceeded gives an intermediate hop with the offender address and the RTT.
  Destination unreachable gives a WinMTR error string (`classifyIcmp()`) in `errorText` and
  does not count as received, as upstream. A port unreachable for UDP and TCP probes means
  target reached. A local error (`SO_EE_ORIGIN_LOCAL`) makes the probe lost at once.
- A failed `sendto()` counts the probe as sent and timed out, with one `infoMessage` per
  errno. A round in which a send succeeded arms this notice again. When no send of the first
  round succeeds, the session emits `errorOccurred`.

### 3.3 Scheduling and bookkeeping

- A 20 ms pump `QTimer` runs while tracing. A round starts every `intervalSec` (monotonic
  deadline); TTL k goes at `roundStart + (k - 1) * stagger`, with
  `stagger = min(20 ms, intervalSec * 1000 / (2 * maxTtl))`, so that no burst trips the ICMP
  rate limits of routers. After a stall, the next round starts one interval later.
- Pending table: `seq (16-bit rolling counter) -> { ttl, round, slot, sentAt }`, with
  RTT = now - sentAt at drain time, as mtr does. It holds at most 60000 entries, so that the
  seq space does not fill. At the cap, the session skips probes and emits one capacity
  `infoMessage` per trace, as for the TCP cap. An entry older than
  `timeoutSec` leaves the table and adds `timedOut++` to its hop.
- Every resolved probe adds one `HopSample` to its hop: the round, the RTT or a negative
  value for no reply, and the wall-clock send time (`QDateTime::currentMSecsSinceEpoch()`).
  This includes ICMP errors, local errors, timeouts and failed sends.
  `HopStats::addSample()` keeps the newest `kHistoryRounds` samples in round order, also when
  a timeout resolves after a reply of a later round.
- `roundCompleted(n)` fires when every probe of round n has an answer or timed out. With
  `maxRounds > 0`, no round starts after that many; draining continues until the last round
  resolves (report mode sends exactly `--cycles` rounds).
- A hop keeps its latest responder and resolves the name again when the responder changes.
- Trimming (upstream `GetMax()`): `maxTtl` is the lowest TTL whose hop is the target (target
  address or `isTarget`), and probing stops beyond it. Else, trailing hops go while the last
  two have the same address. `hops()` returns only the rows `1..maxTtl` with `sent > 0`.
- `hopsChanged` comes at most once per 100 ms (dirty flag and single-shot timer). Teardown
  stops the timer: an `Idle` session never emits it, and slots of `stateChanged(Idle)` read
  the final `hops()` themselves.
- DNS (`DnsResolver`): asynchronous `QHostInfo` lookups, a cache of at most 4096 names, at
  most 8 lookups at a time. It drops PTR names that are not plausible host names. The cache
  keeps no failed lookup: the hop shows its address, and the next lookup of that address
  (a new responder or a new trace) tries again. Forward resolution: a literal IP skips the
  lookup; `IpVersion::Auto` takes the first address that the lookup returns (`QHostInfo`
  keeps the `getaddrinfo` order, which glibc sorts by RFC 6724, so a host with IPv6
  connectivity gets IPv6 first, as with mtr); `V4` and `V6` filter strictly (no match:
  `errorOccurred`).

### 3.4 TCP backend (`TcpProber`)

- One non-blocking `connect()` to `targetAddress:tcpPort` per probe, on its own socket with
  `IP_RECVERR` and the TTL; the socket identifies the probe. At most 256 sockets are in
  flight; at the cap, the session skips the send and emits one `infoMessage`.
- Target reached: a completed handshake (the prober then resets it), an RST, or an ICMP
  port unreachable from a REJECT firewall. The offender is then the hop address. The prober
  reads the error queue before `SO_ERROR`, so that the offender stays known.
- Karn's rule: after a SYN resend (`tcpi_total_retrans > 0`) the outcome counts as lost, so
  answers slower than about 1 s show as loss. Each probe has a new source port, so per-flow
  ECMP can move hops between rows (README and man state this limitation).

## 4. GUI (`src/gui/`, `src/main.cpp`)

Labels, mnemonics and button texts are verbatim upstream (`WinMTR.rc`).

- Main window: the upstream layout. Row 1: the host box, `&Start` (`&Stop` while a trace
  runs), `&Options`, `E&xit`. Row 2: copy and export. Below: the hop table, the optional
  route graph (see Route graph) and the status bar. While a trace runs, the host box and
  Options stay disabled; copy and export still work. Only `hopsChanged` and `stateChanged`
  refresh the GUI.
- Table cells show integers (`sprintf("%d")`); Loss % has no `%` suffix.
- `Clear History` (IDS_STRING_CLEAR_HISTORY) is always the last combo entry. Selecting it, or
  a start with that text (case-insensitive), empties the history without confirmation, as
  upstream. A command-line host is always a trace target, never this command.
- History: when the trace starts (after resolution), the host goes to the top, without
  duplicates, at most `maxLru` entries. Upstream also waits for `InitMTRNet()` to succeed.
- Hop copy (issue #10): Ctrl+C copies the Hostname text of the selected hop. The shortcut
  applies only to the table, so Ctrl+C in the host box copies text as usual.
- Options dialog: the upstream fields, then `Show IP next to hostname` (issue #4),
  `Show route graph` and the `Probing` group. `OptionsDialog.h` holds the ranges, which the
  settings loader and the CLI also use. New values apply to the next trace, except
  `Resolve names`, `Show IP next to hostname` and `Show route graph`, which apply immediately.
- Host properties dialog: read-only and non-modal. Each double-click opens a new dialog, so
  more than one can be open. The dialog follows its hop by TTL through the `HopTableModel`
  signals, also after Stop and Start. When no row has that TTL, it keeps the last values.
  Below the statistics, a `HopGraphWidget` shows the hop history. §8 lists the deviations.
- Route graph: a `HopGraphWidget` with a legend under the table, in a vertical `QSplitter`.
  It has one line per table row, with the colour `seriesColor(ttl - 1)` from a palette
  of 10 colours. The line of the current row is wider and on top. `Show route graph`
  (`options/showGraph`, off by default) shows the graph; when it is hidden, the table uses
  the full height. `ui/graphSplitter` keeps the splitter state.
- Both graphs: the X axis is a window of the newest rounds, one round for each 3 px of plot
  width (at least 2, at most `kHistoryRounds`). The newest round is at the right edge, and
  older rounds leave at the left edge, so the lines do not compress as the history grows.
  A tooltip shows the visible sample nearest to the pointer, within 6 px: the hop number,
  the host text of the row (`displayHost()`), the send time of the probe
  (`yyyy-MM-dd hh:mm:ss`, local time) and the RTT or `Lost`. The point of a lost probe is
  the middle of its tick.

## 5. Report formats (`ReportBuilder`, shared by GUI and CLI)

The upstream format byte for byte, with `LinMTR statistics` in place of `WinMTR statistics`.

```
|------------------------------------------------------------------------------------------|
|                                      LinMTR statistics                                   |
|                       Host              -   %  | Sent | Recv | Best | Avrg | Wrst | Last |
|------------------------------------------------|------|------|------|------|------|------|
|%40s - %4d | %4d | %4d | %4d | %4d | %4d | %4d |      <- per hop
|________________________________________________|______|______|______|______|______|______|
```

Per-hop fields in order: displayHost, loss %, sent, recv, best, avrg, worst, last. A longer
host text stays complete. The footer `LinMTR v<version> GPL V2 - WinMTR port for Linux`
follows, without a newline. The HTML report keeps the upstream tags (`<center><h2>`,
`<table border="1" align="center">`, `<tr><td>` cells) and escapes host texts.

JSON report (`--json`): WinMTR has none, and LinMTR uses its own schema, not the one of
`mtr --json`. `ReportBuilder::json()` writes a `QJsonDocument` in the indented format and
ends it with one newline. `QJsonObject` sorts the keys in alphabetical order; this is
accepted.

- Top level: `hops` (array) and `linmtr` (object).
- `linmtr`: `version` (string), `target` (string, the host as given), `address` (string, the
  resolved target address; `null` before resolution), `protocol` (`icmp`, `udp` or `tcp`, from
  `TraceSession::options()`, so after a fallback to UDP), `rounds` (int, `roundsCompleted()`).
- Each hop: `ttl` (int), `address`, `hostname` and `error` (string, or `null` when the address
  is null or the text is empty), `lossPercent` (int, `HopStats::lossPercent()`), `sent`,
  `received` and `timedOut` (int), `bestMs`, `avgMs`, `worstMs` and `lastMs` (double,
  milliseconds rounded to 3 decimals; 0 without a reply), `isTarget` (bool).

The host name and the address are separate fields, so `-b` has no effect on the JSON report.
The times have no INT_MAX limit, unlike the integer columns of the text and HTML reports.

## 6. CLI (`src/main.cpp`, `src/cli/ReportRunner`)

- `--help`, `--version`, `--report`, `--html` and `--json` need no display. `--help-all` uses
  an offscreen `QApplication`, because Qt lists the generic options of the application class
  that exists.
- GUI mode: `linmtr HOST` starts a trace of HOST. Options replace saved values for the session.
- Report mode ignores the saved settings. `ReportRunner` sets `maxRounds = cycles`, prints
  the report when `roundCompleted` reaches `cycles`, and quits. A watchdog
  (cycles * interval + timeout + 10 s) prints the partial report and exits with 0.
- `--html` and `--json` select the HTML or the JSON report and imply report mode, as
  `mtr --json` does. `--report` together with one of them is valid: the format applies.
  `--html` with `--json` is a usage error (status 2), as `--udp` with `--tcp`. The text and
  HTML bytes do not change: the text report gets one newline after the footer, and the HTML
  report prints as is (it ends with a newline).
- `-c 0` gives `maxRounds = 0` (unlimited): the trace runs until a signal, without a watchdog.
- Signals: `ReportRunner::start()` catches SIGINT, SIGTERM and SIGHUP through a self-pipe (a
  `socketpair` and a `QSocketNotifier` on its read end). The handler restores the default
  action for all three signals and writes one byte. The runner then prints the report of the
  rounds done so far and exits with 0, as the watchdog does. A signal during resolution
  prints the empty report. A second signal of any of the three kinds ends the process at
  once, also when the report write blocks. The handlers are process-wide, so only one runner
  can exist in a process. Its destructor restores `SIG_DFL` and closes the pair. The GUI
  keeps the Qt defaults.
- Exit codes: 0 success (also after the watchdog or a signal), 1 resolve, socket or send
  failure, 2 bad arguments.

## 7. Packaging and conventions

- The PKGBUILD downloads the `v$pkgver` tag archive from GitHub (`source=()` with a sha256
  checksum), so the AUR and clean-chroot builds work. Its `depends` is the set that
  `namcap` computes (`libgcc` and `libstdc++` as qt6-base declares them).
  `packaging/arch/linmtr-git/` is the VCS variant: it builds `main` and provides and
  conflicts with `linmtr`.
- Qt style, `m_` members, one class per file pair, clean under `-Wall -Wextra`. No blocking
  system calls on the GUI thread. Every fd is `SOCK_NONBLOCK | SOCK_CLOEXEC` and closes in
  exactly one place. `CMakeLists.txt` globs `src/**/*.{cpp,h}`: new files need no CMake edit.
  All sources except `main.cpp` go into the static library `linmtr_lib`, which the binary and
  the QtTest tests in `tests/` link. `ctest` runs the tests, and so does the PKGBUILD `check()`.

## 8. Known deviations from WinMTR

1. Round-based probing (as in mtr) instead of one blocking thread per TTL.
2. Loss % does not count probes that wait for a reply (upstream briefly counts them as lost).
3. Reply timeout 3 s by default, configurable (upstream: fixed 5 s).
4. IPv6, UDP and TCP probing (upstream issue #3, with the TCP comment).
5. Report title `LinMTR statistics`; `\n` line endings.
6. No License and Credits dialog; an about label in Options replaces it.
7. New options: `Max hops`, `Protocol`, `TCP port`, `IP version`, `Reply timeout`,
   `Show IP next to hostname`.
8. Host name and IP display (issue #4) and hop copy (issue #10); upstream has neither.
9. Host properties: real statistics and `No response from host` for a hop without an address
   (upstream: zeros); an ICMP error is the comment, not the Name (upstream: Name).
10. Host history: newest first, without duplicates (upstream appends new hosts at the end).
11. The export dialog suggests `linmtr-<host>.txt` or `.html` from the traced host; upstream
    suggests no name.
12. Host properties is non-modal, updates while the trace runs and shows a graph of the hop
    round-trip times. Upstream shows a static modal snapshot.
13. The route graph under the table and the `Show route graph` option are new; upstream has
    no graph.
