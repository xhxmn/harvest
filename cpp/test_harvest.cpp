// Tests for harvest.cpp — build with:
//   g++ -std=c++17 -O2 -Wall -Wextra cpp/test_harvest.cpp -o cpp/test_harvest
#define HARVEST_NO_MAIN
#include "harvest.cpp"

#include <cstdio>
#include <map>
#include <string>
#include <vector>

using namespace harvest;

static int checks = 0;
static int failures = 0;

static void check(bool condition, const std::string& name) {
  ++checks;
  if (!condition) {
    std::printf("FAIL: %s\n", name.c_str());
    ++failures;
  }
}

static void equalText(const std::string& actual, const std::string& expected, const std::string& name) {
  ++checks;
  if (actual != expected) {
    std::printf("FAIL: %s\n  expected: [%s]\n  actual:   [%s]\n", name.c_str(), expected.c_str(),
                actual.c_str());
    ++failures;
  }
}

static void equalInt(long actual, long expected, const std::string& name) {
  ++checks;
  if (actual != expected) {
    std::printf("FAIL: %s\n  expected: %ld\n  actual:   %ld\n", name.c_str(), expected, actual);
    ++failures;
  }
}

/** Join the normalised text of every match with '|' so one call can check a list. */
static std::string texts(const std::string& html, const std::string& selector) {
  const auto document = parseHTML(html);
  std::string out;
  for (const Node* node : queryAll(document.get(), selector)) {
    out += normalizeSpace(textContent(node)) + "|";
  }
  return out;
}

static size_t count(const std::string& html, const std::string& selector) {
  const auto document = parseHTML(html);
  return queryAll(document.get(), selector).size();
}

static std::string normalized(const std::string& html) {
  const auto document = parseHTML(html);
  return normalizeSpace(textContent(document.get()));
}

static Spec testSpec() {
  Spec spec;
  spec.url = "https://s.test/1";
  spec.record = ".item";
  spec.fields = {{"name", "span"}};
  spec.next = "a.next";
  spec.maxPages = 10;
  spec.ignoreRobots = true;
  spec.delay = 0;
  spec.timeout = 1000;
  spec.retries = 0;
  spec.quiet = true;
  return spec;
}

static FetchFn fakeFetch(std::map<std::string, std::string> pages) {
  return [pages](const std::string& url, int, const std::string&, int) -> std::string {
    const auto found = pages.find(url);
    if (found == pages.end()) throw std::runtime_error("404 " + url);
    return found->second;
  };
}

static void testHtml() {
  equalText(texts("<div><p>hello <b>world</b></p></div>", "div p b"), "world|", "nested elements");
  equalText(normalized("<div><p>hello <b>world</b></p></div>"), "hello world", "text content");

  const auto document = parseHTML("<img src=/a.png alt=\"A picture\"><input type=text disabled>");
  const Node* image = queryOne(document.get(), "img");
  check(image != nullptr, "img found");
  equalText(attributeOr(image, "src"), "/a.png", "unquoted attribute");
  equalText(attributeOr(image, "alt"), "A picture", "quoted attribute");
  const Node* input = queryOne(document.get(), "input");
  equalText(attributeOr(input, "type"), "text", "attribute without quotes");
  equalText(attributeOr(input, "disabled"), "", "boolean attribute");

  equalText(texts("<p>one<br>two</p><p>three</p>", "p"), "one two|three|", "void element siblings");
  equalText(normalized("<p>a &amp; b &lt;c&gt; &#233; &#x2014; &hellip;</p>"), "a & b <c> \u00e9 \u2014 \u2026",
            "entities");
  equalText(texts("<p>first<p>second", "p"), "first|second|", "implicit paragraph close");
  equalText(normalized("<div><span>unclosed</div>after"), "unclosed after", "malformed markup");
  equalText(normalized("<div>keep<script>drop()</script><style>p{}</style></div>"), "keep", "script/style");
  equalText(normalized("<p>1 < 2</p>"), "1 < 2", "stray angle bracket");
}

