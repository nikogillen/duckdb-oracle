# Security Policy

## Reporting a vulnerability

Please report security issues **privately** rather than opening a public issue.

Use GitHub's private reporting: on the repository's **Security** tab choose
*"Report a vulnerability"* (Security Advisories). Include the affected version /
commit, a description, and ideally a minimal reproduction.

We aim to acknowledge reports within a few days and will coordinate a fix and
disclosure timeline with you.

## Scope and hardening notes

This extension connects DuckDB to Oracle databases via ODPI-C. A few things worth
knowing when deploying it:

- **Credentials:** prefer DuckDB **secrets** (`CREATE SECRET … TYPE oracle`) over
  putting `user/password@host` in `ATTACH`. A plaintext connect string ends up in
  query history, logs and the DuckDB catalog. See the README for details.
- **Unsigned extensions:** locally built or release binaries are loaded with
  `-unsigned`. Verify release assets against the published `SHA256SUMS` and the
  build-provenance attestations before loading them.
- **Debug logging:** `DPI_DEBUG_LEVEL ≥ 8` and `ora_debug_show_queries` can emit
  SQL text, bind values and row data. Enable only in trusted environments.
- **Dependencies:** ODPI-C is fetched at build time pinned to a specific release
  and verified by SHA-256; DuckDB and the CI tools are pinned git submodules.

## Supported versions

Fixes target the `main` branch and the most recent release for the DuckDB
**1.4 LTS** and **1.5.x** lines.
