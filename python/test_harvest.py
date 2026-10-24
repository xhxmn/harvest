#!/usr/bin/env python3
"""Tests for harvest.py — standard library only: python -m unittest discover -s python"""

import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import harvest  # noqa: E402


def parse(html):
    return harvest.parse_html(html)


def select(value, selector):
    root = value if isinstance(value, harvest.Node) else parse(value)
    return harvest.query_all(root, selector)


def texts(value, selector):
    return [harvest.normalize_text(harvest.text_content(node)) for node in select(value, selector)]


class HTMLTests(unittest.TestCase):
    def test_nested_elements_and_text(self):
        root = parse("<div><p>hello <b>world</b></p></div>")
        self.assertEqual(texts(root, "div p b"), ["world"])
        self.assertEqual(harvest.normalize_text(harvest.text_content(root)), "hello world")

    def test_attributes_quoted_unquoted_and_self_closing(self):
        root = parse('<img src=/a.png alt="A picture"><input type=text disabled>')
        img = harvest.query_one(root, "img")
        self.assertEqual(img.attrs["src"], "/a.png")
        self.assertEqual(img.attrs["alt"], "A picture")
        self.assertEqual(harvest.query_one(root, "input").attrs["type"], "text")
        self.assertEqual(harvest.query_one(root, "input").attrs["disabled"], "")

    def test_void_elements_do_not_swallow_siblings(self):
        root = parse("<p>one<br>two</p><p>three</p>")
        self.assertEqual(texts(root, "p"), ["one two", "three"])

    def test_entities_are_decoded(self):
        root = parse("<p>a &amp; b &lt;c&gt; &#233; &#x2014; &hellip;</p>")
        self.assertEqual(harvest.normalize_text(harvest.text_content(root)), "a & b <c> \u00e9 \u2014 \u2026")

    def test_unclosed_paragraphs_auto_close(self):
        root = parse("<p>first<p>second")
        self.assertEqual(texts(root, "p"), ["first", "second"])

    def test_malformed_markup_does_not_throw(self):
        root = parse("<div><span>unclosed</div>after")
        self.assertEqual(harvest.normalize_text(harvest.text_content(root)), "unclosed after")
        self.assertEqual(texts(root, "span"), ["unclosed"])

    def test_script_and_style_are_ignored_by_text(self):
        root = parse("<div>keep<script>drop()</script><style>p{}</style></div>")
        self.assertEqual(harvest.normalize_text(harvest.text_content(root)), "keep")

    def test_stray_angle_bracket_is_text(self):
        root = parse("<p>1 < 2</p>")
        self.assertEqual(harvest.normalize_text(harvest.text_content(root)), "1 < 2")


class SelectorTests(unittest.TestCase):
    HTML = """
        <div id="main" class="page wide">
          <ul class="list">
            <li class="item first">one</li>
            <li class="item">two</li>
            <li class="item last" data-kind="x">three</li>
          </ul>
          <a href="/next">next</a>
          <a href="https://example.com/abs">abs</a>
        </div>
    """

    def test_tag_class_and_id(self):
        self.assertEqual(len(select(self.HTML, "li")), 3)
        self.assertEqual(len(select(self.HTML, ".item")), 3)
        self.assertEqual(len(select(self.HTML, "li.item")), 3)
        self.assertEqual(len(select(self.HTML, "#main")), 1)
        self.assertEqual(len(select(self.HTML, ".page.wide")), 1)
        self.assertEqual(len(select(self.HTML, ".page.nope")), 0)

    def test_descendant_and_child(self):
        self.assertEqual(len(select(self.HTML, "div li")), 3)
        self.assertEqual(len(select(self.HTML, "ul > li")), 3)
        self.assertEqual(len(select(self.HTML, "div > li")), 0)

    def test_attribute_selectors(self):
        self.assertEqual(len(select(self.HTML, "[data-kind]")), 1)
        self.assertEqual(len(select(self.HTML, '[data-kind="x"]')), 1)
        self.assertEqual(len(select(self.HTML, 'a[href^="/"]')), 1)
        self.assertEqual(len(select(self.HTML, 'a[href$="abs"]')), 1)
        self.assertEqual(len(select(self.HTML, 'a[href*="example"]')), 1)

    def test_pseudo_classes(self):
        self.assertEqual(texts(self.HTML, "li:first-child"), ["one"])
        self.assertEqual(texts(self.HTML, "li:last-child"), ["three"])
        self.assertEqual(texts(self.HTML, "li:nth-child(2)"), ["two"])
        self.assertEqual(len(select(self.HTML, "li:not(.last)")), 2)

    def test_groups(self):
        self.assertEqual(texts(self.HTML, "li:first-child, a"), ["one", "next", "abs"])

    def test_universal(self):
        self.assertTrue(len(select(self.HTML, "*")) > 5)


