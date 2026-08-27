#!/usr/bin/env bash
#
# Local end-to-end test for the duckdb-oracle extension.
#
# Spins up a throwaway Oracle Free container, seeds test data, then runs the
# integration SQL through a locally built DuckDB with the Oracle extension.
#
# Prerequisites:
#   * Docker (the Oracle Free image supports linux/amd64 and linux/arm64)
#   * a locally built extension + duckdb binary (see README "Building from source")
#   * an Oracle Instant Client (Basic or Basic Light) for your platform
#
# Configure via environment variables (defaults in brackets):
#   DUCKDB_BIN   path to the duckdb binary  [duckdb/build/release/duckdb]
#   EXT_PATH     path to oracle.duckdb_extension
#   IC_DIR       Instant Client directory (added to DYLD/LD_LIBRARY_PATH)
#   ORA_PORT     host port for the container  [1521]
#
# These are throwaway credentials for a container that only lives for this run.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DUCKDB_BIN="${DUCKDB_BIN:-$REPO_ROOT/duckdb/build/release/duckdb}"
EXT_PATH="${EXT_PATH:-$REPO_ROOT/duckdb/build/release/repository/v1.5.5/osx_arm64/oracle.duckdb_extension}"
ORA_PORT="${ORA_PORT:-1521}"
CONTAINER="duckdb-oracle-test"
ORA_PASSWORD="test_syspw"
APP_USER="demo"
APP_PASSWORD="test_demopw"
ORA_DSN="${APP_USER}/${APP_PASSWORD}@127.0.0.1:${ORA_PORT}/FREEPDB1"

cleanup() { docker rm -f "$CONTAINER" >/dev/null 2>&1 || true; }
trap cleanup EXIT

echo "==> Starting Oracle Free container"
docker rm -f "$CONTAINER" >/dev/null 2>&1 || true
docker run -d --name "$CONTAINER" \
  -p "${ORA_PORT}:1521" \
  -e ORACLE_PASSWORD="$ORA_PASSWORD" \
  -e APP_USER="$APP_USER" \
  -e APP_USER_PASSWORD="$APP_PASSWORD" \
  gvenzl/oracle-free:latest >/dev/null

echo "==> Waiting for the database to become healthy"
for _ in $(seq 1 60); do
  if docker logs "$CONTAINER" 2>&1 | grep -q "DATABASE IS READY TO USE"; then
    break
  fi
  sleep 5
done

echo "==> Seeding test data"
docker exec -i "$CONTAINER" sqlplus -S "$ORA_DSN" \
  < "$REPO_ROOT/test/sql/setup_oracle.sql"

echo "==> Running integration tests through DuckDB"
LIBPATH_VAR="LD_LIBRARY_PATH"
[ "$(uname)" = "Darwin" ] && LIBPATH_VAR="DYLD_LIBRARY_PATH"
{
  echo "LOAD '$EXT_PATH';"
  echo "ATTACH '$ORA_DSN' AS ora (TYPE oracle);"
  cat "$REPO_ROOT/test/sql/integration.sql"
} | env "${LIBPATH_VAR}=${IC_DIR:-}" "$DUCKDB_BIN" -unsigned

# Spatial tests need Oracle Spatial; setup_oracle.sql only creates T_GEO when it
# is available, so use that as the switch.
if docker exec -i "$CONTAINER" sqlplus -S "$ORA_DSN" <<'SQL' 2>/dev/null | grep -q "T_GEO"
SET HEADING OFF FEEDBACK OFF
SELECT table_name FROM user_tables WHERE table_name = 'T_GEO';
EXIT
SQL
then
  echo "==> Running spatial tests"
  {
    echo "LOAD '$EXT_PATH';"
    echo "ATTACH '$ORA_DSN' AS ora (TYPE oracle);"
    cat "$REPO_ROOT/test/sql/integration_spatial.sql"
  } | env "${LIBPATH_VAR}=${IC_DIR:-}" "$DUCKDB_BIN" -unsigned
else
  echo "==> Skipping spatial tests (Oracle Spatial not available)"
fi

echo "==> Done"
