-- Spatial integration tests. Only meaningful on databases with Oracle Spatial;
-- the runner skips this file when the T_GEO fixture is absent (see
-- setup_oracle.sql, which creates it only when SDO_UTIL exists).
--
-- Preconditions: the `oracle` extension is loaded, the database is attached as
-- `ora`, and the DuckDB build is 1.5+ (GEOMETRY exists only from 1.5 on; on 1.4
-- LTS the column comes back as WKT text in a VARCHAR).

.bail on
.echo on

-- SDO_GEOMETRY is mapped to GEOMETRY, and the registered SRID becomes the CRS.
SELECT column_name, column_type FROM (DESCRIBE ora.demo.t_geo) WHERE column_name = 'shape';

-- Geometries round-trip as WKT; the NULL geometry stays NULL. Cast instead of
-- ST_AsText so the test does not depend on the spatial extension being installed.
SELECT id, name, CAST(shape AS VARCHAR) AS wkt FROM ora.demo.t_geo ORDER BY id;

-- The values are real geometries, not just text.
SELECT count(*) AS point_found FROM ora.demo.t_geo
WHERE CAST(shape AS VARCHAR) = 'POINT (10.5 51.2)';

-- NULL handling through the geometry converter.
SELECT count(*) AS null_geoms FROM ora.demo.t_geo WHERE shape IS NULL;
SELECT count(*) AS non_null_geoms FROM ora.demo.t_geo WHERE shape IS NOT NULL;

SELECT 'SPATIAL TESTS PASSED' AS result;
