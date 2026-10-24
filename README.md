# harvest

**Give it a URL and a few selectors; get rows out.**

A declarative web scraper in two implementations — Python (standard library
only) and C++ (single file, no dependencies beyond `curl`) — that share the same
command line, the same recipe format, and produce **byte-identical output**.

```bash
python python/harvest.py --recipe recipes/authors.json --out results/authors.txt
./cpp/harvest       --recipe recipes/authors.json --out results/authors.txt
# results/authors.txt — same SHA-256 from both
```

<p align="center">
  <a href="https://github.com/xhxmn/harvest/actions/workflows/ci.yml"><img alt="CI" src="https://github.com/xhxmn/harvest/actions/workflows/ci.yml/badge.svg"></a>
  <img alt="Python 3.8+" src="https://img.shields.io/badge/python-3.8%2B-blue">
  <img alt="C++17" src="https://img.shields.io/badge/c%2B%2B-17-blue">
  <img alt="Dependencies: none" src="https://img.shields.io/badge/dependencies-none-brightgreen">
  <img alt="License: MIT" src="https://img.shields.io/badge/license-MIT-green">
</p>

---

## Why two implementations?

Because it is a nice way to show that the hard part of scraping is not the
language: it is picking the right selectors and walking pagination. Both
versions:

- parse HTML into a tree without a DOM library,
- support a practical subset of CSS selectors,
- follow `next` links, dedupe, retry, respect `robots.txt`,
- write CSV, TSV, JSON, JSONL or plain text.

The C++ one is a single `harvest.cpp` (no build system needed); the Python one is
a single `harvest.py` that runs on a stock interpreter. A parity test renders the
same pages through both and compares the bytes.

## What it can do

| You want | Recipe |
| --- | --- |
| every quote + author on a site | `record` + `fields` |
| all usernames on a page and its pagination | `select` + `next` + `dedupe` |
| a whole catalogue | `record` + `next` + `maxPages` |
| just one column as a text file | `select` + `--format txt` |

The "all usernames" case from the prompt looks like this (the same pattern works
on any site whose markup you can inspect):

```bash
harvest https://example.com/comments \
  --select ".comment .username" \
  --next "a.next-page" \
  --max-pages 0 \
  --dedupe \
  --out usernames.txt
```

Only scrape pages you are allowed to scrape: check the site's terms, keep the
default delay, and do not collect personal data you have no business collecting.

## Quick start

### Python

Nothing to install.

```bash
python python/harvest.py --inspect https://quotes.toscrape.com/
```

`--inspect` prints the repeating element groups it finds, which is the fastest
way to pick selectors:

```
elements on page: 151
candidate groups: 9

selector                               count  sample
------------------------------------------------------------------------------------------------
a.tag                                     40  change
div.quote                                 10  “The world as we have created it is a process of our thinkin
small.author                              10  Albert Einstein
span.text                                 10  “The world as we have created it is a process of our thinkin
```

Then scrape:

```bash
python python/harvest.py https://quotes.toscrape.com/ \
  --record .quote \
  --field "quote=.text" \
  --field "author=.author" \
  --field "tags=.tags a.tag" \
  --next "li.next a" --max-pages 10 \
  --out results/quotes.csv
```

### C++

Requires a C++17 compiler and `curl` on `PATH` (it ships with Windows 10+,
macOS and nearly every Linux).

```bash
g++ -std=c++17 -O2 cpp/harvest.cpp -o harvest     # or: make
./harvest --recipe recipes/books.json --out results/books.csv
```

On Windows, `.\cpp\build.ps1` compiles both the tool and its test runner.

## Command line

```
harvest <url> [options]
harvest --recipe recipe.json [options]

What to scrape:
  -r, --record <css>          one row per element matching this selector
  -f, --field <name=css[@attr]>
                              field inside each record (repeatable)
  -s, --select <css[@attr]>   one value per match, exported as one column
  -n, --next <css>            selector for the next-page link
      --max-pages <n>         stop after n pages, 0 = follow until the end (default: 1)
      --limit <n>             stop after n rows
      --join <sep>            separator for fields with several matches (default: " | ")

Network:
      --delay <ms>            pause between requests (default: 1000)
      --timeout <ms>          per request timeout (default: 25000)
      --retries <n>           retries for 429/5xx (default: 2)
      --ua <string>           User-Agent header
      --ignore-robots         do not consult robots.txt

Output:
  -o, --out <file>            .csv .tsv .json .jsonl .txt
      --format <fmt>          csv | tsv | json | jsonl | txt
      --dedupe                drop duplicate rows
      --quiet                 no progress on stderr

Other:
      --inspect               list repeated elements on the page (helps pick selectors)
  -h, --help                  show this help
  -v, --version               show the version
```

Field values default to the element's text; add `@attr` to read an attribute:

