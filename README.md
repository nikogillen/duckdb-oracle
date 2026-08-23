# duckdb-oracle

Query, join and modify **Oracle** databases directly from **DuckDB** — like
`duckdb-postgres`, but for Oracle (via [ODPI-C](https://oracle.github.io/odpi/)).

```sql
LOAD oracle;
ATTACH 'user/password@host:1521/service' AS ora (TYPE oracle);
SELECT * FROM ora.hr.employees LIMIT 10;
```

---

## Features

- 📂 **Attach** an Oracle database (or a single schema) and browse it like a DuckDB catalog.
- 🔎 **Read** any table or view, with **filter & projection pushdown** into Oracle.
- 🔗 **Cross-database joins** between local DuckDB tables and remote Oracle tables.
- ✍️ **Write**: `INSERT`, `UPDATE`, `DELETE`, `MERGE` (row identity via Oracle `ROWID`).
- 🛠️ **DDL**: `CREATE` / `DROP` / `ALTER TABLE`, `CREATE` / `DROP INDEX`.
- 🔐 **Secrets** for credentials (`CREATE SECRET … TYPE oracle`).
- 🧬 **Rich types**: `NUMBER`, `TIMESTAMP`/`TIMESTAMP WITH TIME ZONE`, `CLOB`/`BLOB`,
  `BOOLEAN`, plus Oracle **23ai** `JSON` → DuckDB `JSON` and `VECTOR` → `LIST(FLOAT)`.
- ⚡ Connection pooling, prefetch tuning, client-side health checks.

## Not supported / limitations

- **Read-focused for advanced types:** `JSON` and `VECTOR` are read-only; writing
  them back is not supported.
- **Vectors:** only *dense* `FLOAT32` / `FLOAT64` / `INT8` vectors. Sparse and
  `BINARY`-format vectors are not decoded.
- **Mapped to `VARCHAR`** (not a native type): `INTERVAL YEAR TO MONTH`, `XMLTYPE`,
  `SDO_GEOMETRY` (spatial), `ROWID`/`UROWID`, and any unrecognized type.
- **Identifier case:** Oracle objects created with quoted **lower/mixed-case**
  names are not addressable — names are matched case-insensitively against
  standard (upper-case) Oracle identifiers.
- **No WebAssembly build.**

## Prerequisites

| Requirement | Notes |
|-------------|-------|
| **DuckDB** | 1.4 LTS or 1.5.x (match the extension binary to your DuckDB version) |
| **Oracle Instant Client** | **19c or newer** (Basic or Basic Light), on `PATH` / `LD_LIBRARY_PATH` / `DYLD_LIBRARY_PATH` |
| **Oracle server** | Any supported version for core features; **21c/23ai** for `JSON`, **23ai** for `VECTOR`/`BOOLEAN` |

## Compatibility matrix

Pre-built binaries are published for every combination below:

| Platform | DuckDB 1.4 LTS | DuckDB 1.5.x |
|----------|:--------------:|:------------:|
| Linux x86_64 | ✅ | ✅ |
| Linux arm64 | ✅ | ✅ |
| macOS arm64 (Apple Silicon) | ✅ | ✅ |
| macOS x86_64 | ✅ | ✅ |
| Windows x86_64 | ✅ | ✅ |

## Installation

1. Download the `oracle.duckdb_extension` for your platform and DuckDB version
   from the [Releases page](../../releases).
2. (Recommended) verify it:
   ```bash
   sha256sum -c SHA256SUMS --ignore-missing
   gh attestation verify oracle.duckdb_extension --repo nikogillen/duckdb-oracle
   ```
3. Load it (unsigned extensions must be allowed):
   ```bash
   duckdb -unsigned
   ```
   ```sql
   LOAD '/path/to/oracle.duckdb_extension';
   ```

Binaries are built and attested by CI, not committed to this repo.

## Authentication

Prefer a **secret** — it keeps the password out of your SQL, query history and
the DuckDB catalog (and it is redacted in `duckdb_secrets()`):

```sql
CREATE SECRET ora (
    TYPE oracle,
    user 'scott',
    password 'tiger',
    connectString '//host:1521/service'
);
ATTACH '' AS ora (TYPE oracle, SECRET ora);
```

A secret named `__default_oracle` is applied automatically to any Oracle `ATTACH`
without a `SECRET`. Use `CREATE PERSISTENT SECRET …` to keep it across restarts.

Or pass the connect string directly (Oracle EZConnect syntax):

```
user/password@host/service
user/password@host:1521/service
user/password@MYDB                       -- TNS alias (tnsnames.ora / ORACLE_HOME)
user/password@//host:1521/service?connect_timeout=10
```

## Examples

```sql
ATTACH '' AS ora (TYPE oracle, SECRET ora);
-- Attach only one schema:  ATTACH '' AS ora (TYPE oracle, SECRET ora, SCHEMA 'HR');

-- Read + filter (pushed down to Oracle)
SELECT employee_id, last_name FROM ora.hr.employees WHERE department_id = 10;

-- Join local DuckDB data against Oracle
SELECT e.last_name, b.bonus
FROM ora.hr.employees e
JOIN local_bonus b ON b.employee_id = e.employee_id;

-- Write back (UPDATE/DELETE use ROWID under the hood)
UPDATE ora.hr.employees SET salary = salary * 1.1 WHERE department_id = 10;
DELETE FROM ora.hr.employees WHERE employee_id = 100;

-- Oracle 23ai JSON → DuckDB JSON functions work directly
SELECT json_extract_string(doc, '$.name') AS name FROM ora.app.documents;

-- Oracle 23ai VECTOR → LIST(FLOAT)
SELECT id, embedding[1] AS first_dim, len(embedding) AS dims FROM ora.app.items;
```

## Data type mapping

| Oracle | DuckDB |
|--------|--------|
| `NUMBER(p,s)` | `SMALLINT`/`INTEGER`/`BIGINT`/`DECIMAL` by precision; bare `NUMBER` → `DOUBLE` |
| `BINARY_FLOAT` / `BINARY_DOUBLE` | `FLOAT` / `DOUBLE` |
| `VARCHAR2`, `NVARCHAR2`, `CHAR`, `CLOB`, `NCLOB`, `LONG` | `VARCHAR` |
| `RAW`, `LONG RAW`, `BLOB` | `BLOB` |
| `DATE`, `TIMESTAMP` | `TIMESTAMP` |
| `TIMESTAMP WITH TIME ZONE` | `TIMESTAMP WITH TIME ZONE` |
| `BOOLEAN` (23ai) | `BOOLEAN` |
| `JSON` (21c/23ai) | `JSON` |
| `VECTOR` (23ai) | `LIST(FLOAT)` |
| `INTERVAL DAY TO SECOND` | `INTERVAL` |
| `INTERVAL YEAR TO MONTH`, `XMLTYPE`, spatial, `ROWID`, … | `VARCHAR` |

## Extension options

| Option | Default | Description |
|--------|---------|-------------|
| `ora_connection_limit` | `64` | Max concurrent Oracle connections in the pool |
| `ora_connection_cache` | `true` | Keep connections alive between queries |
| `ora_experimental_filter_pushdown` | `true` | Push `WHERE` filters into Oracle |
| `ora_debug_show_queries` | `false` | Print every Oracle SQL statement to stdout ⚠️ prints data |

```sql
SET ora_debug_show_queries = true;
```

## Building from source

`duckdb` and `extension-ci-tools` are git submodules; ODPI-C is fetched by CMake
at configure time (pinned + SHA-256-verified).

```bash
git clone --recurse-submodules https://github.com/nikogillen/duckdb-oracle
cd duckdb-oracle
make release          # or: make debug
```

Output: `build/release/repository/<duckdb_version>/<platform>/oracle.duckdb_extension`.
To target a specific DuckDB line, check the submodule out first:
`( cd duckdb && git checkout v1.5.5 )` (or `v1.4.5`). Requires a C++ toolchain,
CMake, Ninja, Python 3 and network access.

See [CONTRIBUTING.md](CONTRIBUTING.md) for the test workflow.

## Troubleshooting

Enable ODPI-C tracing **before** launching DuckDB:

```bash
export DPI_DEBUG_LEVEL=1   # errors only; higher = more (see note)
duckdb
```

`DPI_DEBUG_LEVEL` is a bitmask: `1` errors, `4` SQL, `8` bind values, `16` all +
function calls, `64` full trace. The trace goes to stderr (`2>odpi.log`).

> ⚠️ Levels `≥ 8` log bind values, SQL text and row data. Only enable in trusted
> environments and protect/delete the log afterwards.

## How it works

- **Schema browser:** `ATTACH` lists table/view names only; columns and
  constraints load lazily on first use and are cached for the session.
- **Writes:** DuckDB computes new values, then the extension writes each row back
  by its Oracle `ROWID`, so expressions like `salary * 1.1` land on the exact row
  they were read from.

## Security

Credentials, unsigned-extension verification and debug-logging notes are covered
in [SECURITY.md](SECURITY.md). Report vulnerabilities privately (see that file).
