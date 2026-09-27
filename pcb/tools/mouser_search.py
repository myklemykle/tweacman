#!/usr/bin/env python3
"""Search Mouser's catalogue from the command line (Mouser Search API v1, standard library only).

The API key is read from $MOUSER_API_KEY, or else from ~/.config/mouser/api_key (a one-line file).
It is never stored in this repo. Get a key from mouser.com -> My Account -> APIs -> Search API.

Usage:
  python3 tools/mouser_search.py search "100nF 50V X7R radial 5.08mm"
  python3 tools/mouser_search.py search "6.8nF C0G radial" --must C0G --must 50V --qty 4
  python3 tools/mouser_search.py part STX-3120-3B EG1224        (exact manufacturer or Mouser part numbers)
  python3 tools/mouser_search.py part 258 --mfr adafruit        (short numbers match many makers: narrow by --mfr)
  add --json to either command for the raw API response.

Mouser's API has no parametric filters (unlike the website), so --must/--not filter the results here, on
the description + attribute text. The API allows about 30 calls a minute and 1000 a day, so replies are
cached for a day in ~/.cache/mouser_search/ and repeated searches cost nothing.
"""
import argparse, hashlib, json, os, pathlib, re, sys, time, urllib.error, urllib.request

API = "https://api.mouser.com/api/v1/search/"
CACHE = pathlib.Path.home() / ".cache" / "mouser_search"
CACHE_SECONDS = 24 * 3600


def api_key():
    key = os.environ.get("MOUSER_API_KEY", "").strip()
    path = pathlib.Path.home() / ".config" / "mouser" / "api_key"
    if not key and path.exists():
        key = path.read_text().strip()
    if not key:
        sys.exit(f"No Mouser API key: set MOUSER_API_KEY or put the key in {path}")
    return key


