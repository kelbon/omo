#include "omo/stream_parser.hpp"

namespace omo {

static tokens_consumer json_object_collector(sax_token& tok, boost::json::object& out,
                                             dd::with_stack_resource r) {
  assert(tok.kind() == sax_token::object_begin);
  for (;;) {
    co_yield {};
    if (tok.kind() == sax_token::object_end)
      break;
    tok.expect(sax_token::key);
    // нельзя хранить стринг вью, т.к. tok может указывать на part, который может быть удалён
    std::string key(tok.get_str());
    co_yield {};
    boost::json::value property;
    co_yield dd::elements_of(json_value_collector(tok, property, r));
    // берём первый попавшийся, дублирующиеся ключи игнорируем
    out.emplace(key, std::move(property));
  }
}

static tokens_consumer json_array_collector(sax_token& tok, boost::json::array& out,
                                            dd::with_stack_resource r) {
  assert(tok.kind() == sax_token::array_begin);
  for (;;) {
    co_yield {};
    if (tok.kind() == sax_token::array_end)
      break;
    boost::json::value elem;
    co_yield dd::elements_of(json_value_collector(tok, elem, r));
    out.push_back(std::move(elem));
  }
}

static tokens_consumer json_value_collector_impl(sax_token& tok, boost::json::value& out,
                                                 dd::with_stack_resource r) {
  switch (tok.kind()) {
    case sax_token::string:
      out = boost::json::string(tok.get_str());
      return {};
    case sax_token::int64:
      out = tok.get_int64();
      return {};
    case sax_token::uint64:
      out = tok.get_uint64();
      return {};
    case sax_token::double_:
      out = tok.get_double();
      return {};
    case sax_token::bool_:
      out = tok.get_bool();
      return {};
    case sax_token::null:
      out = nullptr;
      return {};
    case sax_token::array_begin:
      out = boost::json::array{};
      return json_array_collector(tok, out.as_array(), r);
    case sax_token::object_begin:
      out = boost::json::object{};
      return json_object_collector(tok, out.as_object(), r);
    case sax_token::array_end:
    case sax_token::object_end:
    case sax_token::key:
    case sax_token::part:
      throw parse_error("json value cannot start with token `{}`", tok);
  }
}

tokens_consumer json_value_collector(sax_token& tok, boost::json::value& out, dd::with_stack_resource r) {
  // просто откладывает инстанцирование на 1 шаг, чтобы не пытаться интерпретировать `tok` пока он ещё не
  // имеет смысла
  co_yield dd::elements_of(json_value_collector_impl(tok, out, r));
}

}  // namespace omo
