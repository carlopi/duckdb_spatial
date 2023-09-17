#pragma once
#include "spatial/common.hpp"
#include "spatial/core/geometry/cursor.hpp"
#include "spatial/core/geometry/geometry.hpp"

namespace spatial {

namespace core {

struct CoreModule {
public:
	static void Register(DatabaseInstance &instance);
};

} // namespace core

} // namespace spatial