def call(endpoint, body):
    """POST one request to the API, or return the cached reply if it's under a day old."""
    CACHE.mkdir(parents=True, exist_ok=True)
    cached = CACHE / (hashlib.sha1(json.dumps([endpoint, body], sort_keys=True).encode()).hexdigest() + ".json")
    if cached.exists() and time.time() - cached.stat().st_mtime < CACHE_SECONDS:
        return json.loads(cached.read_text())
    req = urllib.request.Request(f"{API}{endpoint}?apiKey={api_key()}", data=json.dumps(body).encode(),
                                 headers={"Content-Type": "application/json", "Accept": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            reply = json.load(r)
    except urllib.error.HTTPError as e:
        sys.exit(f"Mouser API error {e.code}: {e.read().decode(errors='replace')[:500]}")
    if reply.get("Errors"):
        sys.exit("Mouser API error: " + "; ".join(e.get("Message", str(e)) for e in reply["Errors"]))
    cached.write_text(json.dumps(reply))
    return reply


def keyword_search(keywords, records=50, in_stock=False):
    return call("keyword", {"SearchByKeywordRequest": {
        "keyword": keywords, "records": records, "startingRecord": 0,
        "searchOptions": "InStock" if in_stock else "None", "searchWithYourSignUpLanguage": "false"}})


def part_search(numbers):
    # The API takes up to 10 part numbers per call, separated by "|".
    parts = []
    for i in range(0, len(numbers), 10):
        reply = call("partnumber", {"SearchByPartRequest": {
            "mouserPartNumber": "|".join(numbers[i:i + 10]), "partSearchOptions": "Exact"}})
        parts += reply["SearchResults"]["Parts"]
    return parts


def stock(p):
    """Units in stock as an int (the API gives it as text like '12345 In Stock')."""
    m = re.match(r"[\d,]+", p.get("AvailabilityInStock") or p.get("Availability") or "")
    return int(m.group().replace(",", "")) if m else 0


def unit_price(p, qty):
    """Price per unit when buying qty, from the price breaks (None if qty is under the first break)."""
    price = None
    for b in p.get("PriceBreaks", []):
        if b["Quantity"] <= qty:
            price = float(re.sub(r"[^\d.]", "", b["Price"]))
    return price


def text(p):
    attrs = " ".join(f"{a['AttributeName']} {a['AttributeValue']}" for a in p.get("ProductAttributes", []))
    return f"{p.get('Description', '')} {p.get('Category', '')} {attrs}"


def norm(number):
    """A part number with case and punctuation dropped, so "S2B-PH-K-S(LF)(SN)" matches "s2bphks(lf)(sn)"."""
    return re.sub(r"[^0-9a-z]", "", number.lower())


def show(parts, qty):
    if not parts:
        print("(no matching parts)")
        return
    price_cols = sorted({1, 10, 100, qty})
    head = f"{'Mouser #':<24} {'Mfr part #':<28} {'Manufacturer':<16} {'Stock':>8} " + \
           " ".join(f"{'@' + str(q):>8}" for q in price_cols) + "  Description"
    print(head)
    for p in parts:
        prices = " ".join(f"{(f'${v:.3f}' if (v := unit_price(p, q)) is not None else '-'):>8}" for q in price_cols)
        flags = " [OBSOLETE]" if "obsolete" in (p.get("LifecycleStatus") or "").lower() else ""
        print(f"{p.get('MouserPartNumber', ''):<24} {p.get('ManufacturerPartNumber', ''):<28.28} "
              f"{p.get('Manufacturer', ''):<16.16} {stock(p):>8} {prices}  {p.get('Description', '')}{flags}")
    if len(parts) == 1:
        p = parts[0]
        min_mult = f"min {p.get('Min')}, mult {p.get('Mult')}"
        print(f"\n  {min_mult}\n  {p.get('ProductDetailUrl', '')}\n  datasheet: {p.get('DataSheetUrl', '')}")
        for a in p.get("ProductAttributes", []):
            print(f"  {a['AttributeName']}: {a['AttributeValue']}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("search", help="keyword search")
    s.add_argument("keywords")
    s.add_argument("--must", action="append", default=[], help="keep only results whose text contains this (repeatable, case-insensitive)")
    s.add_argument("--not", dest="exclude", action="append", default=[], help="drop results whose text contains this (repeatable)")
    s.add_argument("--all", action="store_true", help="include out-of-stock parts (default: in stock only)")
    s.add_argument("-n", type=int, default=20, help="rows to show (default 20)")
    p = sub.add_parser("part", help="exact part-number lookup")
    p.add_argument("numbers", nargs="+")
    p.add_argument("--loose", action="store_true", help="also show the API's near-miss matches")
    for x in (s, p):
        x.add_argument("--qty", type=int, default=1, help="also show the unit price at this quantity")
        x.add_argument("--mfr", help="keep only this manufacturer (substring, case-insensitive)")
        x.add_argument("--json", action="store_true", help="print the raw API parts list")
    a = ap.parse_args()

    if a.cmd == "search":
        parts = keyword_search(a.keywords, in_stock=not a.all)["SearchResults"]["Parts"]
        parts = [p for p in parts
                 if all(m.lower() in text(p).lower() for m in a.must)
                 and not any(x.lower() in text(p).lower() for x in a.exclude)]
        # Cheapest at the requested quantity first; unpriced parts last.
        parts.sort(key=lambda p: (unit_price(p, max(a.qty, 1)) is None, unit_price(p, max(a.qty, 1)) or 0, -stock(p)))
        parts = parts[:a.n]
    else:
        # The API also returns near-misses (any part number containing the query), so keep exact matches only.
        want = {norm(n) for n in a.numbers}
        parts = [p for p in part_search(a.numbers)
                 if a.loose or norm(p.get("ManufacturerPartNumber", "")) in want or norm(p.get("MouserPartNumber", "")) in want]
    if a.mfr:
        parts = [p for p in parts if a.mfr.lower() in p.get("Manufacturer", "").lower()]
    if a.json:
        print(json.dumps(parts, indent=2))
    else:
        show(parts, a.qty)


if __name__ == "__main__":
    main()
