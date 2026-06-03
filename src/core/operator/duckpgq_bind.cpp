#include "duckpgq/core/operator/duckpgq_bind.hpp"
#include "duckpgq/common.hpp"

#include <duckpgq/core/parser/duckpgq_parser.hpp>
#include "duckpgq/core/operator/duckpgq_operator.hpp"
#include <duckpgq/core/utils/duckpgq_utils.hpp>
#include <duckpgq_state.hpp>
#include "duckdb/main/extension_callback_manager.hpp"

#include "duckdb/planner/operator_extension.hpp"

namespace duckdb {

BoundStatement duckpgq_bind(ClientContext &context, Binder &binder, OperatorExtensionInfo *info,
                            SQLStatement &statement) {
	auto duckpgq_state = GetDuckPGQState(context, true);

	auto duckpgq_binder = Binder::CreateBinder(context, &binder);
	auto duckpgq_parse_data = dynamic_cast<DuckPGQParseData *>(duckpgq_state->parse_data.get());
	if (!duckpgq_parse_data) {
		throw BinderException("No DuckPGQ parse data found for duckpgq_bind");
	}
	return duckpgq_binder->Bind(*(duckpgq_parse_data->statement));
}

//------------------------------------------------------------------------------
// Register functions
//------------------------------------------------------------------------------
void CorePGQOperator::RegisterPGQBindOperator(ExtensionLoader &loader) {
	auto &db = loader.GetDatabaseInstance();
	auto &manager = ExtensionCallbackManager::Get(db);
	manager.Register(make_shared_ptr<DuckPGQOperatorExtension>());
}

} // namespace duckdb
