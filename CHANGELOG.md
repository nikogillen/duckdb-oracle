# Changelog

## v2.1.0 (2026-08-27) — DuckDB 1.4 LTS + 1.5.x

Spatial support, wallet authentication, an order-of-magnitude faster bulk insert,
parallel reads of partitioned tables — and the fix that filters were never
actually reaching Oracle.

### Features

- **Oracle Spatial**: `SDO_GEOMETRY` columns are read as DuckDB `GEOMETRY`
  (DuckDB 1.5+), with the column's registered SRID exposed as the CRS
  (`GEOMETRY('EPSG:4326')`). Oracle serializes the value server-side via
  `SDO_UTIL.TO_WKTGEOMETRY`. On DuckDB 1.4 LTS, which has no `GEOMETRY` type, the
  geometry is returned as WKT text in a `VARCHAR`. Only SRIDs that Oracle reports
  as non-legacy are labelled as EPSG codes, since Oracle's legacy SRIDs use a
  different numbering.
- **Oracle Wallet / TNS_ADMIN**: new `config_dir` option (aliases: `tns_admin`,
  `wallet_path`) on `ATTACH` and on `CREATE SECRET`, pointing at a directory with
  `tnsnames.ora` / `sqlnet.ora` / wallet files. External authentication
  (wallet-stored credentials, `/@alias`) is enabled automatically when neither
  user nor password is given.

### Performance

- **Bulk inserts now use ODPI-C array binding** (`dpiStmt_executeMany`): one
  round-trip per chunk instead of one `INSERT` statement per row — roughly **10x
  faster** in local measurements (20k rows: 4.9s → 0.5s). This also fixes values
  the old literal path could not write at all: strings beyond Oracle's 4000-byte
  literal limit and BLOBs beyond the `HEXTORAW` limit. Unsupported column types
  transparently fall back to the previous row-by-row path.
- **Read partitioned tables in parallel**: each partition becomes a work unit on
  its own Oracle connection, so a partitioned table is no longer scanned by a
  single thread. Partitions are disjoint and cover the table, so the result is
  identical to a serial read and no extra privileges are needed. Where the session
  may read the current SCN, all work units are pinned to one snapshot
  (`AS OF SCN`); otherwise the scan still runs, just without that guarantee.
  Turn it off with `SET ora_parallel_scan = false`.
  This only *reads* a split that already exists: the extension never creates
  partitions (its `CREATE TABLE` emits no `PARTITION BY`), so on editions without
  Oracle's separately licensed Partitioning option it is a silent no-op.
- Report Oracle's row count to DuckDB's optimizer, so it can pick sensible join
  orders instead of guessing. Tables without optimizer statistics now report
  *unknown* rather than "0 rows", which previously made them look empty.
- Bound the fetch buffer by memory instead of a fixed row count. ODPI-C allocates
  `rows x column width`, so 2000 rows of a `VARCHAR2(4000)` column reserved ~31 MB
  for a single column (hundreds of MB on wide tables); the buffer is now capped.
- Add two defensive optimizer hints (`ALL_ROWS`, `NO_RESULT_CACHE`) to generated
  scans, protecting against sites that set `FIRST_ROWS` in a logon trigger or run
  with `RESULT_CACHE_MODE=FORCE`.

### Fixes

- **Filters are now actually pushed into Oracle when a database is attached.**
  `ATTACH ... (TYPE oracle)` used the scan function *without* the filter-pushdown
  capability, so DuckDB never handed it a `WHERE` clause: `WHERE id = 5` fetched
  the entire table and filtered locally. Only the `oracle_scan_pushdown` table
  function and `oracle_attach(..., filter_pushdown=true)` ever pushed anything.
  The `ora_experimental_filter_pushdown` setting was documented but read nowhere;
  it now selects the capability as described.
- Never push a filter on a column Oracle cannot compare: a `CLOB`/`NCLOB`/`LONG`
  or `BLOB` raises `ORA-22848: cannot use CLOB type as comparison key`, and `JSON`,
  `VECTOR` and `SDO_GEOMETRY` are read through a server-side serialization whose
  result is not comparable to the stored value. Such predicates stay in DuckDB.
  This matters for ordinary tables: a DuckDB `VARCHAR` column created through this
  extension *is* an Oracle `CLOB`.
