set(EXTENSION_SOURCES
    ${EXTENSION_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/spatial_join_logical.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/spatial_join_physical.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/spatial_join_optimizer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/spatial_operator_extension.cpp
    PARENT_SCOPE)