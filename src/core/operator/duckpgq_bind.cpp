#include "duckpgq/core/operator/duckpgq_bind.hpp"
#include "duckpgq/common.hpp"

#include <duckpgq/core/parser/duckpgq_parser.hpp>
#include "duckpgq/core/operator/duckpgq_operator.hpp"
#include <duckpgq/core/utils/duckpgq_utils.hpp>
#include "duckdb/main/extension_callback_manager.hpp"

#include "duckdb/planner/operator_extension.hpp"
#include "duckdb/parser/statement/create_statement.hpp"
#include "duckdb/parser/statement/drop_statement.hpp"
#include "duckdb/parser/parsed_data/create_property_graph_info.hpp"
#include "duckdb/parser/parsed_data/drop_property_graph_info.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/statement/select_statement.hpp"

namespace duckdb {

namespace {

BoundStatement BindNativePropertyGraphStatement(Binder &binder, SQLStatement &statement, DuckPGQState &duckpgq_state) {
	if (statement.type == StatementType::CREATE_STATEMENT) {
		auto &create = statement.Cast<CreateStatement>();
		if (!dynamic_cast<CreatePropertyGraphInfo *>(create.info.get())) {
			return BoundStatement();
		}
	} else if (statement.type == StatementType::DROP_STATEMENT) {
		auto &drop = statement.Cast<DropStatement>();
		if (!dynamic_cast<DropPropertyGraphInfo *>(drop.info.get())) {
			return BoundStatement();
		}
	} else {
		return BoundStatement();
	}

	duckpgq_state.parse_data = make_uniq_base<ParserExtensionParseData, DuckPGQParseData>(statement.Copy());

	auto table_function_ref = make_uniq<TableFunctionRef>();
	const char *function_name =
	    statement.type == StatementType::CREATE_STATEMENT ? "create_property_graph" : "drop_property_graph";
	table_function_ref->function = make_uniq<FunctionExpression>(function_name, vector<unique_ptr<ParsedExpression>>());

	auto select = make_uniq<SelectStatement>();
	auto select_node = make_uniq<SelectNode>();
	select_node->from_table = std::move(table_function_ref);
	select_node->select_list.push_back(make_uniq<ColumnRefExpression>(vector<string> {"Success"}));
	select->node = std::move(select_node);

	auto duckpgq_binder = Binder::CreateBinder(binder.context, &binder);
	SQLStatement &stmt = *select;
	return duckpgq_binder->Bind(stmt);
}

} // namespace

BoundStatement duckpgq_bind(ClientContext &context, Binder &binder, OperatorExtensionInfo *info,
                            SQLStatement &statement) {
	auto duckpgq_state = GetDuckPGQState(context);

	auto duckpgq_parse_data = dynamic_cast<DuckPGQParseData *>(duckpgq_state->parse_data.get());
	if (duckpgq_parse_data) {
		auto duckpgq_binder = Binder::CreateBinder(context, &binder);
		return duckpgq_binder->Bind(*(duckpgq_parse_data->statement));
	}

	return BindNativePropertyGraphStatement(binder, statement, *duckpgq_state);
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
