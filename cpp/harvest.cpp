// harvest — a declarative web scraper in one C++17 file.
//
// Same command line, same recipe format and byte-identical output as
// python/harvest.py. HTML parsing, CSS selectors, JSON recipes and the
// CSV/JSON writers are all implemented here; HTTP goes through curl(1), which
// ships with Windows 10+, macOS and virtually every Linux distribution.
//
//   g++ -std=c++17 -O2 cpp/harvest.cpp -o harvest
//
// ---------------------------------------------------------------------------
// Build note: `#define HARVEST_NO_MAIN` before including this file to reuse the
// library from tests (see cpp/test_harvest.cpp).

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <chrono>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace harvest {

#ifdef _WIN32
constexpr const char* CURL_BINARY = "curl.exe";
#else
constexpr const char* CURL_BINARY = "curl";
#endif

constexpr const char* VERSION = "1.0.0";
constexpr const char* DEFAULT_UA = "harvest/1.0 (declarative web scraper)";
constexpr const char* DEFAULT_JOIN = " | ";

// ---------------------------------------------------------------------------
// Small string helpers
// ---------------------------------------------------------------------------

inline bool startsWith(const std::string& text, const std::string& prefix) {
  return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

inline bool endsWith(const std::string& text, const std::string& suffix) {
  return text.size() >= suffix.size() &&
         text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

inline std::string toLower(std::string text) {
  for (char& ch : text) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  return text;
}

inline std::string trim(const std::string& text) {
  size_t begin = 0, end = text.size();
  while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) ++begin;
  while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
  return text.substr(begin, end - begin);
}

// ---------------------------------------------------------------------------
// UTF-8 aware whitespace handling (must match Python's str.split())
// ---------------------------------------------------------------------------

inline uint32_t decodeUtf8(const std::string& text, size_t index, size_t& length) {
  const unsigned char first = static_cast<unsigned char>(text[index]);
  if (first < 0x80) {
    length = 1;
    return first;
  }
  if ((first & 0xE0) == 0xC0 && index + 1 < text.size()) {
    length = 2;
    return ((first & 0x1F) << 6) | (static_cast<unsigned char>(text[index + 1]) & 0x3F);
  }
  if ((first & 0xF0) == 0xE0 && index + 2 < text.size()) {
    length = 3;
    return ((first & 0x0F) << 12) | ((static_cast<unsigned char>(text[index + 1]) & 0x3F) << 6) |
           (static_cast<unsigned char>(text[index + 2]) & 0x3F);
  }
  if ((first & 0xF8) == 0xF0 && index + 3 < text.size()) {
    length = 4;
    return ((first & 0x07) << 18) | ((static_cast<unsigned char>(text[index + 1]) & 0x3F) << 12) |
           ((static_cast<unsigned char>(text[index + 2]) & 0x3F) << 6) |
           (static_cast<unsigned char>(text[index + 3]) & 0x3F);
  }
  length = 1;
  return first;
}

inline bool isSpaceCodePoint(uint32_t code) {
  switch (code) {
    case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D:
    case 0x20: case 0x85: case 0xA0: case 0x1680:
    case 0x2028: case 0x2029: case 0x202F: case 0x205F: case 0x3000:
      return true;
    default:
      return code >= 0x2000 && code <= 0x200A;
  }
}

inline bool isSpaceAt(const std::string& text, size_t index, size_t& length) {
  size_t size = 0;
  const uint32_t code = decodeUtf8(text, index, size);
  length = size;
  return isSpaceCodePoint(code);
}

/** Collapse runs of whitespace into single spaces and trim, like Python's " ".join(s.split()). */
inline std::string normalizeSpace(const std::string& text) {
  std::string out;
  bool pendingSpace = false;
  bool started = false;
  size_t index = 0;
  while (index < text.size()) {
    size_t length = 0;
    const uint32_t code = decodeUtf8(text, index, length);
    if (isSpaceCodePoint(code)) {
      if (started) pendingSpace = true;
      index += length;
      continue;
    }
    if (pendingSpace) {
      out.push_back(' ');
      pendingSpace = false;
    }
    out.append(text, index, length);
    started = true;
    index += length;
  }
  return out;
}

// ---------------------------------------------------------------------------
// HTML tree
// ---------------------------------------------------------------------------

struct Node {
  enum Kind { Root, Element, Text } kind = Root;
  std::string tag;
  std::vector<std::pair<std::string, std::string>> attrs;
  std::vector<std::unique_ptr<Node>> children;
  Node* parent = nullptr;
  std::string text;
};

inline const std::string* attribute(const Node* node, const std::string& name) {
  if (!node) return nullptr;
  for (const auto& entry : node->attrs) {
    if (entry.first == name) return &entry.second;
  }
  return nullptr;
}

inline std::string attributeOr(const Node* node, const std::string& name) {
  const std::string* value = attribute(node, name);
  return value ? *value : std::string();
}

inline bool isVoidElement(const std::string& tag) {
  static const std::set<std::string> names = {
      "area", "base", "br", "col", "embed", "hr", "img", "input",
      "link", "meta", "param", "source", "track", "wbr"};
  return names.count(tag) > 0;
}

inline bool isRawTextElement(const std::string& tag) {
  return tag == "script" || tag == "style";
}

inline bool isBlockElement(const std::string& tag) {
  static const std::set<std::string> names = {
      "address", "article", "aside", "blockquote", "dd", "div", "dl", "dt",
      "fieldset", "figcaption", "figure", "footer", "form", "h1", "h2", "h3",
      "h4", "h5", "h6", "header", "hr", "li", "main", "nav", "ol", "p", "pre",
      "section", "table", "tbody", "td", "tfoot", "th", "thead", "tr", "ul"};
  return names.count(tag) > 0;
}

inline const std::map<std::string, std::string>& namedEntities() {
  static const std::map<std::string, std::string> table = {
      {"amp", "&"}, {"lt", "<"}, {"gt", ">"}, {"quot", "\""}, {"apos", "'"},
      {"nbsp", "\xC2\xA0"}, {"hellip", "\xE2\x80\xA6"}, {"mdash", "\xE2\x80\x94"},
      {"ndash", "\xE2\x80\x93"}, {"lsquo", "\xE2\x80\x98"}, {"rsquo", "\xE2\x80\x99"},
      {"ldquo", "\xE2\x80\x9C"}, {"rdquo", "\xE2\x80\x9D"}, {"laquo", "\xC2\xAB"},
      {"raquo", "\xC2\xBB"}, {"bull", "\xE2\x80\xA2"}, {"middot", "\xC2\xB7"},
      {"copy", "\xC2\xA9"}, {"reg", "\xC2\xAE"}, {"trade", "\xE2\x84\xA2"},
      {"deg", "\xC2\xB0"}, {"plusmn", "\xC2\xB1"}, {"times", "\xC3\x97"},
      {"divide", "\xC3\xB7"}, {"frac12", "\xC2\xBD"}, {"frac14", "\xC2\xBC"},
      {"frac34", "\xC2\xBE"}, {"sup2", "\xC2\xB2"}, {"sup3", "\xC2\xB3"},
      {"euro", "\xE2\x82\xAC"}, {"pound", "\xC2\xA3"}, {"yen", "\xC2\xA5"},
      {"cent", "\xC2\xA2"}, {"sect", "\xC2\xA7"}, {"para", "\xC2\xB6"},
      {"dagger", "\xE2\x80\xA0"}, {"prime", "\xE2\x80\xB2"}, {"Prime", "\xE2\x80\xB3"},
      {"larr", "\xE2\x86\x90"}, {"rarr", "\xE2\x86\x92"}, {"uarr", "\xE2\x86\x91"},
      {"darr", "\xE2\x86\x93"}, {"harr", "\xE2\x86\x94"}, {"minus", "\xE2\x88\x92"},
      {"le", "\xE2\x89\xA4"}, {"ge", "\xE2\x89\xA5"}, {"ne", "\xE2\x89\xA0"},
      {"infin", "\xE2\x88\x9E"}, {"radic", "\xE2\x88\x9A"}, {"sum", "\xE2\x88\x91"},
      {"prod", "\xE2\x88\x8F"}, {"int", "\xE2\x88\xAB"}, {"forall", "\xE2\x88\x80"},
      {"exist", "\xE2\x88\x83"}, {"empty", "\xE2\x88\x85"}, {"isin", "\xE2\x88\x88"},
      {"notin", "\xE2\x88\x89"}, {"cap", "\xE2\x88\xA9"}, {"cup", "\xE2\x88\xAA"},
      {"and", "\xE2\x88\xA7"}, {"or", "\xE2\x88\xA8"}, {"eacute", "\xC3\xA9"},
      {"egrave", "\xC3\xA8"}, {"aacute", "\xC3\xA1"}, {"agrave", "\xC3\xA0"},
      {"oacute", "\xC3\xB3"}, {"uuml", "\xC3\xBC"}, {"ouml", "\xC3\xB6"},
      {"auml", "\xC3\xA4"}, {"szlig", "\xC3\x9F"}, {"ccedil", "\xC3\xA7"},
      {"ntilde", "\xC3\xB1"}};
  return table;
}

inline void appendUtf8(std::string& out, uint32_t code) {
  if (code < 0x80) {
    out.push_back(static_cast<char>(code));
  } else if (code < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (code >> 6)));
    out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
  } else if (code < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (code >> 12)));
    out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (code >> 18)));
    out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
  }
}

