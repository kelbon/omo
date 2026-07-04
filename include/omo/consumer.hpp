#pragma once

#include <kelcoro/stack_memory_resource.hpp>

#include <omo/token.hpp>

#include <zal/zal.hpp>

#include <boost/system/error_code.hpp>

namespace omo {

// принимает на вход токены и функцию для создания парсера

struct token_feeder {
 protected:
  using kind_e = sax_token::kind_e;

  dd::stack_resource resource;
  tokens_consumer gen;
  sax_token tok{};
  // must be outside unite generator for string view
  std::string partbuf;
  bool ended = false;
  bool part = false;

  tokens_consumer unite_generate(tokens_consumer old_gen, dd::with_stack_resource r) {
    tok.expect(tok.part);
    part = true;
    partbuf.clear();
    on_scope_failure(destroy_gen) {
      // guarantee order of deallocactions for resources, firstly this generator, then old_gen
      gen = std::move(old_gen);
    };
    // когда подаётся последняя часть вызывается то частью чего это было - string/key
    while (tok.got == tok.part) {
      partbuf += tok.str_m;
      co_yield {};
    }
    partbuf += tok.str_m;
    tok.str_m = partbuf;
    destroy_gen.no_longer_needed();
    (void)gen.release();
    gen = std::move(old_gen);

    part = false;

    co_await dd::this_coro::destroy_and_transfer_control_to(gen.raw_handle().promise().current_worker);
  }

  tokens_consumer starter(auto make_parser, dd::with_stack_resource r) {
    co_yield dd::elements_of(std::move(make_parser)(tok, r));
    ended = true;
    co_yield {};
    throw parse_error("unexpected token `{}` after complete json parsing", tok);
  }

  void on_part(std::string_view s) {
    tok.got = sax_token::part;
    tok.str_m = s;
    if (!part)
      gen = unite_generate(std::move(gen), dd::with_stack_resource(resource));
    consume();
  }

  void consume() {
    // may throw
    ++gen.cur_iterator();
  }

 public:
  explicit token_feeder(std::invocable<sax_token&, dd::with_stack_resource> auto make_parser,
                        std::span<unsigned char> buf)
      : resource(buf), gen(starter(std::move(make_parser), dd::with_stack_resource(resource))) {
  }

  token_feeder(token_feeder&&) = delete;
  void operator=(token_feeder&&) = delete;

  void on_array_begin() {
    tok.got = kind_e::array_begin;
    consume();
  }

  void on_array_end() {
    tok.got = kind_e::array_end;
    consume();
  }

  void on_object_begin() {
    tok.got = kind_e::object_begin;
    consume();
  }

  void on_object_end() {
    tok.got = kind_e::object_end;
    consume();
  }

  void on_string(std::string_view s) {
    tok.got = kind_e::string;
    tok.str_m = s;
    consume();
  }

  void on_key(std::string_view s) {
    tok.got = kind_e::key;
    tok.str_m = s;
    consume();
  }

  void on_int64(int64_t i) {
    tok.got = kind_e::int64;
    tok.int_m = i;
    consume();
  }

  void on_uint64(uint64_t u) {
    tok.got = kind_e::uint64;
    tok.uint_m = u;
    consume();
  }

  void on_double(double d) {
    tok.got = kind_e::double_;
    tok.double_m = d;
    consume();
  }

  void on_bool(bool b) {
    tok.got = kind_e::bool_;
    tok.bool_m = b;
    consume();
  }

  void on_null() {
    tok.got = kind_e::null;
    consume();
  }

  // value parsed already
  [[nodiscard]] bool is_done() const noexcept {
    return ended;
  }
};

// сделано совместимо с boost json: его парсер сам обрабатывает части чисел и при подаче последней части даёт
// вызывает on_<то чего части это были ключа или просто строки>
struct boostjson_token_feeder : token_feeder {
  static constexpr size_t max_array_size = -1;
  static constexpr size_t max_object_size = -1;
  static constexpr size_t max_string_size = -1;
  static constexpr size_t max_key_size = -1;

  using token_feeder::token_feeder;

  bool on_document_begin(boost::system::error_code&) {
    return true;
  }

  bool on_document_end(boost::system::error_code&) {
    return true;
  }

  bool on_array_begin(boost::system::error_code&) {
    token_feeder::on_array_begin();
    return true;
  }

  bool on_array_end(size_t, boost::system::error_code&) {
    token_feeder::on_array_end();
    return true;
  }

  bool on_object_begin(boost::system::error_code&) {
    token_feeder::on_object_begin();
    return true;
  }

  bool on_object_end(size_t, boost::system::error_code&) {
    token_feeder::on_object_end();
    return true;
  }

  bool on_string(std::string_view s, size_t, boost::system::error_code&) {
    token_feeder::on_string(s);
    return true;
  }

  bool on_key(std::string_view s, size_t, boost::system::error_code&) {
    token_feeder::on_key(s);
    return true;
  }

  bool on_int64(int64_t i, std::string_view, boost::system::error_code&) {
    token_feeder::on_int64(i);
    return true;
  }

  bool on_uint64(uint64_t u, std::string_view, boost::system::error_code&) {
    token_feeder::on_uint64(u);
    return true;
  }

  bool on_double(double d, std::string_view, boost::system::error_code&) {
    token_feeder::on_double(d);
    return true;
  }

  bool on_bool(bool b, boost::system::error_code&) {
    token_feeder::on_bool(b);
    return true;
  }

  bool on_null(boost::system::error_code&) {
    token_feeder::on_null();
    return true;
  }

  bool on_string_part(std::string_view s, size_t, boost::system::error_code&) {
    token_feeder::on_part(s);
    return true;
  }

  bool on_comment_part(std::string_view, boost::system::error_code&) {
    return true;
  }

  bool on_key_part(std::string_view s, size_t, boost::system::error_code&) {
    token_feeder::on_part(s);
    return true;
  }

  bool on_number_part(std::string_view, boost::system::error_code&) {
    // boost сам обрабатывает части чисел, парсит их и затем вызывает on_int64 / on_double и тд
    return true;
  }

  bool on_comment(std::string_view, boost::system::error_code&) {
    return true;
  }
};

}  // namespace omo
