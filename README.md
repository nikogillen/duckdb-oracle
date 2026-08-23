# duckdb-oracle

A DuckDB extension that attaches Oracle databases, modelled after `duckdb-postgres` but using Oracle via [ODPI-C](https://oracle.github.io/odpi/).

## Requirements

- Oracle Instant Client **19c or newer** (Basic or Basic Light), reachable via
  `PATH` / `LD_LIBRARY_PATH` / `DYLD_LIBRARY_PATH`. ODPI-C 6.0 dropped support for
  older client libraries. The JSON and VECTOR type mappings additionally require an
  Oracle **21c / 23ai** server.
- The `oracle.duckdb_extension` binary for your platform and DuckDB version

## Installation

Pre-built binaries are published as **GitHub Release assets**, built in CI for
Windows, Linux and macOS against both the DuckDB **1.4 LTS** and **1.5.x** lines.
Each release ships a `SHA256SUMS` file and [build provenance attestations][attest],
so you can verify a binary was built by this repo's workflow from this source.

1. Download the `oracle.duckdb_extension` matching your platform and DuckDB
   version from the [Releases page](../../releases).
2. (Recommended) verify it:
   ```bash
   sha256sum -c SHA256SUMS --ignore-missing
   gh attestation verify oracle.duckdb_extension --repo <owner>/duckdb-oracle
   ```
3. Load it in DuckDB (unsigned extensions must be allowed):
   ```sql
   -- start DuckDB with:  duckdb -unsigned
   LOAD '/path/to/oracle.duckdb_extension';
   ```

Binaries are **no longer committed to this repository** — they are produced and
attested by CI (see [`.github/workflows/release.yml`](.github/workflows/release.yml)).

[attest]: https://docs.github.com/actions/security-guides/using-artifact-attestations-to-establish-provenance-for-builds

## Quick start

```sql
-- Load the extension
LOAD 'oracle';

-- Attach an Oracle database (connect string follows Oracle EZConnect syntax)
ATTACH 'user/password@host:1521/service' AS mydb (TYPE oracle);

-- Optional: attach only a specific schema
ATTACH 'user/password@host:1521/service' AS mydb (TYPE oracle, SCHEMA 'HR');

-- Browse schemas and tables
SHOW ALL TABLES;
SELECT * FROM information_schema.tables WHERE table_catalog = 'mydb';

-- Query Oracle tables directly
SELECT * FROM mydb.hr.employees LIMIT 10;

-- Cross-database joins (DuckDB local + Oracle remote)
SELECT e.employee_id, e.last_name, d.department_name
FROM mydb.hr.employees e
JOIN mydb.hr.departments d ON e.department_id = d.department_id;
```

## Connection string formats

```
-- Basic
user/password@host/service
user/password@host:1521/service

-- With TNS alias (requires tnsnames.ora or ORACLE_HOME set)
user/password@MYDB

-- Full EZConnect Plus (Oracle 19c+)
user/password@//host:1521/service?connect_timeout=10
```

## Authentication with secrets (recommended)

Passing `user/password@host` directly to `ATTACH` puts the password into your SQL
text — and thus into query history, logs and the DuckDB catalog. Prefer a DuckDB
**secret**, which keeps credentials out of the connect string (the password is
redacted in `duckdb_secrets()`):

```sql
CREATE SECRET my_oracle (
    TYPE oracle,
    user 'scott',
    password 'tiger',
    connectString '//host:1521/service'
);

-- Attach using the secret (empty connect string)
ATTACH '' AS mydb (TYPE oracle, SECRET my_oracle);
```

A secret named `__default_oracle` is applied automatically to any Oracle `ATTACH`
that does not name a secret. Use `CREATE PERSISTENT SECRET …` to keep it across
restarts.

## Extension options

| Option | Default | Description |
|--------|---------|-------------|
| `ora_connection_limit` | 64 | Maximum concurrent Oracle connections in the pool |
| `ora_connection_cache` | `true` | Keep connections alive between queries |
| `ora_debug_show_queries` | `false` | Print every Oracle SQL statement to stdout |

```sql
-- Show each Oracle query that gets executed
SET ora_debug_show_queries = true;
```

## Debugging with ODPI-C trace logging

For low-level diagnostics (connection issues, protocol errors, bind parameters), enable ODPI-C debug output by setting the environment variable **before** starting DuckDB:

```bat
:: Windows
set DPI_DEBUG_LEVEL=16
duckdb

:: PowerShell
$env:DPI_DEBUG_LEVEL = 16
duckdb
```

```bash
# Linux / macOS
export DPI_DEBUG_LEVEL=16
duckdb
```

`DPI_DEBUG_LEVEL` is a bitmask. Common values:

| Value | What is logged |
|-------|----------------|
| `1` | Errors |
| `4` | SQL statements sent to Oracle |
| `8` | Bind variable values |
| `16` | All of the above + ODPI-C function calls |
| `64` | Full trace including memory allocations |

The trace is written to stderr. Redirect it to a file with `2>odpi.log`.

> **Warning:** levels `≥ 8` log bind values and SQL text, and the trace can
> contain table data and connection details. Only enable it in trusted
> environments, and protect/delete the log afterwards. For plain connection
> diagnostics, `DPI_DEBUG_LEVEL=1` (errors) is usually enough.

## Data type mapping

| Oracle type | DuckDB type |
|-------------|-------------|
| `NUMBER(p,s)` | `SMALLINT` / `INTEGER` / `BIGINT` / `DECIMAL` (by precision/scale); unconstrained `NUMBER` → `DOUBLE` |
| `BINARY_FLOAT` / `BINARY_DOUBLE` | `FLOAT` / `DOUBLE` |
| `VARCHAR2` / `NVARCHAR2` / `CHAR` / `CLOB` / `NCLOB` / `LONG` | `VARCHAR` |
| `RAW` / `LONG RAW` / `BLOB` | `BLOB` |
| `DATE` / `TIMESTAMP` | `TIMESTAMP` |
| `TIMESTAMP WITH TIME ZONE` | `TIMESTAMP WITH TIME ZONE` |
| `BOOLEAN` (23ai) | `BOOLEAN` |
| `JSON` (21c/23ai) | `JSON` |
| `VECTOR` (23ai) | `LIST(FLOAT)` |
| `INTERVAL YEAR TO MONTH`, `XMLTYPE`, `ROWID`, spatial, … | `VARCHAR` |

`JSON` columns are serialized to text, so DuckDB's JSON functions work directly
(`json_extract_string(col, '$.field')`). `VECTOR` columns (FLOAT32/FLOAT64/INT8)
become a `FLOAT` list, usable with array operations and the VSS extension.

## DML — UPDATE and DELETE

`UPDATE` and `DELETE` are supported and work as expected from DuckDB:

```sql
UPDATE mydb.hr.employees SET salary = 5000  WHERE employee_id = 100;
UPDATE mydb.hr.employees SET salary = salary * 1.1 WHERE department_id = 10;
DELETE FROM mydb.hr.employees WHERE employee_id = 100;
```

### How row identity works (ROWID)

DuckDB evaluates the `SET` expressions and `WHERE` filter on its side and then hands each affected row — along with its **computed new values** — to the Oracle extension one row at a time. The extension must therefore identify each individual Oracle row precisely so it can write back the correct value.

The extension uses Oracle's `ROWID` pseudo-column for this:

1. **Scan phase** — the Oracle scan includes `ROWID` alongside the projected columns. Each row's 18-character ROWID string is stored in a per-transaction registry; a BIGINT index is written into DuckDB's internal row-ID slot.
2. **Write phase** — for each row in the result the extension looks up the ROWID string and executes:
   ```sql
   UPDATE schema.table SET col = <value> WHERE ROWID = 'AAABozAAEAAAAGSAAA'
   ```

Using `ROWID` is necessary — not just a convenience — because DuckDB evaluates expressions like `salary * 1.1` itself before calling the extension. At write-back time the extension only receives the already-computed numeric result (e.g. `5500.00`), not the original expression. A WHERE-clause-only approach would reapply the same WHERE to every row in the batch, meaning all rows would get the value that was computed for the *last* row scanned. `ROWID` ensures each computed value reaches exactly the row it was read from, matching what Oracle's own `UPDATE … SET col = col * 1.1` would produce.

> **Note:** ROWID targets the physical row at scan time. If another session moves the row (e.g. via a table reorganisation or `ALTER TABLE … MOVE`) between the scan and the write-back, the update will silently skip that row. For normal OLTP workloads this is not a concern.

## How it works

- **Schema browser** — on first `ATTACH`, a lightweight `all_tables UNION ALL all_views` query enumerates table/view names per schema. No column data is fetched yet, so the UI loads instantly regardless of schema size.
- **Lazy column loading** — the first time a table is referenced in a query, `ReloadEntry` fires two Oracle round-trips (columns + constraints) to fully populate the entry. The result is cached for the session.
- **USER_* privilege optimization** — when querying the schema of the connected user, the extension uses `user_tab_columns`, `user_tables`, etc. instead of `all_*` views. These skip Oracle's privilege-check layer and are significantly faster on large databases.
- **VIEW detection** — views and base tables are distinguished via a `UNION ALL` (inner-join `all_tables` OR inner-join `all_views`). DuckDB catalog entries are tagged with `CatalogType::VIEW_ENTRY` accordingly.

## Building from source

The extension is built with DuckDB's standard extension toolchain. `duckdb` and
`extension-ci-tools` are git submodules; ODPI-C is fetched by CMake at configure
time (pinned and SHA-256-verified), so it is not vendored.

```bash
git clone --recurse-submodules https://github.com/nikogillen/duckdb-oracle
cd duckdb-oracle
make release          # or: make debug
```

The compiled extension is written to
`build/release/repository/<duckdb_version>/<platform>/oracle.duckdb_extension`.

To build against a specific DuckDB line, check the submodule out at that tag
before building:

```bash
( cd duckdb && git checkout v1.5.5 )   # or v1.4.5 for the LTS line
make release
```

Load a locally built extension with `duckdb -unsigned`, then
`LOAD '/abs/path/to/oracle.duckdb_extension';`.

Requires: a C++ toolchain, CMake, Ninja (`GEN=ninja`), Python 3, and network
access for the one-time ODPI-C download.