inline std::string decodeEntities(const std::string& text) {
  if (text.find('&') == std::string::npos) return text;
  std::string out;
  out.reserve(text.size());
  size_t index = 0;
  while (index < text.size()) {
    if (text[index] != '&') {
      out.push_back(text[index++]);
      continue;
    }
    const size_t semicolon = text.find(';', index);
    if (semicolon == std::string::npos || semicolon - index > 32) {
      out.push_back(text[index++]);
      continue;
    }
    const std::string body = text.substr(index + 1, semicolon - index - 1);
    bool consumed = false;
    if (!body.empty() && body[0] == '#') {
      uint32_t code = 0;
      bool valid = false;
      if (body.size() > 2 && (body[1] == 'x' || body[1] == 'X')) {
        const std::string digits = body.substr(2);
        valid = !digits.empty() &&
                std::all_of(digits.begin(), digits.end(), [](unsigned char ch) { return std::isxdigit(ch); });
        if (valid) code = static_cast<uint32_t>(std::strtoul(digits.c_str(), nullptr, 16));
      } else if (body.size() > 1) {
        const std::string digits = body.substr(1);
        valid = !digits.empty() &&
                std::all_of(digits.begin(), digits.end(), [](unsigned char ch) { return std::isdigit(ch); });
        if (valid) code = static_cast<uint32_t>(std::strtoul(digits.c_str(), nullptr, 10));
      }
      if (valid && code > 0 && code <= 0x10FFFF) {
        appendUtf8(out, code);
        consumed = true;
      }
    } else {
      const auto found = namedEntities().find(body);
      if (found != namedEntities().end()) {
        out += found->second;
        consumed = true;
      }
    }
    if (consumed) {
      index = semicolon + 1;
    } else {
      out.push_back(text[index++]);
    }
  }
  return out;
}

/** Read attributes starting just after the tag name. */
inline void readAttributes(const std::string& html, size_t& index,
                           std::vector<std::pair<std::string, std::string>>& attrs,
                           bool& selfClosing) {
  attrs.clear();
  selfClosing = false;
  while (index < html.size()) {
    const char ch = html[index];
    if (ch == '>') {
      ++index;
      break;
    }
    if (ch == '/') {
      selfClosing = true;
      ++index;
      continue;
    }
    if (std::isspace(static_cast<unsigned char>(ch))) {
      ++index;
      continue;
    }
    const size_t nameStart = index;
    while (index < html.size() && !std::isspace(static_cast<unsigned char>(html[index])) &&
           html[index] != '=' && html[index] != '>' && html[index] != '/') {
      ++index;
    }
    std::string name = toLower(html.substr(nameStart, index - nameStart));
    if (name.empty()) {
      ++index;
      continue;
    }
    while (index < html.size() && std::isspace(static_cast<unsigned char>(html[index]))) ++index;

    std::string value;
    if (index < html.size() && html[index] == '=') {
      ++index;
      while (index < html.size() && std::isspace(static_cast<unsigned char>(html[index]))) ++index;
      if (index < html.size() && (html[index] == '"' || html[index] == '\'')) {
        const char quote = html[index];
        const size_t close = html.find(quote, index + 1);
        if (close == std::string::npos) {
          value = html.substr(index + 1);
          index = html.size();
        } else {
          value = html.substr(index + 1, close - index - 1);
          index = close + 1;
        }
      } else {
        const size_t valueStart = index;
        while (index < html.size() && !std::isspace(static_cast<unsigned char>(html[index])) &&
               html[index] != '>') {
          ++index;
        }
        value = html.substr(valueStart, index - valueStart);
      }
    }
    attrs.emplace_back(name, decodeEntities(value));
  }
}

inline std::unique_ptr<Node> parseHTML(const std::string& html) {
  auto root = std::make_unique<Node>();
  root->kind = Node::Root;
  std::vector<Node*> stack{root.get()};

  auto appendText = [&](const std::string& raw) {
    if (raw.empty()) return;
    auto node = std::make_unique<Node>();
    node->kind = Node::Text;
    node->text = decodeEntities(raw);
    node->parent = stack.back();
    stack.back()->children.push_back(std::move(node));
  };

  const auto autoCloses = [](const std::string& tag, const std::string& open) {
    if (tag == "p") return open == "p";
    if (tag == "li") return open == "li";
    if (tag == "dt" || tag == "dd") return open == "dt" || open == "dd";
    if (tag == "tr") return open == "tr";
    if (tag == "td" || tag == "th") return open == "td" || open == "th";
    if (tag == "option") return open == "option";
    return false;
  };

  size_t index = 0;
  while (index < html.size()) {
    const size_t open = html.find('<', index);
    if (open == std::string::npos) {
      appendText(html.substr(index));
      break;
    }
    if (open > index) appendText(html.substr(index, open - index));

    if (html.compare(open, 4, "<!--") == 0) {
      const size_t close = html.find("-->", open + 4);
      index = close == std::string::npos ? html.size() : close + 3;
      continue;
    }
    if (open + 1 < html.size() && (html[open + 1] == '!' || html[open + 1] == '?')) {
      const size_t close = html.find('>', open);
      index = close == std::string::npos ? html.size() : close + 1;
      continue;
    }
    if (open + 1 < html.size() && html[open + 1] == '/') {
      const size_t close = html.find('>', open);
      if (close == std::string::npos) {
        appendText(html.substr(open));
        break;
      }
      std::string name = toLower(trim(html.substr(open + 2, close - open - 2)));
      const size_t space = name.find_first_of(" \t\r\n");
      if (space != std::string::npos) name = name.substr(0, space);
      for (size_t depth = stack.size(); depth-- > 1;) {
        if (stack[depth]->tag == name) {
          stack.resize(depth);
          break;
        }
      }
      index = close + 1;
      continue;
    }

    size_t nameStart = open + 1;
    if (nameStart >= html.size() || !std::isalpha(static_cast<unsigned char>(html[nameStart]))) {
      appendText("<");
      index = open + 1;
      continue;
    }
    size_t nameEnd = nameStart;
    while (nameEnd < html.size() &&
           (std::isalnum(static_cast<unsigned char>(html[nameEnd])) || html[nameEnd] == '-' ||
            html[nameEnd] == ':' || html[nameEnd] == '_')) {
      ++nameEnd;
    }
    const std::string tag = toLower(html.substr(nameStart, nameEnd - nameStart));

    size_t cursor = nameEnd;
    std::vector<std::pair<std::string, std::string>> attrs;
    bool selfClosing = false;
    readAttributes(html, cursor, attrs, selfClosing);

    while (stack.size() > 1 && autoCloses(tag, stack.back()->tag)) stack.pop_back();

    auto element = std::make_unique<Node>();
    element->kind = Node::Element;
    element->tag = tag;
    element->attrs = attrs;
    element->parent = stack.back();
    Node* raw = element.get();
    stack.back()->children.push_back(std::move(element));

    if (isVoidElement(tag) || selfClosing) {
      index = cursor;
      continue;
    }

    if (isRawTextElement(tag)) {
      std::string needle = "</" + tag;
      const size_t close = html.find(needle, cursor);
      const size_t contentEnd = close == std::string::npos ? html.size() : close;
      const std::string rawText = html.substr(cursor, contentEnd - cursor);
      if (!rawText.empty()) {
        auto textNode = std::make_unique<Node>();
        textNode->kind = Node::Text;
        textNode->text = rawText;
        textNode->parent = raw;
        raw->children.push_back(std::move(textNode));
      }
      if (close == std::string::npos) {
        index = html.size();
      } else {
        const size_t tagEnd = html.find('>', close);
        index = tagEnd == std::string::npos ? html.size() : tagEnd + 1;
      }
      continue;
    }

    stack.push_back(raw);
    index = cursor;
  }

  return root;
}