- Do not drop untranslatable branches of an `OR` filter. Unlike `AND` — where
  dropping a conjunct only widens the predicate and DuckDB filters the remainder —
  dropping a disjunct narrows it, and DuckDB removes a filter it considers fully
  pushed down, so the missing rows would never come back. The whole `OR` is now
  left to DuckDB unless every branch translates.
- Read Oracle `JSON` columns through `JSON_SERIALIZE` instead of the native JSON
  fetch. The native type requires an Oracle client of 21c or newer; with a 19c
  client — the documented minimum — the column arrived as raw OSON in a BLOB and
  never produced usable JSON. This also covers JSON collection tables and duality
  views, which expose their content the same way.
- Fix silent precision loss on `DECIMAL` reads: Oracle `NUMBER` values were routed
  through `double` and an intermediate `int64`, corrupting values with more than
  ~15 significant digits (a `DECIMAL(38,10)` could come back as a completely
  different number). Such columns are now fetched as text and parsed exactly.

### Build & CI

- Cut the Linux integration pipeline from ~44 minutes of machine time per commit
  to a few minutes: the workflow no longer runs twice per commit (`push` on
  feature branches *and* `pull_request` fired for the same commit, and a
  concurrency group cannot collapse those — their refs differ), DuckDB is no
  longer recompiled from scratch (ccache, persisted with `actions/cache`; measured
  99.6% hit rate, 20 min → under 4 min), and the unused `tpch`/`tpcds` extensions
  are no longer built.
- Extend the integration suite: partitioned-table reads, 15 filter-pushdown
  predicates checked against Oracle's own answers, and guards for the LOB, JSON,
  VECTOR and BLOB cases.

### Dependencies

- [ODPI-C v6.0.0](https://github.com/oracle/odpi) — fetched at build time, pinned
  and SHA-256 verified
- [DuckDB v1.4.5 (LTS)](https://github.com/duckdb/duckdb/releases/tag/v1.4.5) and
  [DuckDB v1.5.5](https://github.com/duckdb/duckdb/releases/tag/v1.5.5)

---

## v2.0.1 (2026-08-26) — DuckDB 1.4 LTS + 1.5.x

First release of the 2.x line: ODPI-C 6.0, a security pass over the query
generation, and an attested multi-platform release pipeline. (This entry was
written retroactively — the release was cut without a changelog section.)

### Features

- Map Oracle native **JSON** columns to DuckDB `JSON`, so DuckDB's JSON functions
  work directly on Oracle JSON.
- Map Oracle 23ai **VECTOR** columns to DuckDB `LIST(FLOAT)` (FLOAT32/FLOAT64/INT8
  formats), usable with array/VSS operations.

### Security

- Fix SQL injection through unquoted Oracle identifiers: `QuoteIdentifier` now
  quotes and escapes identifiers (reproducing Oracle's unquoted up-casing, so the
  same standard objects still resolve).
- Escape filter-pushdown literals in the default branch instead of concatenating a
  raw `ToString()`.
- Redact the password from Oracle connection error messages.
- Enable OCI threaded mode (`DPI_MODE_CREATE_THREADED`) for pooled connections.
- Read ODPI-C error messages using `messageLength` (avoid C-string over-read).

### Performance & operations

- Replace the per-pool-return `SELECT 1 FROM DUAL` health check with the
  client-side `dpiConn_getIsHealthy` (saves a network round-trip per pooled query).
- Set server-side prefetch to match the fetch array size for smoother scans.
- Tag Oracle sessions with a client identifier/module (visible in `V$SESSION`).

### Fixes

- Fix scanner crash when querying a table created via the extension in the same
  session (empty `oracle_types`); the entry is now reloaded from the data dictionary.
- Fix CLOB/NCLOB (VARCHAR) and BLOB columns reading back empty; LOB handles are now
  read via `dpiLob_getSize`/`dpiLob_readBytes`.
- Fix connection-pool slot leak when opening a new connection fails.
- Fix macOS/libc++ build error: `OracleTransaction` incomplete type in
  `OracleTransactionManager`'s destructor.

### Dependencies

- ODPI-C is no longer vendored in the repo. It is now fetched at build time by
  CMake, pinned to **v6.0.0** and verified via SHA-256 with `TLS_VERIFY ON`.
  This removes ~260 vendored files and makes future updates a one-line change.
  ODPI-C 6.0 drops support for Oracle Client libraries older than 19c (not a
  concern here). (previously vendored at v5.3.0)

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
  attestations.
- Add `THIRD_PARTY_LICENSES` and ship the licence notices alongside the release
  assets.

---

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
