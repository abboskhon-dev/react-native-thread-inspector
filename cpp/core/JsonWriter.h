#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace rnti {

class JsonWriter {
 public:
  JsonWriter &beginObject() { return open('{'); }
  JsonWriter &endObject() { return close('}'); }
  JsonWriter &beginArray() { return open('['); }
  JsonWriter &endArray() { return close(']'); }

  JsonWriter &key(std::string_view k) {
    comma();
    quote(k);
    out_ += ':';
    afterKey_ = true;
    return *this;
  }

  JsonWriter &str(std::string_view v) {
    comma();
    quote(v);
    return *this;
  }

  JsonWriter &u64(uint64_t v) {
    comma();
    out_ += std::to_string(v);
    return *this;
  }

  JsonWriter &num(double v, int precision = 3) {
    comma();
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.*f", precision, v);
    out_ += buf;
    return *this;
  }

  JsonWriter &boolean(bool v) {
    comma();
    out_ += v ? "true" : "false";
    return *this;
  }

  JsonWriter &null() {
    comma();
    out_ += "null";
    return *this;
  }

  std::string take() { return std::move(out_); }

 private:
  JsonWriter &open(char c) {
    comma();
    out_ += c;
    needComma_.push_back(false);
    return *this;
  }

  JsonWriter &close(char c) {
    needComma_.pop_back();
    out_ += c;
    return *this;
  }

  void comma() {
    if (afterKey_) {
      afterKey_ = false;
      return;
    }
    if (!needComma_.empty()) {
      if (needComma_.back()) out_ += ',';
      needComma_.back() = true;
    }
  }

  // Length of a valid UTF-8 sequence at s[i], or 0.
  static size_t utf8Len(std::string_view s, size_t i) {
    const auto b = [&](size_t k) { return static_cast<unsigned char>(s[k]); };
    const unsigned char c = b(i);
    size_t n;
    uint32_t cp;
    if (c >= 0xc2 && c <= 0xdf) n = 2, cp = c & 0x1f;
    else if (c >= 0xe0 && c <= 0xef) n = 3, cp = c & 0x0f;
    else if (c >= 0xf0 && c <= 0xf4) n = 4, cp = c & 0x07;
    else return 0;
    if (i + n > s.size()) return 0;
    for (size_t k = 1; k < n; ++k) {
      if ((b(i + k) & 0xc0) != 0x80) return 0;
      cp = (cp << 6) | (b(i + k) & 0x3f);
    }
    if ((n == 3 && (cp < 0x800 || (cp >= 0xd800 && cp <= 0xdfff))) || (n == 4 && (cp < 0x10000 || cp > 0x10ffff)))
      return 0;
    return n;
  }

  void quote(std::string_view s) {
    out_ += '"';
    for (size_t i = 0; i < s.size();) {
      const unsigned char c = static_cast<unsigned char>(s[i]);
      if (c >= 0x80) {
        // Labels can come from foreign pointers; never emit a file JSON parsers reject.
        const size_t n = utf8Len(s, i);
        if (n) out_.append(s.substr(i, n));
        else out_ += "\\ufffd";
        i += n ? n : 1;
        continue;
      }
      switch (c) {
        case '"': out_ += "\\\""; break;
        case '\\': out_ += "\\\\"; break;
        case '\n': out_ += "\\n"; break;
        case '\r': out_ += "\\r"; break;
        case '\t': out_ += "\\t"; break;
        default:
          if (c < 0x20) {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\u%04x", c);
            out_ += buf;
          } else {
            out_ += static_cast<char>(c);
          }
      }
      ++i;
    }
    out_ += '"';
  }

  std::string out_;
  std::vector<bool> needComma_;
  bool afterKey_ = false;
};

} // namespace rnti
