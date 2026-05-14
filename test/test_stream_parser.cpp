#include "omo/stream_parser.hpp"
#include <moko3/moko3.hpp>

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

using omo::parse_error;
using omo::sax_token;
using omo::stream_parser;
using omo::tokens_consumer;

static tokens_consumer parse(int64_t& v, sax_token& tok, dd::with_stack_resource) {
  using enum sax_token::kind_e;
  tok.expect(tok.int64 | tok.uint64);
  switch (tok.got) {
    case int64:
      v = tok.int_m;
      return {};
    case uint64:
      v = tok.uint_m;
      return {};
    default:
      omo::unreachable();
  }
}

static tokens_consumer parse(double& v, sax_token& tok, dd::with_stack_resource) {
  using enum sax_token::kind_e;
  tok.expect(tok.int64 | tok.uint64 | tok.double_);
  switch (tok.got) {
    case int64:
      v = tok.int_m;
      return {};
    case uint64:
      v = tok.uint_m;
      return {};
    case double_:
      v = tok.double_m;
      return {};
    default:
      omo::unreachable();
  }
}

static tokens_consumer parse(bool& v, sax_token& tok, dd::with_stack_resource) {
  tok.expect(tok.bool_);
  v = tok.bool_m;
  return {};
}

static tokens_consumer parse(std::string& v, sax_token& tok, dd::with_stack_resource) {
  tok.expect(tok.string);
  v = tok.str_m;
  return {};
}

template <typename T>
static tokens_consumer parse(std::vector<T>& v, sax_token& tok, dd::with_stack_resource r) {
  tok.expect(sax_token::array_begin);
  for (;;) {
    co_yield {};
    if (tok.got == sax_token::array_end)
      break;
    co_yield dd::elements_of(parse(v.emplace_back(), tok, r));
  }
}

template <typename T>
static tokens_consumer parse(std::optional<T>& v, sax_token& tok, dd::with_stack_resource r) {
  if (tok.got == sax_token::null) [[unlikely]]
    return {};
  return parse(v.emplace(T{}), tok, r);
}

static tokens_consumer parse(TestObject& v, sax_token& tok, dd::with_stack_resource r) {
  tok.expect(tok.object_begin);

  for (;;) {
    co_yield {};
    if (tok.got == tok.object_end)
      break;
    tok.expect(tok.key);
    if (tok.str_m == "int_field") {
      co_yield {};
      co_yield dd::elements_of(parse(v.int_field, tok, r));
    } else if (tok.str_m == "double_field") {
      co_yield {};
      co_yield dd::elements_of(parse(v.double_field, tok, r));
    } else if (tok.str_m == "bool_field") {
      co_yield {};
      co_yield dd::elements_of(parse(v.bool_field, tok, r));
    } else if (tok.str_m == "string_field") {
      co_yield {};
      co_yield dd::elements_of(parse(v.string_field, tok, r));
    } else if (tok.str_m == "array_field") {
      co_yield {};
      co_yield dd::elements_of(parse(v.array_field, tok, r));
    } else if (tok.str_m == "nested_objects") {
      co_yield {};
      co_yield dd::elements_of(parse(v.nested_objects, tok, r));
    } else
      throw parse_error("unknown key");
  }
}

static auto parser_maker(auto& obj) {
  return [&obj](sax_token& tok, dd::with_stack_resource r) { return parse(obj, tok, r); };
}

