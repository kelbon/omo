#include "omo/stream_parser.hpp"
#include <moko3/moko3.hpp>

using omo::parse_error;
using omo::sax_token;
using omo::sax_token_value;
using omo::stream_parser;
using omo::stream_parser_tokens;
using omo::tokens_consumer;

static std::vector<sax_token_value> into_tokens(std::string_view json) {
  std::vector<sax_token_value> tokens;
  omo::stream_parser p([&](sax_token& tok, dd::with_stack_resource r) {
    return omo::sax_ignore_json_value(tok, [&tokens](sax_token& t) { tokens.emplace_back(t); }, r);
  });
  p.feed(json, /*end=*/true);
  return tokens;
}

enum mode_e {
  whole,
  by_one,
  two_parts,
};

static void feed(stream_parser& p, std::string_view json, mode_e mode) {
  switch (mode) {
    case whole:
      p.feed(json, /*end=*/true);
      break;
    case by_one:
      while (!json.empty() && !p.p.handler().is_done()) {
        p.feed(json.substr(0, 1), /*end=*/json.size() == 1);
        json.remove_prefix(1);
      }
      break;
    case two_parts: {
      auto mid = json.size() / 2;
      p.feed(json.substr(0, mid), /*end=*/false);
      if (!p.is_done())  // не продолжаем парсинг, если значение уже спаршено (идёт следующее значение уже)
        p.feed(json.substr(mid), /*end=*/true);
      break;
    }
  }
}

static void feed(stream_parser_tokens& p, std::span<const sax_token_value> json, mode_e mode) {
  switch (mode) {
    case whole:
      p.feed(json, /*end=*/true);
      break;
    case by_one:
      while (!json.empty() && !p.is_done()) {
        p.feed(std::span(json).subspan(0, 1), /*end=*/json.size() == 1);
        json = json.subspan(1);
      }
      break;
    case two_parts: {
      auto mid = json.size() / 2;
      p.feed(json.subspan(0, mid), /*end=*/false);
      if (!p.is_done())  // не продолжаем парсинг, если значение уже спаршено (идёт следующее значение уже)
        p.feed(json.subspan(mid), /*end=*/true);
      break;
    }
  }
}

static auto just_collect_value_tokens(std::vector<sax_token_value>& tokens) {
  return [&tokens](sax_token& tok, dd::with_stack_resource r) {
    return omo::collect_json_value_tokens(tok, [&tokens](sax_token& t) { tokens.emplace_back(t); }, r);
  };
}

static std::vector<sax_token_value> into_tokens_by_parts(std::string_view json, mode_e mode) {
  std::vector<sax_token_value> tokens;
  omo::stream_parser p(just_collect_value_tokens(tokens));
  feed(p, json, mode);
  return tokens;
}

struct TestObject {
  // неявное сравнение double должно работать корректно
  bool operator==(const TestObject&) const = default;

  int64_t int_field = 0;
  std::optional<double> double_field;
  std::optional<bool> bool_field;
  std::optional<std::string> string_field;
  std::optional<std::vector<int64_t>> array_field;
  std::optional<std::vector<std::optional<TestObject>>> nested_objects;
};

static tokens_consumer parse(int64_t& v, sax_token& tok, dd::with_stack_resource) {
  using enum sax_token::kind_e;
  tok.expect(tok.int64 | tok.uint64);
  switch (tok.kind()) {
    case int64:
      v = tok.get_int64();
      return {};
    case uint64:
      v = tok.get_uint64();
      return {};
    default:
      omo::unreachable();
  }
}

static tokens_consumer parse(double& v, sax_token& tok, dd::with_stack_resource) {
  using enum sax_token::kind_e;
  tok.expect(tok.int64 | tok.uint64 | tok.double_);
  switch (tok.kind()) {
    case int64:
      v = tok.get_int64();
      return {};
    case uint64:
      v = tok.get_uint64();
      return {};
    case double_:
      v = tok.get_double();
      return {};
    default:
      omo::unreachable();
  }
}

static tokens_consumer parse(bool& v, sax_token& tok, dd::with_stack_resource) {
  tok.expect(tok.bool_);
  v = tok.get_bool();
  return {};
}

static tokens_consumer parse(std::string& v, sax_token& tok, dd::with_stack_resource) {
  tok.expect(tok.string);
  v = tok.get_str();
  return {};
}

