add_subdirectory(main)
add_subdirectory(proj)
add_subdirectory(gdal)
if(SPATIAL_USE_GEOS)
    add_subdirectory(geos)
endif()
add_subdirectory(osm)
add_subdirectory(shapefile)

set(EXTENSION_SOURCES
    ${EXTENSION_SOURCES}
PARENT_SCOPE)