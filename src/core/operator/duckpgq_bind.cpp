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
#include "duckdb/parser/statement/extension_statement.hpp"

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

BoundStatement BindPreparedPGQStatement(ClientContext &context, Binder &binder, DuckPGQState &duckpgq_state) {
	auto duckpgq_parse_data = dynamic_cast<DuckPGQParseData *>(duckpgq_state.parse_data.get());
	if (!duckpgq_parse_data) {
		return BoundStatement();
	}
	auto duckpgq_binder = Binder::CreateBinder(context, &binder);
	return duckpgq_binder->Bind(*duckpgq_parse_data->statement);
}

BoundStatement BindNativePGQStatement(ClientContext &context, Binder &binder, SQLStatement &statement,
                                      DuckPGQState &duckpgq_state) {
	if (!duckpgq_statement_contains_graph_table(&statement)) {
		return BoundStatement();
	}

	duckpgq_state.transform_expression.clear();
	duckpgq_state.match_index = 0;
	duckpgq_state.parse_data = make_uniq_base<ParserExtensionParseData, DuckPGQParseData>(statement.Copy());
	auto *parse_data = dynamic_cast<DuckPGQParseData *>(duckpgq_state.parse_data.get());
	duckpgq_transform_match_expressions(parse_data->statement.get(), duckpgq_state);

	auto duckpgq_binder = Binder::CreateBinder(context, &binder);
	return duckpgq_binder->Bind(*parse_data->statement);
}

SQLStatement *GetBindableStatement(SQLStatement &statement) {
	if (statement.type == StatementType::EXTENSION_STATEMENT) {
		auto &extension_statement = statement.Cast<ExtensionStatement>();
		auto *parse_data = dynamic_cast<DuckPGQParseData *>(extension_statement.parse_data.get());
		if (parse_data) {
			return parse_data->statement.get();
		}
	}
	return &statement;
}

} // namespace

static bool ParseDataAppliesToStatement(SQLStatement &statement, DuckPGQState &duckpgq_state) {
	auto *parse_data = dynamic_cast<DuckPGQParseData *>(duckpgq_state.parse_data.get());
	if (!parse_data || !parse_data->statement) {
		return false;
	}
	if (parse_data->statement.get() == &statement) {
		return true;
	}
	// duckpgq_plan moves ExtensionStatement::parse_data onto duckpgq_state before throwing.
	if (statement.type == StatementType::EXTENSION_STATEMENT) {
		return true;
	}
	return false;
}

BoundStatement duckpgq_bind(ClientContext &context, Binder &binder, OperatorExtensionInfo *info,
                            SQLStatement &statement) {
	auto duckpgq_state = GetDuckPGQState(context);

	if (duckpgq_state->parse_data) {
		if (!ParseDataAppliesToStatement(statement, *duckpgq_state)) {
			duckpgq_state->parse_data.reset();
		} else {
			return BindPreparedPGQStatement(context, binder, *duckpgq_state);
		}
	}

	auto *bindable_statement = GetBindableStatement(statement);
	if (bindable_statement != &statement && duckpgq_statement_contains_graph_table(bindable_statement)) {
		duckpgq_state->transform_expression.clear();
		duckpgq_state->match_index = 0;
		duckpgq_state->parse_data =
		    make_uniq_base<ParserExtensionParseData, DuckPGQParseData>(bindable_statement->Copy());
		auto *parse_data = dynamic_cast<DuckPGQParseData *>(duckpgq_state->parse_data.get());
		duckpgq_transform_match_expressions(parse_data->statement.get(), *duckpgq_state);
		return BindPreparedPGQStatement(context, binder, *duckpgq_state);
	}

	auto native_bind = BindNativePGQStatement(context, binder, statement, *duckpgq_state);
	if (native_bind.plan) {
		return native_bind;
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
