# Extension from this repo
duckdb_extension_load(spatial
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
    LOAD_TESTS
    TEST_PATH test/sql
    INCLUDE_DIR spatial/include
    LINKED_LIBS "../../deps/local/lib/lib*.a"
)