static void testSelectors() {
  const std::string html = R"(
    <div id="main" class="page wide">
      <ul class="list">
        <li class="item first">one</li>
        <li class="item">two</li>
        <li class="item last" data-kind="x">three</li>
      </ul>
      <a href="/next">next</a>
      <a href="https://example.com/abs">abs</a>
    </div>)";

  equalInt(count(html, "li"), 3, "tag");
  equalInt(count(html, ".item"), 3, "class");
  equalInt(count(html, "li.item"), 3, "tag.class");
  equalInt(count(html, "#main"), 1, "id");
  equalInt(count(html, ".page.wide"), 1, "two classes");
  equalInt(count(html, ".page.nope"), 0, "missing class");
  equalInt(count(html, "div li"), 3, "descendant");
  equalInt(count(html, "ul > li"), 3, "child");
  equalInt(count(html, "div > li"), 0, "child does not match grandchildren");
  equalInt(count(html, "[data-kind]"), 1, "attribute exists");
  equalInt(count(html, "[data-kind=\"x\"]"), 1, "attribute equals");
  equalInt(count(html, "a[href^=\"/\"]"), 1, "attribute prefix");
  equalInt(count(html, "a[href$=\"abs\"]"), 1, "attribute suffix");
  equalInt(count(html, "a[href*=\"example\"]"), 1, "attribute contains");
  equalText(texts(html, "li:first-child"), "one|", "first-child");
  equalText(texts(html, "li:last-child"), "three|", "last-child");
  equalText(texts(html, "li:nth-child(2)"), "two|", "nth-child");
  equalInt(count(html, "li:not(.last)"), 2, ":not");
  equalText(texts(html, "li:first-child, a"), "one|next|abs|", "selector group");
  check(count(html, "*") > 5, "universal selector");
}

static void testExtract() {
  const std::string html = R"(
    <article class="post"><h2>First</h2><a href="/a">link</a></article>
    <article class="post"><h2>Second</h2><a href="/b">link</a></article>)";
  const auto document = parseHTML(html);

  Spec spec;
  spec.record = ".post";
  spec.fields = {{"title", "h2"}, {"url", "a@href"}};
  const auto rows = extractRows(document.get(), "https://site.test/blog/", spec);
  equalInt(static_cast<long>(rows.size()), 2, "two records");
  equalText(rows[0][0].second, "First", "record title");
  equalText(rows[0][1].second, "https://site.test/a", "record url is absolutised");

  Spec missing;
  missing.record = ".x";
  missing.fields = {{"a", "span"}, {"b", "b"}};
  const auto missingRows =
      extractRows(parseHTML("<div class=\"x\"><span>v</span></div>").get(), "https://s.test/", missing);
  equalText(missingRows[0][0].second, "v", "present field");
  equalText(missingRows[0][1].second, "", "absent field is empty");

  Spec joined;
  joined.record = ".x";
  joined.fields = {{"t", "a"}};
  const auto joinedRows =
      extractRows(parseHTML("<div class=\"x\"><a>1</a><a>2</a></div>").get(), "https://s.test/", joined);
  equalText(joinedRows[0][0].second, "1 | 2", "multiple matches are joined");

  Spec select;
  select.select = "a@href";
  const auto selectRows =
      extractRows(parseHTML("<a href=\"/one\">1</a><a href=\"/two\">2</a>").get(), "https://s.test/", select);
  equalText(selectRows[0][0].second, "https://s.test/one", "select mode with attribute");
  equalText(selectRows[1][0].second, "https://s.test/two", "second select row");

  const auto parsed = parseField("a@href");
  equalText(parsed.first, "a", "parseField selector");
  equalText(parsed.second, "href", "parseField attribute");
  equalText(parseField(".author").second, "text", "parseField defaults to text");

  const auto nextDocument = parseHTML("<nav><a class=\"next\" href=\"page/2/\">Next</a></nav>");
  equalText(findNext(nextDocument.get(), "a.next", "https://s.test/page/1/"),
            "https://s.test/page/1/page/2/", "findNext resolves relative links");
  const auto container = parseHTML("<li class=\"next\"><a href=\"/p2\">Next</a></li>");
  equalText(findNext(container.get(), "li.next", "https://s.test/"), "https://s.test/p2",
            "findNext accepts a container");
}

static void testUrls() {
  equalText(resolveUrl("https://s.test/a/b", "/x"), "https://s.test/x", "absolute path");
  equalText(resolveUrl("https://s.test/a/b", "c"), "https://s.test/a/c", "relative path");
  equalText(resolveUrl("https://s.test/a/b/", "../c"), "https://s.test/a/c", "parent segment");
  equalText(resolveUrl("https://s.test/a/b", "//cdn.test/x"), "https://cdn.test/x", "protocol relative");
  equalText(resolveUrl("https://s.test/a/b", "https://other.test/z"), "https://other.test/z", "absolute url");
  equalText(resolveUrl("https://s.test/a/b?q=1#f", "?x=2"), "https://s.test/a/b?x=2", "query replacement");
}

static void testOutput() {
  std::vector<Row> rows = {
      {{"name", "a, b"}, {"note", "say \"hi\""}},
      {{"name", "plain"}, {"note", "line\nbreak"}},
  };
  equalText(render(rows, "csv"),
            "name,note\n\"a, b\",\"say \"\"hi\"\"\"\nplain,\"line\nbreak\"\n", "csv quoting");

  std::vector<Row> simple = {{{"a", "1"}, {"b", "2"}}};
  equalText(render(simple, "tsv"), "a\tb\n1\t2\n", "tsv");
  equalText(render(simple, "json"), "[\n  {\n    \"a\": \"1\",\n    \"b\": \"2\"\n  }\n]\n", "json");
  equalText(render(simple, "jsonl"), "{\"a\": \"1\", \"b\": \"2\"}\n", "jsonl");
  equalText(render({{{ "value", "x" }}, {{ "value", "y" }}}, "txt"), "x\ny\n", "txt");
  equalText(render({}, "json"), "[]\n", "empty json");
  equalText(render({{{ "a", "q\"\\\n" }}}, "json"), "[\n  {\n    \"a\": \"q\\\"\\\\\\n\"\n  }\n]\n",
            "json escaping");
}

