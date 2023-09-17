#pragma once

#include "spatial/common.hpp"

#include "proj.h"

namespace spatial {

namespace proj {

struct ProjModule {
public:
	static PJ_CONTEXT *GetThreadProjContext();
	static void Register(DatabaseInstance &instance);
};

} // namespace proj

} // namespace spatial