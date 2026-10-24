#!/usr/bin/env python3
"""harvest — a declarative web scraper written against the Python standard library.

Give it a URL and a recipe (a record selector plus the fields inside it); it
walks pagination and writes rows to CSV, TSV, JSON, JSONL or plain text.

The companion C++ implementation (cpp/harvest.cpp) speaks exactly the same
command line and recipe format, and produces byte-identical output.
"""

from __future__ import annotations

import json
import os
import re
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from html.parser import HTMLParser

VERSION = "1.0.0"
DEFAULT_UA = "harvest/1.0 (declarative web scraper)"
DEFAULT_JOIN = " | "

# Scraped pages are almost always UTF-8; do not let the Windows console code
# page mangle them on the way out.
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8")
    except (AttributeError, ValueError):
        pass

VOID_ELEMENTS = {
    "area", "base", "br", "col", "embed", "hr", "img", "input",
    "link", "meta", "param", "source", "track", "wbr",
}

AUTO_CLOSE = {
    "p": {"p"},
    "li": {"li"},
    "dt": {"dt", "dd"},
    "dd": {"dt", "dd"},
    "tr": {"tr"},
    "td": {"td", "th"},
    "th": {"td", "th"},
    "option": {"option"},
}

URL_ATTRIBUTES = {"href", "src", "action", "poster", "data-src", "data-href"}

# Block level elements separate words when flattened to text.
BLOCK_ELEMENTS = {
    "address", "article", "aside", "blockquote", "dd", "div", "dl", "dt",
    "fieldset", "figcaption", "figure", "footer", "form", "h1", "h2", "h3",
    "h4", "h5", "h6", "header", "hr", "li", "main", "nav", "ol", "p", "pre",
    "section", "table", "tbody", "td", "tfoot", "th", "thead", "tr", "ul",
}


class HarvestError(Exception):
    """A user facing error: bad usage, a failed request, an invalid selector."""


# ---------------------------------------------------------------------------
# HTML tree
# ---------------------------------------------------------------------------


class Node:
    __slots__ = ("kind", "tag", "attrs", "children", "parent", "text")

    def __init__(self, kind, tag=None, attrs=None, parent=None, text=None):
        self.kind = kind  # "root" | "element" | "text"
        self.tag = tag
        self.attrs = attrs or {}
        self.children = []
        self.parent = parent
        self.text = text

    def __repr__(self):  # pragma: no cover - debugging aid
        if self.kind == "text":
            return f"<text {self.text[:20]!r}>"
        return f"<{self.tag or 'root'} {len(self.children)} children>"


