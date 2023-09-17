#include "spatial/common.hpp"
#include "spatial/core/types.hpp"
#include "spatial/geos/functions/scalar.hpp"
#include "spatial/geos/functions/common.hpp"
#include "spatial/geos/geos_wrappers.hpp"
#include "spatial/geos/geos_executor.hpp"

#include "duckdb/main/extension_util.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"
#include "duckdb/common/vector_operations/unary_executor.hpp"
#include "duckdb/common/vector_operations/binary_executor.hpp"

namespace spatial {

namespace geos {

using namespace spatial::core;

static void CoveredByFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &lstate = GEOSFunctionLocalState::ResetAndGet(state);
	auto &left = args.data[0];
	auto &right = args.data[1];
	auto count = args.size();
	GEOSExecutor::ExecuteNonSymmetricPreparedBinary(lstate, left, right, count, result, GEOSCoveredBy_r,
	                                                GEOSPreparedCoveredBy_r);
}

void GEOSScalarFunctions::RegisterStCoveredBy(DatabaseInstance &instance) {
	ScalarFunctionSet set("ST_CoveredBy");

	set.AddFunction(ScalarFunction({GeoTypes::GEOMETRY(), GeoTypes::GEOMETRY()}, LogicalType::BOOLEAN,
	                               CoveredByFunction, nullptr, nullptr, nullptr, GEOSFunctionLocalState::Init));

	ExtensionUtil::RegisterFunction(instance, set);
}

} // namespace geos

} // namespace spatial
