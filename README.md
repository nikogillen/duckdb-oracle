"# duckdb-oracle" 


  loaded = true — the extension is live in DuckDB. To use it against an Oracle database:

  LOAD 'C:\wrk\duckdb-oracle-build\extension\oracle_scanner\oracle_scanner.duckdb_extension';
  
  ATTACH 'user/password@//host:1521/service' AS mydb (TYPE oracle_scanner);
  
  SELECT * FROM mydb.myschema.mytable;

  Use -unsigned when launching the shell since the extension isn't signed. You can also set
  allow_unsigned_extensions=true in your DuckDB config file to avoid needing the flag each time.

Failed to load 'oracle_scanner.duckdb_extension', The file was built specifically for DuckDB version '83ae79e5b7' and can only be loaded with that version of DuckDB. (this version of DuckDB is 'v1.4.4')

