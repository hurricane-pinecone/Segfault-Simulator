#pragma once

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// The Language Server Protocol's transport: JSON-RPC 2.0 messages framed with
// Content-Length headers, URIs and position encodings. Shared by sfs-lsp and by
// LSP clients in the engine's tools.
namespace sfs::lsp
{

/** JSON-RPC and LSP error codes. */
namespace errors
{
inline constexpr int kParseError = -32700;
inline constexpr int kInvalidRequest = -32600;
inline constexpr int kMethodNotFound = -32601;
inline constexpr int kInvalidParams = -32602;
inline constexpr int kInternalError = -32603;
inline constexpr int kServerNotInitialized = -32002;
} // namespace errors

/**
 * Cuts Content-Length framed message bodies out of a byte stream that arrives
 * in pieces. A message over the size limit is skipped, and a header without a
 * Content-Length is dropped; both are reported through `problems`.
 */
class MessageReader
{
public:
  /** The largest body kept: larger ones are skipped. */
  static constexpr std::size_t kMaxMessageSize = 64u * 1024u * 1024u;

  /**
   * Adds bytes from the stream.
   *
   * @param bytes the next bytes, in any split
   * @param messages gains the body of each message completed by them
   * @param problems gains a line for each message skipped or header dropped
   */
  void feed(std::string_view bytes,
            std::vector<std::string>& messages,
            std::vector<std::string>& problems);

private:
  std::string m_buffer;
  std::size_t m_skip = 0; // bytes of an oversized body still to drop
};

/**
 * A message body framed for the stream: its Content-Length header, then the
 * body.
 */
std::string frame(std::string_view body);

/** Appends a message body framed for the stream to `out` (a reused buffer). */
void appendFrame(std::string& out, std::string_view body);

/**
 * `message` as compact JSON; invalid UTF-8 in strings is replaced, never thrown
 * about.
 */
std::string dump(const nlohmann::json& message);

/** A response to the request `id`. */
nlohmann::json response(const nlohmann::json& id, nlohmann::json result);

/**
 * An error response to the request `id` (null when the request couldn't be
 * read).
 */
nlohmann::json
errorResponse(const nlohmann::json& id, int code, std::string_view message);

/** A notification: a message with no id, which gets no answer. */
nlohmann::json notification(std::string_view method, nlohmann::json params);

/** A request to the other side, answered by a response with the same `id`. */
nlohmann::json request(const nlohmann::json& id,
                       std::string_view method,
                       nlohmann::json params);

/**
 * The local path a file:// URI names, with %-escapes decoded; on Windows the
 * drive letter form (file:///c:/x) becomes c:/x.
 *
 * @param uri a file URI
 * @return the path; empty for a URI of another scheme
 */
std::filesystem::path uriToPath(std::string_view uri);

/** The file:// URI of `path`, %-escaping what a URI can't hold. */
std::string pathToUri(const std::filesystem::path& path);

/** What an LSP Position's `character` counts in a line. */
enum class PositionEncoding : uint8_t
{
  Utf8,  // bytes
  Utf16, // UTF-16 code units (the LSP default)
  Utf32, // code points
};

/** The protocol name of an encoding: "utf-8", "utf-16" or "utf-32". */
std::string_view encodingName(PositionEncoding encoding);

/**
 * The byte offset in `line` of an LSP character position.
 *
 * @param line one line's text (UTF-8), without its line break
 * @param character the position's `character`, in `encoding`
 * @param encoding what `character` counts
 * @return the byte offset, clamped to the line's length
 */
uint32_t characterToByte(std::string_view line,
                         uint32_t character,
                         PositionEncoding encoding);

/**
 * The LSP character position of a byte offset in `line`.
 *
 * @param line one line's text (UTF-8), without its line break
 * @param byte a byte offset in it, clamped to its length
 * @param encoding what the result counts
 * @return the position's `character`
 */
uint32_t byteToCharacter(std::string_view line,
                         uint32_t byte,
                         PositionEncoding encoding);

} // namespace sfs::lsp
