#include "sfs/lsp/protocol.h"

#include <algorithm>
#include <cctype>
#include <charconv>

namespace sfs::lsp
{

namespace
{

constexpr std::string_view kHeaderEnd = "\r\n\r\n";
constexpr std::string_view kFileScheme = "file://";

// The bytes a UTF-8 sequence starting with `lead` takes; 1 for a stray byte.
uint32_t sequenceLength(unsigned char lead)
{
  if (lead >= 0xF0 && lead < 0xF8)
    return 4;
  if (lead >= 0xE0)
    return lead < 0xF0 ? 3 : 1;
  if (lead >= 0xC0)
    return 2;
  return 1;
}

// How many units of `encoding` the UTF-8 sequence of `length` bytes counts as.
uint32_t unitsOf(uint32_t length, PositionEncoding encoding)
{
  switch (encoding)
  {
  case PositionEncoding::Utf8:
    return length;
  case PositionEncoding::Utf16:
    return length == 4 ? 2 : 1;
  case PositionEncoding::Utf32:
    return 1;
  }
  return length;
}

// The value of a Content-Length header in `header`, or npos without one.
std::size_t contentLength(std::string_view header)
{
  constexpr std::string_view name = "content-length:";
  std::size_t at = 0;
  while (at < header.size())
  {
    std::size_t end = header.find("\r\n", at);
    if (end == std::string_view::npos)
      end = header.size();
    const std::string_view line = header.substr(at, end - at);
    if (line.size() > name.size() &&
        std::equal(name.begin(),
                   name.end(),
                   line.begin(),
                   [](char a, char b) {
                     return a == std::tolower(static_cast<unsigned char>(b));
                   }))
    {
      std::string_view value = line.substr(name.size());
      while (!value.empty() && value.front() == ' ')
        value.remove_prefix(1);
      std::size_t length = 0;
      const auto [ptr, error] =
          std::from_chars(value.data(), value.data() + value.size(), length);
      if (error == std::errc())
        return length;
    }
    at = end + 2;
  }
  return std::string_view::npos;
}

int hexValue(char c)
{
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}

} // namespace

void MessageReader::feed(std::string_view bytes,
                         std::vector<std::string>& messages,
                         std::vector<std::string>& problems)
{
  if (m_skip > 0)
  {
    const std::size_t dropped = std::min(m_skip, bytes.size());
    m_skip -= dropped;
    bytes.remove_prefix(dropped);
  }
  m_buffer.append(bytes);

  std::size_t at = 0;
  while (true)
  {
    const std::size_t headerEnd = m_buffer.find(kHeaderEnd, at);
    if (headerEnd == std::string::npos)
      break;
    const std::size_t length =
        contentLength(std::string_view(m_buffer).substr(at, headerEnd - at));
    const std::size_t body = headerEnd + kHeaderEnd.size();
    if (length == std::string::npos)
    {
      problems.push_back(
          "a message header without a Content-Length was dropped");
      at = body;
      continue;
    }
    if (length > kMaxMessageSize)
    {
      problems.push_back("a message of " + std::to_string(length) +
                         " bytes is over the limit and was skipped");
      const std::size_t available = m_buffer.size() - body;
      if (available >= length)
      {
        at = body + length;
        continue;
      }
      m_skip = length - available;
      at = m_buffer.size();
      break;
    }
    if (m_buffer.size() - body < length)
      break;
    messages.emplace_back(m_buffer, body, length);
    at = body + length;
  }
  m_buffer.erase(0, at);
}

void appendFrame(std::string& out, std::string_view body)
{
  out += "Content-Length: ";
  out += std::to_string(body.size());
  out += "\r\n\r\n";
  out += body;
}

std::string frame(std::string_view body)
{
  std::string out;
  appendFrame(out, body);
  return out;
}

std::string dump(const nlohmann::json& message)
{
  return message.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

nlohmann::json response(const nlohmann::json& id, nlohmann::json result)
{
  return {{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}};
}

nlohmann::json
errorResponse(const nlohmann::json& id, int code, std::string_view message)
{
  return {{"jsonrpc", "2.0"},
          {"id", id},
          {"error", {{"code", code}, {"message", message}}}};
}

nlohmann::json notification(std::string_view method, nlohmann::json params)
{
  return {
      {"jsonrpc", "2.0"}, {"method", method}, {"params", std::move(params)}};
}

nlohmann::json request(const nlohmann::json& id,
                       std::string_view method,
                       nlohmann::json params)
{
  return {{"jsonrpc", "2.0"},
          {"id", id},
          {"method", method},
          {"params", std::move(params)}};
}

std::filesystem::path uriToPath(std::string_view uri)
{
  if (!uri.starts_with(kFileScheme))
    return {};
  uri.remove_prefix(kFileScheme.size());
  std::string path;
  // file://host/share/x is a network path; file:///x a local one.
  if (!uri.empty() && uri.front() != '/')
    path = "//";
  path.reserve(path.size() + uri.size());
  for (std::size_t i = 0; i < uri.size(); ++i)
  {
    if (uri[i] == '%' && i + 2 < uri.size() && hexValue(uri[i + 1]) >= 0 &&
        hexValue(uri[i + 2]) >= 0)
    {
      path +=
          static_cast<char>(hexValue(uri[i + 1]) * 16 + hexValue(uri[i + 2]));
      i += 2;
    }
    else
      path += uri[i];
  }
  // /c:/x is the drive path c:/x.
  if (path.size() >= 3 && path[0] == '/' &&
      std::isalpha(static_cast<unsigned char>(path[1])) && path[2] == ':')
    path.erase(0, 1);
  return std::filesystem::path(path);
}

std::string pathToUri(const std::filesystem::path& path)
{
  const std::string text = path.generic_string();
  std::string out(kFileScheme);
  if (text.starts_with("//"))
    out.pop_back(); // a network path brings its own host slashes
  else if (!text.starts_with('/'))
    out += '/';
  constexpr char kHex[] = "0123456789ABCDEF";
  for (const char c : text)
  {
    const auto byte = static_cast<unsigned char>(c);
    if (std::isalnum(byte) || c == '/' || c == '-' || c == '_' || c == '.' ||
        c == '~' || c == ':')
      out += c;
    else
    {
      out += '%';
      out += kHex[byte >> 4];
      out += kHex[byte & 0xF];
    }
  }
  return out;
}

std::string_view encodingName(PositionEncoding encoding)
{
  switch (encoding)
  {
  case PositionEncoding::Utf8:
    return "utf-8";
  case PositionEncoding::Utf16:
    return "utf-16";
  case PositionEncoding::Utf32:
    return "utf-32";
  }
  return "utf-16";
}

uint32_t characterToByte(std::string_view line,
                         uint32_t character,
                         PositionEncoding encoding)
{
  if (encoding == PositionEncoding::Utf8)
    return std::min<uint32_t>(character, static_cast<uint32_t>(line.size()));
  uint32_t byte = 0;
  uint32_t units = 0;
  while (byte < line.size() && units < character)
  {
    const auto lead = static_cast<unsigned char>(line[byte]);
    if (lead < 0x80)
    {
      ++byte;
      ++units;
      continue;
    }
    const uint32_t length = std::min<uint32_t>(
        sequenceLength(lead), static_cast<uint32_t>(line.size()) - byte);
    byte += length;
    units += unitsOf(length, encoding);
  }
  return byte;
}

uint32_t
byteToCharacter(std::string_view line, uint32_t byte, PositionEncoding encoding)
{
  byte = std::min<uint32_t>(byte, static_cast<uint32_t>(line.size()));
  if (encoding == PositionEncoding::Utf8)
    return byte;
  uint32_t at = 0;
  uint32_t units = 0;
  while (at < byte)
  {
    const auto lead = static_cast<unsigned char>(line[at]);
    if (lead < 0x80)
    {
      ++at;
      ++units;
      continue;
    }
    const uint32_t length = std::min<uint32_t>(sequenceLength(lead), byte - at);
    at += length;
    units += unitsOf(length, encoding);
  }
  return units;
}

} // namespace sfs::lsp