static auto make_parser(auto& obj) {
  return stream_parser([&obj](sax_token& tok, dd::with_stack_resource r) { return parse(obj, tok, r); });
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

TEST("scalar fields") {
  int mode = GENERATE(0, 1, 2);

  SECTION("int positive") {
    TestObject got;
    auto p = make_parser(got);
    feed(p, R"({"int_field":123})", mode);
    REQUIRE(got == TestObject{.int_field = 123});
  }

  SECTION("int negative") {
    TestObject got;
    auto p = make_parser(got);
    feed(p, R"({"int_field":-42})", mode);
    REQUIRE(got == TestObject{.int_field = -42});
  }

  SECTION("double fractional") {
    TestObject got;
    auto p = make_parser(got);
    feed(p, R"({"double_field":21.12})", mode);
    auto ex = TestObject{.double_field = 21.12};
    REQUIRE(got == ex);
  }

  SECTION("double from integer") {
    TestObject got;
    auto p = make_parser(got);
    feed(p, R"({"double_field":5})", mode);
    REQUIRE(got == TestObject{.double_field = 5.0});
  }

  SECTION("bool true") {
    TestObject got;
    auto p = make_parser(got);
    feed(p, R"({"bool_field":true})", mode);
    REQUIRE(got == TestObject{.bool_field = true});
  }

  SECTION("bool false") {
    TestObject got;
    auto p = make_parser(got);
    feed(p, R"({"bool_field":false})", mode);
    REQUIRE(got == TestObject{.bool_field = false});
  }

  SECTION("string") {
    TestObject got;
    auto p = make_parser(got);
    feed(p, R"({"string_field":"hello"})", mode);
    REQUIRE(got == TestObject{.string_field = "hello"});
  }
}

TEST("optional null") {
  int mode = GENERATE(0, 1, 2);

  SECTION("double null") {
    TestObject got;
    auto p = make_parser(got);
    feed(p, R"({"double_field":null})", mode);
    REQUIRE(!got.double_field.has_value());
  }

  SECTION("string null") {
    TestObject got;
    auto p = make_parser(got);
    feed(p, R"({"string_field":null})", mode);
    REQUIRE(!got.string_field.has_value());
  }
}

TEST("array field") {
  int mode = GENERATE(0, 1, 2);

  SECTION("non-empty") {
    TestObject got;
    auto p = make_parser(got);
    feed(p, R"({"array_field":[1,2,3]})", mode);
    REQUIRE(got == TestObject{.array_field = {std::vector<int64_t>{1, 2, 3}}});
  }

  SECTION("empty") {
    TestObject got;
    auto p = make_parser(got);
    feed(p, R"({"array_field":[]})", mode);
    REQUIRE(got == TestObject{.array_field = {std::vector<int64_t>{}}});
  }

  SECTION("null") {
    TestObject got;
    auto p = make_parser(got);
    feed(p, R"({"array_field":null})", mode);
    REQUIRE(!got.array_field.has_value());
  }
}

TEST("nested objects") {
  int mode = GENERATE(0, 1, 2);

  SECTION("single nested") {
    TestObject got;
    auto p = make_parser(got);
    feed(p, R"({"nested_objects":[{"int_field":7}]})", mode);
    TestObject inner{.int_field = 7};
    REQUIRE(got == TestObject{.nested_objects = {{{inner}}}});
  }

  SECTION("null element in array") {
    TestObject got;
    auto p = make_parser(got);
    feed(p, R"({"nested_objects":[null,{"int_field":1}]})", mode);
    TestObject inner{.int_field = 1};
    REQUIRE(got == TestObject{.nested_objects = {{{std::nullopt, inner}}}});
  }
}

TEST("all fields") {
  int mode = GENERATE(0, 1, 2);

  TestObject got;
  auto p = make_parser(got);
  feed(p,
       R"({"int_field":1,"double_field":2.5,"bool_field":true,)"
       R"("string_field":"x","array_field":[10,20]})",
       mode);

  TestObject expected{
      .int_field = 1,
      .double_field = 2.5,
      .bool_field = true,
      .string_field = "x",
      .array_field = {{10, 20}},
  };
  REQUIRE(got == expected);
}

TEST("parse errors") {
  TestObject got;
  auto p = make_parser(got);
  try {
    p.feed(R"({"unknown_key":1})", /*end=*/true);
    FAIL("should not be reached");
  } catch (parse_error&) {
    // OK
  }
}

REGISTER_TEST_LISTENER(moko3::gtest_listener);
MOKO3_MAIN;
