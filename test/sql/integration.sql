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

-- Filter pushdown with a value containing a single quote. The predicate is sent to
-- Oracle as WHERE "NAME" = 'O''Brien', so a broken escape produces malformed SQL
-- and this statement fails outright.
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
-- Also a filter-pushdown guard: `note` is a DuckDB VARCHAR, which this extension
-- creates as an Oracle CLOB, and Oracle rejects a LOB as a comparison key
-- (ORA-22848). The predicate must therefore stay in DuckDB instead of being sent.
SELECT count(*) AS clob_nonempty FROM ora.demo.dck_roundtrip WHERE note = 'clob text via create';
DROP TABLE ora.demo.dck_roundtrip;

-- ODPI-C 6.0 type mapping: Oracle native JSON → DuckDB JSON, VECTOR → LIST(FLOAT).
SELECT id, json_extract_string(doc, '$.name') AS name FROM ora.demo.t_feat ORDER BY id;
SELECT id, len(emb) AS dims, emb[1] AS first_dim FROM ora.demo.t_feat ORDER BY id;
SELECT count(*) AS json_ok FROM ora.demo.t_feat WHERE json_extract_string(doc, '$.name') = 'Alice';
-- Filter-pushdown guards for the wrapped types: JSON is read through JSON_SERIALIZE
-- and VECTOR through vector decoding, so the stored value is not comparable to the
-- serialized form DuckDB filters on. These predicates must stay in DuckDB.
SELECT count(*) AS json_filter FROM ora.demo.t_feat WHERE doc IS NOT NULL;
SELECT count(*) AS vec_filter  FROM ora.demo.t_feat WHERE emb IS NOT NULL;

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

-- Values the previous literal-based insert could not write at all: strings past
-- Oracle's 4000-byte literal limit (also exercising the dynamic bind path above
-- 8 KB) and blobs past the HEXTORAW limit.
DROP TABLE IF EXISTS ora.demo.dck_big;
CREATE TABLE ora.demo.dck_big AS
SELECT 1 AS id, repeat('x', 5000) AS s, repeat('a', 50000)::BLOB AS payload
UNION ALL
SELECT 2, repeat('y', 40000), NULL;
SELECT id, length(s) AS text_len, octet_length(payload) AS blob_len
FROM ora.demo.dck_big ORDER BY id;
-- Filter-pushdown guard: BLOB is a LOB too, so a predicate on it must not be sent.
SELECT count(*) AS blob_filter FROM ora.demo.dck_big WHERE payload IS NULL;
DROP TABLE ora.demo.dck_big;

-- Remaining bind types round-trip (decimal precision, dates, timestamps, unicode).
DROP TABLE IF EXISTS ora.demo.dck_types;
CREATE TABLE ora.demo.dck_types AS
SELECT 1 AS id,
       12345678901234567890.1234567890::DECIMAL(38,10) AS big_dec,
       DATE '2026-03-01' AS d,
       TIMESTAMP '2026-03-01 12:34:56.654321' AS ts,
       'Grüße ✓ O''Brien' AS unicode_text;
SELECT big_dec, d, ts, unicode_text FROM ora.demo.dck_types;
DROP TABLE ora.demo.dck_types;

-- Parallel scan: a partitioned table is read one partition per connection. The
-- results must be identical to a serial read - no duplicated and no missing rows.
SELECT count(*) AS part_rows, sum(id) AS part_checksum, count(DISTINCT id) AS part_unique,
       min(id) AS part_min, max(id) AS part_max
FROM ora.demo.t_part;
-- Same table with the parallel path disabled, as a cross-check.
SET ora_parallel_scan = false;
SELECT count(*) AS serial_rows, sum(id) AS serial_checksum FROM ora.demo.t_part;
SET ora_parallel_scan = true;
-- Filters still apply per partition.
SELECT count(*) AS part_filtered FROM ora.demo.t_part WHERE id <= 100;

-- Filter pushdown correctness. These predicates are translated into Oracle SQL, so
-- each one must return exactly what Oracle itself would return. The interesting
-- cases are the ones the translator cannot express: it has to fall back rather
-- than emit a narrower predicate, which would silently drop rows. Counts below are
-- Oracle's own answers for this fixture.
--   t_part holds ids 1..20000, grp = MOD(id,10).
SELECT count(*) AS f_eq       FROM ora.demo.t_part WHERE id = 5;              -- 1
SELECT count(*) AS f_range    FROM ora.demo.t_part WHERE id > 10 AND id <= 20; -- 10
SELECT count(*) AS f_or       FROM ora.demo.t_part WHERE id < 3 OR id > 19998; -- 4
SELECT count(*) AS f_in_gap   FROM ora.demo.t_part WHERE id IN (1, 5000);      -- 2
SELECT count(*) AS f_notin    FROM ora.demo.t_part WHERE id NOT IN (1,2,3);    -- 19997
SELECT count(*) AS f_between  FROM ora.demo.t_part WHERE id BETWEEN 10 AND 20; -- 11
SELECT count(*) AS f_notnull  FROM ora.demo.t_part WHERE id IS NOT NULL;       -- 20000
SELECT count(*) AS f_isnull   FROM ora.demo.t_part WHERE id IS NULL;           -- 0
-- Not expressible in Oracle SQL by the translator: must fall back, not narrow.
SELECT count(*) AS f_expr     FROM ora.demo.t_part WHERE id + 0 = 7;           -- 1
SELECT count(*) AS f_distinct FROM ora.demo.t_part WHERE id IS DISTINCT FROM 5; -- 19999
-- An OR whose branches the translator cannot all express: dropping one branch
-- would narrow the predicate and lose rows.
SELECT count(*) AS f_or_mixed FROM ora.demo.t_part WHERE id IN (1, 5000) OR val = 'v7'; -- 3
-- Filters combined with the per-partition scan and with a second column.
SELECT count(*) AS f_part_col FROM ora.demo.t_part WHERE grp = 3;              -- 2000
SELECT sum(id)  AS f_part_sum FROM ora.demo.t_part WHERE grp = 3;              -- 19996000
-- Pushdown can be turned off; the answers must not change.
SET ora_experimental_filter_pushdown = false;
SELECT count(*) AS f_off_eq FROM ora.demo.t_part WHERE id = 5;                 -- 1
SELECT count(*) AS f_off_or FROM ora.demo.t_part WHERE id < 3 OR id > 19998;   -- 4
SET ora_experimental_filter_pushdown = true;

-- Spatial (SDO_GEOMETRY) is covered by integration_spatial.sql, which the runner
-- executes only when the database has Oracle Spatial.
SELECT 'ALL INTEGRATION TESTS PASSED' AS result;