template <typename T>
static tokens_consumer parse(std::vector<T>& v, sax_token& tok, dd::with_stack_resource r) {
  tok.expect(sax_token::array_begin);
  for (;;) {
    co_yield {};
    if (tok.kind() == sax_token::array_end)
      break;
    co_yield dd::elements_of(parse(v.emplace_back(), tok, r));
  }
}

template <typename T>
static tokens_consumer parse(std::optional<T>& v, sax_token& tok, dd::with_stack_resource r) {
  if (tok.kind() == sax_token::null) [[unlikely]]
    return {};
  return parse(v.emplace(T{}), tok, r);
}

static tokens_consumer parse(TestObject& v, sax_token& tok, dd::with_stack_resource r) {
  tok.expect(tok.object_begin);

  for (;;) {
    co_yield {};
    if (tok.kind() == tok.object_end)
      break;
    tok.expect(tok.key);
    if (tok.get_key() == "int_field") {
      co_yield {};
      co_yield dd::elements_of(parse(v.int_field, tok, r));
    } else if (tok.get_key() == "double_field") {
      co_yield {};
      co_yield dd::elements_of(parse(v.double_field, tok, r));
    } else if (tok.get_key() == "bool_field") {
      co_yield {};
      co_yield dd::elements_of(parse(v.bool_field, tok, r));
    } else if (tok.get_key() == "string_field") {
      co_yield {};
      co_yield dd::elements_of(parse(v.string_field, tok, r));
    } else if (tok.get_key() == "array_field") {
      co_yield {};
      co_yield dd::elements_of(parse(v.array_field, tok, r));
    } else if (tok.get_key() == "nested_objects") {
      co_yield {};
      co_yield dd::elements_of(parse(v.nested_objects, tok, r));
    } else
      throw parse_error("unknown key");
  }
}

static stream_parser make_parser(auto& obj) {
  return stream_parser([&obj](sax_token& tok, dd::with_stack_resource r) { return parse(obj, tok, r); });
}

static stream_parser_tokens make_tokens_parser(auto& obj) {
  return stream_parser_tokens(
      [&obj](sax_token& tok, dd::with_stack_resource r) { return parse(obj, tok, r); });
}

template <typename T>
[[nodiscard]] static bool parse_twice(std::string_view json, const T& expected, mode_e mode) {
  {
    T object;
    stream_parser p = make_parser(object);
    feed(p, json, mode);
    if (object != expected)
      return false;
    if (!p.is_done())
      return false;
  }
  {
    T object;
    stream_parser_tokens p = make_tokens_parser(object);
    std::vector tokens = into_tokens(json);
    feed(p, tokens, mode);
    if (object != expected)
      return false;
    if (!p.is_done())
      return false;
  }
  return true;
}

#define GENERATE_ALL_MODES GENERATE(whole, by_one, two_parts)

TEST("parse") {
  mode_e mode = GENERATE_ALL_MODES;

  // scalar fields

  SECTION("int positive") {
    REQUIRE(parse_twice(R"({"int_field":123})", TestObject{.int_field = 123}, mode));
  }

  SECTION("int negative") {
    REQUIRE(parse_twice(R"({"int_field":-42})", TestObject{.int_field = -42}, mode));
  }

  SECTION("double fractional") {
    REQUIRE(parse_twice(R"({"double_field":21.12})", TestObject{.double_field = 21.12}, mode));
  }

  SECTION("double from integer") {
    REQUIRE(parse_twice(R"({"double_field":5})", TestObject{.double_field = 5.0}, mode));
  }

  SECTION("bool true") {
    REQUIRE(parse_twice(R"({"bool_field":true})", TestObject{.bool_field = true}, mode));
  }

  SECTION("bool false") {
    REQUIRE(parse_twice(R"({"bool_field":false})", TestObject{.bool_field = false}, mode));
  }

  SECTION("string") {
    REQUIRE(parse_twice(R"({"string_field":"hello"})", TestObject{.string_field = "hello"}, mode));
  }

  // optional null

  SECTION("double null") {
    REQUIRE(parse_twice(R"({"double_field":null})", TestObject{}, mode));
  }

  SECTION("string null") {
    REQUIRE(parse_twice(R"({"string_field":null})", TestObject{}, mode));
  }

  // array field

  SECTION("non-empty") {
    REQUIRE(parse_twice(R"({"array_field":[1,2,3]})",
                        TestObject{.array_field = {std::vector<int64_t>{1, 2, 3}}}, mode));
  }

  SECTION("empty") {
    REQUIRE(parse_twice(R"({"array_field":[]})", TestObject{.array_field = {std::vector<int64_t>{}}}, mode));
  }

  SECTION("null") {
    REQUIRE(parse_twice(R"({"array_field":null})", TestObject{}, mode));
  }

  // nested objects

  SECTION("single nested") {
    TestObject inner{.int_field = 7};
    REQUIRE(parse_twice(R"({"nested_objects":[{"int_field":7}]})", TestObject{.nested_objects = {{{inner}}}},
                        mode));
  }

  SECTION("null element in array") {
    TestObject inner{.int_field = 1};
    REQUIRE(parse_twice(R"({"nested_objects":[null,{"int_field":1}]})",
                        TestObject{.nested_objects = {{{std::nullopt, inner}}}}, mode));
  }

  // all fields

  REQUIRE(parse_twice(R"({"int_field":1,"double_field":2.5,"bool_field":true,)"
                      R"("string_field":"x","array_field":[10,20]})",
                      TestObject{
                          .int_field = 1,
                          .double_field = 2.5,
                          .bool_field = true,
                          .string_field = "x",
                          .array_field = {{10, 20}},
                      },
                      mode));
}

