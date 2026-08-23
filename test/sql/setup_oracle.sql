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
EXIT