static void testCrawl() {
  std::map<std::string, std::string> pages = {
      {"https://s.test/1", "<div class=\"item\"><span>a</span></div><a class=\"next\" href=\"/2\">n</a>"},
      {"https://s.test/2", "<div class=\"item\"><span>b</span></div><a class=\"next\" href=\"/3\">n</a>"},
      {"https://s.test/3", "<div class=\"item\"><span>c</span></div>"},
  };
  auto result = crawl(testSpec(), fakeFetch(pages));
  equalInt(static_cast<long>(result.first.size()), 3, "pagination collects every page");
  equalInt(result.second, 3, "page count");
  equalText(result.first[2][0].second, "c", "last row");

  Spec limited = testSpec();
  limited.maxPages = 1;
  auto onePage = crawl(limited, fakeFetch(pages));
  equalInt(onePage.second, 1, "max-pages stops early");

  pages["https://s.test/1"] = "<div class=\"item\"><span>a</span></div><div class=\"item\"><span>a</span></div>";
  Spec deduped = testSpec();
  deduped.dedupe = true;
  auto unique = crawl(deduped, fakeFetch(pages));
  equalInt(static_cast<long>(unique.first.size()), 1, "dedupe");

  std::map<std::string, std::string> loop = {
      {"https://s.test/1", "<div class=\"item\"><span>a</span></div><a class=\"next\" href=\"/1\">n</a>"}};
  auto looped = crawl(testSpec(), fakeFetch(loop));
  equalInt(looped.second, 1, "loop detection");

  std::map<std::string, std::string> privatePages = {
      {"https://s.test/robots.txt", "User-agent: *\nDisallow: /private"},
      {"https://s.test/private", "<div class=\"item\"><span>a</span></div>"}};
  Spec guarded = testSpec();
  guarded.url = "https://s.test/private";
  guarded.ignoreRobots = false;
  bool blocked = false;
  try {
    crawl(guarded, fakeFetch(privatePages));
  } catch (const std::exception&) {
    blocked = true;
  }
  check(blocked, "robots.txt disallow is enforced");

  Spec unguarded = testSpec();
  unguarded.ignoreRobots = false;
  auto allowed = crawl(unguarded, fakeFetch({{"https://s.test/1", "<div class=\"item\"><span>a</span></div>"}}));
  equalInt(static_cast<long>(allowed.first.size()), 1, "missing robots.txt allows");
}

static void testCli() {
  const auto defaults = parseArgs({"https://s.test/"});
  equalText(defaults.url, "https://s.test/", "url positional");
  equalInt(defaults.maxPages, 1, "default max-pages");
  equalInt(defaults.delay, 1000, "default delay");
  equalText(defaults.join, " | ", "default join");

  const auto parsed = parseArgs({"https://s.test/", "--record=.p", "--field", "a=.x", "--field=b=.y@href",
                                 "--max-pages=0", "--dedupe"});
  equalText(parsed.record, ".p", "equals form");
  equalInt(static_cast<long>(parsed.fields.size()), 2, "repeated fields");
  equalText(parsed.fields[1].second, ".y@href", "field value");
  equalInt(parsed.maxPages, 0, "max-pages zero");
  check(parsed.dedupe, "dedupe flag");

  bool rejected = false;
  try {
    parseArgs({"https://s.test/", "--field", "nope"});
  } catch (const std::exception&) {
    rejected = true;
  }
  check(rejected, "bad --field is rejected");

  rejected = false;
  try {
    parseArgs({"https://s.test/", "--wat"});
  } catch (const std::exception&) {
    rejected = true;
  }
  check(rejected, "unknown option is rejected");

  rejected = false;
  try {
    parseArgs({"https://s.test/", "--limit", "many"});
  } catch (const std::exception&) {
    rejected = true;
  }
  check(rejected, "non-numeric integer is rejected");

  Spec conflict = parseArgs({"https://s.test/", "--select", ".a", "--record", ".p", "--field", "x=.y"});
  rejected = false;
  try {
    validate(conflict);
  } catch (const std::exception&) {
    rejected = true;
  }
  check(rejected, "select and record conflict");
}

int main() {
#ifdef _WIN32
  SetConsoleOutputCP(CP_UTF8);
#endif
  testHtml();
  testSelectors();
  testExtract();
  testUrls();
  testOutput();
  testCrawl();
  testCli();

  std::printf("%d checks, %d failure(s)\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