inline std::string textContent(const Node* node) {
  if (!node) return std::string();
  if (node->kind == Node::Text) return node->text;
  if (node->tag == "br") return " ";
  if (node->tag == "script" || node->tag == "style") return std::string();
  std::string inner;
  for (const auto& child : node->children) inner += textContent(child.get());
  if (isBlockElement(node->tag)) return " " + inner + " ";
  return inner;
}

inline std::string serialize(const Node* node) {
  if (node->kind == Node::Text) return node->text;
  std::string out = "<" + node->tag;
  for (const auto& entry : node->attrs) {
    if (entry.second.empty()) {
      out += " " + entry.first;
    } else {
      out += " " + entry.first + "=\"" + entry.second + "\"";
    }
  }
  out += ">";
  if (isVoidElement(node->tag)) return out;
  for (const auto& child : node->children) out += serialize(child.get());
  out += "</" + node->tag + ">";
  return out;
}

inline std::string innerHTML(const Node* node) {
  std::string out;
  if (!node) return out;
  for (const auto& child : node->children) out += serialize(child.get());
  return out;
}

inline void collectElements(const Node* node, std::vector<Node*>& out) {
  for (const auto& child : node->children) {
    if (child->kind != Node::Element) continue;
    out.push_back(child.get());
    collectElements(child.get(), out);
  }
}

inline std::vector<Node*> allElements(const Node* root) {
  std::vector<Node*> out;
  collectElements(root, out);
  return out;
}

inline std::vector<Node*> childElements(const Node* node) {
  std::vector<Node*> out;
  for (const auto& child : node->children) {
    if (child->kind == Node::Element) out.push_back(child.get());
  }
  return out;
}

// ---------------------------------------------------------------------------
// CSS selectors
// ---------------------------------------------------------------------------

struct AttributeTest {
  std::string name;
  std::string op;  // "", "=", "~=", "^=", "$=", "*=", "|="
  std::string value;
};

struct PseudoTest {
  std::string name;
  std::string argument;
  bool hasArgument = false;
};

struct Compound {
  std::string tag;  // empty means any
  std::string id;
  std::vector<std::string> classes;
  std::vector<AttributeTest> attributes;
  std::vector<PseudoTest> pseudos;
};

struct Part {
  char combinator = 0;  // 0 = first, ' ' = descendant, '>' = child
  Compound compound;
};

using Group = std::vector<Part>;

inline bool isIdentifierChar(unsigned char ch) {
  return std::isalnum(ch) || ch == '-' || ch == '_' || ch >= 0x80;
}

inline size_t findClosing(const std::string& text, size_t start, char open, char close) {
  int depth = 0;
  char quote = 0;
  for (size_t index = start; index < text.size(); ++index) {
    const char ch = text[index];
    if (quote) {
      if (ch == quote) quote = 0;
      continue;
    }
    if (ch == '"' || ch == '\'') {
      quote = ch;
      continue;
    }
    if (ch == open) {
      ++depth;
    } else if (ch == close) {
      if (--depth == 0) return index;
    }
  }
  throw std::runtime_error(std::string("unbalanced ") + open + " in selector");
}

inline Compound parseCompound(const std::string& text) {
  Compound compound;
  size_t index = 0;
  while (index < text.size()) {
    const char ch = text[index];
    if (ch == '*') {
      ++index;
    } else if (ch == '.') {
      const size_t start = ++index;
      while (index < text.size() && isIdentifierChar(static_cast<unsigned char>(text[index]))) ++index;
      compound.classes.push_back(text.substr(start, index - start));
    } else if (ch == '#') {
      const size_t start = ++index;
      while (index < text.size() && isIdentifierChar(static_cast<unsigned char>(text[index]))) ++index;
      compound.id = text.substr(start, index - start);
    } else if (ch == '[') {
      const size_t close = findClosing(text, index, '[', ']');
      const std::string body = text.substr(index + 1, close - index - 1);
      AttributeTest test;
      size_t cursor = 0;
      while (cursor < body.size() && std::isspace(static_cast<unsigned char>(body[cursor]))) ++cursor;
      const size_t nameStart = cursor;
      while (cursor < body.size() && (isIdentifierChar(static_cast<unsigned char>(body[cursor])) ||
                                      body[cursor] == ':')) {
        ++cursor;
      }
      test.name = toLower(body.substr(nameStart, cursor - nameStart));
      while (cursor < body.size() && std::isspace(static_cast<unsigned char>(body[cursor]))) ++cursor;
      if (cursor < body.size() && body[cursor] != ']') {
        if (body[cursor] == '=') {
          test.op = "=";
          ++cursor;
        } else if (cursor + 1 < body.size() && body[cursor + 1] == '=') {
          test.op = body.substr(cursor, 2);
          cursor += 2;
        } else {
          throw std::runtime_error("invalid attribute selector: [" + body + "]");
        }
        while (cursor < body.size() && std::isspace(static_cast<unsigned char>(body[cursor]))) ++cursor;
        if (cursor < body.size() && (body[cursor] == '"' || body[cursor] == '\'')) {
          const char quote = body[cursor];
          const size_t end = body.find(quote, cursor + 1);
          test.value = end == std::string::npos ? body.substr(cursor + 1)
                                                : body.substr(cursor + 1, end - cursor - 1);
        } else {
          test.value = trim(body.substr(cursor));
        }
      }
      compound.attributes.push_back(test);
      index = close + 1;
    } else if (ch == ':') {
      const size_t start = ++index;
      while (index < text.size() && isIdentifierChar(static_cast<unsigned char>(text[index]))) ++index;
      PseudoTest test;
      test.name = toLower(text.substr(start, index - start));
      if (index < text.size() && text[index] == '(') {
        const size_t close = findClosing(text, index, '(', ')');
        test.argument = trim(text.substr(index + 1, close - index - 1));
        test.hasArgument = true;
        index = close + 1;
      }
      compound.pseudos.push_back(test);
    } else {
      const size_t start = index;
      while (index < text.size() && isIdentifierChar(static_cast<unsigned char>(text[index]))) ++index;
      if (index == start) throw std::runtime_error("invalid selector near \"" + text.substr(index) + "\"");
      compound.tag = toLower(text.substr(start, index - start));
    }
  }
  return compound;
}

inline std::vector<std::string> splitGroups(const std::string& selector) {
  std::vector<std::string> groups;
  std::string buffer;
  int depth = 0;
  char quote = 0;
  for (const char ch : selector) {
    if (quote) {
      buffer.push_back(ch);
      if (ch == quote) quote = 0;
      continue;
    }
    if (ch == '"' || ch == '\'') {
      quote = ch;
      buffer.push_back(ch);
      continue;
    }
    if (ch == '[' || ch == '(') ++depth;
    if (ch == ']' || ch == ')') depth = depth > 0 ? depth - 1 : 0;
    if (ch == ',' && depth == 0) {
      groups.push_back(trim(buffer));
      buffer.clear();
      continue;
    }
    buffer.push_back(ch);
  }
  groups.push_back(trim(buffer));
  groups.erase(std::remove_if(groups.begin(), groups.end(),
                              [](const std::string& group) { return group.empty(); }),
               groups.end());
  return groups;
}

