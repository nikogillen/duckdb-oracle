# Changelog

## v1.4.4 (2026-03-01) — DuckDB 1.4.4 LTS

Initial LTS release of the `oracle_scanner` DuckDB extension, built against DuckDB **v1.4.4 LTS**.

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

```sql
-- Load the extension
LOAD oracle_scanner;

-- Attach an Oracle database
ATTACH 'user/password@host:1521/service' AS oracle_db (TYPE oracle);

-- Query Oracle tables
SELECT * FROM oracle_db.my_table LIMIT 10;
```

### Dependencies

- [ODPI-C v5.3.0](https://github.com/oracle/odpi) — Oracle Database Programming Interface for C
- [DuckDB v1.4.4](https://github.com/duckdb/duckdb/releases/tag/v1.4.4) — LTS release