TEST("parse errors") {
  SECTION("unknown key") {
    TestObject got;
    auto p = make_parser(got);
    try {
      p.feed(R"({"unknown_key":1})", /*end=*/true);
      FAIL("should not be reached");
    } catch (parse_error&) {
      // OK
    }
  }

  SECTION("wrong type for int field") {
    TestObject got;
    auto p = make_parser(got);
    try {
      p.feed(R"({"int_field":"oops"})", /*end=*/true);
      FAIL("should not be reached");
    } catch (parse_error&) {
      // OK
    }
  }

  SECTION("wrong type for bool field") {
    TestObject got;
    auto p = make_parser(got);
    try {
      p.feed(R"({"bool_field":1})", /*end=*/true);
      FAIL("should not be reached");
    } catch (parse_error&) {
      // OK
    }
  }

  SECTION("array expected but got object") {
    TestObject got;
    auto p = make_parser(got);
    try {
      p.feed(R"({"array_field":{}})", /*end=*/true);
      FAIL("should not be reached");
    } catch (parse_error&) {
      // OK
    }
  }

  SECTION("object expected but got array") {
    TestObject got;
    auto p = make_parser(got);
    try {
      p.feed(R"({"nested_objects":[[]]})", /*end=*/true);
      FAIL("should not be reached");
    } catch (parse_error&) {
      // OK
    }
  }
}

TEST("Ignore value") {
  SECTION("single scalar") {
    std::vector<omo::sax_token_value> tokens = into_tokens("1");
    REQUIRE(tokens.size() == 1);
  }

  SECTION("empty array") {
    std::vector<omo::sax_token_value> tokens = into_tokens("[]");
    REQUIRE(tokens.size() == 2);  // array_begin + array_end
  }

  SECTION("nested structure is fully collected without crashing") {
    std::vector<omo::sax_token_value> tokens = into_tokens(R"({"a":[1,2,{"b":null}],"c":"x"})");
    REQUIRE(!tokens.empty());
  }
}

[[nodiscard]] static bool same_output(std::string_view json, mode_e mode) {
  // собранное за один раз
  auto toks1 = into_tokens(json);
  // собранное по частям
  auto toks2 = into_tokens_by_parts(json, mode);
  return toks1 == toks2;
}

static boost::json::value tokens2value(std::span<const sax_token_value> tokens) {
  boost::json::value v;
  auto make_consumer = [&](sax_token& tok, dd::with_stack_resource r) {
    return omo::json_value_collector(tok, v, r);
  };
  stream_parser_tokens parser(make_consumer);
  parser.feed(tokens, /*end=*/true);

  return v;
}