inline Group splitParts(const std::string& group) {
  Group parts;
  std::string buffer;
  int depth = 0;
  char quote = 0;
  char combinator = 0;

  auto flush = [&]() {
    const std::string text = trim(buffer);
    if (!text.empty()) {
      Part part;
      part.combinator = combinator;
      part.compound = parseCompound(text);
      parts.push_back(part);
    }
    buffer.clear();
  };

  for (const char ch : group) {
    if (quote) {
      buffer.push_back(ch);
      if (ch == quote) quote = 0;
      continue;
    }
    if (ch == '"' || ch == '\'') {
      quote = ch;
      buffer.push_back(ch);
      continue;
    }
    if (ch == '[' || ch == '(') {
      ++depth;
      buffer.push_back(ch);
      continue;
    }
    if (ch == ']' || ch == ')') {
      depth = depth > 0 ? depth - 1 : 0;
      buffer.push_back(ch);
      continue;
    }
    if (depth == 0 && ch == '>') {
      flush();
      combinator = '>';
      continue;
    }
    if (depth == 0 && std::isspace(static_cast<unsigned char>(ch))) {
      flush();
      if (combinator != '>') combinator = ' ';
      continue;
    }
    buffer.push_back(ch);
  }
  flush();
  if (!parts.empty()) parts.front().combinator = 0;
  return parts;
}

inline std::vector<Group> parseSelector(const std::string& selector) {
  std::vector<Group> groups;
  for (const auto& group : splitGroups(selector)) groups.push_back(splitParts(group));
  if (groups.empty()) throw std::runtime_error("empty selector: \"" + selector + "\"");
  return groups;
}

inline bool matchesPseudo(const Node* element, const PseudoTest& pseudo);

inline bool matchesCompound(const Node* element, const Compound& compound) {
  if (!element || element->kind != Node::Element) return false;
  if (!compound.tag.empty() && element->tag != compound.tag) return false;
  if (!compound.id.empty() && attributeOr(element, "id") != compound.id) return false;

  if (!compound.classes.empty()) {
    const std::string classes = attributeOr(element, "class");
    std::set<std::string> present;
    std::istringstream stream(classes);
    std::string item;
    while (stream >> item) present.insert(item);
    for (const auto& name : compound.classes) {
      if (!present.count(name)) return false;
    }
  }

  for (const auto& test : compound.attributes) {
    const std::string* actual = attribute(element, test.name);
    if (!actual) return false;
    if (test.op.empty()) continue;
    if (test.op == "=" && *actual != test.value) return false;
    if (test.op == "~=") {
      std::istringstream stream(*actual);
      std::string item;
      bool found = false;
      while (stream >> item) {
        if (item == test.value) {
          found = true;
          break;
        }
      }
      if (!found) return false;
    }
    if (test.op == "^=" && !startsWith(*actual, test.value)) return false;
    if (test.op == "$=" && !endsWith(*actual, test.value)) return false;
    if (test.op == "*=" && actual->find(test.value) == std::string::npos) return false;
    if (test.op == "|=" && *actual != test.value && !startsWith(*actual, test.value + "-")) return false;
  }

  for (const auto& pseudo : compound.pseudos) {
    if (!matchesPseudo(element, pseudo)) return false;
  }
  return true;
}

inline bool matchesPseudo(const Node* element, const PseudoTest& pseudo) {
  const auto position = [&]() -> int {
    if (!element->parent) return -1;
    const auto family = childElements(element->parent);
    for (size_t index = 0; index < family.size(); ++index) {
      if (family[index] == element) return static_cast<int>(index);
    }
    return -1;
  };

  if (pseudo.name == "first-child") return position() == 0;
  if (pseudo.name == "last-child") {
    if (!element->parent) return false;
    const auto family = childElements(element->parent);
    return !family.empty() && family.back() == element;
  }
  if (pseudo.name == "nth-child") {
    if (!element->parent) return false;
    const int index = position() + 1;
    std::string argument = toLower(trim(pseudo.argument));
    if (argument == "odd") return index % 2 == 1;
    if (argument == "even") return index % 2 == 0;
    try {
      return index == std::stoi(argument);
    } catch (...) {
      return false;
    }
  }
  if (pseudo.name == "not") {
    return !matchesCompound(element, parseCompound(pseudo.argument));
  }
  throw std::runtime_error("unsupported pseudo class :" + pseudo.name);
}

inline bool matchesComplex(const Node* element, const Group& parts) {
  if (parts.empty()) return false;
  size_t index = parts.size() - 1;
  if (!matchesCompound(element, parts[index].compound)) return false;

  const Node* current = element;
  while (index > 0) {
    const char combinator = parts[index].combinator;
    const Compound& compound = parts[index - 1].compound;
    if (combinator == '>') {
      current = current ? current->parent : nullptr;
      if (!current || current->kind != Node::Element || !matchesCompound(current, compound)) return false;
    } else {
      const Node* ancestor = current ? current->parent : nullptr;
      const Node* found = nullptr;
      while (ancestor && ancestor->kind == Node::Element) {
        if (matchesCompound(ancestor, compound)) {
          found = ancestor;
          break;
        }
        ancestor = ancestor->parent;
      }
      if (!found) return false;
      current = found;
    }
    --index;
  }
  return true;
}

inline std::vector<const Node*> queryAll(const Node* root, const std::string& selector) {
  const auto groups = parseSelector(selector);
  std::vector<const Node*> matches;
  for (const Node* element : allElements(root)) {
    for (const auto& group : groups) {
      if (matchesComplex(element, group)) {
        matches.push_back(element);
        break;
      }
    }
  }
  return matches;
}

inline const Node* queryOne(const Node* root, const std::string& selector) {
  const auto groups = parseSelector(selector);
  for (const Node* element : allElements(root)) {
    for (const auto& group : groups) {
      if (matchesComplex(element, group)) return element;
    }
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
// Minimal JSON reader (for recipes)
// ---------------------------------------------------------------------------

struct JsonValue {
  enum Type { Null, Bool, Number, String, Array, Object } type = Null;
  bool boolean = false;
  double number = 0;
  std::string text;
  std::vector<JsonValue> items;
  std::vector<std::pair<std::string, JsonValue>> members;
};

class JsonParser {
 public:
  explicit JsonParser(const std::string& source) : source_(source) {}

  JsonValue parseDocument() {
    skipSpace();
    JsonValue value = parseValue();
    skipSpace();
    if (index_ != source_.size()) throw std::runtime_error("trailing data in JSON");
    return value;
  }

 private:
  const std::string& source_;
  size_t index_ = 0;

  void skipSpace() {
    while (index_ < source_.size() && std::isspace(static_cast<unsigned char>(source_[index_]))) ++index_;
  }

  char peek() {
    if (index_ >= source_.size()) throw std::runtime_error("unexpected end of JSON");
    return source_[index_];
  }

  JsonValue parseValue() {
    skipSpace();
    const char ch = peek();
    if (ch == '{') return parseObject();
    if (ch == '[') return parseArray();
    if (ch == '"') {
      JsonValue value;
      value.type = JsonValue::String;
      value.text = parseString();
      return value;
    }
    if (ch == 't' || ch == 'f') return parseBool();
    if (ch == 'n') return parseNull();
    return parseNumber();
  }

  JsonValue parseObject() {
    JsonValue value;
    value.type = JsonValue::Object;
    ++index_;  // {
    skipSpace();
    if (peek() == '}') {
      ++index_;
      return value;
    }
    while (true) {
      skipSpace();
      const std::string key = parseString();
      skipSpace();
      if (peek() != ':') throw std::runtime_error("expected ':' in JSON object");
      ++index_;
      value.members.emplace_back(key, parseValue());
      skipSpace();
      const char ch = peek();
      if (ch == ',') {
        ++index_;
        continue;
      }
      if (ch == '}') {
        ++index_;
        break;
      }
      throw std::runtime_error("expected ',' or '}' in JSON object");
    }
    return value;
  }

  JsonValue parseArray() {
    JsonValue value;
    value.type = JsonValue::Array;
    ++index_;  // [
    skipSpace();
    if (peek() == ']') {
      ++index_;
      return value;
    }
    while (true) {
      value.items.push_back(parseValue());
      skipSpace();
      const char ch = peek();
      if (ch == ',') {
        ++index_;
        continue;
      }
      if (ch == ']') {
        ++index_;
        break;
      }
      throw std::runtime_error("expected ',' or ']' in JSON array");
    }
    return value;
  }

  std::string parseString() {
    if (peek() != '"') throw std::runtime_error("expected string in JSON");
    ++index_;
    std::string out;
    while (true) {
      if (index_ >= source_.size()) throw std::runtime_error("unterminated string in JSON");
      const char ch = source_[index_++];
      if (ch == '"') break;
      if (ch != '\\') {
        out.push_back(ch);
        continue;
      }
      if (index_ >= source_.size()) throw std::runtime_error("unterminated escape in JSON");
      const char escape = source_[index_++];
      switch (escape) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u': {
          uint32_t code = static_cast<uint32_t>(
              std::strtoul(source_.substr(index_, 4).c_str(), nullptr, 16));
          index_ += 4;
          appendUtf8(out, code);
          break;
        }
        default:
          throw std::runtime_error("invalid escape in JSON string");
      }
    }
    return out;
  }

  JsonValue parseBool() {
    JsonValue value;
    value.type = JsonValue::Bool;
    if (source_.compare(index_, 4, "true") == 0) {
      value.boolean = true;
      index_ += 4;
    } else if (source_.compare(index_, 5, "false") == 0) {
      index_ += 5;
    } else {
      throw std::runtime_error("invalid literal in JSON");
    }
    return value;
  }

  JsonValue parseNull() {
    if (source_.compare(index_, 4, "null") != 0) throw std::runtime_error("invalid literal in JSON");
    index_ += 4;
    return JsonValue();
  }

  JsonValue parseNumber() {
    const size_t start = index_;
    while (index_ < source_.size() &&
           (std::isdigit(static_cast<unsigned char>(source_[index_])) || source_[index_] == '-' ||
            source_[index_] == '+' || source_[index_] == '.' || source_[index_] == 'e' ||
            source_[index_] == 'E')) {
      ++index_;
    }
    if (start == index_) throw std::runtime_error("invalid value in JSON");
    JsonValue value;
    value.type = JsonValue::Number;
    value.number = std::strtod(source_.substr(start, index_ - start).c_str(), nullptr);
    return value;
  }
};

