#pragma once

#include "duckpgq/common.hpp"
#include "duckdb/planner/extension_callback.hpp"
#include <duckpgq_state.hpp>

namespace duckdb {
// ExtensionCallback for eager DuckPGQState registration is intentionally disabled:
// InitializeInternalTable opens a nested Connection, which re-enters OnConnectionOpened.
} // namespace duckdb