class ExtractTests(unittest.TestCase):
    def test_record_mode(self):
        html = """
          <article class="post"><h2>First</h2><a href="/a">link</a></article>
          <article class="post"><h2>Second</h2><a href="/b">link</a></article>
        """
        spec = {
            "select": None,
            "record": ".post",
            "fields": {"title": "h2", "url": "a@href"},
            "join": " | ",
        }
        rows = harvest.extract_rows(parse(html), "https://site.test/blog/", spec)
        self.assertEqual([row["title"] for row in rows], ["First", "Second"])
        self.assertEqual(rows[0]["url"], "https://site.test/a")

    def test_missing_field_is_empty(self):
        spec = {"select": None, "record": ".x", "fields": {"a": "span", "b": "b"}, "join": "|"}
        rows = harvest.extract_rows(parse('<div class="x"><span>v</span></div>'), "https://s.test/", spec)
        self.assertEqual(rows, [{"a": "v", "b": ""}])

    def test_multiple_matches_are_joined(self):
        spec = {"select": None, "record": ".x", "fields": {"t": "a"}, "join": " | "}
        rows = harvest.extract_rows(parse('<div class="x"><a>1</a><a>2</a></div>'), "https://s.test/", spec)
        self.assertEqual(rows[0]["t"], "1 | 2")

    def test_select_mode_with_attribute(self):
        spec = {"select": "a@href", "record": None, "fields": {}, "join": " | "}
        rows = harvest.extract_rows(parse('<a href="/one">1</a><a href="/two">2</a>'), "https://s.test/", spec)
        self.assertEqual([row["value"] for row in rows], ["https://s.test/one", "https://s.test/two"])

    def test_parse_field(self):
        self.assertEqual(harvest.parse_field(".author"), (".author", "text"))
        self.assertEqual(harvest.parse_field("a@href"), ("a", "href"))
        self.assertEqual(harvest.parse_field("div@html"), ("div", "html"))

    def test_find_next_resolves_relative_links(self):
        doc = parse('<nav><a class="next" href="page/2/">Next</a></nav>')
        self.assertEqual(harvest.find_next(doc, "a.next", "https://s.test/page/1/"),
                         "https://s.test/page/1/page/2/")
        self.assertIsNone(harvest.find_next(doc, "a.missing", "https://s.test/"))

    def test_next_matching_a_container(self):
        doc = parse('<li class="next"><a href="/p2">Next</a></li>')
        self.assertEqual(harvest.find_next(doc, "li.next", "https://s.test/"), "https://s.test/p2")


class OutputTests(unittest.TestCase):
    rows = [
        {"name": "a, b", "note": 'say "hi"'},
        {"name": "plain", "note": "line\nbreak"},
    ]

    def test_csv_quoting(self):
        text = harvest.render(self.rows, "csv")
        self.assertEqual(
            text,
            'name,note\n"a, b","say ""hi"""\nplain,"line\nbreak"\n',
        )

    def test_tsv_uses_tabs(self):
        text = harvest.render([{"a": "1", "b": "2"}], "tsv")
        self.assertEqual(text, "a\tb\n1\t2\n")

    def test_json(self):
        text = harvest.render([{"a": "1", "b": "2"}], "json")
        self.assertEqual(text, '[\n  {\n    "a": "1",\n    "b": "2"\n  }\n]\n')

    def test_json_escapes(self):
        text = harvest.render([{"a": 'q"\\\n'}], "json")
        self.assertIn('"q\\"\\\\\\n"', text)

    def test_jsonl(self):
        text = harvest.render([{"a": "1"}, {"a": "2"}], "jsonl")
        self.assertEqual(text, '{"a": "1"}\n{"a": "2"}\n')

    def test_txt(self):
        self.assertEqual(harvest.render([{"value": "x"}, {"value": "y"}], "txt"), "x\ny\n")

    def test_empty_output(self):
        self.assertEqual(harvest.render([], "json"), "[]\n")

    def test_write_output_picks_format_from_extension(self):
        import tempfile
        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, "nested", "out.csv")
            fmt = harvest.write_output([{"a": "1"}], path)
            self.assertEqual(fmt, "csv")
            with open(path, encoding="utf-8") as handle:
                self.assertEqual(handle.read(), "a\n1\n")