inline const JsonValue* member(const JsonValue& object, const std::string& key) {
  for (const auto& entry : object.members) {
    if (entry.first == key) return &entry.second;
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
// HTTP through curl
// ---------------------------------------------------------------------------

inline bool isValidHttpUrl(const std::string& url) {
  if (!startsWith(url, "http://") && !startsWith(url, "https://")) return false;
  for (const char ch : url) {
    if (ch == '"' || ch == '\n' || ch == '\r' || static_cast<unsigned char>(ch) < 0x20) return false;
  }
  return true;
}

inline std::string escapeForShell(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  for (const char ch : text) {
    if (ch == '"') continue;  // quotes would break out of the argument
    out.push_back(ch);
  }
  return out;
}

inline std::string httpGetOnce(const std::string& url, int timeoutMs, const std::string& userAgent,
                               bool silent = false) {
  if (!isValidHttpUrl(url)) throw std::runtime_error("refusing to fetch invalid URL: " + url);

  const int seconds = std::max(1, (timeoutMs + 999) / 1000);
  std::ostringstream command;
  command << CURL_BINARY << (silent ? " -s -f -L" : " -sS -f -L")
          << " --compressed --max-time " << seconds
          << " -A \"" << escapeForShell(userAgent) << "\" -- \"" << escapeForShell(url) << "\"";

#ifdef _WIN32
  FILE* pipe = _popen(command.str().c_str(), "rb");
#else
  FILE* pipe = popen(command.str().c_str(), "r");
#endif
  if (!pipe) throw std::runtime_error("cannot run curl (is it installed?)");

  std::string body;
  char buffer[65536];
  size_t read = 0;
  while ((read = std::fread(buffer, 1, sizeof(buffer), pipe)) > 0) body.append(buffer, read);

#ifdef _WIN32
  const int status = _pclose(pipe);
#else
  const int status = pclose(pipe);
#endif
  if (status != 0) {
    throw std::runtime_error("HTTP request failed for " + url + " (curl exit " + std::to_string(status) + ")");
  }
  return body;
}

inline std::string httpGet(const std::string& url, int timeoutMs, const std::string& userAgent,
                           int retries) {
  int attempt = 0;
  while (true) {
    try {
      return httpGetOnce(url, timeoutMs, userAgent);
    } catch (const std::exception&) {
      if (attempt >= retries) throw;
      ++attempt;
      std::this_thread::sleep_for(std::chrono::seconds(std::min(10, 1 << attempt)));
    }
  }
}

// ---------------------------------------------------------------------------
// robots.txt
// ---------------------------------------------------------------------------

inline std::vector<std::pair<std::string, bool>> parseRobots(const std::string& text,
                                                             const std::string& userAgent) {
  std::vector<std::pair<std::string, bool>> rules;
  bool applies = false;
  std::istringstream stream(text);
  std::string line;
  while (std::getline(stream, line)) {
    const size_t hash = line.find('#');
    if (hash != std::string::npos) line = line.substr(0, hash);
    line = trim(line);
    if (line.empty()) continue;
    const size_t colon = line.find(':');
    if (colon == std::string::npos) continue;
    const std::string key = toLower(trim(line.substr(0, colon)));
    const std::string value = trim(line.substr(colon + 1));
    if (key == "user-agent") {
      applies = value == "*" || toLower(userAgent).find(toLower(value)) != std::string::npos;
    } else if ((key == "allow" || key == "disallow") && applies) {
      rules.emplace_back(value, key == "allow");
    }
  }
  return rules;
}

inline std::string originOf(const std::string& url) {
  const size_t schemeEnd = url.find("://");
  if (schemeEnd == std::string::npos) return url;
  const size_t pathStart = url.find('/', schemeEnd + 3);
  return pathStart == std::string::npos ? url : url.substr(0, pathStart);
}

inline bool robotsAllowed(const std::string& url, const std::string& userAgent,
                          const std::string& body, bool haveBody) {
  if (!haveBody) return true;
  std::string path = "/";
  const size_t schemeEnd = url.find("://");
  if (schemeEnd != std::string::npos) {
    const size_t pathStart = url.find('/', schemeEnd + 3);
    if (pathStart != std::string::npos) path = url.substr(pathStart);
  }
  const size_t query = path.find('?');
  if (query != std::string::npos) path = path.substr(0, query);

  int bestLength = -1;
  bool allowed = true;
  for (const auto& rule : parseRobots(body, userAgent)) {
    const std::string& pattern = rule.first;
    if (pattern.empty()) continue;
    bool matched = false;
    if (!pattern.empty() && pattern.back() == '$') {
      matched = path == pattern.substr(0, pattern.size() - 1);
    } else {
      matched = startsWith(path, pattern);
    }
    if (matched && static_cast<int>(pattern.size()) > bestLength) {
      bestLength = static_cast<int>(pattern.size());
      allowed = rule.second;
    }
  }
  return allowed;
}

// ---------------------------------------------------------------------------
// URL joining
// ---------------------------------------------------------------------------

inline std::string stripFragment(const std::string& url) {
  const size_t hash = url.find('#');
  return hash == std::string::npos ? url : url.substr(0, hash);
}

inline std::string normalizePath(const std::string& path) {
  std::vector<std::string> segments;
  std::istringstream stream(path);
  std::string segment;
  while (std::getline(stream, segment, '/')) {
    if (segment.empty() || segment == ".") continue;
    if (segment == "..") {
      if (!segments.empty()) segments.pop_back();
      continue;
    }
    segments.push_back(segment);
  }
  std::string out;
  for (const auto& part : segments) out += "/" + part;
  if (!path.empty() && path.back() == '/' && !out.empty()) out += "/";
  if (out.empty()) out = "/";
  return out;
}

inline std::string resolveUrl(const std::string& base, const std::string& reference) {
  if (reference.empty()) return base;
  if (reference.find("://") != std::string::npos) return stripFragment(reference);

  const size_t schemeEnd = base.find("://");
  if (schemeEnd == std::string::npos) return reference;
  const std::string scheme = base.substr(0, schemeEnd);
  const size_t hostStart = schemeEnd + 3;
  const size_t pathStart = base.find('/', hostStart);
  const std::string host = base.substr(hostStart, pathStart == std::string::npos
                                                      ? std::string::npos
                                                      : pathStart - hostStart);

  if (startsWith(reference, "//")) return stripFragment(scheme + ":" + reference);
  if (startsWith(reference, "/")) return stripFragment(scheme + "://" + host + reference);
  if (startsWith(reference, "#")) return stripFragment(base);
  if (startsWith(reference, "?")) {
    const size_t query = base.find('?');
    return stripFragment((query == std::string::npos ? base : base.substr(0, query)) + reference);
  }

  std::string directory = base;
  const size_t query = directory.find('?');
  if (query != std::string::npos) directory = directory.substr(0, query);
  const size_t lastSlash = directory.rfind('/');
  if (lastSlash != std::string::npos && lastSlash >= hostStart + host.size()) {
    directory = directory.substr(0, lastSlash + 1);
  } else {
    directory = scheme + "://" + host + "/";
  }
  const std::string combined = directory + reference;
  const size_t combinedPathStart = combined.find('/', hostStart);
  if (combinedPathStart == std::string::npos) return stripFragment(combined);
  return stripFragment(combined.substr(0, combinedPathStart) +
                       normalizePath(combined.substr(combinedPathStart)));
}

// ---------------------------------------------------------------------------
// Extraction, crawling and output
// ---------------------------------------------------------------------------

using Row = std::vector<std::pair<std::string, std::string>>;

struct Spec {
  std::string url;
  std::string record;
  std::string select;
  std::string next;
  std::string join = DEFAULT_JOIN;
  std::string userAgent = DEFAULT_UA;
  std::string out;
  std::string format;
  std::vector<std::pair<std::string, std::string>> fields;
  int maxPages = 1;
  int limit = 0;
  int delay = 1000;
  int timeout = 25000;
  int retries = 2;
  bool ignoreRobots = false;
  bool dedupe = false;
  bool quiet = false;
  bool inspect = false;
};

inline bool isUrlAttribute(const std::string& name) {
  static const std::set<std::string> names = {"href", "src", "action", "poster", "data-src", "data-href"};
  return names.count(name) > 0;
}

inline std::pair<std::string, std::string> parseField(const std::string& spec) {
  const size_t at = spec.rfind('@');
  if (at != std::string::npos) {
    const std::string attribute = spec.substr(at + 1);
    if (!attribute.empty() &&
        std::all_of(attribute.begin(), attribute.end(), [](unsigned char ch) {
          return std::isalnum(ch) || ch == '-' || ch == '_' || ch == ':';
        })) {
      return {trim(spec.substr(0, at)), attribute};
    }
  }
  return {trim(spec), "text"};
}

inline std::string fieldValue(const Node* element, const std::string& attribute, const std::string& baseUrl) {
  if (attribute == "html") return innerHTML(element);
  if (attribute != "text") {
    const std::string value = attributeOr(element, attribute);
    if (!value.empty() && isUrlAttribute(attribute)) return resolveUrl(baseUrl, value);
    return value;
  }
  return normalizeSpace(textContent(element));
}

inline std::vector<Row> extractRows(const Node* document, const std::string& pageUrl, const Spec& spec) {
  std::vector<Row> rows;

  if (!spec.select.empty()) {
    const auto field = parseField(spec.select);
    for (const Node* element : queryAll(document, field.first)) {
      Row row;
      row.emplace_back("value", fieldValue(element, field.second, pageUrl));
      rows.push_back(row);
    }
    return rows;
  }

  for (const Node* record : queryAll(document, spec.record)) {
    Row row;
    for (const auto& field : spec.fields) {
      const auto parsed = parseField(field.second);
      const auto matches = queryAll(record, parsed.first);
      std::string joined;
      for (size_t index = 0; index < matches.size(); ++index) {
        if (index) joined += spec.join;
        joined += fieldValue(matches[index], parsed.second, pageUrl);
      }
      row.emplace_back(field.first, joined);
    }
    rows.push_back(row);
  }
  return rows;
}

inline std::string findNext(const Node* document, const std::string& selector, const std::string& baseUrl) {
  if (selector.empty()) return std::string();
  const Node* element = queryOne(document, selector);
  if (!element) return std::string();
  std::string href = attributeOr(element, "href");
  if (href.empty()) {
    const Node* anchor = queryOne(element, "a[href]");
    if (anchor) href = attributeOr(anchor, "href");
  }
  if (href.empty()) return std::string();
  return stripFragment(resolveUrl(baseUrl, href));
}

inline std::vector<Row> dedupeRows(const std::vector<Row>& rows) {
  std::set<std::string> seen;
  std::vector<Row> unique;
  for (const auto& row : rows) {
    std::string key;
    for (const auto& entry : row) key += entry.first + '\x1f' + entry.second + '\x1e';
    if (seen.insert(key).second) unique.push_back(row);
  }
  return unique;
}

using FetchFn = std::function<std::string(const std::string&, int, const std::string&, int)>;

inline std::string httpGetQuiet(const std::string& url, int timeoutMs, const std::string& userAgent) {
  return httpGetOnce(url, timeoutMs, userAgent, true);
}

inline std::pair<std::vector<Row>, int> crawl(const Spec& spec, FetchFn fetch = FetchFn()) {
  std::vector<Row> rows;
  std::set<std::string> visited;
  std::map<std::string, std::string> robotsCache;
  std::string url = spec.url;
  int pages = 0;

  while (!url.empty() && (spec.maxPages == 0 || pages < spec.maxPages)) {
    if (visited.count(url)) break;
    visited.insert(url);

    if (!spec.ignoreRobots) {
      const std::string origin = originOf(url);
      if (!robotsCache.count(origin)) {
        std::string body;
        bool have = false;
        try {
          body = fetch ? fetch(origin + "/robots.txt", spec.timeout, spec.userAgent, 0)
                       : httpGetQuiet(origin + "/robots.txt", spec.timeout, spec.userAgent);
          have = true;
        } catch (const std::exception&) {
          have = false;
        }
        robotsCache[origin] = have ? body : "";
        if (!have) robotsCache[origin] = "";
      }
      const bool haveBody = !robotsCache[origin].empty();
      if (!robotsAllowed(url, spec.userAgent, robotsCache[origin], haveBody)) {
        throw std::runtime_error("robots.txt disallows " + url + " (override with --ignore-robots)");
      }
    }

    const std::string html =
        fetch ? fetch(url, spec.timeout, spec.userAgent, spec.retries)
              : httpGet(url, spec.timeout, spec.userAgent, spec.retries);
    const auto document = parseHTML(html);
    auto pageRows = extractRows(document.get(), url, spec);
    for (auto& row : pageRows) rows.push_back(std::move(row));
    ++pages;
    if (!spec.quiet) {
      std::fprintf(stderr, "page %d: %s (%zu rows)\n", pages, url.c_str(), rows.size());
    }

    if (spec.limit > 0 && static_cast<int>(rows.size()) >= spec.limit) {
      rows.resize(static_cast<size_t>(spec.limit));
      break;
    }

    const std::string nextUrl = spec.next.empty() ? std::string() : findNext(document.get(), spec.next, url);
    if (nextUrl.empty() || visited.count(nextUrl)) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(spec.delay));
    url = nextUrl;
  }

  if (spec.dedupe) rows = dedupeRows(rows);
  return {rows, pages};
}

inline std::vector<std::string> columnNames(const std::vector<Row>& rows) {
  std::vector<std::string> names;
  for (const auto& row : rows) {
    for (const auto& entry : row) {
      if (std::find(names.begin(), names.end(), entry.first) == names.end()) names.push_back(entry.first);
    }
  }
  return names;
}

inline std::string csvEscape(const std::string& value, char delimiter) {
  const bool needsQuotes = value.find(delimiter) != std::string::npos ||
                           value.find('"') != std::string::npos ||
                           value.find('\n') != std::string::npos ||
                           value.find('\r') != std::string::npos ||
                           (!value.empty() && (std::isspace(static_cast<unsigned char>(value.front())) ||
                                               std::isspace(static_cast<unsigned char>(value.back()))));
  if (!needsQuotes) return value;
  std::string out = "\"";
  for (const char ch : value) {
    if (ch == '"') out += "\"\"";
    else out.push_back(ch);
  }
  out += "\"";
  return out;
}

inline std::string jsonEscape(const std::string& value) {
  std::string out;
  for (const char ch : value) {
    const unsigned char byte = static_cast<unsigned char>(ch);
    switch (ch) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (byte < 0x20) {
          char buffer[8];
          std::snprintf(buffer, sizeof(buffer), "\\u%04x", byte);
          out += buffer;
        } else {
          out.push_back(ch);
        }
    }
  }
  return out;
}

