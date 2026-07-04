#pragma once

#include <omo/consumer.hpp>
#include <omo/token.hpp>

#include <boost/json.hpp>

namespace omo {

struct stream_parser {
  boost::json::basic_parser<boostjson_token_feeder> p;

  // `make_parser` должно вернуть omo::tokens_consumer
  explicit stream_parser(std::invocable<sax_token&, dd::with_stack_resource> auto make_parser,
                         std::span<unsigned char> buf = {})
      : p(/*options*/ {}, std::move(make_parser), buf) {
  }

  // Note: MUST NOT be reused after parsing value.
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

}  // namespace omo
