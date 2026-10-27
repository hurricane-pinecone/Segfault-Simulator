#include "../../engine/tests/testHarness.h"

#include "nvim/msgpack.h"

#include <cmath>
#include <string>

using namespace msgpack;

namespace
{
Value roundTrip(const Value& value)
{
  Writer writer;
  writer.value(value);
  Value decoded;
  std::size_t consumed = 0;
  if (decode(writer.data(), consumed, decoded) != DecodeStatus::Ok)
    return Value(std::string("decode failed"));
  if (consumed != writer.data().size())
    return Value(std::string("trailing bytes"));
  return decoded;
}

std::string bytes(std::initializer_list<int> values)
{
  std::string out;
  for (const int v : values)
    out += static_cast<char>(v);
  return out;
}
} // namespace

int main()
{
  TEST("integers of every width should round-trip")
  {
    for (const int64_t v : std::initializer_list<int64_t>{0LL, 1LL, 127LL, 128LL, -1LL, -32LL, -33LL, 255LL, 65535LL,
                            -40000LL, 4294967296LL, -4294967296LL, INT64_MIN, INT64_MAX})
      CHECK(roundTrip(Value(v)).asInt() == v);
  }

  TEST("strings, booleans, nil and floats should round-trip")
  {
    CHECK(roundTrip(Value(std::string("hello"))).asString() == std::string("hello"));
    const std::string longText(70000, 'x'); // str32
    CHECK(roundTrip(Value(longText)).asString().size() == 70000u);
    CHECK(roundTrip(Value(true)).asBool());
    CHECK(!roundTrip(Value(false)).asBool(true));
    CHECK(roundTrip(Value()).isNil());
    CHECK(testing::approx(static_cast<float>(roundTrip(Value(3.25)).asDouble()), 3.25f));
  }

  TEST("nested arrays and maps should round-trip")
  {
    Map map;
    map.emplace_back(Value(std::string("rgb")), Value(true));
    map.emplace_back(Value(std::string("ext_linegrid")), Value(true));
    const Value value(Array{Value(2), Value(std::string("nvim_ui_attach")), Value(Array{Value(80), Value(24), Value(map)})});

    const Value decoded = roundTrip(value);
    CHECK(decoded.asArray().size() == 3u);
    CHECK(decoded.asArray()[1].asString() == std::string("nvim_ui_attach"));
    const Value& options = decoded.asArray()[2].asArray()[2];
    CHECK(options["ext_linegrid"].asBool());
    CHECK(options["missing"].isNil());
  }

  TEST("an incomplete message should ask for more bytes, then decode")
  {
    Writer writer;
    writer.arrayHeader(2).string("redraw").integer(1000);
    const std::string full = writer.data();

    for (std::size_t cut = 0; cut < full.size(); ++cut)
    {
      Value partial;
      std::size_t consumed = 0;
      CHECK(decode(std::string_view(full).substr(0, cut), consumed, partial) ==
            DecodeStatus::NeedMore);
    }
    Value complete;
    std::size_t consumed = 0;
    CHECK(decode(full, consumed, complete) == DecodeStatus::Ok);
    CHECK(complete.asArray()[1].asInt() == 1000);
  }

  TEST("two messages back to back should decode one at a time")
  {
    Writer writer;
    writer.integer(7).string("next");
    std::string_view stream = writer.data();

    Value first, second;
    std::size_t consumed = 0;
    CHECK(decode(stream, consumed, first) == DecodeStatus::Ok);
    stream.remove_prefix(consumed);
    CHECK(decode(stream, consumed, second) == DecodeStatus::Ok);
    CHECK(first.asInt() == 7);
    CHECK(second.asString() == std::string("next"));
  }

  TEST("formats Neovim sends should decode (uint, float32, fixext, bin)")
  {
    Value value;
    std::size_t consumed = 0;
    CHECK(decode(bytes({0xcd, 0x01, 0x00}), consumed, value) == DecodeStatus::Ok);
    CHECK(value.asInt() == 256);
    CHECK(decode(bytes({0xca, 0x3f, 0xc0, 0x00, 0x00}), consumed, value) == DecodeStatus::Ok);
    CHECK(testing::approx(static_cast<float>(value.asDouble()), 1.5f));
    CHECK(decode(bytes({0xd4, 0x00, 0x01}), consumed, value) == DecodeStatus::Ok); // buffer handle
    CHECK(std::holds_alternative<Extension>(value.storage()));
    CHECK(decode(bytes({0xc4, 0x02, 'h', 'i'}), consumed, value) == DecodeStatus::Ok);
    CHECK(value.asString() == std::string("hi"));
    CHECK(decode(bytes({0xd0, 0xff}), consumed, value) == DecodeStatus::Ok); // int8 -1
    CHECK(value.asInt() == -1);
  }

  TEST("bytes that aren't MessagePack should be an error")
  {
    Value value;
    std::size_t consumed = 0;
    CHECK(decode(bytes({0xc1}), consumed, value) == DecodeStatus::Error);
  }

  return testing::report("msgpackTests");
}