inline bool isDataFormat(const std::string& format) {
  return format == "csv" || format == "tsv" || format == "json" || format == "jsonl" || format == "txt";
}

inline std::string render(const std::vector<Row>& rows, const std::string& format) {
  const auto names = columnNames(rows);

  if (format == "csv" || format == "tsv") {
    const char delimiter = format == "tsv" ? '\t' : ',';
    std::string out;
    for (size_t index = 0; index < names.size(); ++index) {
      if (index) out.push_back(delimiter);
      out += csvEscape(names[index], delimiter);
    }
    out += "\n";
    for (const auto& row : rows) {
      for (size_t index = 0; index < names.size(); ++index) {
        if (index) out.push_back(delimiter);
        const auto found = std::find_if(row.begin(), row.end(),
                                        [&](const std::pair<std::string, std::string>& entry) {
                                          return entry.first == names[index];
                                        });
        out += csvEscape(found == row.end() ? std::string() : found->second, delimiter);
      }
      out += "\n";
    }
    return out;
  }

  if (format == "jsonl") {
    std::string out;
    for (const auto& row : rows) {
      out += "{";
      for (size_t index = 0; index < names.size(); ++index) {
        if (index) out += ", ";
        const auto found = std::find_if(row.begin(), row.end(),
                                        [&](const std::pair<std::string, std::string>& entry) {
                                          return entry.first == names[index];
                                        });
        out += "\"" + jsonEscape(names[index]) + "\": \"" +
               jsonEscape(found == row.end() ? std::string() : found->second) + "\"";
      }
      out += "}\n";
    }
    return out;
  }

  if (format == "json") {
    if (rows.empty()) return "[]\n";
    std::string out = "[\n";
    for (size_t rowIndex = 0; rowIndex < rows.size(); ++rowIndex) {
      out += "  {\n";
      for (size_t index = 0; index < names.size(); ++index) {
        const auto found = std::find_if(rows[rowIndex].begin(), rows[rowIndex].end(),
                                        [&](const std::pair<std::string, std::string>& entry) {
                                          return entry.first == names[index];
                                        });
        out += "    \"" + jsonEscape(names[index]) + "\": \"" +
               jsonEscape(found == rows[rowIndex].end() ? std::string() : found->second) + "\"";
        out += index + 1 < names.size() ? ",\n" : "\n";
      }
      out += rowIndex + 1 < rows.size() ? "  },\n" : "  }\n";
    }
    out += "]\n";
    return out;
  }

  if (format == "txt") {
    std::string out;
    const bool single = names.size() == 1 && names[0] == "value";
    for (const auto& row : rows) {
      if (single) {
        const auto found = std::find_if(row.begin(), row.end(),
                                        [](const std::pair<std::string, std::string>& entry) {
                                          return entry.first == "value";
                                        });
        out += found == row.end() ? std::string() : found->second;
      } else {
        for (size_t index = 0; index < names.size(); ++index) {
          if (index) out += " | ";
          const auto found = std::find_if(row.begin(), row.end(),
                                          [&](const std::pair<std::string, std::string>& entry) {
                                            return entry.first == names[index];
                                          });
          out += found == row.end() ? std::string() : found->second;
        }
      }
      out += "\n";
    }
    return out;
  }

  throw std::runtime_error("unknown output format: " + format);
}

