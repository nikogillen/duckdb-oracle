# Tests

End-to-end integration tests for the Oracle extension, run against a real Oracle
database (Oracle Free) in a container.

## Layout

| File | Purpose |
|------|---------|
| `sql/setup_oracle.sql` | Seeds the `T_EMP` test table (run against Oracle as `DEMO`). |
| `sql/integration.sql`  | DuckDB test script: read path, filter pushdown, escaping, UPDATE/DELETE via ROWID, cross-database join. Assumes the `oracle` extension is loaded and a database is attached as `ora`. |
| `run-local-oracle-test.sh` | Orchestrates the whole thing locally (container up → seed → run). |

The connection string (with credentials) is **never** stored in these files — the
runner prepends `LOAD` + `ATTACH` at execution time.

## Running locally

Prerequisites: Docker, a locally built extension + `duckdb` binary (see the repo
README "Building from source"), and an Oracle Instant Client (Basic or Basic Light)
for your platform.

```bash
# Point the runner at your build outputs and Instant Client, then run it.
export EXT_PATH="$PWD/duckdb/build/release/repository/v1.5.5/osx_arm64/oracle.duckdb_extension"
export IC_DIR="/path/to/instantclient"       # dir containing libclntsh.*
./test/run-local-oracle-test.sh
```

The script starts a throwaway `gvenzl/oracle-free:slim` container with disposable
credentials, seeds the data, runs `sql/integration.sql`, and tears the container
down on exit.

## In CI

`.github/workflows/ci-linux-oracle.yml` runs the same tests on a GitHub-hosted
Linux runner with Oracle Free as a service container. Seeding uses the `sqlplus`
that ships inside the Oracle image; the extension test uses the Linux Instant
Client (Basic Light).

## Oracle case handling (why identifiers are upper-cased)

Oracle folds unquoted identifiers to UPPER CASE; DuckDB folds them to lower case.
The extension stores catalog names lower-cased and re-applies Oracle's upper-casing
when it quotes an identifier for generated SQL. That means `ora.demo.t_emp` resolves
to `DEMO.T_EMP`. Case-sensitive objects created with quoted lower/mixed-case names
are not addressable — a pre-existing limitation, unrelated to the quoting fix.
