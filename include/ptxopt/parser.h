#pragma once
#include <string>

#include "ptxopt/ir.h"

namespace ptx {

// Parses PTX text into a Module. Functions that cannot be handled safely
// (nested scopes, brx.idx, parse errors) are kept verbatim.
Module parseModule(const std::string& src);
Module parseFile(const std::string& path);  // throws std::runtime_error if unreadable

}  // namespace ptx
