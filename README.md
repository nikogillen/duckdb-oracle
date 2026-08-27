# duckdb-oracle

Query, join and modify **Oracle** databases directly from **DuckDB** — like
`duckdb-postgres`, but for Oracle (via [ODPI-C](https://oracle.github.io/odpi/)).

[![Latest release](https://img.shields.io/github/v/release/nikogillen/duckdb-oracle?sort=semver)](../../releases)
[![DuckDB](https://img.shields.io/badge/DuckDB-1.4%20LTS%20%7C%201.5.x-yellow)](https://duckdb.org)
[![License](https://img.shields.io/github/license/nikogillen/duckdb-oracle)](LICENSE)

```sql
LOAD oracle;
ATTACH 'user/password@host:1521/service' AS ora (TYPE oracle);
SELECT * FROM ora.hr.employees LIMIT 10;
```

---

## Contents

- [Features](#features) · [Limitations](#not-supported--limitations)
- [Prerequisites](#prerequisites) · [Instant Client setup](#set-up-the-oracle-instant-client) · [Compatibility](#compatibility-matrix)
- [Installation](#installation) · [Authentication](#authentication)
- [**Working with Oracle in DuckDB**](#working-with-oracle-in-duckdb) · [Examples](#more-examples)
- [Data types](#data-type-mapping) · [Options](#extension-options)
- [Build from source](#building-from-source) · [Troubleshooting](#troubleshooting) · [How it works](#how-it-works) · [Security](#security) · [Credits & license](#credits--license)

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
- 🗺️ **Spatial**: `SDO_GEOMETRY` → DuckDB `GEOMETRY`, SRID preserved as CRS.
- 🔑 **Oracle Wallet / tnsnames.ora** via `config_dir` (TNS_ADMIN).
- ⚡ Fast bulk inserts (ODPI-C array binding), **parallel reads of partitioned
  tables**, connection pooling, prefetch tuning.

## Not supported / limitations

- **Read-focused for advanced types:** `JSON`, `VECTOR` and `SDO_GEOMETRY` are
  read-only; writing them back is not supported.
- **Vectors:** only *dense* `FLOAT32` / `FLOAT64` / `INT8` vectors. Sparse and
  `BINARY`-format vectors are not decoded.
- **Geometries** are converted through WKT, so Oracle types WKT cannot express
  (e.g. circular arcs) are not supported.
- **Mapped to `VARCHAR`** (not a native type): `INTERVAL YEAR TO MONTH`, `XMLTYPE`,
  `ROWID`/`UROWID`, and any unrecognized type.
- **Identifier case:** Oracle objects created with quoted **lower/mixed-case**
  names are not addressable — names are matched case-insensitively against
  standard (upper-case) Oracle identifiers.
- **No WebAssembly build.**

## Prerequisites

| Requirement | Notes |
|-------------|-------|
| **DuckDB** | 1.4 LTS or 1.5.x (match the extension binary to your DuckDB version) |
| **Oracle Instant Client** | **19c or newer** (Basic or Basic Light), on `PATH` / `LD_LIBRARY_PATH` / `DYLD_LIBRARY_PATH` |
| **Oracle server** | **19c and later** for core features (older servers reachable by a 19c+ client work too); **21c+** adds `JSON`; **23ai** adds `VECTOR` / `BOOLEAN` |

## Set up the Oracle Instant Client

The extension loads Oracle's client library (`libclntsh` / `oci.dll`) at runtime,
so it must be on your library search path **before** you start DuckDB. Download
**Basic** or **Basic Light** for your OS and architecture from
[Oracle Instant Client downloads](https://www.oracle.com/database/technologies/instant-client/downloads.html)
(19c or newer), then follow the steps for your platform.

<details open>
<summary><b>Linux</b></summary>

```bash
# 1. unzip somewhere, e.g. /opt/oracle
unzip instantclient-basiclite-linux.x64-*.zip -d /opt/oracle
# 2. put it on the library path (this shell)
export LD_LIBRARY_PATH=/opt/oracle/instantclient_23_9:$LD_LIBRARY_PATH
# 3. libaio is required; on Ubuntu 24.04 also symlink the old soname:
sudo apt-get install -y libaio1t64
sudo ln -sf /usr/lib/x86_64-linux-gnu/libaio.so.1t64 /usr/lib/x86_64-linux-gnu/libaio.so.1
```

For a permanent install, add the dir to `/etc/ld.so.conf.d/oracle.conf` and run
`sudo ldconfig`. arm64 clients: use the `linux.arm64` package.
</details>

<details>
<summary><b>macOS</b></summary>

```bash
# 1. unpack the DMG (use arm64 on Apple Silicon, x64 on Intel)
hdiutil attach instantclient-basiclite-macos.arm64-*.dmg
mkdir -p ~/lib && cp -R /Volumes/instantclient-*/ ~/lib/instantclient
# 2. remove the Gatekeeper quarantine flag, or macOS blocks the .dylib files
xattr -dr com.apple.quarantine ~/lib/instantclient
# 3. put it on the library path (this shell)
export DYLD_LIBRARY_PATH=~/lib/instantclient:$DYLD_LIBRARY_PATH
```

Tip: `~/lib` is a good spot because macOS strips `DYLD_*` variables for binaries
in protected locations. Then start `duckdb -unsigned` from the same shell.
</details>

<details>
<summary><b>Windows</b></summary>

```bat
:: 1. unzip somewhere, e.g. C:\oracle\instantclient_23_9
:: 2. put it on PATH (this session)
set PATH=C:\oracle\instantclient_23_9;%PATH%
:: 3. start DuckDB from the same prompt
duckdb.exe -unsigned
```

For a permanent install, add the directory to the system `PATH` (Environment
Variables). The Instant Client also needs the **Microsoft Visual C++
Redistributable** installed.
</details>

> The version folder name (`instantclient_23_9`, …) depends on the package you
> downloaded — adjust the paths accordingly.

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

### Oracle Wallet / tnsnames.ora

Point `config_dir` at a directory holding `tnsnames.ora`, `sqlnet.ora` and/or a
wallet — it is applied as `TNS_ADMIN` for the connection (aliases: `tns_admin`,
`wallet_path`):

```sql
-- TNS alias resolved from <dir>/tnsnames.ora
ATTACH 'user/password@MYDB' AS ora (TYPE oracle, config_dir '/path/to/network/admin');

-- Wallet holding the credentials (external authentication, no user/password)
ATTACH '@MYDB_HIGH' AS ora (TYPE oracle, config_dir '/path/to/wallet');
```

It also works on a secret, so the whole connection lives in one place:

```sql
CREATE SECRET adb (TYPE oracle, connectString 'MYDB_HIGH', config_dir '/path/to/wallet');
ATTACH '' AS ora (TYPE oracle, SECRET adb);
```

Or pass the connect string directly (Oracle EZConnect syntax):

```
user/password@host/service
user/password@host:1521/service
user/password@MYDB                       -- TNS alias (tnsnames.ora / ORACLE_HOME)
user/password@//host:1521/service?connect_timeout=10
```

## Working with Oracle in DuckDB

A typical session, start to finish.

**1. Start DuckDB and load the extension**

```bash
duckdb -unsigned
```
```sql
LOAD '/path/to/oracle.duckdb_extension';
```

**2. Attach the database** (whole DB, or a single schema)

```sql
ATTACH '' AS ora (TYPE oracle, SECRET ora);
-- just one schema:
ATTACH '' AS ora (TYPE oracle, SECRET ora, SCHEMA 'HR');
```

**3. Explore it** like any DuckDB catalog

```sql
SHOW ALL TABLES;                       -- everything visible under `ora`
DESCRIBE ora.hr.employees;             -- columns and types
SELECT * FROM ora.hr.employees LIMIT 10;
```

**4. Query** — filters and projections are pushed down to Oracle

```sql
SELECT department_id, count(*), avg(salary)
FROM ora.hr.employees
WHERE hire_date >= DATE '2020-01-01'
GROUP BY department_id;
```

**5. Combine Oracle with local files** (CSV, Parquet, other databases)

```sql
-- enrich an Oracle table with a local Parquet file
SELECT e.employee_id, e.last_name, r.rating
FROM ora.hr.employees e
JOIN read_parquet('reviews.parquet') r USING (employee_id);
```

**6. Move data in or out** (ETL)

```sql
-- Oracle → local DuckDB table (fast local copy for analytics)
CREATE TABLE emp_local AS SELECT * FROM ora.hr.employees;

-- Oracle → Parquet on disk
COPY (SELECT * FROM ora.hr.employees) TO 'employees.parquet' (FORMAT parquet);

-- local/other data → Oracle
INSERT INTO ora.hr.employees SELECT * FROM emp_local WHERE employee_id > 1000;
```

**7. Modify Oracle data** (write-back uses `ROWID`)

```sql
UPDATE ora.hr.employees SET salary = salary * 1.1 WHERE department_id = 10;
DELETE FROM ora.hr.employees WHERE employee_id = 100;
```

**8. Detach when done**

```sql
DETACH ora;
```

## More examples

```sql
-- Cross-database join (DuckDB-local table ⋈ Oracle table)
SELECT e.last_name, b.bonus
FROM ora.hr.employees e
JOIN bonuses b ON b.employee_id = e.employee_id;

-- Create a table and an index in Oracle from DuckDB
CREATE TABLE ora.hr.audit (id INTEGER, note VARCHAR, at TIMESTAMP);
CREATE INDEX audit_at ON ora.hr.audit (at);

-- Oracle 23ai JSON → DuckDB JSON (DuckDB's JSON functions work directly)
SELECT json_extract_string(doc, '$.name') AS name
FROM ora.app.documents
WHERE json_extract_string(doc, '$.active') = 'true';

-- Oracle 23ai VECTOR → LIST(FLOAT)
SELECT id, len(embedding) AS dims, embedding[1] AS first_dim
FROM ora.app.items;
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
| `SDO_GEOMETRY` (Spatial) | `GEOMETRY` (DuckDB 1.5+, with SRID as CRS); WKT `VARCHAR` on 1.4 LTS |
| `INTERVAL DAY TO SECOND` | `INTERVAL` |
| `INTERVAL YEAR TO MONTH`, `XMLTYPE`, `ROWID`, … | `VARCHAR` |

## Extension options

| Option | Default | Description |
|--------|---------|-------------|
| `ora_connection_limit` | `64` | Max concurrent Oracle connections in the pool |
| `ora_connection_cache` | `true` | Keep connections alive between queries |
| `ora_experimental_filter_pushdown` | `true` | Push `WHERE` filters into Oracle |
| `ora_parallel_scan` | `true` | Read partitioned tables with one connection per partition |
| `ora_debug_show_queries` | `false` | Print every Oracle SQL statement to stdout ⚠️ prints data |

```sql
SET ora_debug_show_queries = true;
```

### A note on `ora_parallel_scan` and Oracle Partitioning

Oracle **Partitioning is a separately licensed option** of Enterprise Edition —
it is not part of Standard Edition 2. The extension therefore **never creates
partitions**: its `CREATE TABLE` emits columns and constraints only, and there is
no way to make it produce a `PARTITION BY` clause.

It only ever *reads* a split that already exists. Before a scan it asks the data
dictionary which partitions the table has; if the table is not partitioned — or
the database has no Partitioning option, or the session may not read
`ALL_TAB_PARTITIONS` — the answer is empty and the table is read as a whole,
exactly as before. Concretely:

```sql
-- non-partitioned table
SELECT /*+ ALL_ROWS NO_RESULT_CACHE */ "ID" FROM "DEMO"."T_EMP"
-- partitioned table: one statement per partition, run concurrently
SELECT /*+ ALL_ROWS NO_RESULT_CACHE */ "ID" FROM "DEMO"."T_PART" PARTITION ("P_2024_Q1")
```

So the feature speeds up databases that are partitioned already and is a silent
no-op everywhere else. `SET ora_parallel_scan = false` disables the partitioned
path entirely if you would rather not have it used at all.

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

## Credits & license

This extension is a port of [duckdb-postgres](https://github.com/duckdb/duckdb-postgres)
to Oracle and builds against [DuckDB](https://github.com/duckdb/duckdb) — both MIT
licensed (© Stichting DuckDB Foundation). Oracle connectivity uses
[ODPI-C](https://oracle.github.io/odpi/) (© Oracle, UPL 1.0 / Apache 2.0).

This project is released under the MIT License — see [LICENSE](LICENSE). The
license texts of bundled third-party code (ODPI-C, DuckDB) are in
[THIRD_PARTY_LICENSES](THIRD_PARTY_LICENSES) and ship with every release.
