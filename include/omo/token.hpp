#pragma once

#include <cstring>
#include <format>

#include <kelcoro/generator.hpp>
#include <kelcoro/noexport/macro.hpp>  // KELCORO_UNREACHABLE

namespace omo {

[[noreturn]] inline void unreachable() noexcept {
  assert(false);
  KELCORO_UNREACHABLE;
}

struct parse_error : std::runtime_error {
  template <typename... Args>
  explicit parse_error(std::format_string<Args...> fmt, Args&&... args)
      : std::runtime_error(std::format(fmt, std::forward<Args>(args)...)) {
  }
  explicit parse_error(const char* p) : std::runtime_error(p) {
  }
};

struct sax_token {
  enum kind_e : uint16_t {
    array_begin = 1 << 0,
    array_end = 1 << 1,
    object_begin = 1 << 2,
    object_end = 1 << 3,
    string = 1 << 4,
    key = 1 << 5,
    int64 = 1 << 6,
    uint64 = 1 << 7,
    double_ = 1 << 8,
    bool_ = 1 << 9,
    null = 1 << 10,
    part = 1 << 11,
  };

 private:
  friend struct sax_token_value;

  union {
    double double_m;
    int64_t int_m;
    uint64_t uint_m;
    std::string_view str_m;
    bool bool_m;
  };

  kind_e got = sax_token::object_begin;

 public:
  sax_token() {
  }
  ~sax_token() {
  }

  void expect(uint16_t tokens) const {
    if (!(got & tokens))
      throw parse_error("unexpected token bits `{:b}`, expected bits: `{:b}`", uint16_t(got), tokens);
  }
  [[nodiscard]] kind_e kind() const noexcept {
    return got;
  }

  static bool is_string(sax_token::kind_e got) noexcept {
    using enum sax_token::kind_e;
    switch (got) {
      case string:
      case key:
      case part:
        return true;
      case array_begin:
      case array_end:
      case object_begin:
      case object_end:
      case int64:
      case uint64:
      case double_:
      case bool_:
      case null:
        return false;
    }
    unreachable();
  }

  const double& get_double() const noexcept {
    assert(got == double_);
    return double_m;
  }
  const int64_t& get_int64() const noexcept {
    assert(got == int64);
    return int_m;
  }
  const uint64_t& get_uint64() const noexcept {
    assert(got == uint64);
    return uint_m;
  }
  // любая строка, part/key/string
  std::string_view get_str() const noexcept {
    assert(is_string(got));
    return str_m;
  }
  std::string_view get_key() const noexcept {
    assert(got == key);
    return str_m;
  }
  std::string_view get_part() const noexcept {
    assert(got == part);
    return str_m;
  }
  const bool& get_bool() const noexcept {
    assert(got == bool_);
    return bool_m;
  }

  void set_double(double d) noexcept {
    got = double_;
    double_m = d;
  }
  void set_int64(int64_t i) noexcept {
    got = int64;
    int_m = i;
  }
  void set_uint64(uint64_t u) noexcept {
    got = uint64;
    uint_m = u;
  }
  // pre: kind одно из key/part/string
  void set_str(std::string_view str, kind_e kind) noexcept {
    assert(kind == part || kind == key || kind == string);
    got = kind;
    str_m = str;
  }
  void set_part(std::string_view str) noexcept {
    got = part;
    str_m = str;
  }
  void set_key(std::string_view str) noexcept {
    got = key;
    str_m = str;
  }
  void set_bool(bool b) noexcept {
    got = bool_;
    bool_m = b;
  }
  void set_null() noexcept {
    got = null;
  }
  void set_object_begin() noexcept {
    got = object_begin;
  }
  void set_object_end() noexcept {
    got = object_end;
  }
  void set_array_begin() noexcept {
    got = array_begin;
  }
  void set_array_end() noexcept {
    got = array_end;
  }
};

struct sax_token_value {
 private:
  union {
    double double_m;
    int64_t int_m;
    uint64_t uint_m;
    std::string str_m;
    bool bool_m;
  };
  sax_token::kind_e got = sax_token::object_begin;

 public:
  sax_token_value() {
  }

  sax_token_value(sax_token token) {
    using enum sax_token::kind_e;
    got = token.got;
    switch (got) {
      case string:
      case key:
      case part:
        std::construct_at(&str_m, token.str_m);
        [[fallthrough]];
      case array_begin:
      case array_end:
      case object_begin:
      case object_end:
      case null:
        return;
      case int64:
        int_m = token.int_m;
        return;
      case uint64:
        uint_m = token.uint_m;
        return;
      case double_:
        double_m = token.double_m;
        return;
      case bool_:
        bool_m = token.bool_m;
        return;
    }
  }

