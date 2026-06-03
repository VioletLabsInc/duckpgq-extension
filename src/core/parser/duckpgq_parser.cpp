
#include "duckpgq/core/parser/duckpgq_parser.hpp"

#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/tableref/showref.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"
#include <duckdb/parser/parsed_data/create_table_info.hpp>
#include <duckdb/parser/parser.hpp>
#include <duckdb/parser/statement/copy_statement.hpp>
#include <duckdb/parser/statement/create_statement.hpp>
#include <duckdb/parser/statement/insert_statement.hpp>
#include <duckpgq/core/functions/table/create_property_graph.hpp>
#include <duckpgq_state.hpp>

#include "duckdb/parser/query_node/cte_node.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include <duckpgq/core/functions/table/describe_property_graph.hpp>
#include <duckpgq/core/functions/table/drop_property_graph.hpp>

#include <duckdb/parser/tableref/matchref.hpp>
#include <duckpgq/core/functions/table/summarize_property_graph.hpp>

#include "duckdb/main/extension_callback_manager.hpp"
#include "duckpgq/core/utils/duckpgq_utils.hpp"
#include "duckdb/common/enums/allow_parser_override.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/parser/parsed_data/create_property_graph_info.hpp"
#include "duckdb/parser/parsed_data/drop_property_graph_info.hpp"
#include "duckdb/parser/query_node/set_operation_node.hpp"
#include "duckdb/parser/statement/explain_statement.hpp"
#include "duckdb/parser/statement/extension_statement.hpp"
#include "duckdb/parser/statement/select_statement.hpp"

