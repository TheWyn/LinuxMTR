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
| RTT | From the kernel, so that a busy GUI thread does not make it larger: the arrival stamp (`SO_TIMESTAMPNS`), or `tcpi_rtt` for a TCP handshake; else the RTT at the read (§3.2, §3.4). Upstream reads `RoundTripTime` from `IcmpSendEcho`. |
| Probe scheduling | Rounds, as in mtr: one probe per TTL per round, one round every `interval` seconds. Upstream uses one blocking thread per TTL. |
| Loss % | The upstream integer formula `100 - 100*recv/sent`, over resolved probes only (replied and timed out). A probe in flight does not count as lost. Host properties shows the loss with one decimal, rounded up (§4). |
| Jitter | Only in Host properties: the mean change of the RTT from one reply to the next, over the whole trace (§3.3). The table and the reports have no jitter, as upstream. |
| Reply timeout | 3.0 s by default, configurable (upstream: fixed 5 s). |
| Defaults | Interval 1.0 s, ping size 64, LRU 128, DNS on, max hops 30 (all upstream). |
| Reports | The upstream layout byte for byte, with `WinMTR statistics` changed to `LinMTR statistics` (same length, so the alignment stays). `\n` line endings (upstream `\r\n`). Report mode adds a JSON report with a LinMTR schema (§5). |
| Settings | QSettings, organization and application `LinMTR` (`~/.config/LinMTR/LinMTR.conf`). |
| Graphs | `HopGraphWidget` draws `HopStats::history`: a live graph of one hop in Host properties, and an optional route graph of all hops under the table. Upstream has no graph. |

## 2. API contract

The GUI talks to the engine only through `TraceSession`, `HopStats` and `TraceOptions`; the
headers are authoritative. `start(target, options)` resolves, then probes continuously (a call
while active calls `stop()` first). `stop()` is a synchronous teardown to `Idle`. `hops()` is a
snapshot of the probed hops in TTL order, trimmed by the upstream `GetMax()` rule (§3.3).
Signals: `stateChanged`, `hopsChanged` (coalesced, §3.3), `roundCompleted(int)`,
`errorOccurred(QString)` (fatal; back to `Idle`) and `infoMessage(QString)` (non-fatal).
Read access: `state()` (`Idle`, `Resolving`, `Tracing`), `target()`, `targetAddress()` (valid
after resolution), `options()` (the sanitized options; the protocol after a UDP fallback),
`roundsCompleted()` and `lastError()`. `HopStats` adds `lossPercent()`, `averageMs()`,
`jitterMs()` (§3.3), `displayHost(resolveNames, showIps)` and `addSample()`. `TraceOptions`
has `intervalSec`, `pingSizeBytes`, `timeoutSec`, `maxHops`, `resolveNames`, `protocol`,
`ipVersion`, `tcpPort` and `maxRounds` (0 = unlimited).
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
  pending probe per TTL per socket. When a socket still has a probe of the TTL in flight, the
  session skips the probe (no `sent++`) and emits one `infoMessage` per trace. With the GUI
  and CLI settings, a socket sends the next probe of a TTL more than `timeout` after the
  earlier one. Thus the session skips no probe.
- Port-only match: an answer without the payload tag goes to the pending probe of its TTL on
  its socket. IPv4 routers that quote only the UDP header (28 bytes with the IP header, as
  RFC 792 asks) send such answers. A UDP service on the target also sends them. An ICMPv6
  error has up to 1280 bytes (RFC 4443), so it normally has the tag. A Linux ICMP error has
  up to 576 bytes. Thus a Linux router, and this host after a failed ARP resolution, quote
  the payload, and the tag gives an exact match. The match by port is exact for an answer
  that arrives within the reply timeout. Limitation: a socket sends the next probe of a TTL
  about N intervals after the earlier one. An answer without the tag that arrives later goes
  to the newer probe, if that probe still waits. A time exceeded or a port unreachable then
  counts as the reply of the newer probe, with a wrong RTT. The row can thus show too little
  loss. An ICMP error makes the newer probe lost, with that error text. The answer of the
  newer probe then finds no probe, and the session ignores it. Thus a wrong match needs an
  answer without the tag that arrives about N intervals late. LinMTR accepts this limitation.
  The error queue gives the UDP payload without the UDP header, so the quoted UDP checksum
  (different for each probe) is not available. The arrival stamp cannot tell a late answer
  from an answer to the newer probe, and a larger pool only makes the necessary delay longer.

