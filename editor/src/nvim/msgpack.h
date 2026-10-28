#pragma once

#include <concepts>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

// A small MessagePack codec: just what Neovim's RPC needs (every type of the
// spec, decoded into a value tree; encoding through a writer). Streaming-safe:
// decode() reports when a message is still incomplete.
namespace msgpack
{

class Value;
using Array = std::vector<Value>;
using Map = std::vector<std::pair<Value, Value>>;

/** Binary data (bin family), kept apart from strings. */
struct Binary
{
  std::string bytes;
};

/** An extension value (Neovim uses these for buffer/window/tabpage handles). */
struct Extension
{
  int8_t type = 0;
  std::string bytes;
};

class Value
{
public:
  using Storage = std::variant<std::monostate, // nil
                               bool,
                               int64_t,
                               uint64_t,
                               double,
                               std::string,
                               Binary,
                               Array,
                               Map,
                               Extension>;

  Value() = default;

  // Integers map to int64_t / uint64_t by signedness (so Value(5) isn't
  // ambiguous between them); everything else to its alternative.
  template <std::integral T>
    requires(!std::same_as<T, bool>)
  Value(T value)
      : m_storage(std::is_signed_v<T> ? Storage(static_cast<int64_t>(value))
                                      : Storage(static_cast<uint64_t>(value)))
  {
  }
  template <typename T>
    requires(!std::integral<T> || std::same_as<T, bool>) &&
            (!std::same_as<std::decay_t<T>, Value>) &&
            std::constructible_from<Storage, T>
  Value(T value) : m_storage(std::move(value))
  {
  }

  bool isNil() const { return std::holds_alternative<std::monostate>(m_storage); }
  bool isInteger() const;
  bool isString() const { return std::holds_alternative<std::string>(m_storage); }
  bool isArray() const { return std::holds_alternative<Array>(m_storage); }
  bool isMap() const { return std::holds_alternative<Map>(m_storage); }

  /** Integer value (from either signedness), or `fallback`. */
  int64_t asInt(int64_t fallback = 0) const;
  bool asBool(bool fallback = false) const;
  double asDouble(double fallback = 0.0) const;
  /** String (or binary) contents, or empty. */
  const std::string& asString() const;
  /** Array elements, or an empty array. */
  const Array& asArray() const;
  /** Map entries, or an empty map. */
  const Map& asMap() const;

  /** The entry with a string key, or nil. */
  const Value& operator[](std::string_view key) const;

  const Storage& storage() const { return m_storage; }

private:
  Storage m_storage;
};

enum class DecodeStatus
{
  Ok,
  NeedMore, // the input ends mid-value: wait for more bytes
  Error,    // not MessagePack
};

/**
 * Decode one value from the front of `input`.
 *
 * @param consumed set to the bytes used when Ok
 */
DecodeStatus decode(std::string_view input, std::size_t& consumed, Value& out);

/** Appends MessagePack to a buffer. */
class Writer
{
public:
  Writer& nil();
  Writer& boolean(bool value);
  Writer& integer(int64_t value);
  Writer& floating(double value);
  Writer& string(std::string_view value);
  Writer& arrayHeader(std::size_t count);
  Writer& mapHeader(std::size_t count);

  /** Any Value, recursively. */
  Writer& value(const Value& value);

  const std::string& data() const { return m_out; }
  std::string take() { return std::move(m_out); }

private:
  void bigEndian(uint64_t value, int bytes);

  std::string m_out;
};

} // namespace msgpack
