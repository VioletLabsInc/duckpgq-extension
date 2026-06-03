PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=duckpgq
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile

# Match CI: clone fork DuckDB and checkout the release ref before building.
ci-duckdb-version:
	cd duckdb && git fetch origin --tags && git checkout v1.5.3

.PHONY: ci-check
ci-check: ci-duckdb-version format-check release test_release
