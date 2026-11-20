# hen/idbe: the Friend List's title names and icons

The HOME Menu's Friend List shows "Could not acquire this software's title" for a friend's game
when its download of the title's name and icon fails:
`https://idbe-ctr.cdn.nintendo.net/icondata/10/<title ID>[-<version>].idbe`
([3dbrew: IDBE](https://www.3dbrew.org/wiki/IDBE)). With Pretendo selected, Nimbus's URL rule
`nintendo\.net -> pretendo.cc` turns the host into a name that does not exist.

So this one request reaches the real server, held to the bar of "Link a 3DS" and below it in
what is sent. The decisions, the cache, the budget and the log line are in
`idbe_passthrough.{h,cpp}` here (standard library only); the HTTP service uses them in
`Context::MakeRequest` and `Context::MakeIconServerRequest`
(`src/core/hle/service/http/http_c.cpp`), and the frontend gives the name lookup
(`hen_idbe_lookup`, `src/citra_switch/main.cpp`). The rules, the cache and the log line are
checked by a host test (`test/run.sh`); this document records no console run.

## 1. The bar: what "Link a 3DS" does

From `src/citra_switch/guest_dns.{h,cpp}` and `link_3ds.cpp` (what was done about DNS, and
what leaves the console during a setup):

| | The setup | This exception |
|---|---|---|
| When | only while a setup program runs (`GuestDns::Enable` around it, `main.cpp`) | in normal play, whenever the Friend List asks |
| Hosts | `nus.cdn.c.shop.nintendowifi.net` and others under `c.shop.nintendowifi.net` (whatever the 3DS installer asks) | exactly `idbe-ctr.cdn.nintendo.net`, port 443 |
| Name lookup | the console's DNS first; if it gives loopback, zero or nothing, one plain UDP question to 1.1.1.1, then 8.8.8.8 (`dns=` in `multiplayer.txt`); printed as `hen: dns: ...` | the same function (`GuestDns::Resolve`), for this one name |
| Sent to Nintendo | the 3DS's own requests unchanged: device ID, region, country | a GET of a path holding a title ID and maybe a version |
| Client certificate | the 3DS family's shared one (ClCertA) | **none**, even if the applet set one |
| Headers | the installer's own | a fixed set of four; the applet's are logged, not sent |
| Withheld in both | anything of the Switch (serial, account, certificates); no system setting read or changed; rooms, lobby and the 3DS connection never use the own-DNS path | same |
| Server certificate checked | no (`http_c.cpp`: verification off for every request of the emulated service) | no (same; see Risks) |

Two points where the exception goes further than the setup did:
it is contact with Nintendo **outside** a setup (without it, in normal play, Nintendo's names
stay blocked), and on a network whose DNS blocks Nintendo's names it goes around that block for
this one name.

## 2. The exception

Rules (all in `Idbe::Classify`, tested):

- HTTPS, port 443, method GET.
- Host exactly `idbe-ctr.cdn.nintendo.net` (any letter case). No trailing dot, no subdomain, no
  sibling. 3dbrew's IDBE page documents only this host for the 3DS; no other host is accepted.
- Path exactly `/icondata/<2 hex>/<16 hex>[-<1 to 10 digits>].idbe`. No query, fragment or
  further segment. The path sent is rebuilt from the parsed numbers (`IconRequest::Path()`),
  not copied from the request.
- Anything that fails a rule takes the service's usual way, unchanged (with Pretendo's rules:
  the name that does not exist).

What is sent: `Host`, `User-Agent: Mozilla/5.0`, `Accept: */*`, `Connection: close`
(`Idbe::RequestHeaders()`). No cookies, no body, no client certificate. Redirects are not
followed. Connect and read timeouts 5 s each, 10 s for the whole transfer. The body is dropped
above 64 KiB (documented size: 14034 bytes).

What the applet gets: on status 200 with a body of 1..65536 bytes, status 200 with
`Content-Type` and `Content-Length` and the body; no other response header (so no `Set-Cookie`).
On anything else (no address, no connection, timeout, other status, too large, over budget)
exactly what it gets without this exception: a request that completed with no response.

Per run: each (title ID, version) is asked once; the answer, or "the server had none", is kept
in memory (`Idbe::Session`). At most 64 requests go out per run; after that only kept answers
are served. A request that got no answer at all is not kept and may be tried again within the
budget.

### Nothing else is opened

While this request is made, every other Nintendo name stays exactly as blocked as before. No
DNS setting is overridden, for the console or for the emulator:

- The address found for `idbe-ctr.cdn.nintendo.net` is given to **one HTTPS client object, for
  that one connection** (`set_hostname_addr_map` on a client made for this request). It is not
  put anywhere another lookup could read it.
- `hen_idbe_lookup` answers for this one name and returns 0 for any other. It is a separate
  pointer from the setup's `hen_guest_lookup`, which stays null in normal play, so the HTTP
  service's other requests and the socket service's lookups (`soc_u.cpp`) resolve through
  the console's DNS as always: Nintendo's other names keep getting the block's answer.
- `GuestDns::Enable` is not called; no system setting is read or written.
- The connection goes to the one address the name server gave. Redirects are not followed, so
  the server cannot send us to another host.
- It has to stay this way: `GuestDns::Enable(true)` or `hen_guest_lookup` used for this would
  both open every name for as long as they are on.

What does happen on a blocking network: one DNS question for this one name goes to 1.1.1.1
(or 8.8.8.8) once per run.

### Where the exemption is

`URLReplacer::Apply` has one caller for requests: `Context::MakeRequest`. The test is in
`MakeRequest`, before that call, and returns early. It is not an exception inside the rule list
or `Apply`: rules are data that Nimbus adds, deletes and saves
(`sysdata/http_hle_replace_rules.txt`); a test in code survives every reload, and `Apply` is
also used for socket lookups (`HenApplyUrlRules`), which must not change.

## 3. The log line

One line per request of the applet, dev and release (`Idbe::LogLine`; test prints examples):

```
hen: idbe: GET https://idbe-ctr.cdn.nintendo.net/icondata/10/0004000000030800-1040.idbe (the program asked for <its URL>): sent; to 23.x.x.x; headers sent: [Host: ...] [User-Agent: Mozilla/5.0] [Accept: */*] [Connection: close]; the program's own headers, not sent: [Name] [Name]; client certificate withheld: none; status 200, 14034 bytes; request 3 of 64 this run
```

Release prints the applet's header names; a dev build their values too.

Reading the line:

| Look for | Good | Then |
|---|---|---|
| `the program's own headers` (dev build: values) | none, or nothing that names a console or account | if the server refuses our fixed set and one of these is needed and harmless, pass that one through |
| `client certificate withheld` | says which the applet wanted; none was sent either way | if `ClCertA` and the server answers 403 without it: stop; sending it is a decision to take first |
| `(the program asked for ...)` | host and path as documented; which of the two URL forms; `%02X` is 10 | another host or path: it did not qualify, the old failure line appears instead |
| `status`, `bytes` | 200, 14034 | 404: the server has no file for this title/version; 403/other: the server refuses us |
| `hen: dns: idbe:` | one line per run | on a network whose DNS blocks the name: "says 127.0.0.1; used ... from 1.1.1.1" |
| count | one `sent` per title per run, the rest `cache` | repeated `sent` for one title: the applet retries on failure |
| no other `nintendo` host anywhere in the log | | |

## 4. Risks

- The server certificate is not checked (as for every request of this service), and the
  address may come from an unencrypted DNS answer. Someone on the path could answer instead.
  They could supply at most 64 KiB that the applet then decrypts and hash-checks; they learn a
  title ID. Checking the server against the 3DS's root certificates would close this; the
  emulated service has none.
- Nintendo's CDN sees: the home's address, a friend's title ID and maybe version, a TLS
  handshake that is not a 3DS's, four generic headers. Nothing of the Switch or the account.
- Contact in normal play, around a DNS block where there is one (section 1).
- A long friend list: bounded to 64 requests a run, one per title.
- If the server is gone or refuses: nothing changes for the user except up to 5 s per failed
  attempt on the request's own thread (the applet waits on it either way).

What working looks like, with Pretendo selected and a friend in a game: one
`hen: idbe: ... sent ... status 200, 14034 bytes` line, the game's name and icon in the Friend
List, a `cache` line on reopening, no certificate sent, no other Nintendo host in the log.

## Files

- `idbe_passthrough.h`, `idbe_passthrough.cpp`: decisions, cache, budget, log line. Standard
  library only.
- `test/idbe_test.cpp`, `test/run.sh`: host test (clang++, address and UB sanitizers).
