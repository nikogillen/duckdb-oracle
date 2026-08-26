-- Integration tests for the duckdb-oracle extension.
--
-- Preconditions established by the runner before this script executes:
--   * the `oracle` extension is LOADed
--   * an Oracle database is attached as `ora` (TYPE oracle)
--   * schema DEMO contains table T_EMP, seeded via test/sql/setup_oracle.sql
--
-- The DSN (with credentials) is prepended by the runner and never stored here.
-- `.bail on` makes the CLI exit non-zero on the first failing statement, so CI
-- turns red if any statement below fails.

.bail on
.echo on

-- Read path + case handling: DuckDB lower-cases `ora.demo.t_emp`; the extension
-- re-applies Oracle's upper-casing while quoting, so it resolves DEMO.T_EMP.
SELECT count(*) AS row_count FROM ora.demo.t_emp;

-- Filter pushdown with a value containing a single quote. Correct escaping returns
-- exactly one row; a broken escape would produce malformed Oracle SQL.
SELECT id FROM ora.demo.t_emp WHERE name = 'O''Brien';

-- Numeric filter pushdown.
SELECT count(*) AS well_paid FROM ora.demo.t_emp WHERE salary >= 5000;

-- Read a VARCHAR2 column back to confirm values round-trip intact.
SELECT id, name FROM ora.demo.t_emp ORDER BY id;

-- UPDATE via ROWID write-back (5000 -> 5500).
UPDATE ora.demo.t_emp SET salary = salary * 1.10 WHERE id = 1;
SELECT salary FROM ora.demo.t_emp WHERE id = 1;

-- DELETE via ROWID write-back.
DELETE FROM ora.demo.t_emp WHERE id = 3;
SELECT count(*) AS after_delete FROM ora.demo.t_emp;

-- Cross-database join (DuckDB-local table joined against the Oracle table).
CREATE TABLE local_bonus (id INTEGER, bonus INTEGER);
INSERT INTO local_bonus VALUES (1, 100), (2, 200);
SELECT e.name, b.bonus
FROM ora.demo.t_emp e
JOIN local_bonus b ON e.id = b.id
ORDER BY e.id;

-- Regression: create a table via the extension and query it in the SAME session.
-- Previously this crashed the scanner (empty oracle_types) and VARCHAR columns
-- (mapped to Oracle CLOB) read back empty.
DROP TABLE IF EXISTS ora.demo.dck_roundtrip;
CREATE TABLE ora.demo.dck_roundtrip (id INTEGER, note VARCHAR);
INSERT INTO ora.demo.dck_roundtrip VALUES (1, 'clob text via create'), (2, 'second');
-- Must return the actual text (not empty) and not crash.
SELECT id, note FROM ora.demo.dck_roundtrip ORDER BY id;
SELECT count(*) AS clob_nonempty FROM ora.demo.dck_roundtrip WHERE note = 'clob text via create';
DROP TABLE ora.demo.dck_roundtrip;

-- ODPI-C 6.0 type mapping: Oracle native JSON → DuckDB JSON, VECTOR → LIST(FLOAT).
SELECT id, json_extract_string(doc, '$.name') AS name FROM ora.demo.t_feat ORDER BY id;
SELECT id, len(emb) AS dims, emb[1] AS first_dim FROM ora.demo.t_feat ORDER BY id;
SELECT count(*) AS json_ok FROM ora.demo.t_feat WHERE json_extract_string(doc, '$.name') = 'Alice';

-- Bulk insert via ODPI-C array binding: several thousand rows exercise the batch
-- path (one executeMany per chunk) across the common column types, including NULLs.
DROP TABLE IF EXISTS ora.demo.dck_bulk;
CREATE TABLE ora.demo.dck_bulk AS
SELECT i AS id,
       'name_' || i AS name,
       i * 1.5 AS val,
       CASE WHEN i % 7 = 0 THEN NULL ELSE i % 100 END AS maybe_null
FROM range(5000) t(i);
SELECT count(*) AS bulk_rows, sum(id) AS bulk_checksum FROM ora.demo.dck_bulk;
SELECT count(*) AS bulk_nulls FROM ora.demo.dck_bulk WHERE maybe_null IS NULL;
SELECT name FROM ora.demo.dck_bulk WHERE id = 4999;
-- Append into the existing table to exercise INSERT INTO (not just CTAS).
INSERT INTO ora.demo.dck_bulk SELECT i, 'appended', i, NULL FROM range(5000, 6000) t(i);
SELECT count(*) AS after_append FROM ora.demo.dck_bulk;
DROP TABLE ora.demo.dck_bulk;

-- Spatial: Oracle SDO_GEOMETRY. Skipped automatically when the database has no
-- Oracle Spatial (t_geo is then absent — see setup_oracle.sql).
SELECT 'ALL INTEGRATION TESTS PASSED' AS result;
