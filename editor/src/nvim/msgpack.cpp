#include "nvim/msgpack.h"

#include <bit>
#include <cstring>

namespace msgpack
{

// --- Value ---------------------------------------------------------------------

bool Value::isInteger() const
{
  return std::holds_alternative<int64_t>(m_storage) ||
         std::holds_alternative<uint64_t>(m_storage);
}

int64_t Value::asInt(int64_t fallback) const
{
  if (const auto* v = std::get_if<int64_t>(&m_storage))
    return *v;
  if (const auto* v = std::get_if<uint64_t>(&m_storage))
    return static_cast<int64_t>(*v);
  return fallback;
}

bool Value::asBool(bool fallback) const
{
  if (const auto* v = std::get_if<bool>(&m_storage))
    return *v;
  return fallback;
}

double Value::asDouble(double fallback) const
{
  if (const auto* v = std::get_if<double>(&m_storage))
    return *v;
  if (isInteger())
    return static_cast<double>(asInt());
  return fallback;
}

const std::string& Value::asString() const
{
  static const std::string empty;
  if (const auto* v = std::get_if<std::string>(&m_storage))
    return *v;
  if (const auto* v = std::get_if<Binary>(&m_storage))
    return v->bytes;
  return empty;
}

const Array& Value::asArray() const
{
  static const Array empty;
  if (const auto* v = std::get_if<Array>(&m_storage))
    return *v;
  return empty;
}

const Map& Value::asMap() const
{
  static const Map empty;
  if (const auto* v = std::get_if<Map>(&m_storage))
    return *v;
  return empty;
}

const Value& Value::operator[](std::string_view key) const
{
  static const Value nil;
  for (const auto& [k, v] : asMap())
    if (k.isString() && k.asString() == key)
      return v;
  return nil;
}

// --- decoding ------------------------------------------------------------------

namespace
{

class Reader
{
public:
  explicit Reader(std::string_view input) : m_input(input) {}

  std::size_t position() const { return m_position; }

  DecodeStatus read(Value& out, int depth = 0)
  {
    if (depth > 64)
      return DecodeStatus::Error; // malicious nesting
    uint8_t tag = 0;
    if (!byte(tag))
      return DecodeStatus::NeedMore;

    if (tag <= 0x7f)
      return set(out, static_cast<int64_t>(tag));
    if (tag >= 0xe0)
      return set(out, static_cast<int64_t>(static_cast<int8_t>(tag)));
    if ((tag & 0xf0) == 0x80)
      return map(out, tag & 0x0f, depth);
    if ((tag & 0xf0) == 0x90)
      return array(out, tag & 0x0f, depth);
    if ((tag & 0xe0) == 0xa0)
      return str(out, tag & 0x1f);

    uint64_t length = 0;
    switch (tag)
    {
    case 0xc0:
      return set(out, std::monostate{});
    case 0xc2:
      return set(out, false);
    case 0xc3:
      return set(out, true);
    case 0xc4: case 0xc5: case 0xc6: // bin 8/16/32
      if (!number(length, 1 << (tag - 0xc4)))
        return DecodeStatus::NeedMore;
      return bin(out, length);
    case 0xc7: case 0xc8: case 0xc9: // ext 8/16/32
      if (!number(length, 1 << (tag - 0xc7)))
        return DecodeStatus::NeedMore;
      return ext(out, length);
    case 0xca: // float 32
    {
      uint64_t bits = 0;
      if (!number(bits, 4))
        return DecodeStatus::NeedMore;
      return set(out, static_cast<double>(std::bit_cast<float>(static_cast<uint32_t>(bits))));
    }
    case 0xcb: // float 64
    {
      uint64_t bits = 0;
      if (!number(bits, 8))
        return DecodeStatus::NeedMore;
      return set(out, std::bit_cast<double>(bits));
    }
    case 0xcc: case 0xcd: case 0xce: case 0xcf: // uint 8/16/32/64
    {
      uint64_t value = 0;
      if (!number(value, 1 << (tag - 0xcc)))
        return DecodeStatus::NeedMore;
      return set(out, value);
    }
    case 0xd0: case 0xd1: case 0xd2: case 0xd3: // int 8/16/32/64
    {
      const int bytes = 1 << (tag - 0xd0);
      uint64_t raw = 0;
      if (!number(raw, bytes))
        return DecodeStatus::NeedMore;
      // Sign-extend from `bytes` wide.
      const int shift = 64 - bytes * 8;
      return set(out, static_cast<int64_t>(raw << shift) >> shift);
    }
    case 0xd4: case 0xd5: case 0xd6: case 0xd7: case 0xd8: // fixext 1..16
      return ext(out, 1u << (tag - 0xd4));
    case 0xd9: case 0xda: case 0xdb: // str 8/16/32
      if (!number(length, 1 << (tag - 0xd9)))
        return DecodeStatus::NeedMore;
      return str(out, length);
    case 0xdc: case 0xdd: // array 16/32
      if (!number(length, tag == 0xdc ? 2 : 4))
        return DecodeStatus::NeedMore;
      return array(out, length, depth);
    case 0xde: case 0xdf: // map 16/32
      if (!number(length, tag == 0xde ? 2 : 4))
        return DecodeStatus::NeedMore;
      return map(out, length, depth);
    default:
      return DecodeStatus::Error; // 0xc1 is never used
    }
  }

private:
  template <typename T>
  static DecodeStatus set(Value& out, T value)
  {
    out = Value(std::move(value));
    return DecodeStatus::Ok;
  }