### 3.2 Receive path and answers

Socket notifiers and each pump tick drain the normal queue and the error queue until
`EAGAIN`, at most 1024 messages per call. Qt wakes Read notifiers on `POLLERR`.
- Echo reply: the hop takes the source address, the RTT and `isTarget`.
- Error queue: time exceeded gives an intermediate hop with the offender address and the RTT.
  Destination unreachable gives a WinMTR error string (`classifyIcmp()`) in `errorText` and
  does not count as received, as upstream. A port unreachable for UDP and TCP probes means
  target reached. A local error (`SO_EE_ORIGIN_LOCAL`) makes the probe lost at once.
- An answer that gives `errorText` does not change the hop address or `isTarget`, as upstream
  (`SetName()` only). A router can send the error for each TTL up to `maxHops`. Then each of
  these rows shows the error text, as upstream. With the address of the router on these rows,
  the trimming (§3.3) stops the probes at that router until the trace ends. Exception: an
  error from the target address makes the hop the target hop (§8 item 15). Each reply (echo
  reply, time exceeded, target reached) clears `errorText` (§8 item 17).
- A failed `sendto()` counts the probe as sent and timed out, with one `infoMessage` per
  errno. A round in which a send succeeded arms this notice again. When no send of the first
  round succeeds, the session emits `errorOccurred`.
- RTT: the RTT ends at the time when the kernel received the answer, not at the read. Every
  probe socket has `SO_TIMESTAMPNS`. Thus each `recvmsg()`, from the normal queue or the error
  queue, also gives the arrival stamp (`CLOCK_REALTIME`, `decodeArrivalStamp()`). The session
  converts the stamp when it reads the answer (`replyRttNs()`): RTT = (now - sentAtNs) - (wall
  clock - stamp). It reads the wall clock first, so a delay between the two clock reads makes
  the RTT larger, not smaller. The kernel turns the stamps on for the whole host, not for one
  socket. Thus, while a trace runs, the kernel stamps every packet that the host receives.
- RTT at the read: an answer without a kernel value (a TCP reset, an answer without a stamp)
  gets RTT = now - sentAtNs. A kernel value that is not in (0, now - sentAtNs] also gives the
  RTT at the read. Only a wall-clock step or a suspend between the arrival and the read makes
  the converted value wrong. The wait is the time from the arrival to the read. A backward
  step longer than the wait, and a forward step or a suspend at least as long as the RTT, give
  a value out of range. A shorter backward step makes the RTT larger by the step, but not
  larger than the RTT at the read. A shorter forward step or suspend makes the RTT smaller by
  its length. The kernel turns its stamps on with a delay when the first socket of the host
  asks for them. An answer that arrives before that gets the read time as its stamp, thus
  about the RTT at the read.
- Late answers: each pump tick reads the sockets before the sweep (§3.3). Thus an answer
  that came in time counts, also when it then waited for this thread. A kernel value at or
  above `timeoutSec` shows an answer that came late. The session ignores such an answer, and
  the sweep expires the probe, as for an answer after the sweep. Thus an RTT from the kernel
  is always less than the reply timeout. This applies to all kinds of answers. A late ICMP
  error does not set `errorText`, and a late error from the target does not make its hop the
  target hop. Upstream also ignores late answers (`IcmpSendEcho` returns no reply after its
  timeout). An answer without a kernel value (a TCP reset) counts when the session reads it
  before the sweep. The read can come up to one pump period (20 ms) after the timeout, or
  later while the thread is busy. Its RTT at the read can include a wait for this thread.
  Thus a check on that value could make an answer that came in time a loss. Two rare cases
  make an answer that came in time late. Both need an answer that waits for this thread
  until after the timeout. In the first, the kernel took the stamp at the read (see above).
  In the second, the wall clock steps back during the wait.

