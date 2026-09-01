# Endpoints, and the things that were already tried

Read this before touching `part.py`'s HTTP layer. Do not re-probe what is here.

## Endpoint status as recorded (probed 2026-08-18)

| endpoint | status |
| --- | --- |
| `wmsc.lcsc.com/ftps/wm/product/detail` | works, detail by C-code, full parameters |
| `easyeda.com/api/eda/product/search` (POST, form-encoded) | works, keyword search, `pageSize` up to 200 |
| `jlcpcb.com/api/overseas-pcb-order/v1/shoppingCart/smtGood/selectSmtComponentList` (POST, JSON) | works, no auth, 200 rows/call **with parsed attributes** |
| `wmsc.lcsc.com/wmsc/product/detail` | dead, 404 JSON |
| `wmsc.lcsc.com/ftps/wm/search/global` | blocked, Akamai Access Denied |
| `wmsc.lcsc.com/ftps/wm/{search/product,product/search,product/list,catalog/list}` | dead, "static resource unavailable" |
| `cart.jlcpcb.com/.../selectSmtComponentList` | superseded by the `jlcpcb.com/api/...` path above |

The constants are at the top of `part.py`. When `selftest` shows DEAD, fix the
constant. Never write a replacement scraper.

## LCSC cannot filter or sort server-side. Do not go looking again.

Probed exhaustively against the easyeda search endpoint: `sortField`, `sortOrder`,
`orderBy`, `catalogId`, `paramNameValueMap`, `attributes`, `paramList`, `filters`,
`paramValueList`, `selectedParams`, `paramMap`, `attributeFilter`, `params`,
`searchParam`, `stockFlag`. Every one either changed nothing or returned `total=0`.
Its search rows carry **no parameters at all** - only mpn, number, manufacturer,
package, stock, price, url. `needAggs=true` returns facet lists but they cannot be
sent back as filters.

This is why `pick` works the way it does: LCSC candidates must be fetched by
`product/detail` one at a time to learn their parameters. That is cached and parallel,
so it is fast, but it is why there is a `--pool` cap.

## JLCPCB Basic vs Extended

LCSC has no concept of "Basic" - it is a JLC assembly-library field. So:

- `--basic` pools straight from JLC's base library (server-side filter, authoritative,
  ~1 call per keyword, and much faster than filtering LCSC results).
- Without `--basic`, `pick` annotates only the rows it finally displays, one lookup per
  C-code. The `jlc` column shows `BASIC`, `ext` or `-`.
- **Do not infer Basic/Extended from a JLC keyword search**: its relevance ranking
  drops parts that do exist in the library, which shows up as a false `-`.

```bash
part.py jlc /mnt/project/meshtastic.net       # whole board, with refdes
part.py jlc C45783 C1525 C2650956
```

For "source this whole board" or "what's blocking assembly", use `check board.net`
instead of running `bom` then `jlc` separately - one netlist parse, one table with both
LCSC price and JLC Basic/Extended per line, plus the components with no LCSC code at
all (the ones actually blocking assembly, not just costing more).

`jlc` on a `.net` file parses the `LCSC Part` property via kicad-review's `knet.py`.
It must not regex for `C\d+` on a netlist - that matches capacitor refdes like `C108`.

## DigiKey setup

Optional. LCSC and JLC work with no configuration. For DigiKey, get free credentials at
developer.digikey.com (create an app, Production, Product Information V4), then:

```bash
export DIGIKEY_CLIENT_ID=...
export DIGIKEY_CLIENT_SECRET=...
```

or write `~/.config/partsearch/config.json` as
`{"digikey_client_id": "...", "digikey_client_secret": "..."}`.

Defaults are `--site CA --currency CAD`. The OAuth2 token is cached with its expiry.
DigiKey participates in `search`/`show` only, not in `pick`.

The DigiKey response normaliser (`_dk_norm`) has not been exercised against a live v4
response. If `selftest` shows auth OK but search DEAD, the field mapping there is the
place to look, not the request code.