class CrawlTests(unittest.TestCase):
    def make_fetch(self, pages):
        def fetch(url, **kwargs):
            if url in pages:
                return pages[url]
            raise harvest.HarvestError(f"404 {url}")
        return fetch

    def spec(self, **overrides):
        base = {
            "url": "https://s.test/1", "record": ".item", "fields": {"name": "span"},
            "select": None, "next": "a.next", "max_pages": 10, "limit": 0,
            "join": " | ", "delay": 0, "timeout": 1000, "retries": 0,
            "user_agent": harvest.DEFAULT_UA, "ignore_robots": True,
            "dedupe": False,
        }
        base.update(overrides)
        return base

    def test_pagination_collects_every_page(self):
        pages = {
            "https://s.test/1": '<div class="item"><span>a</span></div><a class="next" href="/2">n</a>',
            "https://s.test/2": '<div class="item"><span>b</span></div><a class="next" href="/3">n</a>',
            "https://s.test/3": '<div class="item"><span>c</span></div>',
        }
        rows, count = harvest.crawl(self.spec(), fetch=self.make_fetch(pages), sleep=lambda _: None)
        self.assertEqual([row["name"] for row in rows], ["a", "b", "c"])
        self.assertEqual(count, 3)

    def test_max_pages_stops_early(self):
        pages = {f"https://s.test/{n}": '<div class="item"><span>x</span></div><a class="next" href="/3">n</a>'
                 for n in (1, 2, 3)}
        rows, count = harvest.crawl(self.spec(max_pages=1), fetch=self.make_fetch(pages), sleep=lambda _: None)
        self.assertEqual(count, 1)
        self.assertEqual(len(rows), 1)

    def test_limit_trims_rows(self):
        pages = {"https://s.test/1": "".join(f'<div class="item"><span>{n}</span></div>' for n in range(5))}
        rows, _ = harvest.crawl(self.spec(limit=2), fetch=self.make_fetch(pages), sleep=lambda _: None)
        self.assertEqual(len(rows), 2)

    def test_loop_is_detected(self):
        pages = {"https://s.test/1": '<div class="item"><span>a</span></div><a class="next" href="/1">n</a>'}
        rows, count = harvest.crawl(self.spec(), fetch=self.make_fetch(pages), sleep=lambda _: None)
        self.assertEqual(count, 1)
        self.assertEqual(len(rows), 1)

    def test_dedupe(self):
        pages = {"https://s.test/1": '<div class="item"><span>a</span></div><div class="item"><span>a</span></div>'}
        rows, _ = harvest.crawl(self.spec(dedupe=True), fetch=self.make_fetch(pages), sleep=lambda _: None)
        self.assertEqual(len(rows), 1)

    def test_robots_disallow_is_enforced(self):
        pages = {
            "https://s.test/robots.txt": "User-agent: *\nDisallow: /private",
            "https://s.test/private": '<div class="item"><span>a</span></div>',
        }
        with self.assertRaises(harvest.HarvestError):
            harvest.crawl(self.spec(url="https://s.test/private", ignore_robots=False),
                          fetch=self.make_fetch(pages), sleep=lambda _: None)

    def test_robots_allow_when_missing(self):
        pages = {"https://s.test/1": '<div class="item"><span>a</span></div>'}
        rows, _ = harvest.crawl(self.spec(ignore_robots=False),
                                fetch=self.make_fetch(pages), sleep=lambda _: None)
        self.assertEqual(len(rows), 1)


class CliTests(unittest.TestCase):
    def test_defaults(self):
        options = harvest.parse_args(["https://s.test/"])
        self.assertEqual(options["url"], "https://s.test/")
        self.assertEqual(options["max_pages"], 1)
        self.assertEqual(options["delay"], 1000)
        self.assertEqual(options["join"], " | ")

    def test_equals_and_repeated_fields(self):
        options = harvest.parse_args([
            "https://s.test/", "--record=.p", "--field", "a=.x", "--field=b=.y@href",
            "--max-pages=0", "--dedupe",
        ])
        self.assertEqual(options["record"], ".p")
        self.assertEqual(options["fields"], {"a": ".x", "b": ".y@href"})
        self.assertEqual(options["max_pages"], 0)
        self.assertTrue(options["dedupe"])

    def test_bad_field_is_rejected(self):
        with self.assertRaises(harvest.HarvestError):
            harvest.parse_args(["https://s.test/", "--field", "nope"])

    def test_unknown_option_is_rejected(self):
        with self.assertRaises(harvest.HarvestError):
            harvest.parse_args(["https://s.test/", "--wat"])

    def test_integer_options_are_validated(self):
        with self.assertRaises(harvest.HarvestError):
            harvest.parse_args(["https://s.test/", "--limit", "many"])

    def test_select_and_record_conflict(self):
        options = harvest.parse_args([
            "https://s.test/", "--select", ".a", "--record", ".p", "--field", "x=.y",
        ])
        with self.assertRaises(harvest.HarvestError):
            harvest.validate(options)

    def test_help_and_version(self):
        self.assertTrue(harvest.parse_args(["--help"])["help"])
        self.assertTrue(harvest.parse_args(["-v"])["version"])

    def test_recipe_is_merged(self):
        import json
        import tempfile
        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, "r.json")
            with open(path, "w", encoding="utf-8") as handle:
                json.dump({"url": "https://s.test/", "record": ".p", "fields": {"a": ".x"},
                           "maxPages": 3}, handle)
            options = harvest.parse_args(["--recipe", path])
            harvest.apply_recipe(options, harvest.load_recipe(path))
            self.assertEqual(options["record"], ".p")
            self.assertEqual(options["fields"], {"a": ".x"})
            self.assertEqual(options["max_pages"], 3)


if __name__ == "__main__":
    unittest.main(verbosity=2)
