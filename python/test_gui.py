#!/usr/bin/env python3
"""Tests for the Tkinter front-end. Skipped automatically when tkinter or a
display is unavailable (for example on a headless CI runner)."""

import functools
import http.server
import os
import sys
import threading
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

GUI_AVAILABLE = True
GUI_REASON = ""
try:
    import tkinter  # noqa: F401
    import harvest_gui
except Exception as error:  # pragma: no cover - depends on the machine
    GUI_AVAILABLE = False
    GUI_REASON = f"tkinter unavailable: {error}"

FIXTURES = os.path.join(os.path.dirname(HERE), "fixtures")


def start_fixture_server():
    handler = functools.partial(http.server.SimpleHTTPRequestHandler, directory=FIXTURES)
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    return server, server.server_address[1]


@unittest.skipUnless(GUI_AVAILABLE, GUI_REASON or "tkinter unavailable")
class GuiTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        try:
            cls.server, cls.port = start_fixture_server()
        except OSError as error:  # pragma: no cover
            raise unittest.SkipTest(f"cannot start fixture server: {error}")

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()

    def setUp(self):
        import tkinter as tk

        try:
            self.root = tk.Tk()
        except tk.TclError as error:  # pragma: no cover - headless
            raise unittest.SkipTest(f"no display: {error}")
        self.root.withdraw()
        self.app = harvest_gui.HarvestApp(self.root)

    def tearDown(self):
        self.app.stop_event.set()
        if self.app.worker:
            self.app.worker.join(timeout=5)
        self.root.destroy()

    # -- pure helpers ----------------------------------------------------

    def test_parse_fields(self):
        fields = self.app._parse_fields("quote=.text\n\n# comment\nauthor=.author\ntags=.tags a\n")
        self.assertEqual(fields, {"quote": ".text", "author": ".author", "tags": ".tags a"})

    def test_parse_fields_rejects_bad_lines(self):
        with self.assertRaises(ValueError):
            self.app._parse_fields("nope")
        with self.assertRaises(ValueError):
            self.app._parse_fields("=selector")

    def test_build_spec_record_mode(self):
        self.app.url_var.set("https://example.com/")
        self.app.record_var.set(".quote")
        self.app.fields_text.insert("1.0", "author=.author")
        self.app.next_var.set("a.next")
        self.app.max_pages_var.set("3")
        self.app.delay_var.set("0")
        spec = self.app._build_spec()
        self.assertEqual(spec["url"], "https://example.com/")
        self.assertEqual(spec["record"], ".quote")
        self.assertEqual(spec["fields"], {"author": ".author"})
        self.assertEqual(spec["max_pages"], 3)
        self.assertEqual(spec["delay"], 0)
        self.assertTrue(spec["dedupe"])
        self.assertFalse(spec["ignore_robots"])

    def test_build_spec_select_mode(self):
        self.app.url_var.set("https://example.com/")
        self.app.notebook.select(1)
        self.app.select_var.set(".username")
        spec = self.app._build_spec()
        self.assertEqual(spec["select"], ".username")
        self.assertEqual(spec["record"], None)

    def test_build_spec_validates_input(self):
        with self.assertRaises(ValueError):
            self.app._build_spec()  # no URL
        self.app.url_var.set("ftp://example.com")
        with self.assertRaises(ValueError):
            self.app._build_spec()
        self.app.url_var.set("https://example.com/")
        with self.assertRaises(ValueError):
            self.app._build_spec()  # no record

    def test_fill_form_roundtrip(self):
        self.app._fill_form(harvest_gui.EXAMPLE)
        self.assertEqual(self.app.url_var.get(), harvest_gui.EXAMPLE["url"])
        self.assertEqual(self.app.record_var.get(), ".quote")
        self.assertIn("author=.author", self.app.fields_text.get("1.0", "end"))
        spec = self.app._build_spec()
        self.assertEqual(spec["fields"]["tags"], ".tags a.tag")

    # -- end to end ------------------------------------------------------

    def test_scrape_into_table(self):
        base = f"http://127.0.0.1:{self.port}/page1.html"
        self.app.url_var.set(base)
        self.app.record_var.set(".item")
        self.app.fields_text.insert("1.0", "name=.name\nprice=.price")
        self.app.next_var.set("a.next")
        self.app.max_pages_var.set("2")
        self.app.delay_var.set("0")

        self.app.start()
        deadline = time.time() + 15
        while time.time() < deadline:
            self.root.update()
            if self.app.status_var.get().startswith(("完成", "出错", "参数")):
                break
            time.sleep(0.02)

        self.assertTrue(self.app.status_var.get().startswith("完成"), self.app.status_var.get())
        self.assertEqual(len(self.app.rows), 4)
        self.assertEqual([row["name"] for row in self.app.rows], ["alpha", "beta", "gamma", "delta"])
        self.assertEqual(len(self.app.tree.get_children()), 4)
        self.assertIn("共 4 行", self.app.count_label.cget("text"))
        self.assertIn("name", self.app.tree["columns"])

    def test_scrape_writes_file(self):
        import tempfile

        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, "out.csv")
            self.app.url_var.set(f"http://127.0.0.1:{self.port}/page1.html")
            self.app.record_var.set(".item")
            self.app.fields_text.insert("1.0", "name=.name")
            self.app.max_pages_var.set("1")
            self.app.delay_var.set("0")
            self.app.out_var.set(path)

            self.app.start()
            deadline = time.time() + 15
            while time.time() < deadline:
                self.root.update()
                if self.app.status_var.get().startswith(("完成", "出错", "参数")):
                    break
                time.sleep(0.02)

            self.assertTrue(os.path.exists(path), "output file should be written")
            with open(path, encoding="utf-8", newline="") as handle:
                self.assertEqual(handle.read(), "name\nalpha\nbeta\n")
            self.assertEqual(self.app.output_path, path)

    def test_stop_between_pages(self):
        # A slow delay does not matter here: the worker checks the flag after
        # every page, so stopping must end the run quickly.
        self.app.url_var.set(f"http://127.0.0.1:{self.port}/page1.html")
        self.app.record_var.set(".item")
        self.app.fields_text.insert("1.0", "name=.name")
        self.app.next_var.set("a.next")
        self.app.max_pages_var.set("5")
        self.app.delay_var.set("5000")
        self.app.start()
        self.root.update()
        self.app.stop()
        deadline = time.time() + 10
        while time.time() < deadline:
            self.root.update()
            if self.app.status_var.get() in ("已停止", "出错"):
                break
            time.sleep(0.02)
        self.assertEqual(self.app.status_var.get(), "已停止")


if __name__ == "__main__":
    unittest.main(verbosity=2)