[[nodiscard]] static bool test_cut_json(std::string_view json, std::string_view expected_value,
                                        std::string_view expected_tail) {
  const std::string_view init_json = json;
  std::string_view val = omo::cut_json_value(json);
  if (!expected_value.empty()) {
    boost::json::value expected = boost::json::parse(expected_value);
    std::vector toks = into_tokens(init_json);
    bool found = false;
    auto it = omo::cut_json_value(toks, found);
    if (!found)
      return false;
    boost::json::value v = tokens2value(std::span(toks).subspan(0, std::distance(toks.begin(), it)));
    if (expected != v)
      return false;
  } else {
    std::vector toks = into_tokens(init_json);
    bool found = false;
    auto it = omo::cut_json_value(toks, found);
    if (found || it != toks.end())
      return false;
  }
  return val == expected_value && json == expected_tail;
}

TEST("cut json value") {
  // --- базовые примитивы ---
  REQUIRE(test_cut_json("{}{}", "{}", "{}"));
  REQUIRE(test_cut_json("[1,2] [3,4]", "[1,2] ", "[3,4]"));
  REQUIRE(test_cut_json("1 2 3", "1 ", "2 3"));
  REQUIRE(test_cut_json(R"("a" "b")", R"("a" )", R"("b")"));
  REQUIRE(test_cut_json("null null", "null ", "null"));
  REQUIRE(test_cut_json(R"({"a":1} trailing garbage)", R"({"a":1} )", "trailing garbage"));

  // --- булевы значения ---
  REQUIRE(test_cut_json("true false", "true ", "false"));
  REQUIRE(test_cut_json("false,true", "false", ",true"));

  // --- числа: разные форматы ---
  REQUIRE(test_cut_json("0 1", "0 ", "1"));
  REQUIRE(test_cut_json("-1 2", "-1 ", "2"));
  REQUIRE(test_cut_json("-0.5 2", "-0.5 ", "2"));
  REQUIRE(test_cut_json("1.5e10 x", "1.5e10 ", "x"));
  REQUIRE(test_cut_json("1E+10,2", "1E+10", ",2"));
  REQUIRE(test_cut_json("1e-10 tail", "1e-10 ", "tail"));
  REQUIRE(test_cut_json("123456789012345 rest", "123456789012345 ", "rest"));

  // --- строки: экранирование, юникод, пустая строка ---
  REQUIRE(test_cut_json(R"("" rest)", R"("" )", "rest"));
  REQUIRE(test_cut_json(R"("hello\"world" rest)", R"("hello\"world" )", "rest"));
  REQUIRE(test_cut_json(R"("line\nbreak" rest)", R"("line\nbreak" )", "rest"));
  REQUIRE(test_cut_json(R"("back\\slash" rest)", R"("back\\slash" )", "rest"));
  REQUIRE(test_cut_json(R"("\u0041\u00e9" rest)", R"("\u0041\u00e9" )", "rest"));
  REQUIRE(test_cut_json(R"("{not an object}" rest)", R"("{not an object}" )", "rest"));
  REQUIRE(test_cut_json(R"("[not,an,array]" rest)", R"("[not,an,array]" )", "rest"));

  // --- массивы: пустые, вложенные, смешанные типы ---
  REQUIRE(test_cut_json("[] rest", "[] ", "rest"));
  REQUIRE(test_cut_json("[[1,2],[3,4]] rest", "[[1,2],[3,4]] ", "rest"));
  REQUIRE(test_cut_json(R"([1,"a",true,null,{"x":1}] rest)", R"([1,"a",true,null,{"x":1}] )", "rest"));
  REQUIRE(test_cut_json(R"(["]", "}"] rest)", R"(["]", "}"] )", "rest"));

  // --- объекты: пустые, вложенные ---
  REQUIRE(test_cut_json("{} rest", "{} ", "rest"));
  REQUIRE(test_cut_json(R"({"a":{"b":{"c":1}}} rest)", R"({"a":{"b":{"c":1}}} )", "rest"));
  REQUIRE(
      test_cut_json(R"({"key with }": "value with {"} rest)", R"({"key with }": "value with {"} )", "rest"));
  REQUIRE(test_cut_json(R"({"a":[1,2,{"b":3}]} rest)", R"({"a":[1,2,{"b":3}]} )", "rest"));

  // --- пробелы/переносы вокруг значения ---
  REQUIRE(test_cut_json("   1 2", "   1 ", "2"));
  REQUIRE(test_cut_json("\n\t{\"a\":1}\n\ttail", "\n\t{\"a\":1}\n\t", "tail"));
  REQUIRE(test_cut_json("1\r\n2", "1\r\n", "2"));

  // --- нет хвоста / нет разделителя ---
  REQUIRE(test_cut_json("42", "42", ""));
  REQUIRE(test_cut_json(R"({"a":1})", R"({"a":1})", ""));
  REQUIRE(test_cut_json("[1,2,3]", "[1,2,3]", ""));
  REQUIRE(test_cut_json(R"("only string")", R"("only string")", ""));

  // --- хвост — произвольный мусор (не JSON) ---
  REQUIRE(test_cut_json("1 }}}invalid[[[", "1 ", "}}}invalid[[["));
  REQUIRE(test_cut_json(R"("a" !@#$%^&*)", R"("a" )", "!@#$%^&*"));
  REQUIRE(test_cut_json("null ", "null ", ""));

  // --- числа/значения впритык к разделителям без пробела ---
  REQUIRE(test_cut_json("1,2,3", "1", ",2,3"));
  REQUIRE(test_cut_json(R"({"a":1},{"b":2})", R"({"a":1})", R"(,{"b":2})"));
  REQUIRE(test_cut_json("[1][2]", "[1]", "[2]"));

  // --- глубокая вложенность ---
  REQUIRE(test_cut_json("[[[[[1]]]]] rest", "[[[[[1]]]]] ", "rest"));
}

