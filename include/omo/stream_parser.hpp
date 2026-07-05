#pragma once

#include "omo/token_feeder.hpp"
#include "omo/token.hpp"

#include <boost/json.hpp>

#include <ranges>

namespace omo {

inline std::string_view cut_json_value(std::string_view& json) {
  boost::json::basic_parser<boostjson_noop_handler> p(/*opt=*/{});
  boost::system::error_code ec;
  size_t cutted;
  try {
    cutted = p.write_some(/*more=*/true, json.data(), json.size(), ec);
  } catch (std::exception& e) {
    throw parse_error("invalid json, {}", e.what());
  }
  if (ec)
    throw parse_error("invalid json, {}", ec.message());
  std::string_view json_value = json.substr(0, cutted);
  json.remove_prefix(cutted);
  return json_value;
}

struct stream_parser {
  boost::json::basic_parser<boostjson_token_feeder> p;

  // `make_parser` должно вернуть omo::tokens_consumer
  explicit stream_parser(std::invocable<sax_token&, dd::with_stack_resource> auto make_parser,
                         std::span<unsigned char> buf = {})
      : p(/*options*/ {}, std::move(make_parser), buf) {
  }

  // Note: MUST NOT be reused after parsing value
  // each value should be parsed by new stream_parser
  // возвращает количество успешно обработанных символов (может быть меньше data.size())
  // если одно json value распаршено и есть следующее, то парсинг прекращается и возвращается длина
  // только обработанных данных (до следующего значения)
  size_t feed(std::string_view data, bool end) {
    boost::system::error_code ec;
    size_t ret;
    try {
      ret = p.write_some(!end, data.data(), data.size(), ec);
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
    return ret;
  }

  // значение было распаршено
  [[nodiscard]] bool is_done() const noexcept {
    return p.handler().is_done();
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

  // если одно json value распаршено и есть следующее, то парсинг прекращается
  // если после feed is_done() == true, но не все токены обработаны, это значит что оставшееся - другое json
  // значение
  // возвращает количество обработанных токенов
  // pre: tokens НЕ содержит part
  template <std::ranges::range R>
  size_t feed(R&& tokens, bool end) {
    auto it = gen.cur_iterator();
    constexpr bool can_distance = std::ranges::random_access_range<R>;
    size_t handled = 0;
    auto b = std::ranges::begin(tokens);
    auto e = std::ranges::end(tokens);
    for (; b != e; ++b) {
      if (it == gen.end()) [[unlikely]]  // парсер кончился раньше чем токены
        break;
      auto& t = *b;
      // инвариант, что генератор никогда не увидит part
      assert(t.kind() != sax_token::part);
      tok = t.to_view();
      ++it;
      if constexpr (!can_distance)
        ++handled;
    }
    if (end && it != gen.end())  // токены кончились раньше парсера
      throw parse_error("input is over, but parsing not ended");
    if constexpr (can_distance)
      return std::distance(std::ranges::begin(tokens), b);
    else
      return handled;
  }

  // значение было распаршено
  [[nodiscard]] bool is_done() const noexcept {
    return gen.empty();
  }
};

// `on_token` вызывается передавая tok каждый раз когда новый токен получен
// может выдавать и part токены
inline tokens_consumer sax_ignore_json_value(sax_token& tok, auto on_token, dd::with_stack_resource r) {
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
      case sax_token::part:
        co_yield {};
        on_token(tok);
        break;
    }
  }
}

// не может выдать part токен, складывает их корректно в один конечный токен
inline tokens_consumer collect_json_value_tokens(sax_token& token, auto on_token, dd::with_stack_resource r) {
  auto foo = [part = std::optional<sax_token_value>{}, on_token = std::move(on_token)](sax_token& s) mutable {
    if (part) [[unlikely]] {
      if (s.kind() == sax_token::part) {
        part->combine_parts(s.get_str());
      } else if (s.kind() == sax_token::key || s.kind() == sax_token::string) {
        part->replace_part(s.get_str(), s.kind());
        sax_token part_tok = part->to_view();
        on_token(part_tok);  // если надо сохранить парт, то в on_token будет создано значение заново
        part.reset();
      } else {
        throw parse_error("invalid `part` token");
      }
      return;
    }
    if (s.kind() == sax_token::part) [[unlikely]] {
      part = s;
      return;
    }
    on_token(s);
    return;
  };
  return sax_ignore_json_value(token, std::move(foo), r);
};

}  // namespace omo
