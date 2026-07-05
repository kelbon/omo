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
  omo::stream_parser p([&](sax_token& tok, dd::with_stack_resource) {
    return omo::sax_ignore_json_value(tok, [&tokens](sax_token& t) { tokens.emplace_back(t); });
  });
  p.feed(json, /*end=*/true);
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

static auto make_parser(auto& obj) {
  return stream_parser([&obj](sax_token& tok, dd::with_stack_resource r) { return parse(obj, tok, r); });
}

static auto make_tokens_parser(auto& obj) {
  return stream_parser_tokens(
      [&obj](sax_token& tok, dd::with_stack_resource r) { return parse(obj, tok, r); });
}

static void feed(stream_parser& p, std::string_view json, int mode) {
  switch (mode) {
    case 0:
      p.feed(json, /*end=*/true);
      break;
    case 1:
      while (!json.empty()) {
        p.feed(json.substr(0, 1), /*end=*/json.size() == 1);
        json.remove_prefix(1);
      }
      break;
    case 2: {
      auto mid = json.size() / 2;
      p.feed(json.substr(0, mid), /*end=*/false);
      p.feed(json.substr(mid), /*end=*/true);
      break;
    }
  }
}

static void feed(stream_parser_tokens& p, std::span<const sax_token_value> json, int mode) {
  switch (mode) {
    case 0:
      p.feed(json, /*end=*/true);
      break;
    case 1:
      while (!json.empty()) {
        p.feed(std::span(json).subspan(0, 1), /*end=*/json.size() == 1);
        json = json.subspan(1);
      }
      break;
    case 2: {
      auto mid = json.size() / 2;
      p.feed(json.subspan(0, mid), /*end=*/false);
      p.feed(json.subspan(mid), /*end=*/true);
      break;
    }
  }
}

template <typename T>
[[nodiscard]] static bool parse_twice(std::string_view json, const T& expected, int mode) {
  {
    T object;
    stream_parser p1 = make_parser(object);
    feed(p1, json, mode);
    if (object != expected)
      return false;
  }
  {
    T object;
    stream_parser_tokens p2 = make_tokens_parser(object);
    std::vector tokens = into_tokens(json);
    feed(p2, tokens, mode);
    if (object != expected)
      return false;
  }
  return true;
}

TEST("scalar fields") {
  int mode = GENERATE(0, 1, 2);

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
}

TEST("optional null") {
  int mode = GENERATE(0, 1, 2);

  SECTION("double null") {
    REQUIRE(parse_twice(R"({"double_field":null})", TestObject{}, mode));
  }

  SECTION("string null") {
    REQUIRE(parse_twice(R"({"string_field":null})", TestObject{}, mode));
  }
}

TEST("array field") {
  int mode = GENERATE(0, 1, 2);

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
}

TEST("nested objects") {
  int mode = GENERATE(0, 1, 2);

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
}

TEST("all fields") {
  int mode = GENERATE(0, 1, 2);

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
REGISTER_TEST_LISTENER(moko3::gtest_listener);
MOKO3_MAIN;