TEST("collect value tokens") {
  mode_e mode = GENERATE_ALL_MODES;

  // -- пустые контейнеры --
  REQUIRE(same_output("{}", mode));
  REQUIRE(same_output("[]", mode));
  REQUIRE(same_output("\"\"", mode));

  // -- примитивы --
  REQUIRE(same_output("null", mode));
  REQUIRE(same_output("true", mode));
  REQUIRE(same_output("false", mode));

  // -- числа --
  REQUIRE(same_output("0", mode));
  REQUIRE(same_output("42", mode));
  REQUIRE(same_output("-17", mode));
  REQUIRE(same_output("3.14", mode));
  REQUIRE(same_output("-0.001", mode));
  REQUIRE(same_output("1e10", mode));
  REQUIRE(same_output("1E-10", mode));
  REQUIRE(same_output("2.5e+3", mode));

  // -- строки --
  REQUIRE(same_output("\"hello\"", mode));
  REQUIRE(same_output("\"hello world\"", mode));
  REQUIRE(same_output("\"line1\\nline2\"", mode));
  REQUIRE(same_output("\"tab\\tchar\"", mode));
  REQUIRE(same_output("\"quote\\\"inside\"", mode));
  REQUIRE(same_output("\"backslash\\\\\"", mode));
  REQUIRE(same_output("\"unicode\\u00e9\"", mode));
  REQUIRE(same_output("\"emoji\\ud83d\\ude00\"", mode));  // surrogate pair
  REQUIRE(same_output("\"{ not really an object }\"", mode));
  REQUIRE(same_output("\"[1,2,3] looks like array but is string\"", mode));

  // -- простые объекты/массивы --
  REQUIRE(same_output("{\"a\":1}", mode));
  REQUIRE(same_output("{\"a\":1,\"b\":2}", mode));
  REQUIRE(same_output("[1,2,3]", mode));
  REQUIRE(same_output("[1,\"two\",3.0,null,true,false]", mode));

  // -- вложенность --
  REQUIRE(same_output("{\"a\":{\"b\":{\"c\":1}}}", mode));
  REQUIRE(same_output("[[1,2],[3,4]]", mode));
  REQUIRE(same_output("[[[]]]", mode));
  REQUIRE(same_output("{\"a\":[1,2,{\"b\":3}]}", mode));
  REQUIRE(same_output("[{\"a\":1},{\"b\":2}]", mode));
  REQUIRE(same_output("{\"arr\":[[1,[2,3]],{\"x\":[4,5]}]}", mode));

  // -- ключи и значения с "опасными" символами --
  REQUIRE(same_output("{\"key with spaces\":1}", mode));
  REQUIRE(same_output("{\"\":\"empty key\"}", mode));
  REQUIRE(same_output("{\"a\":\"\"}", mode));
  REQUIRE(same_output("{\"nested\":{}}", mode));
  REQUIRE(same_output("{\"nested\":[]}", mode));

  // -- пробелы/форматирование --
  REQUIRE(same_output("   {}   ", mode));
  REQUIRE(same_output("{ \"a\" : 1 , \"b\" : 2 }", mode));
  REQUIRE(same_output("[\n  1,\n  2,\n  3\n]", mode));
  REQUIRE(same_output("{\r\n\t\"a\"\r\n:\r\n1\r\n}", mode));

  // -- несколько значений подряд (проверка что отрезается именно первое) --
  REQUIRE(same_output("{}{}", mode));
  REQUIRE(same_output("[1,2] [3,4]", mode));
  REQUIRE(same_output("1 2 3", mode));
  REQUIRE(same_output("\"a\" \"b\"", mode));
  REQUIRE(same_output("null null", mode));
  REQUIRE(same_output("{\"a\":1} trailing garbage", mode));

  // -- большие/глубокие структуры --
  REQUIRE(same_output("[1,2,3,4,5,6,7,8,9,10,11,12,13,14,15]", mode));
  REQUIRE(same_output("{\"a\":{\"b\":{\"c\":{\"d\":{\"e\":{\"f\":1}}}}}}", mode));

  // -- смешанные массивы объектов с разными типами полей --
  REQUIRE(
      same_output("[{\"id\":1,\"tags\":[\"x\",\"y\"],\"active\":true,\"meta\":null},"
                  "{\"id\":2,\"tags\":[],\"active\":false,\"meta\":{\"note\":\"ok\"}}]",
                  mode));

  // -- числа на границах --
  REQUIRE(same_output("9223372036854775807", mode));     // INT64_MAX
  REQUIRE(same_output("-9223372036854775808", mode));    // INT64_MIN
  REQUIRE(same_output("1.7976931348623157e308", mode));  // DBL_MAX
  REQUIRE(same_output("0.0", mode));
  REQUIRE(same_output("-0", mode));
}

