#!/usr/bin/env python3
"""harvest-gui — a Tkinter front-end for harvest.

    python python/harvest_gui.py

Type a URL, describe what to scrape with CSS selectors, press 开始抓取. The
crawl runs on a worker thread, so the window stays responsive; results are
previewed in a table and can be written to CSV/TSV/JSON/JSONL/TXT.

The module is importable without opening a window (the window is only created in
main()), which is what the tests use.
"""

from __future__ import annotations

import json
import os
import queue
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import harvest  # noqa: E402

import tkinter as tk  # noqa: E402
from tkinter import filedialog, messagebox, ttk  # noqa: E402

APP_TITLE = "harvest — 网页抓取"
PREVIEW_LIMIT = 300

FORMATS = ["自动（按扩展名）", "csv", "tsv", "json", "jsonl", "txt"]
DATA_FORMATS = {"csv", "tsv", "json", "jsonl", "txt"}

EXAMPLE = {
    "url": "https://quotes.toscrape.com/",
    "record": ".quote",
    "fields": "quote=.text\nauthor=.author\ntags=.tags a.tag",
    "next": "li.next a",
    "max_pages": 10,
}


class StopRequested(Exception):
    """Raised from the progress callback when the user presses 停止."""


class HarvestApp:
    def __init__(self, root: tk.Tk):
        self.root = root
        self.queue: "queue.Queue[tuple]" = queue.Queue()
        self.worker: threading.Thread | None = None
        self.stop_event = threading.Event()
        self.rows: list[dict] = []
        self.output_path: str | None = None

        self.url_var = tk.StringVar()
        self.record_var = tk.StringVar()
        self.select_var = tk.StringVar()
        self.next_var = tk.StringVar()
        self.max_pages_var = tk.StringVar(value="10")
        self.delay_var = tk.StringVar(value="300")
        self.dedupe_var = tk.BooleanVar(value=True)
        self.robots_var = tk.BooleanVar(value=True)
        self.out_var = tk.StringVar()
        self.format_var = tk.StringVar(value=FORMATS[0])
        self.status_var = tk.StringVar(value="就绪")

        self._build_ui()
        self._set_running(False)
        self.root.after(80, self._drain)

    # -- layout ----------------------------------------------------------

    def _build_ui(self) -> None:
        style = ttk.Style(self.root)
        for theme in ("vista", "clam", "default"):
            if theme in style.theme_names():
                style.theme_use(theme)
                break
        style.configure("Treeview", rowheight=22)

        self.root.title(APP_TITLE)
        self.root.geometry("1080x800")
        self.root.minsize(880, 620)
        self.root.protocol("WM_DELETE_WINDOW", self._on_close)

        outer = ttk.Frame(self.root, padding=12)
        outer.grid(row=0, column=0, sticky="nsew")
        self.root.columnconfigure(0, weight=1)
        self.root.rowconfigure(0, weight=1)
        outer.columnconfigure(0, weight=1)
        outer.rowconfigure(4, weight=1)

        self._build_url_row(outer, row=0)
        self._build_tabs(outer, row=1)
        self._build_options(outer, row=2)
        self._build_output(outer, row=3)
        self._build_results(outer, row=4)
        self._build_log(outer, row=5)
        self._build_actions(outer, row=6)

        self.root.bind("<Control-Return>", lambda _event: self.start())

    def _build_url_row(self, parent: ttk.Frame, row: int) -> None:
        box = ttk.Frame(parent)
        box.grid(row=row, column=0, sticky="ew", pady=(0, 10))
        box.columnconfigure(1, weight=1)

        ttk.Label(box, text="网址").grid(row=0, column=0, padx=(0, 8))
        entry = ttk.Entry(box, textvariable=self.url_var)
        entry.grid(row=0, column=1, sticky="ew")
        entry.focus_set()

        ttk.Button(box, text="分析页面", command=self.inspect).grid(row=0, column=2, padx=(8, 0))
        ttk.Button(box, text="载入示例", command=self.load_example).grid(row=0, column=3, padx=(6, 0))
        ttk.Button(box, text="载入 recipe", command=self.load_recipe).grid(row=0, column=4, padx=(6, 0))
        ttk.Button(box, text="保存 recipe", command=self.save_recipe).grid(row=0, column=5, padx=(6, 0))

    def _build_tabs(self, parent: ttk.Frame, row: int) -> None:
        self.notebook = ttk.Notebook(parent)
        self.notebook.grid(row=row, column=0, sticky="ew")

        record_tab = ttk.Frame(self.notebook, padding=10)
        record_tab.columnconfigure(1, weight=1)
        ttk.Label(record_tab, text="记录选择器").grid(row=0, column=0, sticky="w")
        ttk.Entry(record_tab, textvariable=self.record_var).grid(row=0, column=1, sticky="ew", padx=(8, 0))
        ttk.Label(
            record_tab,
            text="字段（每行一个：名称=选择器，可用 @属性 取 href / title / class 等）",
        ).grid(row=1, column=0, columnspan=2, sticky="w", pady=(10, 4))

        fields_box = ttk.Frame(record_tab)
        fields_box.grid(row=2, column=0, columnspan=2, sticky="nsew")
        record_tab.rowconfigure(2, weight=1)
        self.fields_text = tk.Text(fields_box, height=6, wrap="none", undo=True, font=("Consolas", 10))
        fields_scroll = ttk.Scrollbar(fields_box, orient="vertical", command=self.fields_text.yview)
        self.fields_text.configure(yscrollcommand=fields_scroll.set)
        self.fields_text.grid(row=0, column=0, sticky="nsew")
        fields_scroll.grid(row=0, column=1, sticky="ns")
        fields_box.columnconfigure(0, weight=1)

        select_tab = ttk.Frame(self.notebook, padding=10)
        select_tab.columnconfigure(1, weight=1)
        ttk.Label(select_tab, text="选择器").grid(row=0, column=0, sticky="w")
        ttk.Entry(select_tab, textvariable=self.select_var).grid(row=0, column=1, sticky="ew", padx=(8, 0))
        ttk.Label(
            select_tab,
            text="每个匹配输出一个值，导出成单列（适合“抓这个页面上的所有用户名”）。",
        ).grid(row=1, column=0, columnspan=2, sticky="w", pady=(10, 0))

        self.notebook.add(record_tab, text="  表格：每条记录一行  ")
        self.notebook.add(select_tab, text="  单列：每个匹配一条  ")

    def _build_options(self, parent: ttk.Frame, row: int) -> None:
        box = ttk.LabelFrame(parent, text="翻页与网络", padding=10)
        box.grid(row=row, column=0, sticky="ew", pady=(10, 0))
        box.columnconfigure(1, weight=1)

        ttk.Label(box, text="下一页链接选择器").grid(row=0, column=0, sticky="w")
        ttk.Entry(box, textvariable=self.next_var).grid(row=0, column=1, sticky="ew", padx=(8, 12))
        ttk.Label(box, text="（留空表示只抓一页）").grid(row=0, column=2, sticky="w")

        stats = ttk.Frame(box)
        stats.grid(row=1, column=0, columnspan=3, sticky="w", pady=(8, 0))
        ttk.Label(stats, text="最多页数").pack(side="left")
        ttk.Spinbox(stats, from_=0, to=9999, width=6, textvariable=self.max_pages_var).pack(
            side="left", padx=(6, 16)
        )
        ttk.Label(stats, text="请求间隔 (ms)").pack(side="left")
        ttk.Spinbox(stats, from_=0, to=60000, increment=100, width=7, textvariable=self.delay_var).pack(
            side="left", padx=(6, 16)
        )
        ttk.Checkbutton(stats, text="去重", variable=self.dedupe_var).pack(side="left", padx=(0, 16))
        ttk.Checkbutton(stats, text="遵守 robots.txt", variable=self.robots_var).pack(side="left")
        ttk.Label(stats, text="（0 页 = 一直翻到没有下一页）", foreground="#666").pack(side="left", padx=(12, 0))

    def _build_output(self, parent: ttk.Frame, row: int) -> None:
        box = ttk.Frame(parent)
        box.grid(row=row, column=0, sticky="ew", pady=(10, 0))
        box.columnconfigure(1, weight=1)

        ttk.Label(box, text="保存到").grid(row=0, column=0, padx=(0, 8))
        ttk.Entry(box, textvariable=self.out_var).grid(row=0, column=1, sticky="ew")
        ttk.Button(box, text="浏览…", command=self.browse_output).grid(row=0, column=2, padx=(8, 0))
        ttk.Combobox(box, textvariable=self.format_var, values=FORMATS, width=16, state="readonly").grid(
            row=0, column=3, padx=(8, 0)
        )

    def _build_results(self, parent: ttk.Frame, row: int) -> None:
        box = ttk.LabelFrame(parent, text="结果", padding=6)
        box.grid(row=row, column=0, sticky="nsew", pady=(10, 0))
        box.columnconfigure(0, weight=1)
        box.rowconfigure(1, weight=1)

        header = ttk.Frame(box)
        header.grid(row=0, column=0, columnspan=2, sticky="ew")
        header.columnconfigure(0, weight=1)
        self.count_label = ttk.Label(header, text="还没有结果")
        self.count_label.grid(row=0, column=0, sticky="w")
        self.open_button = ttk.Button(header, text="打开文件", command=self.open_output, state="disabled")
        self.open_button.grid(row=0, column=1, sticky="e")

        self.tree = ttk.Treeview(box, show="headings", selectmode="browse")
        self.tree.grid(row=1, column=0, sticky="nsew")
        y_scroll = ttk.Scrollbar(box, orient="vertical", command=self.tree.yview)
        y_scroll.grid(row=1, column=1, sticky="ns")
        x_scroll = ttk.Scrollbar(box, orient="horizontal", command=self.tree.xview)
        x_scroll.grid(row=2, column=0, sticky="ew")
        self.tree.configure(yscrollcommand=y_scroll.set, xscrollcommand=x_scroll.set)

    def _build_log(self, parent: ttk.Frame, row: int) -> None:
        box = ttk.LabelFrame(parent, text="日志", padding=6)
        box.grid(row=row, column=0, sticky="ew", pady=(10, 0))
        box.columnconfigure(0, weight=1)

        self.log = tk.Text(box, height=7, wrap="word", state="disabled", font=("Consolas", 9))
        self.log.grid(row=0, column=0, sticky="nsew")
        scroll = ttk.Scrollbar(box, orient="vertical", command=self.log.yview)
        scroll.grid(row=0, column=1, sticky="ns")
        self.log.configure(yscrollcommand=scroll.set)

    def _build_actions(self, parent: ttk.Frame, row: int) -> None:
        bar = ttk.Frame(parent)
        bar.grid(row=row, column=0, sticky="ew", pady=(10, 0))
        bar.columnconfigure(2, weight=1)

        self.start_button = ttk.Button(bar, text="开始抓取", command=self.start)
        self.start_button.grid(row=0, column=0)
        self.stop_button = ttk.Button(bar, text="停止", command=self.stop)
        self.stop_button.grid(row=0, column=1, padx=(8, 12))
        self.progress = ttk.Progressbar(bar, mode="indeterminate", length=150)
        self.progress.grid(row=0, column=2, sticky="w")
        ttk.Label(bar, textvariable=self.status_var).grid(row=0, column=3, sticky="e")

    # -- small helpers ---------------------------------------------------

    @staticmethod
    def _parse_fields(text: str) -> dict:
        fields = {}
        for line in text.splitlines():
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            name, separator, selector = line.partition("=")
            if not separator or not name.strip() or not selector.strip():
                raise ValueError(f'字段格式应为 "名称=选择器"，无法解析：{line}')
            fields[name.strip()] = selector.strip()
        return fields

    @staticmethod
    def _int(value: str, label: str, minimum: int) -> int:
        try:
            number = int(str(value).strip())
        except (TypeError, ValueError):
            raise ValueError(f"{label} 需要是整数，当前值：{value!r}") from None
        if number < minimum:
            raise ValueError(f"{label} 不能小于 {minimum}")
        return number

    def _format_choice(self) -> str | None:
        choice = self.format_var.get()
        return choice if choice in DATA_FORMATS else None

    def _log(self, text: str) -> None:
        self.log.configure(state="normal")
        self.log.insert("end", f"[{time.strftime('%H:%M:%S')}] {text}\n")
        self.log.see("end")
        self.log.configure(state="disabled")

    def _set_running(self, running: bool) -> None:
        if running:
            self.start_button.state(["disabled"])
            self.stop_button.state(["!disabled"])
            self.progress.start(12)
        else:
            self.start_button.state(["!disabled"])
            self.stop_button.state(["disabled"])
            self.progress.stop()

    # -- form <-> spec ---------------------------------------------------

    def _build_spec(self) -> dict:
        url = self.url_var.get().strip()
        if not url:
            raise ValueError("请填写网址")
        if not url.lower().startswith(("http://", "https://")):
            raise ValueError("网址需要以 http:// 或 https:// 开头")

        spec = harvest.default_spec()
        spec["url"] = url
        spec["next"] = self.next_var.get().strip()
        spec["max_pages"] = self._int(self.max_pages_var.get(), "最多页数", 0)
        spec["delay"] = self._int(self.delay_var.get(), "请求间隔", 0)
        spec["dedupe"] = bool(self.dedupe_var.get())
        spec["ignore_robots"] = not bool(self.robots_var.get())
        spec["quiet"] = True
        spec["retries"] = 1

        if self.notebook.index(self.notebook.select()) == 0:
            record = self.record_var.get().strip()
            if not record:
                raise ValueError("请填写记录选择器")
            fields = self._parse_fields(self.fields_text.get("1.0", "end"))
            if not fields:
                raise ValueError("请至少填写一个字段")
            spec["record"] = record
            spec["fields"] = fields
        else:
            selector = self.select_var.get().strip()
            if not selector:
                raise ValueError("请填写选择器")
            spec["select"] = selector
        return spec

    def _fill_form(self, data: dict) -> None:
        self.url_var.set(data.get("url", ""))
        self.next_var.set(data.get("next") or "")
        self.max_pages_var.set(str(data.get("maxPages", data.get("max_pages", 10))))
        self.delay_var.set(str(data.get("delay", 300)))
        self.dedupe_var.set(bool(data.get("dedupe", True)))
        self.robots_var.set(not bool(data.get("ignoreRobots", data.get("ignore_robots", False))))
        fields = data.get("fields")
        if isinstance(fields, dict):
            text = "\n".join(f"{name}={spec}" for name, spec in fields.items())
        elif isinstance(fields, str):
            text = fields
        else:
            text = ""
        self.fields_text.delete("1.0", "end")
        self.fields_text.insert("1.0", text)
        self.record_var.set(data.get("record") or "")
        self.select_var.set(data.get("select") or "")
        self.notebook.select(1 if data.get("select") else 0)

    # -- actions ---------------------------------------------------------

    def load_example(self) -> None:
        self._fill_form(EXAMPLE)
        self._log("已填入示例：quotes.toscrape.com 的语录表格。")

    def save_recipe(self) -> None:
        try:
            spec = self._build_spec()
        except ValueError as error:
            messagebox.showerror(APP_TITLE, str(error))
            return
        recipe = {
            "name": "gui-recipe",
            "url": spec["url"],
            "next": spec["next"] or None,
            "maxPages": spec["max_pages"],
            "delay": spec["delay"],
            "dedupe": spec["dedupe"],
            "ignoreRobots": spec["ignore_robots"],
        }
        if spec["select"]:
            recipe["select"] = spec["select"]
        else:
            recipe["record"] = spec["record"]
            recipe["fields"] = spec["fields"]
        path = filedialog.asksaveasfilename(
            title="保存 recipe", defaultextension=".json", filetypes=[("Recipe", "*.json"), ("All files", "*.*")]
        )
        if not path:
            return
        with open(path, "w", encoding="utf-8") as handle:
            json.dump(recipe, handle, ensure_ascii=False, indent=2)
        self._log(f"recipe 已保存：{path}")

    def load_recipe(self) -> None:
        path = filedialog.askopenfilename(
            title="载入 recipe", filetypes=[("Recipe", "*.json"), ("All files", "*.*")]
        )
        if not path:
            return
        try:
            with open(path, "r", encoding="utf-8") as handle:
                data = json.load(handle)
            if not isinstance(data, dict):
                raise ValueError("recipe 必须是一个 JSON 对象")
        except (OSError, ValueError, json.JSONDecodeError) as error:
            messagebox.showerror(APP_TITLE, f"无法读取 recipe：{error}")
            return
        self._fill_form(data)
        self._log(f"recipe 已载入：{path}")

    def browse_output(self) -> None:
        path = filedialog.asksaveasfilename(
            title="保存结果",
            defaultextension=".csv",
            filetypes=[("CSV", "*.csv"), ("JSON", "*.json"), ("JSON Lines", "*.jsonl"),
                       ("TSV", "*.tsv"), ("Text", "*.txt"), ("All files", "*.*")],
        )
        if path:
            self.out_var.set(path)

    def open_output(self) -> None:
        if not self.output_path or not os.path.exists(self.output_path):
            return
        try:
            if sys.platform.startswith("win"):
                os.startfile(self.output_path)  # type: ignore[attr-defined]
            elif sys.platform == "darwin":
                os.system(f'open "{self.output_path}"')
            else:
                os.system(f'xdg-open "{self.output_path}"')
        except OSError as error:
            messagebox.showerror(APP_TITLE, f"无法打开文件：{error}")

    def start(self) -> None:
        if self.worker and self.worker.is_alive():
            return
        try:
            spec = self._build_spec()
        except ValueError as error:
            self._log(f"⚠ {error}")
            self.status_var.set("参数不完整")
            return

        self.stop_event.clear()
        self._clear_results()
        self._set_running(True)
        self.status_var.set("抓取中…")
        self._log(f"开始：{spec['url']}")
        output = self.out_var.get().strip() or None
        # Tk variables may only be read on the main thread, so resolve the
        # output format here and hand it to the worker.
        self.worker = threading.Thread(target=self._run, args=(spec, output, self._format_choice()), daemon=True)
        self.worker.start()

    def stop(self) -> None:
        self.stop_event.set()
        self.status_var.set("正在停止…")
        self._log("已请求停止，当前页完成后退出。")

    def _run(self, spec: dict, output: str | None, format_choice: str | None) -> None:
        try:
            rows, pages = harvest.crawl(spec, log=self._thread_log)
            written = None
            if output:
                written = output
                harvest.write_output(rows, output, format_choice)
            self.queue.put(("done", rows, pages, written))
        except StopRequested:
            self.queue.put(("stopped",))
        except harvest.HarvestError as error:
            self.queue.put(("error", str(error)))
        except Exception as error:  # noqa: BLE001 - surface anything to the UI
            self.queue.put(("error", f"{type(error).__name__}: {error}"))

    def _thread_log(self, page: int, url: str, count: int) -> None:
        self.queue.put(("log", f"第 {page} 页：{url}（累计 {count} 行）"))
        if self.stop_event.is_set():
            raise StopRequested

    def inspect(self) -> None:
        url = self.url_var.get().strip()
        if not url.lower().startswith(("http://", "https://")):
            messagebox.showwarning(APP_TITLE, "请先填写一个 http(s) 网址")
            return
        self._log(f"分析页面：{url}")
        threading.Thread(target=self._inspect_worker, args=(url,), daemon=True).start()

    def _inspect_worker(self, url: str) -> None:
        try:
            html = harvest.http_get(url, timeout=20, user_agent=harvest.DEFAULT_UA, retries=1)
            report = harvest.inspect_page(harvest.parse_html(html))
            self.queue.put(("inspect", report))
        except Exception as error:  # noqa: BLE001
            self.queue.put(("error", f"分析失败：{error}"))

    # -- queue plumbing --------------------------------------------------

    def _drain(self) -> None:
        try:
            while True:
                message = self.queue.get_nowait()
                self._handle(message)
        except queue.Empty:
            pass
        self.root.after(80, self._drain)

    def _handle(self, message: tuple) -> None:
        kind = message[0]
        if kind == "log":
            self._log(message[1])
        elif kind == "inspect":
            self._log("页面里重复出现的元素：")
            self.log.configure(state="normal")
            self.log.insert("end", message[1] + "\n")
            self.log.see("end")
            self.log.configure(state="disabled")
        elif kind == "done":
            _, rows, pages, written = message
            self._finish(rows, pages, written)
        elif kind == "stopped":
            self._set_running(False)
            self.status_var.set("已停止")
            self._log("已停止。")
        elif kind == "error":
            self._set_running(False)
            self.status_var.set("出错")
            self._log(f"✗ {message[1]}")

    def _finish(self, rows: list[dict], pages: int, written: str | None) -> None:
        self._set_running(False)
        self.rows = rows
        self._fill_table(rows)
        self.output_path = written
        if written:
            self.open_button.state(["!disabled"])
            self.status_var.set(f"完成：{len(rows)} 行 / {pages} 页 → {os.path.basename(written)}")
            self._log(f"已写入 {written}（{len(rows)} 行，{pages} 页）")
        else:
            self.status_var.set(f"完成：{len(rows)} 行 / {pages} 页（未保存）")
            self._log(f"抓取完成：{len(rows)} 行，{pages} 页。")

    def _clear_results(self) -> None:
        for item in self.tree.get_children():
            self.tree.delete(item)
        self.tree.configure(columns=())
        self.count_label.configure(text="抓取中…")
        self.open_button.state(["disabled"])
        self.output_path = None

    def _fill_table(self, rows: list[dict]) -> None:
        columns = harvest.column_names(rows)
        self.tree.configure(columns=columns)
        for name in columns:
            longest = max([len(name)] + [len(str(row.get(name, ""))) for row in rows[:80]])
            self.tree.heading(name, text=name)
            self.tree.column(name, width=max(90, min(360, longest * 8 + 24)), anchor="w", stretch=True)
        for row in rows[:PREVIEW_LIMIT]:
            self.tree.insert("", "end", values=[row.get(column, "") for column in columns])
        shown = min(len(rows), PREVIEW_LIMIT)
        suffix = "" if len(rows) <= PREVIEW_LIMIT else f"（预览前 {PREVIEW_LIMIT} 行）"
        self.count_label.configure(text=f"共 {len(rows)} 行，显示 {shown} 行{suffix}")

    def _on_close(self) -> None:
        self.stop_event.set()
        self.root.destroy()


def main() -> int:
    try:  # crisp text on high DPI Windows displays
        from ctypes import windll

        windll.shcore.SetProcessDpiAwareness(1)
    except Exception:  # noqa: BLE001 - non-Windows or old Windows
        pass

    root = tk.Tk()
    HarvestApp(root)
    root.mainloop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
