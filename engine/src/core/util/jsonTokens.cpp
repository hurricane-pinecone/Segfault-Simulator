#include "engine/core/util/jsonTokens.h"

#include <algorithm>
#include <cstdint>

namespace sfs
{

namespace
{
// Deeper nesting than this is refused rather than recursed into.
constexpr int kMaxDepth = 512;

class Scanner
{
public:
  explicit Scanner(std::string_view text) : m_text(text) {}

  bool document()
  {
    skipSpace();
    std::string pointer;
    if (!value(pointer, 0))
      return false;
    skipSpace();
    return m_at == m_text.size();
  }

  std::vector<JsonToken> tokens;

private:
  void skipSpace()
  {
    while (m_at < m_text.size() &&
           (m_text[m_at] == ' ' || m_text[m_at] == '\t' ||
            m_text[m_at] == '\n' || m_text[m_at] == '\r'))
      ++m_at;
  }

  bool consume(char c)
  {
    skipSpace();
    if (m_at >= m_text.size() || m_text[m_at] != c)
      return false;
    ++m_at;
    return true;
  }

  bool value(std::string& pointer, int depth)
  {
    if (depth > kMaxDepth || m_at >= m_text.size())
      return false;
    const char c = m_text[m_at];
    if (c == '{')
      return object(pointer, depth);
    if (c == '[')
      return array(pointer, depth);
    JsonToken token;
    token.pointer = pointer;
    token.begin = m_at;
    if (c == '"')
    {
      token.kind = JsonToken::Kind::String;
      if (!string(token.text))
        return false;
    }
    else if (c == '-' || (c >= '0' && c <= '9'))
    {
      token.kind = JsonToken::Kind::Number;
      if (!number())
        return false;
      token.text = m_text.substr(token.begin, m_at - token.begin);
    }
    else
    {
      token.kind = JsonToken::Kind::Literal;
      bool matched = false;
      for (const std::string_view word : {"true", "false", "null"})
        if (m_text.substr(m_at, word.size()) == word)
        {
          m_at += word.size();
          token.text = word;
          matched = true;
          break;
        }
      if (!matched)
        return false;
    }
    token.end = m_at;
    tokens.push_back(std::move(token));
    return true;
  }

  bool object(std::string& pointer, int depth)
  {
    ++m_at;
    if (consume('}'))
      return true;
    const std::size_t base = pointer.size();
    do
    {
      skipSpace();
      if (m_at >= m_text.size() || m_text[m_at] != '"')
        return false;
      JsonToken key;
      key.kind = JsonToken::Kind::Key;
      key.begin = m_at;
      if (!string(key.text))
        return false;
      key.end = m_at;
      pointer.resize(base);
      pointer += '/';
      pointer += jsonPointerSegment(key.text);
      key.pointer = pointer;
      tokens.push_back(std::move(key));
      if (!consume(':'))
        return false;
      skipSpace();
      if (!value(pointer, depth + 1))
        return false;
    } while (consume(','));
    pointer.resize(base);
    return consume('}');
  }

  bool array(std::string& pointer, int depth)
  {
    ++m_at;
    if (consume(']'))
      return true;
    const std::size_t base = pointer.size();
    std::size_t index = 0;
    do
    {
      pointer.resize(base);
      pointer += '/';
      pointer += std::to_string(index++);
      skipSpace();
      if (!value(pointer, depth + 1))
        return false;
    } while (consume(','));
    pointer.resize(base);
    return consume(']');
  }

  bool number()
  {
    const std::size_t start = m_at;
    const auto digits = [&]
    {
      const std::size_t from = m_at;
      while (m_at < m_text.size() && m_text[m_at] >= '0' && m_text[m_at] <= '9')
        ++m_at;
      return m_at > from;
    };
    if (m_text[m_at] == '-')
      ++m_at;
    if (m_at < m_text.size() && m_text[m_at] == '0')
      ++m_at;
    else if (!digits())
      return false;
    if (m_at < m_text.size() && m_text[m_at] == '.')
    {
      ++m_at;
      if (!digits())
        return false;
    }
    if (m_at < m_text.size() && (m_text[m_at] == 'e' || m_text[m_at] == 'E'))
    {
      ++m_at;
      if (m_at < m_text.size() && (m_text[m_at] == '+' || m_text[m_at] == '-'))
        ++m_at;
      if (!digits())
        return false;
    }
    return m_at > start;
  }

