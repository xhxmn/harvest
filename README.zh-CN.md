# harvest

**给它一个 URL 和几条选择器，它给你数据行。**

一个用两种语言实现的声明式爬虫：**Python**（只用标准库）和 **C++**（单文件，
除 `curl` 外无依赖）。两者共用同一套命令行和 recipe 格式，输出**逐字节一致**。

```bash
python python/harvest.py --recipe recipes/authors.json --out results/authors.txt
./cpp/harvest            --recipe recipes/authors.json --out results/authors.txt
# 两个文件 SHA-256 相同
```

<p align="center">
  <a href="https://github.com/xhxmn/harvest/actions/workflows/ci.yml"><img alt="CI" src="https://github.com/xhxmn/harvest/actions/workflows/ci.yml/badge.svg"></a>
  <img alt="Python 3.8+" src="https://img.shields.io/badge/python-3.8%2B-blue">
  <img alt="C++17" src="https://img.shields.io/badge/c%2B%2B-17-blue">
  <img alt="Dependencies: none" src="https://img.shields.io/badge/dependencies-none-brightgreen">
  <img alt="License: MIT" src="https://img.shields.io/badge/license-MIT-green">
</p>

---

## 能做什么

| 你想要 | 怎么配 |
| --- | --- |
| 抓某页所有条目及其字段 | `--record` + `--field` |
| 抓某个 URL 下所有用户名（含翻页） | `--select` + `--next` + `--dedupe` |
| 抓整站目录 | `--record` + `--next` + `--max-pages` |
| 只导出一列纯文本 | `--select` + `--format txt` |

你举的例子——「给一个视频网站和一个用户名，爬到该 URL 下所有用户名」——对应的
命令就是这个形状（把选择器换成目标站点的即可）：

```bash
harvest https://example.com/comments \
  --select ".comment .username" \
  --next "a.next-page" \
  --max-pages 0 \
  --dedupe \
  --out usernames.txt
```

只抓你有权抓的页面：看站点条款、保留默认延时、不要收集不该收集的个人信息。

## 快速开始

### Python

无需安装任何东西。

```bash
python python/harvest.py --inspect https://quotes.toscrape.com/
```

`--inspect` 会列出页面上重复出现的元素组，用来快速挑选选择器：

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

然后直接抓：

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

需要 C++17 编译器和 `curl`（Windows 10+ / macOS / 主流 Linux 自带）。

```bash
g++ -std=c++17 -O2 cpp/harvest.cpp -o harvest     # 或者 make
./harvest --recipe recipes/books.json --out results/books.csv
```

Windows 下运行 `.\cpp\build.ps1` 可同时编译工具和测试程序。

## 命令行

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

字段默认取元素文本，加 `@attr` 取属性：

```
--field "url=h3 a@href"      # 相对链接会按页面 URL 补全
--field "rating=p@class"     # 读取 class="star-rating Three"
--field "html=.body@html"    # 取内部 HTML
```

### 选择器支持

标签、`*`、`.class`、`#id`、`[attr]`、`[attr=value]`、`[attr^=]`、`[attr$=]`、
`[attr*=]`、`[attr~=]`、`[attr|=]`，后代与 `>` 子元素组合，`,` 分组，以及
`:first-child`、`:last-child`、`:nth-child(...)`、`:not(...)`。不做 XPath，
不执行 JavaScript。

## Recipe

recipe 就是把这些参数写成 JSON，方便复用和 review：

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

命令行参数优先于 recipe，所以同一个 recipe 可以加 `--max-pages 2` 先试跑。

## 已跑出的结果

下面这些文件就是本仓库 recipe 的真实产物；两种实现写出的文件逐字节相同。

| Recipe | 结果 | 内容 |
| --- | --- | --- |
| `recipes/authors.json` | [`results/authors.txt`](results/authors.txt) | 10 页 100 条 quote 里的 50 位不同作者 |
| `recipes/quotes.json` | [`results/quotes.csv`](results/quotes.csv) | 100 条 quote，含作者、标签、作者主页 |
| `recipes/books.json` | [`results/books.csv`](results/books.csv) | 1000 本书，含价格、评分、库存、商品链接 |

`results/books.csv` 里的一行：

```csv
title,price,rating,availability,url
A Light in the Attic,£51.77,star-rating Three,In stock,https://books.toscrape.com/catalogue/a-light-in-the-attic_1000/index.html
```

两个站点都是公开的爬虫练习站。

## 输出格式

| 格式 | 形式 |
| --- | --- |
| `csv` / `tsv` | 表头 + 行，RFC 4180 引号规则，`\n` 换行 |
| `json` | 对象数组，两空格缩进 |
| `jsonl` | 每行一个对象 |
| `txt` | `--select` 模式下一行一个值，否则用 `" \| "` 连接字段 |

## 礼貌与合规

- 默认读取并遵守 `robots.txt`（`--ignore-robots` 可关闭）。
- 请求串行、默认间隔 1000 ms（`--delay` 可调）。
- 发送可识别的 User-Agent（`--ua` 可改）。
- 临时性失败会按退避策略重试。
- 只做普通 GET：不执行 JavaScript、不带 cookie、不登录。

请只抓你有权限读取的页面，并留意站点服务条款；即使技术上可行，批量收集个人
信息在很多地区也是违法的。

## 测试

```bash
make test
python -m unittest discover -s python -p "test_*.py"   # 44 个测试
./cpp/test_harvest                                     # 80 项检查
```

一致性验证就是两条命令加一次哈希比较：

```bash
python python/harvest.py --recipe recipes/quotes.json --out py.csv --quiet
./cpp/harvest            --recipe recipes/quotes.json --out cpp.csv --quiet
sha256sum py.csv cpp.csv
```

不想联网也可以：`fixtures/` 里有一个迷你站点，CI 就是用它对两版输出做逐字节比较的。

```bash
python -m http.server 8765 --directory fixtures &
python python/harvest.py http://127.0.0.1:8765/page1.html --record .item \
  --field "name=.name" --next "a.next" --max-pages 2 --delay 0 --out py.csv --quiet
./cpp/harvest http://127.0.0.1:8765/page1.html --record .item \
  --field "name=.name" --next "a.next" --max-pages 2 --delay 0 --out cpp.csv --quiet
cmp py.csv cpp.csv
```

## 项目结构

```
harvest/
├── python/
│   ├── harvest.py           # 完整实现，仅标准库
│   └── test_harvest.py      # 44 个 unittest 用例
├── cpp/
│   ├── harvest.cpp          # 完整实现，C++17，单文件
│   └── test_harvest.cpp     # 80 项检查，直接 include harvest.cpp
├── recipes/                 # 开箱即用的抓取配方
├── results/                 # 真实抓取结果，已提交
├── fixtures/                # 离线一致性测试用的迷你站点
├── Makefile                 # make / make test / make demo
└── cpp/build.ps1            # 没有 make 时的 Windows 构建脚本
```

## 限制

- 不执行 JavaScript：数据如果只在脚本执行后出现，这个工具抓不到（那种场景要用
  浏览器自动化）。
- C++ 版按 UTF-8 处理页面；Python 版会遵循响应里的 charset。
- CSS 引擎是有意做的子集，不是完整的 Selectors Level 4。

## 许可证

[MIT](LICENSE)