TEST("stream_parser_tokens stops after first value") {
  auto check = [](std::string_view first_json, std::string_view second_json) {
    std::vector<sax_token_value> first_tokens = into_tokens(first_json);
    std::vector<sax_token_value> second_tokens = into_tokens(second_json);

    std::vector<sax_token_value> combined = first_tokens;
    combined.insert(combined.end(), second_tokens.begin(), second_tokens.end());

    std::vector<sax_token_value> collected;
    omo::stream_parser_tokens p(just_collect_value_tokens(collected));

    // фидим сразу оба value одним span'ом
    size_t consumed = std::distance(combined.begin(), p.feed(combined, /*end=*/false));

    // должно быть съедено ровно столько токенов, сколько в первом value
    if (consumed != first_tokens.size())
      return false;

    // парсер должен считать себя завершённым после первого value
    if (!p.is_done())
      return false;

    // собраны должны быть только токены первого value, второй нетронут
    if (collected != first_tokens)
      return false;

    return true;
  };

  SECTION("two scalars") {
    REQUIRE(check("1", "2"));
  }

  SECTION("object then string") {
    REQUIRE(check("{}", "\"another\""));
  }

  SECTION("array then number") {
    REQUIRE(check("[1,2]", "3"));
  }

  SECTION("object then object") {
    REQUIRE(check(R"({"a":1})", R"({"b":2})"));
  }

  SECTION("nested then scalar") {
    REQUIRE(check(R"({"a":[1,{"b":2}]})", "null"));
  }
}

