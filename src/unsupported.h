#pragma once

#include "types.h"

namespace sdfhook {

/// Reports a value the code has no case for, once per distinct value, and counts it into the run
/// summary. A default branch that quietly picks something reasonable hides the gaps this project
/// exists to find.
void reportUnsupported(const char* what, u64 value);

u64 getUnsupportedCount();
void printUnsupported();

}  // namespace sdfhook