class _TreeBuilder(HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.root = Node("root")
        self._stack = [self.root]

    # -- helpers ---------------------------------------------------------
    def _open(self, tag, attrs, self_closing):
        closers = AUTO_CLOSE.get(tag)
        if closers:
            while len(self._stack) > 1 and self._stack[-1].tag in closers:
                self._stack.pop()
        parent = self._stack[-1]
        node = Node("element", tag=tag, attrs=attrs, parent=parent)
        parent.children.append(node)
        if tag in VOID_ELEMENTS or self_closing:
            return
        self._stack.append(node)

    # -- HTMLParser hooks ------------------------------------------------
    def handle_starttag(self, tag, attrs):
        self._open(tag, {name: (value or "") for name, value in attrs}, False)

    def handle_startendtag(self, tag, attrs):
        self._open(tag, {name: (value or "") for name, value in attrs}, True)

    def handle_endtag(self, tag):
        for index in range(len(self._stack) - 1, 0, -1):
            if self._stack[index].tag == tag:
                del self._stack[index:]
                return

    def handle_data(self, data):
        if not data:
            return
        self._stack[-1].children.append(Node("text", text=data, parent=self._stack[-1]))


def parse_html(html: str) -> Node:
    """Parse HTML into a tree of Nodes."""
    builder = _TreeBuilder()
    builder.feed(html)
    builder.close()
    return builder.root


def elements(node, predicate=None):
    """Yield every element descendant in document order."""
    for child in node.children:
        if child.kind != "element":
            continue
        if predicate is None or predicate(child):
            yield child
        yield from elements(child, predicate)


def child_elements(node):
    return [child for child in node.children if child.kind == "element"]


def text_content(node) -> str:
    if node.kind == "text":
        return node.text
    if node.tag == "br":
        return " "
    if node.tag in ("script", "style"):
        return ""
    inner = "".join(text_content(child) for child in node.children)
    return f" {inner} " if node.tag in BLOCK_ELEMENTS else inner


def normalize_text(value: str) -> str:
    return re.sub(r"\s+", " ", value).strip()


def inner_html(node) -> str:
    return "".join(serialize(child) for child in node.children)


def serialize(node) -> str:
    if node.kind == "text":
        return node.text
    attributes = "".join(
        f' {name}="{value}"' if value != "" else f" {name}"
        for name, value in node.attrs.items()
    )
    if node.tag in VOID_ELEMENTS:
        return f"<{node.tag}{attributes}>"
    return f"<{node.tag}{attributes}>{inner_html(node)}</{node.tag}>"


# ---------------------------------------------------------------------------
# CSS selectors
# ---------------------------------------------------------------------------

_TOKEN = re.compile(
    r"""
      (?P<tag>\*|[a-zA-Z][\w-]*)
    | \.(?P<cls>[-\w]+)
    | \#(?P<id>[-\w]+)
    | \[(?P<attr>[^\[\]]*)\]
    | :(?P<pseudo>[-\w]+)(?:\((?P<arg>[^()]*)\))?
    """,
    re.VERBOSE,
)

_ATTR = re.compile(r"^\s*([-\w:]+)\s*(?:([~^$*|]?=)\s*(.*?)\s*)?$", re.DOTALL)


def split_groups(selector: str):
    """Split ``a, b`` on top level commas."""
    groups, buffer, depth, quote = [], [], 0, None
    for char in selector:
        if quote:
            buffer.append(char)
            if char == quote:
                quote = None
            continue
        if char in ("'", '"'):
            quote = char
            buffer.append(char)
            continue
        if char in "[(":
            depth += 1
        elif char in ")]":
            depth = max(0, depth - 1)
        if char == "," and depth == 0:
            groups.append("".join(buffer))
            buffer = []
            continue
        buffer.append(char)
    groups.append("".join(buffer))
    return [group.strip() for group in groups if group.strip()]


def split_parts(group: str):
    """Split a complex selector into ``(combinator, compound)`` parts."""
    parts, buffer = [], []
    depth, quote = 0, None
    combinator = None

    def flush():
        nonlocal buffer
        text = "".join(buffer).strip()
        if text:
            parts.append((combinator, text))
        buffer = []

    for char in group:
        if quote:
            buffer.append(char)
            if char == quote:
                quote = None
            continue
        if char in ("'", '"'):
            quote = char
            buffer.append(char)
            continue
        if char in "[(":
            depth += 1
            buffer.append(char)
            continue
        if char in ")]":
            depth = max(0, depth - 1)
            buffer.append(char)
            continue
        if depth == 0 and char == ">":
            flush()
            combinator = ">"
            continue
        if depth == 0 and char.isspace():
            flush()
            if combinator != ">":
                combinator = " "
            continue
        buffer.append(char)

    flush()
    if parts:
        parts[0] = (None, parts[0][1])
    return parts


def parse_compound(text: str):
    compound = {"tag": None, "id": None, "classes": [], "attributes": [], "pseudos": []}
    position = 0
    while position < len(text):
        match = _TOKEN.match(text, position)
        if not match:
            raise HarvestError(f'invalid selector near "{text[position:]}"')
        position = match.end()

        if match.group("tag"):
            token = match.group("tag")
            compound["tag"] = None if token == "*" else token.lower()
        elif match.group("cls"):
            compound["classes"].append(match.group("cls"))
        elif match.group("id"):
            compound["id"] = match.group("id")
        elif match.group("attr") is not None:
            body = match.group("attr")
            parsed = _ATTR.match(body)
            if not parsed:
                raise HarvestError(f"invalid attribute selector [{body}]")
            name, operator, value = parsed.group(1), parsed.group(2), parsed.group(3) or ""
            value = value.strip()
            if len(value) >= 2 and value[0] in "\"'" and value[-1] == value[0]:
                value = value[1:-1]
            compound["attributes"].append((name.lower(), operator, value))
        elif match.group("pseudo"):
            compound["pseudos"].append((match.group("pseudo").lower(), match.group("arg")))
    return compound


def parse_selector(selector: str):
    return [[(combinator, parse_compound(compound)) for combinator, compound in split_parts(group)]
            for group in split_groups(selector)]


def _matches_pseudo(element, name, argument):
    if name == "first-child":
        parent = element.parent
        return parent is not None and child_elements(parent)[0] is element
    if name == "last-child":
        parent = element.parent
        return parent is not None and child_elements(parent)[-1] is element
    if name == "nth-child":
        parent = element.parent
        if parent is None:
            return False
        index = child_elements(parent).index(element) + 1
        argument = (argument or "").strip().lower()
        if argument == "odd":
            return index % 2 == 1
        if argument == "even":
            return index % 2 == 0
        try:
            return index == int(argument)
        except ValueError:
            return False
    if name == "not":
        inner = parse_compound(argument or "")
        return not _matches_compound(element, inner)
    raise HarvestError(f"unsupported pseudo class :{name}")


def _matches_compound(element, compound):
    if element.kind != "element":
        return False
    tag = compound["tag"]
    if tag and element.tag != tag:
        return False
    if compound["id"] and element.attrs.get("id") != compound["id"]:
        return False
    if compound["classes"]:
        classes = set(element.attrs.get("class", "").split())
        if not all(name in classes for name in compound["classes"]):
            return False
    for name, operator, value in compound["attributes"]:
        actual = element.attrs.get(name)
        if actual is None:
            return False
        if operator is None:
            continue
        if operator == "=" and actual != value:
            return False
        if operator == "~=" and value not in actual.split():
            return False
        if operator == "^=" and not actual.startswith(value):
            return False
        if operator == "$=" and not actual.endswith(value):
            return False
        if operator == "*=" and value not in actual:
            return False
        if operator == "|=" and not (actual == value or actual.startswith(value + "-")):
            return False
    for name, argument in compound["pseudos"]:
        if not _matches_pseudo(element, name, argument):
            return False
    return True


def _matches_complex(element, parts):
    index = len(parts) - 1
    if not _matches_compound(element, parts[index][1]):
        return False
    current = element
    # Each part stores the combinator that links it to the part before it, so
    # walking backwards we read the combinator of the current part and the
    # compound of the previous one.
    while index > 0:
        combinator = parts[index][0]
        compound = parts[index - 1][1]
        if combinator == ">":
            current = current.parent
            if current is None or current.kind != "element" or not _matches_compound(current, compound):
                return False
        else:
            ancestor = current.parent
            found = None
            while ancestor is not None and ancestor.kind == "element":
                if _matches_compound(ancestor, compound):
                    found = ancestor
                    break
                ancestor = ancestor.parent
            if found is None:
                return False
            current = found
        index -= 1
    return True


def query_all(root, selector):
    groups = parse_selector(selector)
    results = []
    for element in elements(root):
        for parts in groups:
            if _matches_complex(element, parts):
                results.append(element)
                break
    return results


def query_one(root, selector):
    groups = parse_selector(selector)
    for element in elements(root):
        for parts in groups:
            if _matches_complex(element, parts):
                return element
    return None


# ---------------------------------------------------------------------------
# Extraction
# ---------------------------------------------------------------------------


def parse_field(spec: str):
    """``css@attr`` -> (css, attr); a bare selector means text."""
    if "@" in spec:
        selector, attribute = spec.rsplit("@", 1)
        if re.fullmatch(r"[-\w:]+", attribute):
            return selector.strip(), attribute
    return spec.strip(), "text"


def field_value(element, attribute, base_url):
    if attribute == "html":
        return inner_html(element)
    if attribute != "text":
        value = element.attrs.get(attribute, "")
        if value and attribute in URL_ATTRIBUTES:
            return urllib.parse.urljoin(base_url, value)
        return value
    return normalize_text(text_content(element))


def extract_rows(document, page_url, spec):
    rows = []
    if spec["select"]:
        selector, attribute = parse_field(spec["select"])
        for element in query_all(document, selector):
            rows.append({"value": field_value(element, attribute, page_url)})
        return rows

    for record in query_all(document, spec["record"]):
        row = {}
        for name, field_spec in spec["fields"].items():
            selector, attribute = parse_field(field_spec)
            values = [field_value(match, attribute, page_url) for match in query_all(record, selector)]
            row[name] = spec["join"].join(values)
        rows.append(row)
    return rows


def find_next(document, selector, base_url):
    element = query_one(document, selector)
    if element is None:
        return None
    href = element.attrs.get("href")
    if not href:
        anchor = query_one(element, "a[href]")
        href = anchor.attrs.get("href") if anchor is not None else None
    if not href:
        return None
    return urllib.parse.urljoin(base_url, href)


# ---------------------------------------------------------------------------
# HTTP, retries and robots.txt
# ---------------------------------------------------------------------------


def http_get(url, timeout=25.0, user_agent=DEFAULT_UA, retries=2):
    """Fetch a URL as text, retrying transient failures."""
    headers = {
        "User-Agent": user_agent,
        "Accept": "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8",
        "Accept-Encoding": "identity",
    }
    attempt = 0
    while True:
        request = urllib.request.Request(url, headers=headers)
        try:
            with urllib.request.urlopen(request, timeout=timeout) as response:
                payload = response.read()
                charset = response.headers.get_content_charset() or "utf-8"
                return payload.decode(charset, errors="replace")
        except urllib.error.HTTPError as error:
            transient = error.code == 429 or 500 <= error.code < 600
            if not transient or attempt >= retries:
                raise HarvestError(f"HTTP {error.code} for {url}") from error
        except (urllib.error.URLError, TimeoutError, OSError) as error:
            if attempt >= retries:
                raise HarvestError(f"request failed for {url}: {error}") from error
        attempt += 1
        time.sleep(min(2 ** attempt, 10))


def parse_robots(text, user_agent):
    """Return a list of (path, allowed) rules for our user agent."""
    rules = []
    applies = False
    for line in text.splitlines():
        line = line.split("#", 1)[0].strip()
        if not line or ":" not in line:
            continue
        key, value = (part.strip() for part in line.split(":", 1))
        key = key.lower()
        if key == "user-agent":
            applies = value == "*" or value.lower() in user_agent.lower()
        elif key in ("allow", "disallow") and applies:
            rules.append((value, key == "allow"))
    return rules


def robots_allowed(url, user_agent, fetch, cache):
    parsed = urllib.parse.urlsplit(url)
    origin = f"{parsed.scheme}://{parsed.netloc}"
    if origin not in cache:
        try:
            cache[origin] = parse_robots(fetch(origin + "/robots.txt"), user_agent)
        except HarvestError:
            cache[origin] = []  # no robots.txt, or unreachable: allow
    path = parsed.path or "/"
    best = None
    for pattern, allowed in cache[origin]:
        if not pattern:
            continue
        if pattern.endswith("$"):
            matched = path == pattern[:-1]
        else:
            matched = path.startswith(pattern)
        if matched and (best is None or len(pattern) > best[0]):
            best = (len(pattern), allowed)
    return True if best is None else best[1]


# ---------------------------------------------------------------------------
# Crawling
# ---------------------------------------------------------------------------


def dedupe_rows(rows):
    seen, unique = set(), []
    for row in rows:
        key = tuple(row.items())
        if key in seen:
            continue
        seen.add(key)
        unique.append(row)
    return unique


def crawl(spec, fetch=http_get, sleep=time.sleep, log=None):
    """Walk the pagination and return (rows, pages_fetched)."""
    rows = []
    visited = set()
    robots_cache = {}
    url = spec["url"]
    pages = 0

    while url and (spec["max_pages"] == 0 or pages < spec["max_pages"]):
        if url in visited:
            break
        visited.add(url)

        if not spec["ignore_robots"] and not robots_allowed(
            url, spec["user_agent"], lambda target: fetch(target, timeout=spec["timeout"],
                                                        user_agent=spec["user_agent"], retries=0),
            robots_cache,
        ):
            raise HarvestError(f"robots.txt disallows {url} (override with --ignore-robots)")

        html = fetch(url, timeout=spec["timeout"], user_agent=spec["user_agent"],
                     retries=spec["retries"])
        document = parse_html(html)
        page_rows = extract_rows(document, url, spec)
        rows.extend(page_rows)
        pages += 1
        if log:
            log(pages, url, len(rows))

        if spec["limit"] and len(rows) >= spec["limit"]:
            rows = rows[: spec["limit"]]
            break

        if not spec["next"]:
            break
        next_url = find_next(document, spec["next"], url)
        if not next_url or next_url in visited:
            break
        sleep(spec["delay"] / 1000.0)
        url = next_url

    if spec["dedupe"]:
        rows = dedupe_rows(rows)
    return rows, pages


# ---------------------------------------------------------------------------
# Output
# ---------------------------------------------------------------------------


def column_names(rows):
    names = []
    for row in rows:
        for name in row:
            if name not in names:
                names.append(name)
    return names


def csv_escape(value, delimiter):
    if any(char in value for char in (delimiter, '"', "\n", "\r")) or value != value.strip():
        return '"' + value.replace('"', '""') + '"'
    return value


def render(rows, fmt, delimiter=None):
    names = column_names(rows)
    if delimiter is None:
        delimiter = "\t" if fmt == "tsv" else ","

    if fmt in ("csv", "tsv"):
        lines = [delimiter.join(csv_escape(name, delimiter) for name in names)]
        for row in rows:
            lines.append(delimiter.join(csv_escape(row.get(name, ""), delimiter) for name in names))
        return "\n".join(lines) + "\n"

    if fmt == "jsonl":
        return "".join(
            "{" + ", ".join(f'"{json_escape(name)}": "{json_escape(row.get(name, ""))}"' for name in names) + "}\n"
            for row in rows
        )

    if fmt == "json":
        if not rows:
            return "[]\n"
        lines = ["["]
        for row_index, row in enumerate(rows):
            lines.append("  {")
            for index, name in enumerate(names):
                comma = "," if index < len(names) - 1 else ""
                lines.append(f'    "{json_escape(name)}": "{json_escape(row.get(name, ""))}"{comma}')
            lines.append("  }" + ("," if row_index < len(rows) - 1 else ""))
        lines.append("]")
        return "\n".join(lines) + "\n"

    if fmt == "txt":
        if names == ["value"]:
            return "".join(row.get("value", "") + "\n" for row in rows)
        return "".join(" | ".join(row.get(name, "") for name in names) + "\n" for row in rows)

    raise HarvestError(f"unknown output format: {fmt}")


def json_escape(value):
    out = []
    for char in value:
        if char == '"':
            out.append('\\"')
        elif char == "\\":
            out.append("\\\\")
        elif char == "\n":
            out.append("\\n")
        elif char == "\r":
            out.append("\\r")
        elif char == "\t":
            out.append("\\t")
        elif ord(char) < 0x20:
            out.append(f"\\u{ord(char):04x}")
        else:
            out.append(char)
    return "".join(out)


FORMATS = {"csv": ",", "tsv": "\t", "json": None, "jsonl": None, "txt": None}
EXTENSIONS = {"csv": "csv", "tsv": "tsv", "json": "json", "jsonl": "jsonl", "txt": "txt"}


def write_output(rows, path, fmt=None):
    if fmt is None:
        extension = os.path.splitext(path)[1].lstrip(".").lower()
        fmt = extension if extension in EXTENSIONS else "json"
    text = render(rows, fmt, FORMATS[fmt])
    directory = os.path.dirname(os.path.abspath(path))
    if directory:
        os.makedirs(directory, exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="") as handle:
        handle.write(text)
    return fmt


# ---------------------------------------------------------------------------
# Inspect mode
# ---------------------------------------------------------------------------


def inspect_page(document, limit=20):
    counts = {}
    for element in elements(document):
        key = (element.tag, element.attrs.get("class", ""))
        counts.setdefault(key, []).append(element)

    candidates = [(len(nodes), key, nodes[0]) for key, nodes in counts.items() if len(nodes) >= 3]
    candidates.sort(key=lambda item: (-item[0], item[1][0]))

    lines = [f"elements on page: {sum(1 for _ in elements(document))}",
             f"candidate groups: {len(candidates)}", ""]
    lines.append(f"{'selector':<38} {'count':>5}  sample")
    lines.append("-" * 96)
    for count, (tag, class_name), sample in candidates[:limit]:
        selector = tag + ("." + ".".join(class_name.split()) if class_name else "")
        text = normalize_text(text_content(sample))[:60]
        lines.append(f"{selector:<38} {count:>5}  {text}")
    return "\n".join(lines) + "\n"


# ---------------------------------------------------------------------------
# Command line
# ---------------------------------------------------------------------------

HELP = f"""harvest {VERSION} — declarative web scraper

Usage:
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

Examples:
  harvest https://quotes.toscrape.com/ --select .author --next "li.next a" --max-pages 10 --dedupe
  harvest --recipe recipes/quotes.json --out results/quotes.csv
"""


def default_spec():
    return {
        "url": None,
        "record": None,
        "fields": {},
        "select": None,
        "next": None,
        "max_pages": 1,
        "limit": 0,
        "join": DEFAULT_JOIN,
        "delay": 1000,
        "timeout": 25000,
        "retries": 2,
        "user_agent": DEFAULT_UA,
        "ignore_robots": False,
        "dedupe": False,
        "out": None,
        "format": None,
        "recipe": None,
        "quiet": False,
        "inspect": False,
        "help": False,
        "version": False,
    }


VALUE_OPTIONS = {
    "-r": "record", "--record": "record",
    "-s": "select", "--select": "select",
    "-n": "next", "--next": "next",
    "--max-pages": "max_pages",
    "--limit": "limit",
    "--join": "join",
    "--delay": "delay",
    "--timeout": "timeout",
    "--retries": "retries",
    "--ua": "user_agent",
    "-o": "out", "--out": "out",
    "--format": "format",
    "--recipe": "recipe",
    "-f": "field", "--field": "field",
}

FLAG_OPTIONS = {
    "-v": "version", "--version": "version",
    "--dedupe": "dedupe",
    "--ignore-robots": "ignore_robots",
    "--quiet": "quiet",
    "--inspect": "inspect",
}

INTEGER_FIELDS = {"max_pages", "limit", "delay", "timeout", "retries"}


def parse_args(argv, base=None):
    options = json.loads(json.dumps(base if base is not None else default_spec()))
    options["fields"] = dict(options.get("fields") or {})
    positional = []
    index = 0
    while index < len(argv):
        argument = argv[index]
        name, separator, inline = argument.partition("=")
        if name in ("-h", "--help"):
            if separator:
                raise HarvestError(f"option {name} does not take a value")
            options["help"] = True
            index += 1
            continue
        if name in FLAG_OPTIONS:
            if separator:
                raise HarvestError(f"option {name} does not take a value")
            options[FLAG_OPTIONS[name]] = True
            index += 1
            continue
        if name in VALUE_OPTIONS:
            key = VALUE_OPTIONS[name]
            if separator:
                value = inline
            else:
                if index + 1 >= len(argv):
                    raise HarvestError(f"option {name} needs a value")
                value = argv[index + 1]
                index += 1
            if key == "field":
                field_name, equals, spec = value.partition("=")
                if not equals or not field_name.strip():
                    raise HarvestError(f'--field expects "name=selector", got "{value}"')
                options["fields"][field_name.strip()] = spec.strip()
            elif key in INTEGER_FIELDS:
                try:
                    options[key] = int(value)
                except ValueError:
                    raise HarvestError(f"option {name} expects an integer, got {value!r}") from None
                if options[key] < 0:
                    raise HarvestError(f"option {name} must not be negative")
            else:
                options[key] = value
            index += 1
            continue
        if argument.startswith("-"):
            raise HarvestError(f"unknown option {argument}")
        positional.append(argument)
        index += 1

    if positional:
        if options["url"]:
            raise HarvestError("only one URL can be given")
        options["url"] = positional[0]
    return options


def scan_recipe(argv):
    """Find --recipe before parsing, so command line flags can override it."""
    for index, argument in enumerate(argv):
        if argument == "--recipe":
            if index + 1 >= len(argv):
                raise HarvestError("option --recipe needs a value")
            return argv[index + 1]
        if argument.startswith("--recipe="):
            return argument.split("=", 1)[1]
    return None


def load_recipe(path):
    try:
        with open(path, "r", encoding="utf-8") as handle:
            recipe = json.load(handle)
    except OSError as error:
        raise HarvestError(f"cannot read recipe {path}: {error}") from error
    except json.JSONDecodeError as error:
        raise HarvestError(f"recipe {path} is not valid JSON: {error}") from error
    if not isinstance(recipe, dict):
        raise HarvestError(f"recipe {path} must be a JSON object")
    return recipe


def apply_recipe(options, recipe):
    mapping = {
        "url": "url",
        "record": "record",
        "select": "select",
        "next": "next",
        "maxPages": "max_pages",
        "max_pages": "max_pages",
        "limit": "limit",
        "join": "join",
        "delay": "delay",
        "timeout": "timeout",
        "retries": "retries",
        "userAgent": "user_agent",
        "user_agent": "user_agent",
        "ignoreRobots": "ignore_robots",
        "ignore_robots": "ignore_robots",
        "dedupe": "dedupe",
        "out": "out",
        "format": "format",
    }
    for source, target in mapping.items():
        if source in recipe and recipe[source] is not None:
            options[target] = recipe[source]
    fields = recipe.get("fields")
    if isinstance(fields, dict):
        options["fields"].update({name: str(spec) for name, spec in fields.items()})


def validate(options):
    if not options["url"]:
        raise HarvestError("no URL given")
    parsed = urllib.parse.urlsplit(options["url"])
    if parsed.scheme not in ("http", "https"):
        raise HarvestError(f"unsupported URL scheme: {parsed.scheme or '(none)'}")
    if options["inspect"]:
        return
    if not (options["fields"] and options["record"]) and not options["select"]:
        raise HarvestError("nothing to scrape: pass --record with --field, or --select")
    if options["select"] and (options["fields"] or options["record"]):
        raise HarvestError("--select cannot be combined with --record/--field")
    if not options["record"] and options["fields"]:
        raise HarvestError("--field requires --record")


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    try:
        recipe_path = scan_recipe(argv)
        base = default_spec()
        if recipe_path:
            apply_recipe(base, load_recipe(recipe_path))
            base["recipe"] = recipe_path
        options = parse_args(argv, base)
        if options["help"]:
            sys.stdout.write(HELP)
            return 0
        if options["version"]:
            sys.stdout.write(f"harvest {VERSION}\n")
            return 0
        validate(options)

        if options["inspect"]:
            html = http_get(options["url"], timeout=options["timeout"] / 1000.0,
                            user_agent=options["user_agent"], retries=options["retries"])
            sys.stdout.write(inspect_page(parse_html(html)))
            return 0

        progress = None
        if not options["quiet"]:
            def progress(page, url, count):
                sys.stderr.write(f"page {page}: {url} ({count} rows)\n")

        rows, pages = crawl(options, log=progress)

        if options["out"]:
            fmt = write_output(rows, options["out"], options["format"])
            if not options["quiet"]:
                sys.stderr.write(f"wrote {len(rows)} rows from {pages} page(s) to {options['out']} ({fmt})\n")
        else:
            fmt = options["format"] or "json"
            sys.stdout.write(render(rows, fmt))
        return 0
    except HarvestError as error:
        sys.stderr.write(f"harvest: {error}\n")
        return 1
    except KeyboardInterrupt:  # pragma: no cover
        sys.stderr.write("harvest: interrupted\n")
        return 130


if __name__ == "__main__":
    sys.exit(main())
