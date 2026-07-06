#pragma once

#include "omo/token_feeder.hpp"
#include "omo/token.hpp"

#include <boost/json.hpp>

#include <ranges>

namespace omo {

struct stream_parser {
  boost::json::basic_parser<boostjson_token_feeder> p;

  // `make_parser` должно вернуть omo::tokens_consumer
  // Note: при вызове make_parser токен ещё не имеет смысла, его нельзя пытаться интерпретировать
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

namespace noexport {

static const sax_token& as_token_view(const sax_token& s) {
  return s;
}
static sax_token as_token_view(const sax_token_value& v) {
  return v.to_view();
}

}  // namespace noexport

// возвращает итератор на первый необработанный токен
// останавливается если 'c' больше не может принимать токены
// pre: `tok` это тот токен с которого забирает токены 'c'
template <std::ranges::range R>
std::ranges::iterator_t<R> feed_tokens_to(tokens_consumer& c, sax_token& tok, R&& tokens) {
  auto it = c.cur_iterator();

  auto b = std::ranges::begin(tokens);
  auto e = std::ranges::end(tokens);
  for (; b != e; ++b) {
    if (it == c.end()) [[unlikely]]  // парсер кончился раньше чем токены
      break;
    auto& t = *b;
    // инвариант, что генератор никогда не увидит part
    assert(t.kind() != sax_token::part);
    tok = noexport::as_token_view(t);
    ++it;
  }

  return b;
}

struct stream_parser_tokens {
 private:
  sax_token tok;
  dd::stack_resource resource;
  tokens_consumer consumer;

 public:
  // Note: каждое значение должно обрабатываться своим stream_parse_tokens
  // `make_parser` должно вернуть omo::tokens_consumer
  // Note: при вызове make_parser токен ещё не имеет смысла, его нельзя пытаться интерпретировать
  explicit stream_parser_tokens(std::invocable<sax_token&, dd::with_stack_resource> auto make_parser,
                                std::span<unsigned char> buf = {})
      : resource(buf), consumer(make_parser(tok, dd::with_stack_resource(resource))) {
  }

  // если одно json value распаршено и есть следующее, то парсинг прекращается
  // если после feed is_done() == true, но не все токены обработаны, это значит что оставшееся - другое json
  // значение
  // возвращает итератор на первый необработанный токен (или end если обработаны все)
  // pre: tokens НЕ содержит part
  template <std::ranges::range R>
  std::ranges::iterator_t<R> feed(R&& tokens, bool end) {
    auto b = feed_tokens_to(consumer, tok, std::forward<R>(tokens));
    if (end && consumer.cur_iterator() != consumer.end())  // токены кончились раньше парсера
      throw parse_error("input is over, but parsing not ended");
    return b;
  }

  // значение было распаршено
  [[nodiscard]] bool is_done() const noexcept {
    return consumer.empty();
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
}

// Внезапно... Собирает boost json value из токенов.
// Важная вещь для совмещения подходов dom/sax парсинга, т.к. позволяет сделать заглушку в sax парсинге
// переиспользуя dom парсинг
//
// бросает исключение только при навалидном json
//
// pre: токены не должны содержать part
tokens_consumer json_value_collector(sax_token& tok, boost::json::value& out, dd::with_stack_resource r);

// возвращает итератор на первый элемент представляющий следующее значение
// выставляет found_value_end = true, если конец значения удалось найти
//
// pre: json подан с начала, предыдущие токены не пропущены
template <std::ranges::range R>
[[nodiscard]] std::ranges::iterator_t<R> cut_json_value(R&& json, bool& found_value_end) {
  stream_parser_tokens parser([](sax_token& tok, dd::with_stack_resource r) {
    return sax_ignore_json_value(tok, [](sax_token&) {}, r);
  });

  auto it = parser.feed(std::forward<R>(json), /*end=*/false);
  found_value_end = parser.is_done();
  return it;
}

// возвращает пустую строку, если `json` не содержит полного джсон значения
// pre: json подан с начала, предыдущие данные не пропущены
inline std::string_view cut_json_value(std::string_view& json) {
  stream_parser parser([](sax_token& tok, dd::with_stack_resource r) {
    return sax_ignore_json_value(tok, [](sax_token&) {}, r);
  });
  // end везде false, чтобы не спровоцировать ошибку, если просто не хватило джсона до полного значения
  size_t cutted = parser.feed(json, /*end=*/false);
  if (!parser.is_done()) {
    // добавляем пробел, чтобы избежать неоднозначности:
    // json = "43" может означать как просто значение 43, так и часть json "4321"
    parser.feed(" ", /*end=*/false);
    if (!parser.is_done())
      return {};
  }
  std::string_view json_value = json.substr(0, cutted);
  json.remove_prefix(cutted);
  return json_value;
}

}  // namespace omo