namespace duckdb {

namespace {

static bool StatementContainsPGQ(SQLStatement *statement);

static bool TableRefContainsPGQ(TableRef *table_ref) {
	if (!table_ref) {
		return false;
	}
	if (auto table_function_ref = dynamic_cast<TableFunctionRef *>(table_ref)) {
		if (table_function_ref->match_expression) {
			return true;
		}
		auto function = dynamic_cast<FunctionExpression *>(table_function_ref->function.get());
		return function && function->function_name == "duckpgq_match";
	}
	if (auto join_ref = dynamic_cast<JoinRef *>(table_ref)) {
		return TableRefContainsPGQ(join_ref->left.get()) || TableRefContainsPGQ(join_ref->right.get());
	}
	if (auto subquery_ref = dynamic_cast<SubqueryRef *>(table_ref)) {
		return StatementContainsPGQ(subquery_ref->subquery.get());
	}
	return false;
}

static bool SelectNodeContainsPGQ(SelectNode *node) {
	if (!node) {
		return false;
	}
	if (TableRefContainsPGQ(node->from_table.get())) {
		return true;
	}
	for (auto const &kv_pair : node->cte_map.map) {
		auto const &cte = kv_pair.second;
		if (StatementContainsPGQ(cte->query.get())) {
			return true;
		}
	}
	return false;
}

static bool QueryNodeContainsPGQ(QueryNode *node) {
	if (!node) {
		return false;
	}
	if (auto select_node = dynamic_cast<SelectNode *>(node)) {
		return SelectNodeContainsPGQ(select_node);
	}
	if (auto cte_node = dynamic_cast<CTENode *>(node)) {
		return QueryNodeContainsPGQ(cte_node->child.get());
	}
	if (auto set_operation_node = dynamic_cast<SetOperationNode *>(node)) {
		for (auto &child : set_operation_node->children) {
			if (QueryNodeContainsPGQ(child.get())) {
				return true;
			}
		}
	}
	return false;
}

static bool StatementContainsPGQ(SQLStatement *statement) {
	if (!statement) {
		return false;
	}
	switch (statement->type) {
	case StatementType::SELECT_STATEMENT:
		return QueryNodeContainsPGQ(statement->Cast<SelectStatement>().node.get());
	case StatementType::CREATE_STATEMENT: {
		auto &create_statement = statement->Cast<CreateStatement>();
		if (dynamic_cast<CreatePropertyGraphInfo *>(create_statement.info.get())) {
			return true;
		}
		auto create_table = dynamic_cast<CreateTableInfo *>(create_statement.info.get());
		return create_table && StatementContainsPGQ(create_table->query.get());
	}
	case StatementType::DROP_STATEMENT:
		return dynamic_cast<DropPropertyGraphInfo *>(statement->Cast<DropStatement>().info.get()) != nullptr;
	case StatementType::EXPLAIN_STATEMENT:
		return StatementContainsPGQ(statement->Cast<ExplainStatement>().stmt.get());
	case StatementType::COPY_STATEMENT: {
		auto &copy_statement = statement->Cast<CopyStatement>();
		auto select_node = dynamic_cast<SelectNode *>(copy_statement.info->select_statement.get());
		return select_node && TableRefContainsPGQ(select_node->from_table.get());
	}
	case StatementType::INSERT_STATEMENT:
		return StatementContainsPGQ(statement->Cast<InsertStatement>().select_statement.get());
	default:
		return false;
	}
}

static bool QueryMightContainPGQ(const string &query) {
	auto lower_query = StringUtil::Lower(query);
	return StringUtil::Contains(lower_query, "graph_table") || StringUtil::Contains(lower_query, "property graph");
}

static ParserExtension MakeDuckPGQParserExtension() {
	ParserExtension extension;
	extension.parse_function = duckpgq_parse;
	extension.plan_function = duckpgq_plan;
	extension.parser_override = duckpgq_parser_override;
	extension.parser_info = make_shared_ptr<DuckPGQParserExtensionInfo>();
	return extension;
}

} // namespace

ParserOverrideResult duckpgq_parser_override(ParserExtensionInfo *, const string &query, ParserOptions &options) {
	if (!QueryMightContainPGQ(query)) {
		return ParserOverrideResult();
	}

	try {
		ParserOptions inner_options = options;
		inner_options.parser_override_setting = AllowParserOverride::DEFAULT_OVERRIDE;
		Parser parser(inner_options);
		parser.ParseQuery(query);
		if (parser.statements.empty()) {
			return ParserOverrideResult();
		}

		vector<unique_ptr<SQLStatement>> result;
		bool contains_pgq = false;
		auto extension = MakeDuckPGQParserExtension();
		for (auto &statement : parser.statements) {
			if (StatementContainsPGQ(statement.get())) {
				contains_pgq = true;
				result.push_back(make_uniq<ExtensionStatement>(
				    extension, make_uniq_base<ParserExtensionParseData, DuckPGQParseData>(statement->Copy())));
			} else {
				result.push_back(std::move(statement));
			}
		}

		if (!contains_pgq) {
			return ParserOverrideResult();
		}
		return ParserOverrideResult(std::move(result));
	} catch (std::exception &error) {
		return ParserOverrideResult(error);
	}
}

ParserExtensionParseResult duckpgq_parse(ParserExtensionInfo *info, const std::string &query) {
	Parser parser;
	parser.ParseQuery((query[0] == '-') ? query.substr(1, query.length()) : query);
	if (parser.statements.size() != 1) {
		throw Exception(ExceptionType::PARSER, "More than one statement detected, please only give one.");
	}
	return ParserExtensionParseResult(
	    make_uniq_base<ParserExtensionParseData, DuckPGQParseData>(std::move(parser.statements[0])));
}

void duckpgq_find_match_function(TableRef *table_ref, DuckPGQState &duckpgq_state) {
	// TODO(dtenwolde) add support for other style of tableRef (e.g. PivotRef)
	if (auto table_function_ref = dynamic_cast<TableFunctionRef *>(table_ref)) {
		auto function = dynamic_cast<FunctionExpression *>(table_function_ref->function.get());
		if (!function || function->function_name != "duckpgq_match") {
			return;
		}
		MatchExpression *match_expr = table_function_ref->match_expression.get();
		if (!match_expr && !function->children.empty()) {
			match_expr = dynamic_cast<MatchExpression *>(function->children[0].get());
		}
		if (!match_expr) {
			throw BinderException("duckpgq_match is missing a MATCH expression");
		}
		if (table_function_ref->alias.empty()) {
			table_function_ref->alias = match_expr->alias;
		}
		int32_t match_index = duckpgq_state.match_index++;
		if (table_function_ref->match_expression) {
			duckpgq_state.transform_expression[match_index] =
			    unique_ptr_cast<MatchExpression, ParsedExpression>(std::move(table_function_ref->match_expression));
		} else {
			duckpgq_state.transform_expression[match_index] = std::move(function->children[0]);
		}
		function->children.clear();
		auto function_identifier = make_uniq<ConstantExpression>(Value::CreateValue(match_index));
		function->children.push_back(std::move(function_identifier));
	} else if (auto join_ref = dynamic_cast<JoinRef *>(table_ref)) {
		// Handle JoinRef case
		duckpgq_find_match_function(join_ref->left.get(), duckpgq_state);
		duckpgq_find_match_function(join_ref->right.get(), duckpgq_state);
	} else if (auto subquery_ref = dynamic_cast<SubqueryRef *>(table_ref)) {
		// Handle SubqueryRef case
		auto subquery = subquery_ref->subquery.get();
		duckpgq_find_select_statement(subquery, duckpgq_state);
	}
}

ParserExtensionPlanResult duckpgq_find_select_statement(SQLStatement *statement, DuckPGQState &duckpgq_state) {
	const auto select_statement = dynamic_cast<SelectStatement *>(statement);
	auto node = dynamic_cast<SelectNode *>(select_statement->node.get());
	CTENode *cte_node = nullptr;

	// Check if node is not a SelectNode
	if (!node) {
		// Attempt to cast to CTENode
		cte_node = dynamic_cast<CTENode *>(select_statement->node.get());
		if (cte_node) {
			// Get the child node as a SelectNode if cte_node is valid
			node = dynamic_cast<SelectNode *>(cte_node->child.get());
		}
	}

	// Check if node is a ShowRef
	if (node) {
		const auto describe_node = dynamic_cast<ShowRef *>(node->from_table.get());
		if (describe_node) {
			ParserExtensionPlanResult result;
			result.requires_valid_transaction = true;
			result.return_type = StatementReturnType::QUERY_RESULT;
			if (describe_node->show_type == ShowType::SUMMARY) {
				result.function = SummarizePropertyGraphFunction();
				result.parameters.push_back(Value(describe_node->table_name));
				return result;
			}
			if (describe_node->show_type == ShowType::DESCRIBE) {
				result.function = DescribePropertyGraphFunction();
				return result;
			}
			throw BinderException("Unknown show type %s found.", describe_node->show_type);
		}
	}

	CommonTableExpressionMap *cte_map = nullptr;
	if (node) {
		cte_map = &node->cte_map;
	} else if (cte_node) {
		cte_map = &cte_node->cte_map;
	}

	if (!cte_map) {
		return {};
	}

	for (auto const &kv_pair : cte_map->map) {
		auto const &cte = kv_pair.second;

		auto *cte_select_statement = dynamic_cast<SelectStatement *>(cte->query.get());
		if (!cte_select_statement) {
			continue;
		}

		auto *select_node = dynamic_cast<SelectNode *>(cte_select_statement->node.get());
		if (!select_node) {
			continue; // The SelectStatement has no SelectNode, skip.
		}

		// If we get here, we know select_node is valid.
		duckpgq_find_match_function(select_node->from_table.get(), duckpgq_state);
	}
	if (node) {
		duckpgq_find_match_function(node->from_table.get(), duckpgq_state);
	} else {
		throw Exception(ExceptionType::INTERNAL, "node is a nullptr.");
	}
	return {};
}

ParserExtensionPlanResult duckpgq_handle_statement(SQLStatement *statement, DuckPGQState &duckpgq_state) {
	if (statement->type == StatementType::SELECT_STATEMENT) {
		auto result = duckpgq_find_select_statement(statement, duckpgq_state);
		if (result.function.bind == nullptr) {
			throw Exception(ExceptionType::BINDER, "use duckpgq_bind instead");
		}
		return result;
	}
	if (statement->type == StatementType::CREATE_STATEMENT) {
		const auto &create_statement = statement->Cast<CreateStatement>();
		const auto create_property_graph = dynamic_cast<CreatePropertyGraphInfo *>(create_statement.info.get());
		if (create_property_graph) {
			ParserExtensionPlanResult result;
			result.function = CreatePropertyGraphFunction();
			result.requires_valid_transaction = true;
			result.return_type = StatementReturnType::QUERY_RESULT;
			return result;
		}
		const auto create_table = reinterpret_cast<CreateTableInfo *>(create_statement.info.get());
		duckpgq_handle_statement(create_table->query.get(), duckpgq_state);
	}
	if (statement->type == StatementType::DROP_STATEMENT) {
		ParserExtensionPlanResult result;
		result.function = DropPropertyGraphFunction();
		result.requires_valid_transaction = true;
		result.return_type = StatementReturnType::QUERY_RESULT;
		return result;
	}
	if (statement->type == StatementType::EXPLAIN_STATEMENT) {
		auto &explain_statement = statement->Cast<ExplainStatement>();
		duckpgq_handle_statement(explain_statement.stmt.get(), duckpgq_state);
	}
	if (statement->type == StatementType::COPY_STATEMENT) {
		const auto &copy_statement = statement->Cast<CopyStatement>();
		const auto select_node = dynamic_cast<SelectNode *>(copy_statement.info->select_statement.get());
		duckpgq_find_match_function(select_node->from_table.get(), duckpgq_state);
		throw Exception(ExceptionType::BINDER, "use duckpgq_bind instead");
	}
	if (statement->type == StatementType::INSERT_STATEMENT) {
		const auto &insert_statement = statement->Cast<InsertStatement>();
		duckpgq_handle_statement(insert_statement.select_statement.get(), duckpgq_state);
	}

	throw Exception(ExceptionType::NOT_IMPLEMENTED,
	                StatementTypeToString(statement->type) + "has not been implemented yet for DuckPGQ queries");
}

ParserExtensionPlanResult duckpgq_plan(ParserExtensionInfo *, ClientContext &context,
                                       unique_ptr<ParserExtensionParseData> parse_data) {
	auto duckpgq_state = GetDuckPGQState(context);
	duckpgq_state->parse_data = std::move(parse_data);
	auto duckpgq_parse_data = dynamic_cast<DuckPGQParseData *>(duckpgq_state->parse_data.get());

	if (!duckpgq_parse_data) {
		throw Exception(ExceptionType::BINDER, "No DuckPGQ parse data found");
	}

	auto statement = duckpgq_parse_data->statement.get();
	return duckpgq_handle_statement(statement, *duckpgq_state);
}

//------------------------------------------------------------------------------
// Register functions
//------------------------------------------------------------------------------
void CorePGQParser::RegisterPGQParserExtension(ExtensionLoader &loader) {
	auto &db = loader.GetDatabaseInstance();
	auto &config = DBConfig::GetConfig(db);
	config.SetOptionByName("allow_parser_override_extension", Value("fallback"));
	auto &manager = ExtensionCallbackManager::Get(db);
	manager.Register(DuckPGQParserExtension());
}

} // namespace duckdb
