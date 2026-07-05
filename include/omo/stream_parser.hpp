#pragma once

#include "omo/consumer.hpp"
#include "omo/token.hpp"

#include <boost/json.hpp>

#include <ranges>

namespace omo {

struct stream_parser {
  boost::json::basic_parser<boostjson_token_feeder> p;

  // `make_parser` должно вернуть omo::tokens_consumer
  explicit stream_parser(std::invocable<sax_token&, dd::with_stack_resource> auto make_parser,
                         std::span<unsigned char> buf = {})
      : p(/*options*/ {}, std::move(make_parser), buf) {
  }

  // Note: MUST NOT be reused after parsing value
  // each value should be parsed by new stream_parser
  void feed(std::string_view data, bool end) {
    boost::system::error_code ec;
    try {
      p.write_some(!end, data.data(), data.size(), ec);
    } catch (std::exception& e) {
      throw parse_error("stream_parser fail, err: `{}`, is last part: {}, input: `{}`", e.what(), end, data);
    }
    if (ec) [[unlikely]] {
      throw parse_error("stream_parser fail, err: `{}`, is last part: {}, input: `{}`", ec.message(), end,
                        data);
    }
    // The json output has ended, but the generator is still expecting more tokens
    if (end && !p.handler().is_done()) [[unlikely]]
      throw parse_error("input is over, but parsing not ended, input: `{}`", data);
  }
};

struct stream_parser_tokens {
 private:
  sax_token tok;
  dd::stack_resource resource;
  tokens_consumer gen;

 public:
  // Note: каждое значение должно обрабатываться своим stream_parse_tokens
  // `make_parser` должно вернуть omo::tokens_consumer
  explicit stream_parser_tokens(std::invocable<sax_token&, dd::with_stack_resource> auto make_parser,
                                std::span<unsigned char> buf = {})
      : resource(buf), gen(make_parser(tok, dd::with_stack_resource(resource))) {
  }

  // pre: tokens НЕ содержит part
  void feed(std::ranges::range auto&& tokens, bool end) {
    auto it = gen.cur_iterator();
    for (const sax_token_value& t : tokens) {
      // инвариант, что генератор никогда не увидит part
      assert(t.kind() != sax_token::part);
      if (it == gen.end()) [[unlikely]]  // парсер кончился раньше чем токены
        throw parse_error("parser ends, but not all tokens are consumed");
      tok = t.to_view();
      ++it;
    }
    if (end && it != gen.end())  // токены кончились раньше парсера
      throw parse_error("input is over, but parsing not ended");
  }

  // то же самое, что feed но для вью на токены
  // pre: tokens НЕ содержит part
  void feed_views(std::ranges::range auto&& tokens, bool end) {
    auto it = gen.cur_iterator();
    for (const sax_token& t : tokens) {
      // инвариант, что генератор никогда не увидит part
      assert(t.kind() != sax_token::part);
      if (it == gen.end()) [[unlikely]]  // парсер кончился раньше чем токены
        throw parse_error("parser ends, but not all tokens are consumed");
      tok = t;
      ++it;
    }
    if (end && it != gen.end())  // токены кончились раньше парсера
      throw parse_error("input is over, but parsing not ended");
  }
};

// `on_token` вызывается передавая tok каждый раз когда новый токен получен
inline tokens_consumer sax_ignore_json_value(sax_token& tok, auto on_token) {
  on_token(tok);
  switch (tok.kind()) {
    case sax_token::string:
    case sax_token::int64:
    case sax_token::uint64:
    case sax_token::double_:
    case sax_token::bool_:
    case sax_token::null:
      co_return;
    default:
      break;
  }
  size_t depth = 0;
  for (;;) {
    switch (tok.kind()) {
      case sax_token::array_begin:
      case sax_token::object_begin:
        ++depth;
        co_yield {};
        on_token(tok);
        break;
      case sax_token::part:
        unreachable();
      case sax_token::array_end:
      case sax_token::object_end:
        --depth;
        if (depth == 0)
          co_return;
        [[fallthrough]];
      case sax_token::string:
      case sax_token::int64:
      case sax_token::uint64:
      case sax_token::double_:
      case sax_token::bool_:
      case sax_token::null:
      case sax_token::key:
        co_yield {};
        on_token(tok);
        break;
    }
  }
}

}  // namespace omo
