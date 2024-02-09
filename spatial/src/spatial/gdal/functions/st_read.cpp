#include "duckdb/parser/parsed_data/create_table_function_info.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"
#include "duckdb/planner/filter/conjunction_filter.hpp"
#include "duckdb/planner/filter/constant_filter.hpp"
#include "duckdb/planner/table_filter.hpp"
#include "duckdb/function/function.hpp"
#include "duckdb/function/replacement_scan.hpp"

#include "spatial/common.hpp"
#include "spatial/core/types.hpp"
#include "spatial/gdal/functions.hpp"
#include "spatial/gdal/file_handler.hpp"
#include "spatial/core/geometry/geometry_factory.hpp"

#include "ogrsf_frmts.h"

namespace spatial {

namespace gdal {

enum SpatialFilterType { Wkb, Rectangle };

struct SpatialFilter {
	SpatialFilterType type;
	explicit SpatialFilter(SpatialFilterType type_p) : type(type_p) {};
};

struct RectangleSpatialFilter : SpatialFilter {
	double min_x, min_y, max_x, max_y;
	RectangleSpatialFilter(double min_x_p, double min_y_p, double max_x_p, double max_y_p)
	    : SpatialFilter(SpatialFilterType::Rectangle), min_x(min_x_p), min_y(min_y_p), max_x(max_x_p), max_y(max_y_p) {
	}
};

struct WKBSpatialFilter : SpatialFilter {
	OGRGeometryH geom;
	explicit WKBSpatialFilter(const string &wkb_p) : SpatialFilter(SpatialFilterType::Wkb), geom(nullptr) {
		auto ok = OGR_G_CreateFromWkb(wkb_p.c_str(), nullptr, &geom, (int)wkb_p.size());
		if (ok != OGRERR_NONE) {
			throw InvalidInputException("WKBSpatialFilter: could not create geometry from WKB");
		}
	}
	~WKBSpatialFilter() {
		OGR_G_DestroyGeometry(geom);
	}
};

// TODO: Verify that this actually corresponds to the same sql subset expected by OGR SQL
static string FilterToGdal(const TableFilter &filter, const string &column_name) {

	switch (filter.filter_type) {
	case TableFilterType::CONSTANT_COMPARISON: {
		auto &constant_filter = filter.Cast<ConstantFilter>();
		return KeywordHelper::WriteOptionallyQuoted(column_name) +
		       ExpressionTypeToOperator(constant_filter.comparison_type) + constant_filter.constant.ToSQLString();
	}
	case TableFilterType::CONJUNCTION_AND: {
		auto &and_filter = filter.Cast<ConjunctionAndFilter>();
		vector<string> filters;
		for (const auto &child_filter : and_filter.child_filters) {
			filters.push_back(FilterToGdal(*child_filter, column_name));
		}
		return StringUtil::Join(filters, " AND ");
	}
	case TableFilterType::CONJUNCTION_OR: {
		auto &or_filter = filter.Cast<ConjunctionOrFilter>();
		vector<string> filters;
		for (const auto &child_filter : or_filter.child_filters) {
			filters.push_back(FilterToGdal(*child_filter, column_name));
		}
		return StringUtil::Join(filters, " OR ");
	}
	case TableFilterType::IS_NOT_NULL: {
		return KeywordHelper::WriteOptionallyQuoted(column_name) + " IS NOT NULL";
	}
	case TableFilterType::IS_NULL: {
		return KeywordHelper::WriteOptionallyQuoted(column_name) + " IS NULL";
	}
	default:
		throw NotImplementedException("FilterToGdal: filter type not implemented");
	}
}

static string FilterToGdal(const TableFilterSet &set, const vector<idx_t> &column_ids,
                           const vector<string> &column_names) {

	vector<string> filters;
	for (auto &input_filter : set.filters) {
		auto col_idx = column_ids[input_filter.first];
		auto &col_name = column_names[col_idx];
		filters.push_back(FilterToGdal(*input_filter.second, col_name));
	}
	return StringUtil::Join(filters, " AND ");
}

struct GdalScanFunctionData : public TableFunctionData {
	idx_t layer_idx;
	bool sequential_layer_scan = false;
	bool keep_wkb = false;
	unordered_set<idx_t> geometry_column_ids;
	vector<string> layer_creation_options;
	unique_ptr<SpatialFilter> spatial_filter;
	GDALDatasetUniquePtr dataset;
	idx_t max_threads;
	// before they are renamed
	vector<string> all_names;
	vector<LogicalType> all_types;
	atomic<idx_t> lines_read;
	ArrowTableType arrow_table;
};

struct GdalScanLocalState : ArrowScanLocalState {
	core::GeometryFactory factory;
	explicit GdalScanLocalState(unique_ptr<ArrowArrayWrapper> current_chunk, ClientContext &context)
	    : ArrowScanLocalState(std::move(current_chunk)), factory(BufferAllocator::Get(context)) {
	}
};

struct GdalScanGlobalState : ArrowScanGlobalState {};

//------------------------------------------------------------------------------
// Bind
//------------------------------------------------------------------------------
unique_ptr<FunctionData> GdalTableFunction::Bind(ClientContext &context, TableFunctionBindInput &input,
                                                 vector<LogicalType> &return_types, vector<string> &names) {

	auto &config = DBConfig::GetConfig(context);
	if (!config.options.enable_external_access) {
		throw PermissionException("Scanning GDAL files is disabled through configuration");
	}

	// First scan for "options" parameter
	auto gdal_open_options = vector<char const *>();
	auto options_param = input.named_parameters.find("open_options");
	if (options_param != input.named_parameters.end()) {
		for (auto &param : ListValue::GetChildren(options_param->second)) {
			gdal_open_options.push_back(StringValue::Get(param).c_str());
		}
	}
	if (!gdal_open_options.empty()) {
		gdal_open_options.push_back(nullptr);
	}

	auto gdal_allowed_drivers = vector<char const *>();
	auto drivers_param = input.named_parameters.find("allowed_drivers");
	if (drivers_param != input.named_parameters.end()) {
		for (auto &param : ListValue::GetChildren(drivers_param->second)) {
			gdal_allowed_drivers.push_back(StringValue::Get(param).c_str());
		}
	}
	if (!gdal_allowed_drivers.empty()) {
		gdal_allowed_drivers.push_back(nullptr);
	}

	auto gdal_sibling_files = vector<char const *>();
	auto siblings_params = input.named_parameters.find("sibling_files");
	if (siblings_params != input.named_parameters.end()) {
		for (auto &param : ListValue::GetChildren(drivers_param->second)) {
			gdal_sibling_files.push_back(StringValue::Get(param).c_str());
		}
	}
	if (!gdal_sibling_files.empty()) {
		gdal_sibling_files.push_back(nullptr);
	}

	// Now we can open the dataset
	auto raw_file_name = input.inputs[0].GetValue<string>();
	auto &ctx_state = GDALClientContextState::GetOrCreate(context);
	auto prefixed_file_name = ctx_state.GetPrefix() + raw_file_name;
	auto dataset =
	    GDALDatasetUniquePtr(GDALDataset::Open(prefixed_file_name.c_str(), GDAL_OF_VECTOR | GDAL_OF_VERBOSE_ERROR,
	                                           gdal_allowed_drivers.empty() ? nullptr : gdal_allowed_drivers.data(),
	                                           gdal_open_options.empty() ? nullptr : gdal_open_options.data(),
	                                           gdal_sibling_files.empty() ? nullptr : gdal_sibling_files.data()));

	if (dataset == nullptr) {
		auto error = string(CPLGetLastErrorMsg());
		throw IOException("Could not open file: " + raw_file_name + " (" + error + ")");
	}

	// Double check that the dataset have any layers
	if (dataset->GetLayerCount() <= 0) {
		throw IOException("Dataset does not contain any layers");
	}

	// Now we can bind the additonal options
	auto result = make_uniq<GdalScanFunctionData>();
	bool max_batch_size_set = false;
	for (auto &kv : input.named_parameters) {
		auto loption = StringUtil::Lower(kv.first);
		if (loption == "layer") {

			// Find layer by index
			if (kv.second.type() == LogicalType::INTEGER) {
				auto layer_idx = IntegerValue::Get(kv.second);
				if (layer_idx < 0) {
					throw BinderException("Layer index must be positive");
				}
				if (layer_idx > dataset->GetLayerCount()) {
					throw BinderException(
					    StringUtil::Format("Layer index too large (%s > %s)", layer_idx, dataset->GetLayerCount()));
				}
				result->layer_idx = (idx_t)layer_idx;
			}

			// Find layer by name
			if (kv.second.type() == LogicalTypeId::VARCHAR) {
				auto name = StringValue::Get(kv.second).c_str();
				bool found = false;
				for (auto layer_idx = 0; layer_idx < dataset->GetLayerCount(); layer_idx++) {
					if (strcmp(dataset->GetLayer(layer_idx)->GetName(), name) == 0) {
						result->layer_idx = (idx_t)layer_idx;
						found = true;
						break;
					}
				}
				if (!found) {
					throw BinderException(StringUtil::Format("Layer '%s' could not be found in dataset", name));
				}
			}
		}

		if (loption == "spatial_filter_box" && kv.second.type() == core::GeoTypes::BOX_2D()) {
			if (result->spatial_filter) {
				throw BinderException("Only one spatial filter can be specified");
			}
			auto &children = StructValue::GetChildren(kv.second);
			auto minx = DoubleValue::Get(children[0]);
			auto miny = DoubleValue::Get(children[1]);
			auto maxx = DoubleValue::Get(children[2]);
			auto maxy = DoubleValue::Get(children[3]);
			result->spatial_filter = make_uniq<RectangleSpatialFilter>(minx, miny, maxx, maxy);
		}

		if (loption == "spatial_filter" && kv.second.type() == core::GeoTypes::WKB_BLOB()) {
			if (result->spatial_filter) {
				throw BinderException("Only one spatial filter can be specified");
			}
			auto wkb = StringValue::Get(kv.second);
			result->spatial_filter = make_uniq<WKBSpatialFilter>(wkb);
		}

		if (loption == "max_threads") {
			auto max_threads = IntegerValue::Get(kv.second);
			if (max_threads <= 0) {
				throw BinderException("'max_threads' parameter must be positive");
			}
			result->max_threads = (idx_t)max_threads;
		}

		if (loption == "sequential_layer_scan") {
			result->sequential_layer_scan = BooleanValue::Get(kv.second);
		}

		if (loption == "max_batch_size") {
			auto max_batch_size = IntegerValue::Get(kv.second);
			if (max_batch_size <= 0) {
				throw BinderException("'max_batch_size' parameter must be positive");
			}
			result->layer_creation_options.push_back(StringUtil::Format("MAX_FEATURES_IN_BATCH=%d", max_batch_size));
			max_batch_size_set = true;
		}

		if (loption == "keep_wkb") {
			result->keep_wkb = BooleanValue::Get(kv.second);
		}
	}

	// set default max_threads
	if (result->max_threads == 0) {
		result->max_threads = context.db->NumberOfThreads();
	}

	// Defaults
	result->layer_creation_options.push_back("INCLUDE_FID=NO");
	if (max_batch_size_set == false) {
		// Set default max batch size to standard vector size
		result->layer_creation_options.push_back(StringUtil::Format("MAX_FEATURES_IN_BATCH=%d", STANDARD_VECTOR_SIZE));
	}

	// set layer options
	char **lco = nullptr;
	for (auto &option : result->layer_creation_options) {
		lco = CSLAddString(lco, option.c_str());
	}

	// Get the schema for the selected layer
	auto layer = dataset->GetLayer(result->layer_idx);
	struct ArrowArrayStream stream;
	if (!layer->GetArrowStream(&stream, lco)) {
		// layer is owned by GDAL, we do not need to destory it
		CSLDestroy(lco);
		throw IOException("Could not get arrow stream from layer");
	}
	CSLDestroy(lco);

	struct ArrowSchema schema;
	if (stream.get_schema(&stream, &schema) != 0) {
		if (stream.release) {
			stream.release(&stream);
		}
		throw IOException("Could not get arrow schema from layer");
	}

	// The Arrow API will return attributes in this order
	// 1. FID column
	// 2. all ogr field attributes
	// 3. all geometry columns

	auto attribute_count = schema.n_children;
	auto attributes = schema.children;

	result->all_names.reserve(attribute_count + 1);
	names.reserve(attribute_count + 1);

	for (idx_t col_idx = 0; col_idx < (idx_t)attribute_count; col_idx++) {
		auto &attribute = *attributes[col_idx];

		const char ogc_flag[] = {'\x01', '\0', '\0', '\0', '\x14', '\0', '\0', '\0', 'A', 'R', 'R', 'O', 'W',
		                         ':',    'e',  'x',  't',  'e',    'n',  's',  'i',  'o', 'n', ':', 'n', 'a',
		                         'm',    'e',  '\a', '\0', '\0',   '\0', 'o',  'g',  'c', '.', 'w', 'k', 'b'};

		auto arrow_type = GetArrowLogicalType(attribute);
		auto column_name = string(attribute.name);
		auto duckdb_type = arrow_type->GetDuckType();

		if (duckdb_type.id() == LogicalTypeId::BLOB && attribute.metadata != nullptr &&
		    strncmp(attribute.metadata, ogc_flag, sizeof(ogc_flag)) == 0) {
			// This is a WKB geometry blob
			result->arrow_table.AddColumn(col_idx, std::move(arrow_type));

			if (result->keep_wkb) {
				return_types.emplace_back(core::GeoTypes::WKB_BLOB());
			} else {
				return_types.emplace_back(core::GeoTypes::GEOMETRY());
				if (column_name == "wkb_geometry") {
					column_name = "geom";
				}
			}
			result->geometry_column_ids.insert(col_idx);

		} else if (attribute.dictionary) {
			auto dictionary_type = GetArrowLogicalType(attribute);
			return_types.emplace_back(dictionary_type->GetDuckType());
			arrow_type->SetDictionary(std::move(dictionary_type));
			result->arrow_table.AddColumn(col_idx, std::move(arrow_type));
		} else {
			return_types.emplace_back(arrow_type->GetDuckType());
			result->arrow_table.AddColumn(col_idx, std::move(arrow_type));
		}

		// keep these around for projection/filter pushdown later
		// does GDAL even allow duplicate/missing names?
		result->all_names.push_back(column_name);

		if (column_name.empty()) {
			names.push_back("v" + to_string(col_idx));
		} else {
			names.push_back(column_name);
		}
	}

	schema.release(&schema);
	stream.release(&stream);

	GdalTableFunction::RenameColumns(names);

	result->dataset = std::move(dataset);
	result->all_types = return_types;

	return std::move(result);
};

void GdalTableFunction::RenameColumns(vector<string> &names) {
	unordered_map<string, idx_t> name_map;
	for (auto &column_name : names) {
		// put it all lower_case
		auto low_column_name = StringUtil::Lower(column_name);
		if (name_map.find(low_column_name) == name_map.end()) {
			// Name does not exist yet
			name_map[low_column_name]++;
		} else {
			// Name already exists, we add _x where x is the repetition number
			string new_column_name = column_name + "_" + std::to_string(name_map[low_column_name]);
			auto new_column_name_low = StringUtil::Lower(new_column_name);
			while (name_map.find(new_column_name_low) != name_map.end()) {
				// This name is already here due to a previous definition
				name_map[low_column_name]++;
				new_column_name = column_name + "_" + std::to_string(name_map[low_column_name]);
				new_column_name_low = StringUtil::Lower(new_column_name);
			}
			column_name = new_column_name;
			name_map[new_column_name_low]++;
		}
	}
}

idx_t GdalTableFunction::MaxThreads(ClientContext &context, const FunctionData *bind_data_p) {
	auto data = (const GdalScanFunctionData *)bind_data_p;
	return data->max_threads;
}

OGRLayer *open_layer(const GdalScanFunctionData &data) {

	// Get selected layer
	OGRLayer *layer = nullptr;
	if (data.sequential_layer_scan) {
		// Get the layer from the dataset by scanning through the layers
		for (int i = 0; i < data.dataset->GetLayerCount(); i++) {
			layer = data.dataset->GetLayer(i);
			if (i == (int)data.layer_idx) {
				// desired layer found
				break;
			}
			// else scan through and empty the layer
			OGRFeature *feature;
			while ((feature = layer->GetNextFeature()) != nullptr) {
				OGRFeature::DestroyFeature(feature);
			}
		}
	} else {
		// Otherwise get the layer directly
		layer = data.dataset->GetLayer(data.layer_idx);
	}

	// Apply spatial filter (if we got one)
	if (data.spatial_filter != nullptr) {
		if (data.spatial_filter->type == SpatialFilterType::Rectangle) {
			auto &rect = (RectangleSpatialFilter &)*data.spatial_filter;
			layer->SetSpatialFilterRect(rect.min_x, rect.min_y, rect.max_x, rect.max_y);
		} else if (data.spatial_filter->type == SpatialFilterType::Wkb) {
			auto &filter = (WKBSpatialFilter &)*data.spatial_filter;
			layer->SetSpatialFilter(OGRGeometry::FromHandle(filter.geom));
		}
	}

	return layer;
}

//-----------------------------------------------------------------------------
// Init global
//-----------------------------------------------------------------------------
unique_ptr<GlobalTableFunctionState> GdalTableFunction::InitGlobal(ClientContext &context,
                                                                   TableFunctionInitInput &input) {
	auto &data = input.bind_data->Cast<GdalScanFunctionData>();
	auto global_state = make_uniq<GdalScanGlobalState>();

	auto layer = open_layer(data);
	// TODO: Apply projection pushdown

	// Apply predicate pushdown
	// We simply create a string out of the predicates and pass it to GDAL.
	if (input.filters) {
		auto filter_clause = FilterToGdal(*input.filters, input.column_ids, data.all_names);
		layer->SetAttributeFilter(filter_clause.c_str());
	}

	// Create arrow stream from layer

	global_state->stream = make_uniq<ArrowArrayStreamWrapper>();

	// set layer options
	char **lco = nullptr;
	for (auto &option : data.layer_creation_options) {
		lco = CSLAddString(lco, option.c_str());
	}
	if (!layer->GetArrowStream(&global_state->stream->arrow_array_stream, lco)) {
		CSLDestroy(lco);
		throw IOException("Could not get arrow stream");
	}
	CSLDestroy(lco);

	global_state->max_threads = GdalTableFunction::MaxThreads(context, input.bind_data.get());

	if (input.CanRemoveFilterColumns()) {
		global_state->projection_ids = input.projection_ids;
		for (const auto &col_idx : input.column_ids) {
			if (col_idx == COLUMN_IDENTIFIER_ROW_ID) {
				global_state->scanned_types.emplace_back(LogicalType::ROW_TYPE);
			} else {
				global_state->scanned_types.push_back(data.all_types[col_idx]);
			}
		}
	}

	return std::move(global_state);
}

//-----------------------------------------------------------------------------
// Init Local
//-----------------------------------------------------------------------------
unique_ptr<LocalTableFunctionState> GdalTableFunction::InitLocal(ExecutionContext &context,
                                                                 TableFunctionInitInput &input,
                                                                 GlobalTableFunctionState *global_state_p) {

	auto &global_state = global_state_p->Cast<ArrowScanGlobalState>();
	auto current_chunk = make_uniq<ArrowArrayWrapper>();
	auto result = make_uniq<GdalScanLocalState>(std::move(current_chunk), context.client);
	result->column_ids = input.column_ids;
	result->filters = input.filters.get();
	if (input.CanRemoveFilterColumns()) {
		result->all_columns.Initialize(context.client, global_state.scanned_types);
	}

	if (!ArrowScanParallelStateNext(context.client, input.bind_data.get(), *result, global_state)) {
		return nullptr;
	}

	return std::move(result);
}

//-----------------------------------------------------------------------------
// Scan
//-----------------------------------------------------------------------------
void GdalTableFunction::Scan(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	if (!input.local_state) {
		return;
	}
	auto &data = (GdalScanFunctionData &)*input.bind_data;
	auto &state = (GdalScanLocalState &)*input.local_state;
	auto &global_state = (GdalScanGlobalState &)*input.global_state;

	//! Out of tuples in this chunk
	if (state.chunk_offset >= (idx_t)state.chunk->arrow_array.length) {
		if (!ArrowScanParallelStateNext(context, input.bind_data.get(), state, global_state)) {
			return;
		}
	}
	auto output_size = MinValue<int64_t>(STANDARD_VECTOR_SIZE, state.chunk->arrow_array.length - state.chunk_offset);
	data.lines_read += output_size;

	if (global_state.CanRemoveFilterColumns()) {
		state.all_columns.Reset();
		state.all_columns.SetCardinality(output_size);
		ArrowToDuckDB(state, data.arrow_table.GetColumns(), state.all_columns, data.lines_read - output_size, false);
		output.ReferenceColumns(state.all_columns, global_state.projection_ids);
	} else {
		output.SetCardinality(output_size);
		ArrowToDuckDB(state, data.arrow_table.GetColumns(), output, data.lines_read - output_size, false);
	}

	if (!data.keep_wkb) {
		// Find the geometry columns
		for (idx_t col_idx = 0; col_idx < state.column_ids.size(); col_idx++) {
			auto mapped_idx = state.column_ids[col_idx];
			if (data.geometry_column_ids.find(mapped_idx) != data.geometry_column_ids.end()) {
				// Found a geometry column
				// Convert the WKB columns to a geometry column
				state.factory.allocator.Reset();
				auto &wkb_vec = output.data[col_idx];
				Vector geom_vec(core::GeoTypes::GEOMETRY(), output_size);
				UnaryExecutor::Execute<string_t, string_t>(wkb_vec, geom_vec, output_size, [&](string_t input) {
					auto geometry = state.factory.FromWKB(input.GetDataUnsafe(), input.GetSize());
					return state.factory.Serialize(geom_vec, geometry);
				});
				output.data[col_idx].ReferenceAndSetType(geom_vec);
			}
		}
	}

	output.Verify();
	state.chunk_offset += output.size();
}

unique_ptr<NodeStatistics> GdalTableFunction::Cardinality(ClientContext &context, const FunctionData *data) {
	auto &gdal_data = data->Cast<GdalScanFunctionData>();
	auto result = make_uniq<NodeStatistics>();

	if (gdal_data.sequential_layer_scan) {
		// It would be too expensive to calculate the cardinality for a sequential layer scan
		// as we would have to scan through all the layers twice.
		return result;
	}

	// Some drivers wont return a feature count if it would be to expensive to calculate
	// (unless we pass 1 "= force" to the function calculate)
	auto layer = open_layer(gdal_data);

	auto count = layer->GetFeatureCount(0);
	if (count > -1) {
		result->has_estimated_cardinality = true;
		result->estimated_cardinality = count;
	}

	return result;
}

unique_ptr<TableRef> GdalTableFunction::ReplacementScan(ClientContext &, const string &table_name,
                                                        ReplacementScanData *) {

	auto lower_name = StringUtil::Lower(table_name);
	// Check if the table name ends with some common geospatial file extensions
	if (StringUtil::EndsWith(lower_name, ".gpkg") || StringUtil::EndsWith(lower_name, ".fgb")) {

		auto table_function = make_uniq<TableFunctionRef>();
		vector<unique_ptr<ParsedExpression>> children;
		children.push_back(make_uniq<ConstantExpression>(Value(table_name)));
		table_function->function = make_uniq<FunctionExpression>("ST_Read", std::move(children));
		return std::move(table_function);
	}
	// else not something we can replace
	return nullptr;
}

void GdalTableFunction::Register(DatabaseInstance &db) {

	TableFunctionSet set("ST_Read");
	TableFunction scan({LogicalType::VARCHAR}, GdalTableFunction::Scan, GdalTableFunction::Bind,
	                   GdalTableFunction::InitGlobal, GdalTableFunction::InitLocal);

	scan.cardinality = GdalTableFunction::Cardinality;
	scan.get_batch_index = ArrowTableFunction::ArrowGetBatchIndex;

	scan.projection_pushdown = true;
	scan.filter_pushdown = true;

	scan.named_parameters["open_options"] = LogicalType::LIST(LogicalType::VARCHAR);
	scan.named_parameters["allowed_drivers"] = LogicalType::LIST(LogicalType::VARCHAR);
	scan.named_parameters["sibling_files"] = LogicalType::LIST(LogicalType::VARCHAR);
	scan.named_parameters["spatial_filter_box"] = core::GeoTypes::BOX_2D();
	scan.named_parameters["spatial_filter"] = core::GeoTypes::WKB_BLOB();
	scan.named_parameters["layer"] = LogicalType::VARCHAR;
	scan.named_parameters["sequential_layer_scan"] = LogicalType::BOOLEAN;
	scan.named_parameters["max_batch_size"] = LogicalType::INTEGER;
	scan.named_parameters["keep_wkb"] = LogicalType::BOOLEAN;
	set.AddFunction(scan);

	ExtensionUtil::RegisterFunction(db, set);

	// Replacement scan
	auto &config = DBConfig::GetConfig(db);
	config.replacement_scans.emplace_back(GdalTableFunction::ReplacementScan);
}

} // namespace gdal

} // namespace spatial