```
--field "url=h3 a@href"      # resolves relative URLs against the page URL
--field "rating=p@class"     # reads class="star-rating Three"
--field "html=.body@html"    # inner HTML, useful for embedded markup
```

### CSS support

Tag names, `*`, `.class`, `#id`, `[attr]`, `[attr=value]`, `[attr^=]`, `[attr$=]`,
`[attr*=]`, `[attr~=]`, `[attr|=]`, descendant and `>` child combinators,
selector groups with `,`, and `:first-child`, `:last-child`, `:nth-child(...)`,
`:not(...)`. That covers what sites actually use; no XPath, no JavaScript
rendering.

## Recipes

A recipe is just the flags as JSON, so a scrape is reproducible and reviewable:

```json
{
  "name": "quotes",
  "url": "https://quotes.toscrape.com/",
  "record": ".quote",
  "fields": {
    "quote": ".text",
    "author": ".author",
    "tags": ".tags a.tag",
    "author_url": "a[href^=\"/author/\"]@href"
  },
  "next": "li.next a",
  "maxPages": 10,
  "delay": 300
}
```

Command line flags override recipe values, so the same recipe can be reused with
`--max-pages 2` for a quick test.

## Results

These files were produced by the recipes in this repository; both
implementations write byte-identical copies.

| Recipe | Output | What is in it |
| --- | --- | --- |
| `recipes/authors.json` | [`results/authors.txt`](results/authors.txt) | 50 distinct authors from 100 quotes, 10 pages |
| `recipes/quotes.json` | [`results/quotes.csv`](results/quotes.csv) | 100 quotes with author, tags and author URL |
| `recipes/books.json` | [`results/books.csv`](results/books.csv) | 1000 books with price, rating, stock and product URL |

A row from `results/books.csv`:

```csv
title,price,rating,availability,url
A Light in the Attic,£51.77,star-rating Three,In stock,https://books.toscrape.com/catalogue/a-light-in-the-attic_1000/index.html
```

Both sites are public sandboxes built for scraping practice.

## Output formats

| Format | Shape |
| --- | --- |
| `csv` / `tsv` | header row, RFC 4180 quoting, `\n` endings |
| `json` | array of objects, two-space indent |
| `jsonl` | one object per line |
| `txt` | one value per line in `--select` mode, otherwise fields joined with `" \| "` |

## Politeness

- `robots.txt` is fetched and enforced by default (`--ignore-robots` opts out).
- Requests are sequential and spaced by `--delay` (1000 ms default).
- A descriptive `User-Agent` is sent; override with `--ua`.
- Transient failures are retried with backoff.
- The scraper only does plain HTTP GETs: no JavaScript, no cookies, no login.

Use it on pages you have permission to read, and keep an eye on the site's terms
of service. Scraping people's personal data may be illegal where you live even
when it is technically possible.

## Tests

```bash
make test          # or: npm-less, just two commands
python -m unittest discover -s python -p "test_*.py"   # 44 tests
./cpp/test_harvest                                     # 80 checks
```

The parity check is a shell one-liner: run both on a recipe and compare hashes.

```bash
python python/harvest.py --recipe recipes/quotes.json --out /tmp/py.csv --quiet
./cpp/harvest            --recipe recipes/quotes.json --out /tmp/cpp.csv --quiet
sha256sum /tmp/py.csv /tmp/cpp.csv
```

There is also an offline version that uses the fixtures in `fixtures/` instead of
the network (this is what CI runs):

```bash
python -m http.server 8765 --directory fixtures &
python python/harvest.py http://127.0.0.1:8765/page1.html --record .item \
  --field "name=.name" --next "a.next" --max-pages 2 --delay 0 --out /tmp/py.csv --quiet
./cpp/harvest http://127.0.0.1:8765/page1.html --record .item \
  --field "name=.name" --next "a.next" --max-pages 2 --delay 0 --out /tmp/cpp.csv --quiet
cmp /tmp/py.csv /tmp/cpp.csv
```

## Project layout

```
harvest/
├── python/
│   ├── harvest.py           # the whole tool, standard library only
│   └── test_harvest.py      # 44 unittest cases
├── cpp/
│   ├── harvest.cpp          # the whole tool, C++17, single file
│   └── test_harvest.cpp     # 80 checks, includes harvest.cpp
├── recipes/                 # ready-to-run scraping recipes
├── results/                 # actual scraped output, committed
├── fixtures/                # tiny local site used by the offline parity test
├── Makefile                 # make / make test / make demo
└── cpp/build.ps1            # Windows build without make
```

## Limitations

- No JavaScript rendering: if the data is only in the DOM after script execution,
  this tool will not see it (use a browser automation tool for that).
- C++ assumes UTF-8 pages; Python honours the response charset.
- The CSS engine is a deliberate subset, not a full Selectors Level 4
  implementation.

## License

[MIT](LICENSE)