  bool byte(uint8_t& out)
  {
    if (m_position >= m_input.size())
      return false;
    out = static_cast<uint8_t>(m_input[m_position++]);
    return true;
  }

  bool number(uint64_t& out, int bytes)
  {
    if (m_input.size() - m_position < static_cast<std::size_t>(bytes))
      return false;
    out = 0;
    for (int i = 0; i < bytes; ++i)
      out = (out << 8) | static_cast<uint8_t>(m_input[m_position++]);
    return true;
  }

  bool bytes(std::string& out, uint64_t length)
  {
    if (m_input.size() - m_position < length)
      return false;
    out.assign(m_input.substr(m_position, length));
    m_position += length;
    return true;
  }

  DecodeStatus str(Value& out, uint64_t length)
  {
    std::string text;
    if (!bytes(text, length))
      return DecodeStatus::NeedMore;
    return set(out, std::move(text));
  }

  DecodeStatus bin(Value& out, uint64_t length)
  {
    Binary data;
    if (!bytes(data.bytes, length))
      return DecodeStatus::NeedMore;
    return set(out, std::move(data));
  }

  DecodeStatus ext(Value& out, uint64_t length)
  {
    uint8_t type = 0;
    if (!byte(type))
      return DecodeStatus::NeedMore;
    Extension extension{static_cast<int8_t>(type), {}};
    if (!bytes(extension.bytes, length))
      return DecodeStatus::NeedMore;
    return set(out, std::move(extension));
  }

  DecodeStatus array(Value& out, uint64_t count, int depth)
  {
    Array items;
    items.reserve(std::min<uint64_t>(count, 4096));
    for (uint64_t i = 0; i < count; ++i)
    {
      Value item;
      if (const DecodeStatus status = read(item, depth + 1); status != DecodeStatus::Ok)
        return status;
      items.push_back(std::move(item));
    }
    return set(out, std::move(items));
  }

  DecodeStatus map(Value& out, uint64_t count, int depth)
  {
    Map entries;
    entries.reserve(std::min<uint64_t>(count, 4096));
    for (uint64_t i = 0; i < count; ++i)
    {
      Value key, value;
      if (const DecodeStatus status = read(key, depth + 1); status != DecodeStatus::Ok)
        return status;
      if (const DecodeStatus status = read(value, depth + 1); status != DecodeStatus::Ok)
        return status;
      entries.emplace_back(std::move(key), std::move(value));
    }
    return set(out, std::move(entries));
  }