inline std::string inferFormat(const std::string& path) {
  const size_t dot = path.rfind('.');
  if (dot != std::string::npos) {
    const std::string extension = toLower(path.substr(dot + 1));
    if (isDataFormat(extension)) return extension;
  }
  return "json";
}

inline void writeOutput(const std::vector<Row>& rows, const std::string& path, const std::string& format) {
  const std::string resolved = format.empty() ? inferFormat(path) : format;
  const std::string text = render(rows, resolved);
  const size_t slash = path.find_last_of("/\\");
  if (slash != std::string::npos) {
#ifdef _WIN32
    std::string directory = path.substr(0, slash);
    // Create the directory tree, ignoring "already exists".
    std::string current;
    std::istringstream stream(directory);
    std::string segment;
    while (std::getline(stream, segment, '/')) {
      if (segment.empty() && current.empty()) continue;
      current += segment + "/";
      CreateDirectoryA(current.c_str(), nullptr);
    }
#else
    std::string directory = path.substr(0, slash);
    std::string command = "mkdir -p \"" + escapeForShell(directory) + "\"";
    if (std::system(command.c_str()) != 0) { /* best effort */ }
#endif
  }
  std::ofstream stream(path, std::ios::binary);
  if (!stream) throw std::runtime_error("cannot write " + path);
  stream << text;
}

// ---------------------------------------------------------------------------
// Inspect mode
// ---------------------------------------------------------------------------

inline std::string inspectPage(const Node* document, size_t limit = 20) {
  std::map<std::pair<std::string, std::string>, std::vector<const Node*>> groups;
  for (const Node* element : allElements(document)) {
    groups[{element->tag, attributeOr(element, "class")}].push_back(element);
  }

  std::vector<std::pair<size_t, std::pair<std::string, std::string>>> candidates;
  for (const auto& entry : groups) {
    if (entry.second.size() >= 3) candidates.emplace_back(entry.second.size(), entry.first);
  }
  std::sort(candidates.begin(), candidates.end(), [](const auto& left, const auto& right) {
    if (left.first != right.first) return left.first > right.first;
    return left.second.first < right.second.first;
  });

  std::ostringstream out;
  out << "elements on page: " << allElements(document).size() << "\n";
  out << "candidate groups: " << candidates.size() << "\n\n";
  out << "selector                               count  sample\n";
  out << std::string(96, '-') << "\n";
  for (size_t index = 0; index < candidates.size() && index < limit; ++index) {
    const auto& entry = candidates[index];
    std::string selector = entry.second.first;
    if (!entry.second.second.empty()) {
      std::istringstream stream(entry.second.second);
      std::string item;
      while (stream >> item) selector += "." + item;
    }
    std::string sample = normalizeSpace(textContent(groups[entry.second].front()));
    if (sample.size() > 60) sample = sample.substr(0, 60);
    std::ostringstream line;
    line << selector;
    std::string padded = line.str();
    if (padded.size() < 38) padded += std::string(38 - padded.size(), ' ');
    out << padded << " " << std::setw(5) << entry.first << "  " << sample << "\n";
  }
  return out.str();
}

}  // namespace harvest

// ---------------------------------------------------------------------------
// Command line
// ---------------------------------------------------------------------------

namespace harvest {

inline const char* HELP = R"(harvest 1.0.0 — declarative web scraper

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
)";

inline int toInt(const std::string& value, const std::string& name) {
  try {
    size_t consumed = 0;
    const int number = std::stoi(value, &consumed);
    if (consumed != value.size() || number < 0) throw std::invalid_argument("range");
    return number;
  } catch (...) {
    throw std::runtime_error("option " + name + " expects a non-negative integer, got \"" + value + "\"");
  }
}

