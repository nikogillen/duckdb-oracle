# Contributing

Thanks for your interest in improving the DuckDB Oracle extension.

## Development setup

```bash
git clone --recurse-submodules https://github.com/nikogillen/duckdb-oracle
cd duckdb-oracle
make release            # or: make debug
```

`duckdb` and `extension-ci-tools` are git submodules; ODPI-C is downloaded by
CMake at configure time (pinned + SHA-256-verified). To target a specific DuckDB
line, check the `duckdb` submodule out at that tag (e.g. `v1.5.5` or `v1.4.5`)
before building. The build output is under
`build/release/repository/<duckdb_version>/<platform>/`.

## Running the tests

Integration tests run against a real Oracle database. Locally:

```bash
# starts a throwaway Oracle Free container, seeds data, runs the tests
./test/run-local-oracle-test.sh
```

You will need Docker, a local build, and an Oracle Instant Client (see
`test/README.md`). In CI the same tests run on Linux against Oracle Free
(`.github/workflows/ci-linux-oracle.yml`).

## Pull requests

- Branch from `main`; keep changes focused and commits descriptive.
- Update `CHANGELOG.md` (the `Unreleased` section) for user-visible changes.
- Add or extend tests for new behaviour where practical.
- Make sure the CI (build + Oracle integration) is green before requesting review.

## Security

Please report vulnerabilities privately — see [SECURITY.md](SECURITY.md).