  // Four hex digits of a \u escape.
  bool hex4(uint32_t& out)
  {
    if (m_at + 4 > m_text.size())
      return false;
    out = 0;
    for (int i = 0; i < 4; ++i)
    {
      const char c = m_text[m_at++];
      out <<= 4;
      if (c >= '0' && c <= '9')
        out |= static_cast<uint32_t>(c - '0');
      else if (c >= 'a' && c <= 'f')
        out |= static_cast<uint32_t>(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F')
        out |= static_cast<uint32_t>(c - 'A' + 10);
      else
        return false;
    }
    return true;
  }

  static void appendUtf8(std::string& out, uint32_t code)
  {
    if (code < 0x80)
      out += static_cast<char>(code);
    else if (code < 0x800)
    {
      out += static_cast<char>(0xC0 | (code >> 6));
      out += static_cast<char>(0x80 | (code & 0x3F));
    }
    else if (code < 0x10000)
    {
      out += static_cast<char>(0xE0 | (code >> 12));
      out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (code & 0x3F));
    }
    else
    {
      out += static_cast<char>(0xF0 | (code >> 18));
      out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
      out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (code & 0x3F));
    }
  }

  // A quoted string at m_at, decoded into `out`.
  bool string(std::string& out)
  {
    ++m_at;
    while (m_at < m_text.size())
    {
      const char c = m_text[m_at++];
      if (c == '"')
        return true;
      if (static_cast<unsigned char>(c) < 0x20)
        return false;
      if (c != '\\')
      {
        out += c;
        continue;
      }
      if (m_at >= m_text.size())
        return false;
      switch (m_text[m_at++])
      {
      case '"':
        out += '"';
        break;
      case '\\':
        out += '\\';
        break;
      case '/':
        out += '/';
        break;
      case 'b':
        out += '\b';
        break;
      case 'f':
        out += '\f';
        break;
      case 'n':
        out += '\n';
        break;
      case 'r':
        out += '\r';
        break;
      case 't':
        out += '\t';
        break;
      case 'u':
      {
        uint32_t code = 0;
        if (!hex4(code))
          return false;
        // A surrogate pair spells one code point past the first plane.
        if (code >= 0xD800 && code < 0xDC00 && m_at + 1 < m_text.size() &&
            m_text[m_at] == '\\' && m_text[m_at + 1] == 'u')
        {
          m_at += 2;
          uint32_t low = 0;
          if (!hex4(low) || low < 0xDC00 || low >= 0xE000)
            return false;
          code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
        }
        appendUtf8(out, code);
        break;
      }
      default:
        return false;
      }
    }
    return false;
  }

  std::string_view m_text;
  std::size_t m_at = 0;
};
} // namespace

std::optional<std::vector<JsonToken>> scanJsonTokens(std::string_view text)
{
  Scanner scanner(text);
  if (!scanner.document())
    return std::nullopt;
  return std::move(scanner.tokens);
}

std::string jsonPointerSegment(std::string_view segment)
{
  std::string out;
  out.reserve(segment.size());
  for (const char c : segment)
  {
    if (c == '~')
      out += "~0";
    else if (c == '/')
      out += "~1";
    else
      out += c;
  }
  return out;
}

std::optional<std::string> applyTextEdits(std::string_view text,
                                          std::vector<TextEdit> edits)
{
  std::sort(edits.begin(),
            edits.end(),
            [](const TextEdit& a, const TextEdit& b)
            { return a.begin < b.begin; });
  std::string out;
  out.reserve(text.size());
  std::size_t at = 0;
  for (const TextEdit& edit : edits)
  {
    if (edit.begin < at || edit.end < edit.begin || edit.end > text.size())
      return std::nullopt;
    out.append(text.substr(at, edit.begin - at));
    out += edit.text;
    at = edit.end;
  }
  out.append(text.substr(at));
  return out;
}

} // namespace sfs
