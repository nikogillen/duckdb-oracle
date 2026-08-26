# Changelog

## Unreleased

### Features

- **Oracle Spatial**: `SDO_GEOMETRY` columns are now read as DuckDB `GEOMETRY`
  (DuckDB 1.5+), with the column's registered SRID exposed as the CRS
  (`GEOMETRY('EPSG:4326')`). Oracle serializes the value server-side via
  `SDO_UTIL.TO_WKTGEOMETRY`. On DuckDB 1.4 LTS, which has no `GEOMETRY` type, the
  geometry is returned as WKT text in a `VARCHAR`. Only SRIDs that Oracle reports
  as non-legacy are labelled as EPSG codes, since Oracle's legacy SRIDs use a
  different numbering.
- **Oracle Wallet / TNS_ADMIN**: new `config_dir` option (aliases: `tns_admin`,
  `wallet_path`) on `ATTACH` and on `CREATE SECRET`, pointing at a directory with
  `tnsnames.ora` / `sqlnet.ora` / wallet files. External authentication (wallet-stored
  credentials, `/@alias`) is enabled automatically when neither user nor password
  is given.

### Performance

- **Bulk inserts now use ODPI-C array binding** (`dpiStmt_executeMany`): one
  round-trip per chunk instead of one `INSERT` statement per row — roughly **10x
  faster** in local measurements (20k rows: 4.9s → 0.5s). This also fixes values
  that the old literal path could not write at all: strings beyond Oracle's
  4000-byte literal limit and BLOBs beyond the `HEXTORAW` limit. Unsupported column
  types transparently fall back to the previous row-by-row path.

### Fixes

- Fix silent precision loss on `DECIMAL` reads: Oracle `NUMBER` values were routed
  through `double` and an intermediate `int64`, corrupting values with more than
  ~15 significant digits (a `DECIMAL(38,10)` could come back as a completely
  different number). Such columns are now fetched as text and parsed exactly.

### Features

- Map Oracle 23ai **VECTOR** columns to DuckDB `LIST(FLOAT)` (FLOAT32/FLOAT64/INT8
  formats), usable with array/VSS operations.
- Map Oracle native **JSON** columns to DuckDB `JSON` (recursive serialization),
  so DuckDB's JSON functions work directly on Oracle JSON.

### Fixes

- Fix scanner crash when querying a table created via the extension in the same
  session (empty `oracle_types`); the entry is now reloaded from the data dictionary.
- Fix CLOB/NCLOB (VARCHAR) and BLOB columns reading back empty; LOB handles are now
  read via `dpiLob_getSize`/`dpiLob_readBytes`.

### Performance & operations

- Replace the per-pool-return `SELECT 1 FROM DUAL` health check with the
  client-side `dpiConn_getIsHealthy` (saves a network round-trip per pooled query).
- Set server-side prefetch to match the fetch array size for smoother scans.
- Tag Oracle sessions with a client identifier/module (visible in `V$SESSION`).

### Dependencies

- ODPI-C is no longer vendored in the repo. It is now fetched at build time by
  CMake, pinned to **v6.0.0** and verified via SHA-256 with `TLS_VERIFY ON`.
  This removes ~260 vendored files and makes future updates a one-line change.
  ODPI-C 6.0 drops support for Oracle Client libraries older than 19c (not a
  concern here). (previously vendored at v5.3.0)

### Security

- Fix SQL injection through unquoted Oracle identifiers: `QuoteIdentifier` now
  quotes and escapes identifiers (reproducing Oracle's unquoted up-casing, so the
  same standard objects still resolve).
- Escape filter-pushdown literals in the default branch instead of concatenating a
  raw `ToString()`.
- Redact the password from Oracle connection error messages.
- Enable OCI threaded mode (`DPI_MODE_CREATE_THREADED`) for pooled connections.
- Read ODPI-C error messages using `messageLength` (avoid C-string over-read).

### Fixes

- Fix connection-pool slot leak when opening a new connection fails.
- Fix macOS/libc++ build error: `OracleTransaction` incomplete type in
  `OracleTransactionManager`'s destructor.

### Build & CI

- Add native macOS arm64 build workflow (self-hosted runner) and a Linux + Oracle
  Free integration workflow (GitHub-hosted).
- Add end-to-end integration tests against Oracle Free (`test/`).

