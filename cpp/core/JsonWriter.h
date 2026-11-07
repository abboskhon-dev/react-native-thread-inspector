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

  void quote(std::string_view s) {
    out_ += '"';
    for (unsigned char c : s) {
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
    }
    out_ += '"';
  }

  std::string out_;
  std::vector<bool> needComma_;
  bool afterKey_ = false;
};

} // namespace rnti
