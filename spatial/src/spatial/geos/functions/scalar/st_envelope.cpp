#include "spatial/common.hpp"
#include "spatial/core/types.hpp"
#include "spatial/geos/functions/scalar.hpp"
#include "spatial/geos/functions/common.hpp"
#include "spatial/geos/geos_wrappers.hpp"

#include "duckdb/main/extension_util.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"
#include "duckdb/common/vector_operations/unary_executor.hpp"
#include "duckdb/common/vector_operations/binary_executor.hpp"

namespace spatial {

namespace geos {

using namespace spatial::core;

static void EnvelopeFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &lstate = GEOSFunctionLocalState::ResetAndGet(state);
	auto &ctx = lstate.ctx.GetCtx();
	UnaryExecutor::ExecuteWithNulls<string_t, string_t>(
	    args.data[0], result, args.size(), [&](string_t &geometry_blob, ValidityMask &mask, idx_t i) {
		    auto geometry = lstate.ctx.Deserialize(geometry_blob);
		    auto envelope = make_uniq_geos(ctx, GEOSEnvelope_r(ctx, geometry.get()));
		    return lstate.ctx.Serialize(result, envelope);
	    });
}

void GEOSScalarFunctions::RegisterStEnvelope(DatabaseInstance &instance) {
	ScalarFunctionSet set("ST_Envelope");

	set.AddFunction(ScalarFunction({GeoTypes::GEOMETRY()}, GeoTypes::GEOMETRY(), EnvelopeFunction, nullptr, nullptr,
	                               nullptr, GEOSFunctionLocalState::Init));

	ExtensionUtil::RegisterFunction(instance, set);
}

} // namespace geos

} // namespace spatial