inline void applyRecipe(Spec& spec, const JsonValue& recipe) {
  const auto text = [&](const char* key) -> const JsonValue* {
    const JsonValue* value = member(recipe, key);
    return value && value->type == JsonValue::String ? value : nullptr;
  };
  const auto number = [&](const char* key) -> const JsonValue* {
    const JsonValue* value = member(recipe, key);
    return value && value->type == JsonValue::Number ? value : nullptr;
  };
  const auto boolean = [&](const char* key) -> const JsonValue* {
    const JsonValue* value = member(recipe, key);
    return value && value->type == JsonValue::Bool ? value : nullptr;
  };

  if (const auto* value = text("url")) spec.url = value->text;
  if (const auto* value = text("record")) spec.record = value->text;
  if (const auto* value = text("select")) spec.select = value->text;
  if (const auto* value = text("next")) spec.next = value->text;
  if (const auto* value = text("join")) spec.join = value->text;
  if (const auto* value = text("out")) spec.out = value->text;
  if (const auto* value = text("format")) spec.format = value->text;
  if (const auto* value = text("userAgent")) spec.userAgent = value->text;
  if (const auto* value = text("user_agent")) spec.userAgent = value->text;
  if (const auto* value = number("maxPages")) spec.maxPages = static_cast<int>(value->number);
  if (const auto* value = number("max_pages")) spec.maxPages = static_cast<int>(value->number);
  if (const auto* value = number("limit")) spec.limit = static_cast<int>(value->number);
  if (const auto* value = number("delay")) spec.delay = static_cast<int>(value->number);
  if (const auto* value = number("timeout")) spec.timeout = static_cast<int>(value->number);
  if (const auto* value = number("retries")) spec.retries = static_cast<int>(value->number);
  if (const auto* value = boolean("dedupe")) spec.dedupe = value->boolean;
  if (const auto* value = boolean("ignoreRobots")) spec.ignoreRobots = value->boolean;
  if (const auto* value = boolean("ignore_robots")) spec.ignoreRobots = value->boolean;

  if (const JsonValue* fields = member(recipe, "fields"); fields && fields->type == JsonValue::Object) {
    spec.fields.clear();
    for (const auto& entry : fields->members) {
      if (entry.second.type == JsonValue::String) spec.fields.emplace_back(entry.first, entry.second.text);
    }
  }
}

inline std::string loadRecipe(const std::string& path, JsonValue& target) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) throw std::runtime_error("cannot read recipe " + path);
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  try {
    target = JsonParser(buffer.str()).parseDocument();
  } catch (const std::exception& error) {
    throw std::runtime_error("recipe " + path + " is not valid JSON: " + error.what());
  }
  return path;
}

inline std::string scanRecipe(const std::vector<std::string>& argv) {
  for (size_t index = 0; index < argv.size(); ++index) {
    if (argv[index] == "--recipe") {
      if (index + 1 >= argv.size()) throw std::runtime_error("option --recipe needs a value");
      return argv[index + 1];
    }
    if (startsWith(argv[index], "--recipe=")) return argv[index].substr(9);
  }
  return std::string();
}

inline Spec parseArgs(const std::vector<std::string>& argv, Spec base = Spec()) {
  Spec spec = std::move(base);
  std::vector<std::string> positional;

  const auto valueFor = [&](const std::string& name, size_t& index,
                            const std::string& inlineValue, bool hasInline) {
    if (hasInline) return inlineValue;
    if (index + 1 >= argv.size()) throw std::runtime_error("option " + name + " needs a value");
    return argv[++index];
  };

  for (size_t index = 0; index < argv.size(); ++index) {
    const std::string argument = argv[index];
    if (argument == "-h" || argument == "--help") {
      std::cout << HELP;
      std::exit(0);
    }
    if (argument == "-v" || argument == "--version") {
      std::cout << "harvest " << VERSION << "\n";
      std::exit(0);
    }

    std::string name = argument;
    std::string inlineValue;
    bool hasInline = false;
    const size_t equals = argument.find('=');
    if (equals != std::string::npos) {
      name = argument.substr(0, equals);
      inlineValue = argument.substr(equals + 1);
      hasInline = true;
    }

    if (name == "--dedupe") { spec.dedupe = true; continue; }
    if (name == "--ignore-robots") { spec.ignoreRobots = true; continue; }
    if (name == "--quiet") { spec.quiet = true; continue; }
    if (name == "--inspect") { spec.inspect = true; continue; }
    if (name == "--recipe") { valueFor(name, index, inlineValue, hasInline); continue; }

    if (name == "-r" || name == "--record") { spec.record = valueFor(name, index, inlineValue, hasInline); continue; }
    if (name == "-s" || name == "--select") { spec.select = valueFor(name, index, inlineValue, hasInline); continue; }
    if (name == "-n" || name == "--next") { spec.next = valueFor(name, index, inlineValue, hasInline); continue; }
    if (name == "-o" || name == "--out") { spec.out = valueFor(name, index, inlineValue, hasInline); continue; }
    if (name == "--format") { spec.format = valueFor(name, index, inlineValue, hasInline); continue; }
    if (name == "--join") { spec.join = valueFor(name, index, inlineValue, hasInline); continue; }
    if (name == "--ua") { spec.userAgent = valueFor(name, index, inlineValue, hasInline); continue; }
    if (name == "--max-pages") { spec.maxPages = toInt(valueFor(name, index, inlineValue, hasInline), name); continue; }
    if (name == "--limit") { spec.limit = toInt(valueFor(name, index, inlineValue, hasInline), name); continue; }
    if (name == "--delay") { spec.delay = toInt(valueFor(name, index, inlineValue, hasInline), name); continue; }
    if (name == "--timeout") { spec.timeout = toInt(valueFor(name, index, inlineValue, hasInline), name); continue; }
    if (name == "--retries") { spec.retries = toInt(valueFor(name, index, inlineValue, hasInline), name); continue; }

    if (name == "-f" || name == "--field") {
      const std::string value = valueFor(name, index, inlineValue, hasInline);
      const size_t separator = value.find('=');
      if (separator == std::string::npos || separator == 0) {
        throw std::runtime_error("--field expects \"name=selector\", got \"" + value + "\"");
      }
      spec.fields.emplace_back(trim(value.substr(0, separator)), trim(value.substr(separator + 1)));
      continue;
    }

    if (!argument.empty() && argument[0] == '-') throw std::runtime_error("unknown option " + argument);
    positional.push_back(argument);
  }

  if (!positional.empty()) {
    if (!spec.url.empty()) throw std::runtime_error("only one URL can be given");
    spec.url = positional.front();
  }
  return spec;
}

inline void validate(const Spec& spec) {
  if (spec.url.empty()) throw std::runtime_error("no URL given");
  if (!startsWith(spec.url, "http://") && !startsWith(spec.url, "https://")) {
    throw std::runtime_error("unsupported URL scheme (expected http or https)");
  }
  if (spec.inspect) return;
  if (spec.select.empty() && (spec.fields.empty() || spec.record.empty())) {
    throw std::runtime_error("nothing to scrape: pass --record with --field, or --select");
  }
  if (!spec.select.empty() && (!spec.fields.empty() || !spec.record.empty())) {
    throw std::runtime_error("--select cannot be combined with --record/--field");
  }
  if (spec.record.empty() && !spec.fields.empty()) {
    throw std::runtime_error("--field requires --record");
  }
}

inline int run(int argc, char** argv) {
  std::vector<std::string> args(argv + 1, argv + argc);
  try {
    std::string recipePath = scanRecipe(args);
    Spec base;
    if (!recipePath.empty()) {
      JsonValue recipe;
      loadRecipe(recipePath, recipe);
      applyRecipe(base, recipe);
    }
    Spec spec = parseArgs(args, base);
    validate(spec);

    if (spec.inspect) {
      const std::string html = httpGet(spec.url, spec.timeout, spec.userAgent, spec.retries);
      const auto document = parseHTML(html);
      std::cout << inspectPage(document.get());
      return 0;
    }

    const auto result = crawl(spec);
    const auto& rows = result.first;

    if (!spec.out.empty()) {
      writeOutput(rows, spec.out, spec.format);
      if (!spec.quiet) {
        const std::string format = spec.format.empty() ? inferFormat(spec.out) : spec.format;
        std::fprintf(stderr, "wrote %zu rows from %d page(s) to %s (%s)\n", rows.size(),
                     result.second, spec.out.c_str(), format.c_str());
      }
    } else {
      const std::string format = spec.format.empty() ? "json" : spec.format;
      std::cout << render(rows, format);
    }
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "harvest: %s\n", error.what());
    return 1;
  }
}

}  // namespace harvest

#ifndef HARVEST_NO_MAIN

int main(int argc, char** argv) {
#ifdef _WIN32
  SetConsoleOutputCP(CP_UTF8);
#endif
  return harvest::run(argc, argv);
}

#endif  // HARVEST_NO_MAIN