  sax_token_value(sax_token_value&& rhs) noexcept {
    if (is_string(rhs.got)) {
      got = rhs.got;
      std::construct_at(&str_m, std::move(rhs.str_m));
    } else {
      std::memcpy((void*)this, &rhs, sizeof(*this));
    }
  }

  sax_token_value& operator=(sax_token_value&& rhs) noexcept {
    assert(&rhs != this);  // for perf
    std::destroy_at(this);
    std::construct_at(this, std::move(rhs));
    return *this;
  }

  [[nodiscard]] sax_token::kind_e kind() const noexcept {
    return got;
  }

  // позволяет вместо нескольких токенов part создать один с более длинной строкой
  // pre: *this это part
  void combine_parts(std::string_view str) {
    assert(is_string(kind()));
    str_m += str;
  }

  // позволяет заменить часть на полноценный токен складывая значение с 'str'
  // например part("a") + part("b") + key("c") = key("abc")
  // pre: k должен быть key/string
  void replace_part(std::string_view str, sax_token::kind_e k) {
    assert(k == sax_token::key || k == sax_token::string);
    assert(kind() == sax_token::part);
    str_m += str;
    got = k;
  }

  sax_token to_view() const noexcept {
    using enum sax_token::kind_e;
    sax_token token;
    token.got = got;
    switch (got) {
      case string:
      case key:
      case part:
        token.str_m = str_m;
        [[fallthrough]];
      case array_begin:
      case array_end:
      case object_begin:
      case object_end:
      case null:
        break;
      case int64:
        token.int_m = int_m;
        break;
      case uint64:
        token.uint_m = uint_m;
        break;
      case double_:
        token.double_m = double_m;
        break;
      case bool_:
        token.bool_m = bool_m;
        break;
    }

    return token;
  }

  static bool is_string(sax_token::kind_e got) noexcept {
    return sax_token::is_string(got);
  }

  ~sax_token_value() {
    destroy();
  }

  void expect(uint16_t tokens) const {
    if (!(got & tokens))
      throw parse_error("unexpected token bits `{:b}`, expected bits: `{:b}`", uint16_t(got), tokens);
  }

 private:
  void destroy() noexcept {
    if (is_string(got)) {
      std::destroy_at(&str_m);
      got = sax_token::object_begin;
    }
  }
};

using tokens_consumer = dd::generator<dd::nothing_t>;

template <typename>
struct sax_parser {
  // required interface:
  //   static tokens_consumer parse(T& v, sax_token& tok)
  // Note: 'tok' never will be 'part', its handled by token producer
};

}  // namespace omo

namespace std {

template <>
struct formatter<::omo::sax_token> : std::formatter<std::string_view> {
  auto format(::omo::sax_token const& tok, auto& ctx) const -> decltype(ctx.out()) {
    switch (tok.kind()) {
      case ::omo::sax_token::array_begin:
        return std::format_to(ctx.out(), "array_begin");
      case ::omo::sax_token::array_end:
        return std::format_to(ctx.out(), "array_end");
      case ::omo::sax_token::object_begin:
        return std::format_to(ctx.out(), "object_begin");
      case ::omo::sax_token::object_end:
        return std::format_to(ctx.out(), "object_end");
      case ::omo::sax_token::string:
        return std::format_to(ctx.out(), "string `{}`", tok.get_str());
      case ::omo::sax_token::key:
        return std::format_to(ctx.out(), "key `{}`", tok.get_key());
      case ::omo::sax_token::int64:
        return std::format_to(ctx.out(), "int64 `{}`", tok.get_int64());
      case ::omo::sax_token::uint64:
        return std::format_to(ctx.out(), "uint64 `{}`", tok.get_uint64());
      case ::omo::sax_token::double_:
        return std::format_to(ctx.out(), "double `{}`", tok.get_double());
      case ::omo::sax_token::bool_:
        return std::format_to(ctx.out(), "boolean `{}`", tok.get_bool());
      case ::omo::sax_token::null:
        return std::format_to(ctx.out(), "null");
      case ::omo::sax_token::part:
        return std::format_to(ctx.out(), "part `{}`", tok.get_part());
    }
    ::omo::unreachable();
  }
};

}  // namespace std