### 3.3 Scheduling and bookkeeping

- A 20 ms pump `QTimer` runs while tracing. A round starts every `intervalSec` (monotonic
  deadline). TTL k is due at `roundStart + (k - 1) * stagger`, with
  `stagger = min(20 ms, intervalSec * 1000 / (2 * maxTtl))` and `maxTtl` of the trimming
  (below) at the start of the round. Each pump tick sends all probes that are due. Thus with
  a stagger below 20 ms, the probes go in groups of about `20 ms / stagger`
  (`40 ms * maxTtl / intervalSec`). At `-i 0.1` with 30 hops, a round goes as 1, 12, 12 and
  5 probes (64 hops: 1, 25, 26 and 12). At `-i 1` with 30 hops, one or two probes go per
  tick (64 hops: two or three).
- The groups do not change the number of probes that a router gets in a round: each probe
  of a round expires at another router. Only a host that answers for more than one TTL gets
  more than one probe of a round. These hosts are the target before the trimming stops at
  it, and a router that sends an ICMP error for each TTL after it. Their rate limits count
  per second (Linux, IPv4: 6 at once, then 1 per second for each destination; IPv6 allows
  more). The spread is shorter than half an interval, so it has almost no effect on these
  limits. The stagger lets the trimming stop a round soon after the target first answers. It
  also keeps the groups small, as mtr spreads its probes over the interval.
- After a stall, the session sends the rest of the earlier round at once and starts the due
  round. The next round starts one interval later: the session does not send the rounds that
  it missed.
- Pending table: `seq (16-bit rolling counter) -> { ttl, round, slot, sentAtNs, sentAtMs }`.
  `sentAtNs` is the monotonic send time for the RTT (§3.2) and the timeout; `sentAtMs` is the
  wall-clock send time for the `HopSample`. It holds at most 60000 entries, so that the seq
  space does not fill. At the cap, the session skips probes and emits one capacity
  `infoMessage` per trace, as for the TCP cap. At each pump tick, the sweep
  (`sweepTimeouts()`) runs after the read of the sockets (§3.2). It removes each entry that
  is `timeoutSec` or older and adds `timedOut++` to its hop.
- Every resolved probe adds one `HopSample` to its hop: the round, the RTT or a negative
  value for no reply, and the wall-clock send time (`QDateTime::currentMSecsSinceEpoch()`).
  This includes ICMP errors, local errors, timeouts and failed sends.
  `HopStats::addSample()` keeps the newest `kHistoryRounds` samples in round order, also when
  a timeout resolves after a reply of a later round.
- Jitter: each reply after the first adds |RTT - `lastMs`| to `jitterTotalMs` before `lastMs`
  takes the new RTT (§3.2). `jitterMs()` = `jitterTotalMs / (received - 1)`; it is 0 with
  fewer than two replies. Thus the pairs follow the order in which the session reads the
  replies, as `lastMs` does, also for a reply out of round order. A lost probe, an ICMP error
  or a failed send has no RTT, so the reply after it pairs with the reply before it. Replies
  from different routers at one TTL make one sequence, as for the other times.
- mtr pairs the replies in the same way (`ui/net.c:269-274` in mtr 0.96). Its `Javg` counts
  the first reply as a jitter of 0 (`ui/net.c:276-281`). Thus its running mean divides the
  sum by the number of replies, not by the number of differences (`ui/net.c:300-302`). Its
  value is about `(received - 1) / received` times the LinMTR value: for two replies of 10
  and 30 ms, 10 ms against 20 ms. `Javg` is also an integer number of microseconds, and it
  counts an ICMP unreachable answer as a reply.
- `roundCompleted(n)` fires when every probe of round n has an answer or timed out. With
  `maxRounds > 0`, no round starts after that many; draining continues until the last round
  resolves (report mode sends exactly `--cycles` rounds).