TEST("stream_parser_tokens feed return value counts consumed tokens exactly") {
  SECTION("single feed, whole value, no trailing data") {
    std::vector<sax_token_value> tokens = into_tokens(R"({"a":[1,2,3]})");

    std::vector<sax_token_value> collected;
    omo::stream_parser_tokens p(just_collect_value_tokens(collected));

    size_t consumed = std::distance(tokens.begin(), p.feed(tokens, /*end=*/true));
    REQUIRE(consumed == tokens.size());
    REQUIRE(p.is_done());
    REQUIRE(collected == tokens);
  }

  SECTION("feed one token at a time sums up to total") {
    std::vector<sax_token_value> tokens = into_tokens(R"({"a":[1,2,3]})");

    std::vector<sax_token_value> collected;
    omo::stream_parser_tokens p(just_collect_value_tokens(collected));

    size_t total_consumed = 0;
    std::span<const sax_token_value> remaining(tokens);
    while (!remaining.empty() && !p.is_done()) {
      bool last = remaining.size() == 1;
      std::span tofeed = remaining.subspan(0, 1);
      size_t consumed = std::distance(tofeed.begin(), p.feed(tofeed, /*end=*/last));
      REQUIRE(consumed == 1);
      total_consumed += consumed;
      remaining = remaining.subspan(1);
    }

    REQUIRE(total_consumed == tokens.size());
    REQUIRE(p.is_done());
    REQUIRE(collected == tokens);
  }

  SECTION("consumed count excludes tokens belonging to a following value") {
    std::vector<sax_token_value> first = into_tokens("[1,2,3]");
    std::vector<sax_token_value> second = into_tokens(R"({"x":1})");

    std::vector<sax_token_value> combined = first;
    combined.insert(combined.end(), second.begin(), second.end());

    omo::stream_parser_tokens p([](sax_token& tok, dd::with_stack_resource r) {
      return omo::sax_ignore_json_value(tok, [](sax_token&) {}, r);
    });

    size_t consumed = std::distance(combined.begin(), p.feed(combined, /*end=*/false));
    REQUIRE(consumed == first.size());
    REQUIRE(p.is_done());

    // остаток можно доскормить новому парсеру и получить второе значение целиком
    std::vector<sax_token_value> collected_second;
    omo::stream_parser_tokens p2(just_collect_value_tokens(collected_second));
    std::span<const sax_token_value> tail(combined);
    tail = tail.subspan(consumed);
    size_t consumed2 = std::distance(tail.begin(), p2.feed(tail, /*end=*/true));
    REQUIRE(consumed2 == second.size());
    REQUIRE(p2.is_done());
    REQUIRE(collected_second == second);
  }
}

static boost::json::value parse_by_generator(std::string_view json, mode_e mode) {
  unsigned char buf[512];
  boost::json::value v;
  stream_parser parser(
      [&](sax_token& tok, dd::with_stack_resource r) { return omo::json_value_collector(tok, v, r); }, buf);
  feed(parser, json, mode);
  return v;
}

[[nodiscard]] static bool test_collecting_json_value(std::string_view json, mode_e mode) {
  boost::json::value v = boost::json::parse(json);
  boost::json::value v2 = parse_by_generator(json, mode);
  return v == v2;
}

TEST("collecting json value") {
  mode_e mode = GENERATE_ALL_MODES;
  REQUIRE(test_collecting_json_value("0", mode));
  REQUIRE(test_collecting_json_value("{}", mode));
  REQUIRE(test_collecting_json_value("[]", mode));
  REQUIRE(test_collecting_json_value("null", mode));
  REQUIRE(test_collecting_json_value("\"\"", mode));
  REQUIRE(test_collecting_json_value("true", mode));
  REQUIRE(test_collecting_json_value("false", mode));
  REQUIRE(test_collecting_json_value("1", mode));
  REQUIRE(test_collecting_json_value("-1", mode));
  REQUIRE(test_collecting_json_value("1.5", mode));
  REQUIRE(test_collecting_json_value("1e10", mode));
  REQUIRE(test_collecting_json_value("-1.5e-10", mode));
  REQUIRE(test_collecting_json_value("true", mode));
  REQUIRE(test_collecting_json_value("false", mode));
  REQUIRE(test_collecting_json_value(R"("")", mode));
  REQUIRE(test_collecting_json_value(R"("hello")", mode));
  REQUIRE(test_collecting_json_value(R"("\"\\\/\b\f\n\r\t")", mode));
  REQUIRE(test_collecting_json_value(R"("\u0000")", mode));
  REQUIRE(test_collecting_json_value(R"("\u20AC")", mode));
  REQUIRE(test_collecting_json_value(R"("\uD83D\uDE00")", mode));
  REQUIRE(test_collecting_json_value("[1]", mode));
  REQUIRE(test_collecting_json_value("[null,true,false,\"x\",{},[]]", mode));
  REQUIRE(test_collecting_json_value("[[[[]]]]", mode));
  REQUIRE(test_collecting_json_value(R"({"a":1})", mode));
  REQUIRE(test_collecting_json_value(R"({"a":1,"b":null,"c":true,"d":[],"e":{}})", mode));
  REQUIRE(test_collecting_json_value(R"({"":{"":{}}})", mode));
  REQUIRE(test_collecting_json_value(R"({"a":[1,{"b":[true,null,"x"]},3],"c":{"d":[]}})", mode));
}

REGISTER_TEST_LISTENER(moko3::gtest_listener);
MOKO3_MAIN;
