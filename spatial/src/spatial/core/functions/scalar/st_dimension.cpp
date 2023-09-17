#include "spatial/common.hpp"
#include "spatial/core/types.hpp"
#include "spatial/core/functions/scalar.hpp"
#include "spatial/core/functions/common.hpp"
#include "spatial/core/geometry/geometry.hpp"

#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"
#include "duckdb/main/extension_util.hpp"
#include "duckdb/common/vector_operations/unary_executor.hpp"
#include "duckdb/common/vector_operations/binary_executor.hpp"

namespace spatial {

namespace core {

//------------------------------------------------------------------------------
// GEOMETRY
//------------------------------------------------------------------------------
static void DimensionFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &lstate = GeometryFunctionLocalState::ResetAndGet(state);

	auto count = args.size();
	auto &input = args.data[0];

	UnaryExecutor::Execute<string_t, int32_t>(input, result, count, [&](string_t input) {
		auto geometry = lstate.factory.Deserialize(input);
		return geometry.Dimension();
	});
}

void CoreScalarFunctions::RegisterStDimension(DatabaseInstance &instance) {
	ScalarFunctionSet set("ST_Dimension");

	set.AddFunction(ScalarFunction({GeoTypes::GEOMETRY()}, LogicalType::INTEGER, DimensionFunction, nullptr, nullptr,
	                               nullptr, GeometryFunctionLocalState::Init));

	ExtensionUtil::RegisterFunction(instance, set);
	// info.on_conflict = OnCreateConflict::ALTER_ON_CONFLICT;
}

} // namespace core

} // namespace spatial