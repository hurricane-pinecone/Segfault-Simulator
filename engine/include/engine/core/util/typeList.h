#pragma once

namespace sfs::detail
{

/** A list of types, for compile-time work over several at once. */
template <typename... TTypes>
struct TypeList
{
};

} // namespace sfs::detail
