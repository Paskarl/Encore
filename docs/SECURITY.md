# Security notes

Pick has two network-facing parts:

| Part | Listens on | Who can reach it | Authentication |
|---|---|---|---|
| `version.dll` (in the game) | `127.0.0.1:47321` (TCP, IPv4 loopback only) | Programs on this PC | None; requests from web pages are refused (see below) |
| Companion (`PickCompanion.exe`) | All interfaces, port 47322 | Devices on the local network | Pairing token on every `/api` route |

The companion is the only part reachable from other devices. It is not started by the game on
its own: it runs only after the player picks **PICK** in the main menu (or starts it by hand),
and stops when its window is closed.

## What was reviewed

Review of October 2026 (commit `31e3f5a` and the fixes after it).

- **Pairing token**: 128 random bits from `RandomNumberGenerator`, hex. Compared with
  `CryptographicOperations.FixedTimeEquals`. Stored in `%LOCALAPPDATA%\Pick\companion-token.txt`
  (user profile, normal user ACL). Shown only on the PC: the console QR code, the `/pair` page
  (loopback address and loopback Host header only) and, after a short-code claim, to the phone.
  In pairing URLs the token is in the `#fragment`, so it never reaches request lines or logs.
- **Short pairing codes**: 6 digits from `RandomNumberGenerator`, issued only to loopback with a
  loopback Host header (the game's menu entry), one at a time, valid 10 minutes, single use, void
  after 5 wrong guesses. A guesser on the LAN has a 5 in 1,000,000 chance per code the player
  issues.
- **Unpaired device on the same Wi-Fi** can load the phone page (static, no data), and try a
  pairing code (see above). Every `/api` route answers 401. `/pair` and `/pair/code` answer 403.
- **Source addresses**: requests from outside loopback, RFC 1918, link-local and IPv6 ULA are
  refused with 403, so a router port forward does not expose the companion to the internet.
- **Input validation**: checksums and md5s must be 32 hex digits; search text is limited
  (library 200, enchor.us 1–100 characters, page 1–50, limit 1–200) and URL-escaped before it is
  forwarded. The companion forwards a fixed set of Pick requests, never arbitrary paths.
  Request bodies are limited to 16 KB; malformed JSON or a wrong content type is a 400/403.
- **Phone page**: all server and chart data is inserted with `textContent`, never as HTML; the
  only `innerHTML` assignments are constant strings. CSP `default-src 'none'`, no framing.
  Cross-origin pages can't call `/api` (the Bearer header needs a CORS preflight, which the
  companion doesn't allow).
- **enchor.us downloads**: only for md5s from a search result of this session; the download URL
  is built from the validated md5. The file is written as `<md5>.sng.part` in
  `<songs folder>\Pick Downloads`, limited to 1 GB, deleted on any failure, and renamed to
  `<artist> - <title> (<charter>).sng` with every character that is invalid in a file name
  (including `\ / :`) replaced, so names from search results can't leave that folder. The `.sng`
  is never unpacked; only its chart is hashed. Metadata lengths are checked before reading.
- **Pick's loopback listener**: binds `127.0.0.1` only. It refuses requests that carry an
  `Origin` header or a Host other than `127.0.0.1`, `localhost` or `[::1]`, so a web page open on
  the PC can't trigger rescans or queue songs (CSRF), or read the library through DNS rebinding.
  Headers are limited to 8 KB, bodies to 64 KB.
- **Starting the companion from the menu**: `CreateProcessW` with the full path
  `<game folder>\PickCompanion\PickCompanion.exe` (or `PICK_COMPANION`) as the application
  name, a quoted command line with fixed arguments (`--pick-port <n> --no-browser`), and the
  companion's folder as working directory. No search path, no shell.

## Fixed in this review

- Pick's loopback listener accepted requests from web pages (CSRF / DNS rebinding); now 403.
- A corrupt `.sng` could make the companion allocate up to 2 GB per metadata string.
- Request bodies up to Kestrel's default 30 MB were accepted; a wrong content type caused a 500.
- The launcher used a `MAX_PATH` buffer and could start a truncated path in long game folders.

## Accepted, with reasons

- **Plain HTTP on the LAN.** The token travels unencrypted, so someone who can sniff the Wi-Fi
  (open or shared networks) can take it. TLS would need a self-signed certificate that every
  phone warns about. The token only allows searching the library, queueing songs, rescanning and
  downloading charts from enchor.us; it gives no file or shell access. Use the companion on a
  home network you trust.
- **The token does not expire.** A paired phone stays paired. To revoke all phones, close the
  companion, delete `%LOCALAPPDATA%\Pick\companion-token.txt` (or start it once with
  `--new-token`), and pair again.
- **Anyone on the LAN can burn a pairing code** with 5 wrong guesses. That only means picking
  PICK again for a new code; it doesn't give access.
- **The companion listens on all interfaces** while it runs, so phones can reach it. Windows asks
  whether to allow it in the firewall on first start; allow it for private networks only.
- **The token file is readable by the Windows user**, like any other per-user secret. A program
  running as that user could also replace `version.dll`, so encrypting the token wouldn't help.
- **Port squatting on the PC**: another local program listening on 47322 first could answer the
  menu's `/pair/code` request with its own code. That needs code running on the PC already.
- **Local programs can use Pick's loopback API without a token.** That is its purpose (tools,
  stream decks, the companion). Only web pages are refused.
- **Download size**: a single chart may be up to 1 GB, one download at a time, only on a tap from
  a paired phone.

## Reporting

Please report security problems privately through GitHub's "Report a vulnerability" on
<https://github.com/Paskarl/clonehero-pick/security>, not in a public issue.