  std::string_view m_input;
  std::size_t m_position = 0;
};

} // namespace

DecodeStatus decode(std::string_view input, std::size_t& consumed, Value& out)
{
  Reader reader(input);
  const DecodeStatus status = reader.read(out);
  if (status == DecodeStatus::Ok)
    consumed = reader.position();
  return status;
}

// --- encoding ------------------------------------------------------------------

void Writer::bigEndian(uint64_t value, int bytes)
{
  for (int i = bytes - 1; i >= 0; --i)
    m_out += static_cast<char>((value >> (i * 8)) & 0xff);
}

Writer& Writer::nil()
{
  m_out += static_cast<char>(0xc0);
  return *this;
}

Writer& Writer::boolean(bool value)
{
  m_out += static_cast<char>(value ? 0xc3 : 0xc2);
  return *this;
}

Writer& Writer::integer(int64_t value)
{
  if (value >= 0 && value <= 0x7f)
    m_out += static_cast<char>(value);
  else if (value < 0 && value >= -32)
    m_out += static_cast<char>(static_cast<int8_t>(value));
  else if (value >= INT32_MIN && value <= INT32_MAX)
  {
    m_out += static_cast<char>(0xd2);
    bigEndian(static_cast<uint32_t>(static_cast<int32_t>(value)), 4);
  }
  else
  {
    m_out += static_cast<char>(0xd3);
    bigEndian(static_cast<uint64_t>(value), 8);
  }
  return *this;
}

Writer& Writer::floating(double value)
{
  m_out += static_cast<char>(0xcb);
  bigEndian(std::bit_cast<uint64_t>(value), 8);
  return *this;
}

Writer& Writer::string(std::string_view value)
{
  const std::size_t length = value.size();
  if (length <= 31)
    m_out += static_cast<char>(0xa0 | length);
  else if (length <= 0xff)
  {
    m_out += static_cast<char>(0xd9);
    bigEndian(length, 1);
  }
  else if (length <= 0xffff)
  {
    m_out += static_cast<char>(0xda);
    bigEndian(length, 2);
  }
  else
  {
    m_out += static_cast<char>(0xdb);
    bigEndian(length, 4);
  }
  m_out += value;
  return *this;
}

Writer& Writer::arrayHeader(std::size_t count)
{
  if (count <= 15)
    m_out += static_cast<char>(0x90 | count);
  else if (count <= 0xffff)
  {
    m_out += static_cast<char>(0xdc);
    bigEndian(count, 2);
  }
  else
  {
    m_out += static_cast<char>(0xdd);
    bigEndian(count, 4);
  }
  return *this;
}

Writer& Writer::mapHeader(std::size_t count)
{
  if (count <= 15)
    m_out += static_cast<char>(0x80 | count);
  else if (count <= 0xffff)
  {
    m_out += static_cast<char>(0xde);
    bigEndian(count, 2);
  }
  else
  {
    m_out += static_cast<char>(0xdf);
    bigEndian(count, 4);
  }
  return *this;
}

Writer& Writer::value(const Value& value)
{
  std::visit(
      [this](const auto& v)
      {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, std::monostate>)
          nil();
        else if constexpr (std::is_same_v<T, bool>)
          boolean(v);
        else if constexpr (std::is_same_v<T, int64_t>)
          integer(v);
        else if constexpr (std::is_same_v<T, uint64_t>)
          integer(static_cast<int64_t>(v));
        else if constexpr (std::is_same_v<T, double>)
          floating(v);
        else if constexpr (std::is_same_v<T, std::string>)
          string(v);
        else if constexpr (std::is_same_v<T, Binary>)
        {
          m_out += static_cast<char>(0xc6);
          bigEndian(v.bytes.size(), 4);
          m_out += v.bytes;
        }
        else if constexpr (std::is_same_v<T, Array>)
        {
          arrayHeader(v.size());
          for (const Value& item : v)
            this->value(item);
        }
        else if constexpr (std::is_same_v<T, Map>)
        {
          mapHeader(v.size());
          for (const auto& [key, item] : v)
          {
            this->value(key);
            this->value(item);
          }
        }
        else if constexpr (std::is_same_v<T, Extension>)
        {
          m_out += static_cast<char>(0xc9);
          bigEndian(v.bytes.size(), 4);
          m_out += static_cast<char>(v.type);
          m_out += v.bytes;
        }
      },
      value.storage());
  return *this;
}

} // namespace msgpack
