-- Seed data for the integration tests, executed against Oracle as the DEMO user.
-- Idempotent: drops (ignoring "does not exist") and recreates the table, so each
-- test run starts clean. Uses VARCHAR2 so the read-back exercises the common type.
-- Row id=2 carries an embedded single quote to verify literal escaping.
SET FEEDBACK OFF
SET DEFINE OFF
BEGIN
  EXECUTE IMMEDIATE 'DROP TABLE t_emp';
EXCEPTION
  WHEN OTHERS THEN NULL;
END;
/
CREATE TABLE t_emp (id NUMBER(10), name VARCHAR2(100), salary NUMBER(10,2));
INSERT INTO t_emp VALUES (1, 'Alice', 5000);
INSERT INTO t_emp VALUES (2, 'O''Brien', 6200.50);
INSERT INTO t_emp VALUES (3, 'Chen', 4800);
BEGIN
  EXECUTE IMMEDIATE 'DROP TABLE t_feat';
EXCEPTION
  WHEN OTHERS THEN NULL;
END;
/
CREATE TABLE t_feat (id NUMBER(10), doc JSON, emb VECTOR(3, FLOAT32));
INSERT INTO t_feat VALUES (1, '{"name":"Alice","tags":["x","y"]}', TO_VECTOR('[1.5, 2.5, 3.5]'));
INSERT INTO t_feat VALUES (2, '{"nested":{"k":true}}', TO_VECTOR('[-0.25, 10, 0]'));
COMMIT;

-- Partitioned fixture for the parallel (partition-per-thread) scan.
BEGIN
  EXECUTE IMMEDIATE 'DROP TABLE t_part';
EXCEPTION WHEN OTHERS THEN NULL;
END;
/
CREATE TABLE t_part (id NUMBER(10), grp NUMBER(4), val VARCHAR2(50))
PARTITION BY HASH (id) PARTITIONS 8;
INSERT INTO t_part SELECT LEVEL, MOD(LEVEL,10), 'v'||LEVEL FROM dual CONNECT BY LEVEL <= 20000;
COMMIT;

-- Spatial fixture. Wrapped so the whole block is skipped on databases without
-- Oracle Spatial (SDO_GEOMETRY/MDSYS missing) instead of failing the setup.
DECLARE
  has_spatial NUMBER;
BEGIN
  SELECT COUNT(*) INTO has_spatial FROM all_objects
   WHERE object_name = 'SDO_UTIL' AND object_type = 'PACKAGE';
  IF has_spatial = 0 THEN
    RETURN;
  END IF;
  BEGIN
    EXECUTE IMMEDIATE 'DROP TABLE t_geo';
  EXCEPTION WHEN OTHERS THEN NULL;
  END;
  EXECUTE IMMEDIATE
    'CREATE TABLE t_geo (id NUMBER(10), name VARCHAR2(50), shape SDO_GEOMETRY)';
  EXECUTE IMMEDIATE q'[INSERT INTO t_geo VALUES (1,'point',
     SDO_GEOMETRY(2001,4326,SDO_POINT_TYPE(10.5,51.2,NULL),NULL,NULL))]';
  EXECUTE IMMEDIATE q'[INSERT INTO t_geo VALUES (2,'line',
     SDO_GEOMETRY(2002,4326,NULL,SDO_ELEM_INFO_ARRAY(1,2,1),SDO_ORDINATE_ARRAY(1,1,5,5)))]';
  EXECUTE IMMEDIATE q'[INSERT INTO t_geo VALUES (3,'nullgeom', NULL)]';
  -- Register the SRID so the extension can expose it as the GEOMETRY CRS.
  EXECUTE IMMEDIATE q'[DELETE FROM user_sdo_geom_metadata WHERE table_name = 'T_GEO']';
  EXECUTE IMMEDIATE q'[INSERT INTO user_sdo_geom_metadata VALUES ('T_GEO','SHAPE',
     SDO_DIM_ARRAY(SDO_DIM_ELEMENT('X',-180,180,0.001),
                   SDO_DIM_ELEMENT('Y',-90,90,0.001)), 4326)]';
  COMMIT;
END;
/
EXIT