- A hop keeps the address of its latest reply or of an ICMP error from the target (§3.2,
  §8 item 16). It resolves the name again when the address changes.
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
- RTT: a completed handshake gives `tcpi_rtt` of `TCP_INFO` (microseconds), which the kernel
  measures from the SYN to the SYN-ACK. The prober reads it with the resend check, in the same
  `getsockopt()` and before it closes the socket. An ICMP answer gives the stamp of its error
  queue entry (§3.2). An RST gives no kernel value, so it uses the RTT at the read. The socket
  that `open()` makes to check the permission stays open with `SO_TIMESTAMPNS`. The kernel
  turns its stamps off when the last socket that uses them closes; this socket keeps them on.

## 4. GUI (`src/gui/`, `src/main.cpp`)

Labels, mnemonics and button texts are verbatim upstream (`WinMTR.rc`).

- Main window: the upstream layout. Row 1: the host box, `&Start` (`&Stop` while a trace
  runs), `&Options`, `E&xit`. Row 2: copy and export. Below: the hop table, the optional
  route graph (see Route graph) and the status bar. While a trace runs, the host box and
  Options stay disabled; copy and export still work. Only `hopsChanged` and `stateChanged`
  refresh the GUI.
- Status bar: `Ready`, `Resolving host <host>...` (the upstream text),
  `Tracing <host> (<address>)`, or the message of the last `errorOccurred`. An `infoMessage`
  covers it for 10 s. The address is `targetAddress()`, the address that the probes go to.
  Each start resolves a host name again, so a new DNS answer shows there.
  `MainWindow::tracingText()` leaves out the address while it is null. It also leaves it out
  for a host that is an IP address in a form that `start()` accepts (also in brackets). Thus
  the text does not depend on when the session sets the address: for an IP address,
  `start()` emits `Tracing` before it sets `targetAddress()`.
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
- Host properties values: the times use `%.1f` ms, and `Jitter:` under `Worst:` shows
  `jitterMs()` (§3.3). `Loss (%):` shows the loss with one decimal. The dialog calculates
  tenths of a percent in integers, `1000 - 1000*recv/done`, with done = received + timed out.
  This is the table formula with one more digit, so it also rounds up. Thus the table shows
  the dialog value rounded up to an integer. With a reply, the value is at most 99.9 (the
  table shows 100 for 1 reply of 2001 probes). Thus 0.0 means that no probe was lost, and
  100.0 means that no reply came.
- Route graph: a `HopGraphWidget` with a legend under the table, in a vertical `QSplitter`.
  It has one line per table row, with the colour `seriesColor(ttl - 1)` from a palette
  of 10 colours. The line of the current row is wider and on top. `Show route graph`
  (`options/showGraph`, off by default) shows the graph; when it is hidden, the table uses
  the full height. `ui/graphSplitter` keeps the splitter state.
