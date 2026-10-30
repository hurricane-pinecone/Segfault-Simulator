#include "engine/core/util/fatalError.h"

#include "engine/core/logger/logger.h"

namespace sfs
{

void fatalError(const std::string& message)
{
  LOG_ERROR(message);
  throw FatalError(message);
}

} // namespace sfs