### Distribution

- Stop committing pre-built extension binaries to the repo (removed `extensions/`).
- Add a release pipeline (`.github/workflows/release.yml`) that builds the
  extension for Windows, Linux and macOS against DuckDB **1.4 LTS** and **1.5.x**
  via DuckDB's official reusable distribution workflow, then publishes the binaries
  as GitHub Release assets with a `SHA256SUMS` file and build-provenance
  attestations. README updated with install + verification instructions.

## v1.5.5 (2026-07-24) — DuckDB 1.5.5

Bump to DuckDB **v1.5.5**.

### Dependencies

- [ODPI-C v5.3.0](https://github.com/oracle/odpi) — Oracle Database Programming Interface for C
- [DuckDB v1.5.5](https://github.com/duckdb/duckdb/releases/tag/v1.5.5)

---

## v1.5.4 (2026-06-19) — DuckDB 1.5.4

Bump to DuckDB **v1.5.4**.

### Fixes

- Fix `TIMESTAMP WITH TIME ZONE` type being truncated to plain `TIMESTAMP`
- Fix `num_rows` returning hardcoded 0 for base tables (now reads from `user_tables`)
- Fix `TIMESTAMP_TZ` filter pushdown to emit correct Oracle `AT TIME ZONE 'UTC'` literal

### Dependencies

- [ODPI-C v5.3.0](https://github.com/oracle/odpi) — Oracle Database Programming Interface for C
- [DuckDB v1.5.4](https://github.com/duckdb/duckdb/releases/tag/v1.5.4)

---

## v1.5.3 (2026-05-23) — DuckDB 1.5.3

Bump to DuckDB **v1.5.3**.

### Dependencies

- [ODPI-C v5.3.0](https://github.com/oracle/odpi) — Oracle Database Programming Interface for C
- [DuckDB v1.5.3](https://github.com/duckdb/duckdb/releases/tag/v1.5.3)

---

## v1.4.4 (2026-03-01) — DuckDB 1.4.4 LTS

Initial LTS release of the `oracle` DuckDB extension, built against DuckDB **v1.4.4 LTS**.

### Features

- Connect to Oracle databases via DuckDB's `ATTACH` syntax
- Full Oracle table scanning using ODPI-C v5.3.0
- Filter pushdown for efficient query execution
- Data type conversion between Oracle and DuckDB types
- Secret management for Oracle credentials
- Oracle storage extension interface
- Query logging and debugging support
- Connection pooling

### Supported Platforms

- Linux x86_64
- Linux ARM64
- macOS x86_64
- macOS ARM64 (Apple Silicon)
- Windows x86_64

### Usage

#### Basic connection

```sql
-- Load the extension
LOAD oracle;

-- Attach an Oracle database
ATTACH 'user/password@host:1521/service' AS oracle_db (TYPE oracle);

-- Query Oracle tables and views
SELECT * FROM oracle_db.my_table LIMIT 10;
```

#### Using secrets (recommended)

Secrets keep credentials out of connection strings and query history.

```sql
-- Create a named secret
CREATE SECRET my_oracle_secret (
    TYPE oracle,
    user 'scott',
    password 'tiger',
    connectString '//myhost:1521/MYSERVICE'
);

-- Attach using the secret
ATTACH '' AS oracle_db (TYPE oracle, SECRET my_oracle_secret);
```

#### Default secret (auto-applied)

A secret named `__default_oracle` is picked up automatically for any Oracle
`ATTACH` that does not specify a `SECRET` option:

```sql
CREATE SECRET __default_oracle (
    TYPE oracle,
    user 'scott',
    password 'tiger',
    connectString '//myhost:1521/MYSERVICE'
);

-- No SECRET= needed
ATTACH '' AS oracle_db (TYPE oracle);
```

#### Persistent secrets

```sql
-- Survives DuckDB restarts
CREATE PERSISTENT SECRET my_oracle_secret (
    TYPE oracle,
    user 'scott',
    password 'tiger',
    connectString '//myhost:1521/MYSERVICE'
);
```

### Dependencies

- [ODPI-C v5.3.0](https://github.com/oracle/odpi) — Oracle Database Programming Interface for C
- [DuckDB v1.4.4](https://github.com/duckdb/duckdb/releases/tag/v1.4.4) — LTS release