- Both graphs: the X axis is a window of the newest rounds, one round for each 3 px of plot
  width (at least 2, at most `kHistoryRounds`). The newest round is at the right edge, and
  older rounds leave at the left edge, so the lines do not compress as the history grows.
  A line that is wider than 1 device pixel (the line of the current row, and each line on a
  screen with a scale above 1) is a set of separate segments, not one polyline. Qt strokes
  a wide polyline as one path, which takes from tens of ms to seconds for a full window.
  Time labels under the X axis use the send times of the visible rounds. The step between
  them is the smallest of 1 s .. 24 h (`labelStepMs()`) that keeps the labels at least 1.5
  label widths apart. The labels are at local-time multiples of the step, and each label
  has a vertical gridline. A wall-clock jump in the window can put two labels less than
  4 px apart. Then the graph does not draw the second label or its gridline. The text is
  `hh:mm:ss`, or `hh:mm` for a step of 1 min or more.
  The bottom margin is 28 px, for the lost-probe ticks and the time labels.
  A tooltip shows the visible sample nearest to the pointer, within 6 px: the hop number,
  the host text of the row (`displayHost()`), the send time of the probe
  (`yyyy-MM-dd hh:mm:ss`, local time) and the RTT or `Lost`. The point of a lost probe is
  the middle of its tick. The lines move each round, thus the graph does not wait for the
  help event of Qt: it uses mouse tracking and shows the tooltip at once on each pointer
  move. A filled circle (radius 4 px, the colour of the series, a 1.5 px outline in the
  window text colour) marks that sample on top of all lines. When the samples change under
  a pointer that does not move, the graph finds the nearest sample again and updates the
  tooltip and the circle. When the pointer leaves the graph, the graph hides the tooltip
  and removes the circle. As with the tooltips of Qt, there is no tooltip while no window
  of LinMTR is active. A repaint costs CPU and delays the read of replies. The delay does not
  change an RTT from the kernel (§3.2). It makes the RTT of a TCP reset, or of an answer
  without a stamp, larger. Thus a pointer move repaints the graph only when the circle moves.

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
  A test can give `TraceSession` a fake backend (`setBackendFactoryForTests()` in `Prober.h`),
  so that it drives the session without probe sockets.

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
9. Host properties: a hop without an address shows its real statistics (upstream: zeros).
   Without an ICMP error, its comment is `No response from host` (upstream: empty). On a hop
   with an address, an ICMP error is the comment, not the Name (upstream: Name, and the
   comment `Host alive.`).
10. Host history: newest first, without duplicates (upstream appends new hosts at the end).
11. The export dialog suggests `linmtr-<host>.txt` or `.html` from the traced host; upstream
    suggests no name.
12. Host properties is non-modal, updates while the trace runs and shows a graph of the hop
    round-trip times. Upstream shows a static modal snapshot. The dialog also shows `Jitter:`,
    the loss with one decimal and the times with real tenths. Upstream has no jitter, and it
    shows the integer loss and times of the table (`%.1f` of an integer).
13. The route graph under the table and the `Show route graph` option are new; upstream has
    no graph.
14. Ping size: 0 to 8192 bytes (`OptionsDialog.h`; the CLI check and the engine clamp use the
    same range). Upstream checks no range. LinMTR sets no `IP_MTU_DISCOVER` option, so the
    kernel default applies and a probe larger than the path MTU can be fragmented. Upstream
    sets `IPFLAG_DONT_FRAGMENT` on every probe.
15. An ICMP error from the target address makes its hop the target hop, and the probes stop
    there. Upstream sets no address for an ICMP error. Thus, until the target replies,
    upstream probes all 30 TTLs and shows the error on each hop from the target on.
16. A hop shows the address of its latest reply (or of an ICMP error from the target, item
    15). A reply from a new address (for example on a load-balanced path) replaces the
    address, and the hop resolves the new name (§3.3). The trimming (§3.3) uses these
    addresses. Upstream `SetAddr()` keeps the first address of a hop until the trace ends
    and resolves its name once. In both, the statistics of a hop include the replies from
    all its addresses.
17. A reply clears the error text of its hop (§3.2). Thus a row shows an ICMP error only
    until the next reply. Upstream writes the error text into the name of the hop
    (`SetName()`), and a reply does not change the name. Only the reverse lookup writes the
    name again, and upstream starts it once, at the first address of the hop. Thus an error
    that comes after that lookup ends stays until the trace ends. With name resolution off,
    every error stays.
18. Status bar: `Ready` while no trace runs (upstream: its version and the Appnor text).
    While a trace runs, it shows `Tracing <host> (<address>)` for a host name and
    `Tracing <host>` for an IP address (upstream:
    `Double click on host name for more information.`). A permanent field shows
    `Double click on a hop for details` (upstream: a `www.appnor.com` link). A trace error
    shows in the status bar (upstream: `Unable to resolve hostname.` in a message box).
    `Resolving host <host>...` is the upstream text. LinMTR has no
    `Waiting for last packets in order to stop trace ...` (upstream `WinMTRDialog.cpp:1171`
    and `:1184`), because Stop ends the trace at once (§2).
