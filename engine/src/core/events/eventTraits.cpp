#include "engine/core/events/eventTraits.h"

namespace sfs::detail
{

const char* eventKindName(EventKind kind)
{
  switch (kind)
  {
  case EventKind::News:
    return "news";
  case EventKind::EntityNews:
    return "entity news";
  case EventKind::Request:
    return "request";
  case EventKind::Response:
    return "response";
  case EventKind::None:
    break;
  }
  return "not an event";
}

} // namespace sfs::detail
