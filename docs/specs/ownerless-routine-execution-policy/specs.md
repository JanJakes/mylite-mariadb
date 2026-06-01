# Ownerless Routine Execution Policy

## Problem Statement

Ownerless mode rejects stored-routine DDL because routine definitions mutate
`mysql.proc` and `mysql.procs_priv` through metadata paths that are not yet
coordinated for cross-process ownerless writers. Existing routines created in
ordinary exclusive mode can still be invoked with top-level `CALL`, and a
procedure body can execute DML or DDL without that body text passing through
MyLite's top-level ownerless SQL policy.

Until routine execution has a parser-aware ownerless design, top-level
ownerless `CALL` should be explicitly rejected before MariaDB can execute the
stored procedure body.

## Source Findings

MariaDB base line:

- `mariadb-11.8.6`
- source ref `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`

Relevant source paths:

- `mariadb/sql/sql_yacc.yy`
  - top-level `CALL` grammar builds a call statement through
    `Lex->call_statement_start(...)`.
  - stored-program statement grammar also supports direct procedure calls
    without the `CALL` keyword inside stored program bodies.
- `mariadb/sql/sql_parse.cc`
  - `Sql_cmd_call::execute()` opens and locks the routine/table dependencies,
    resolves the procedure, then calls `do_execute_sp()`.
  - `do_execute_sp()` executes the routine through
    `sp->execute_procedure(thd, &thd->lex->value_list)`.
- `mariadb/sql/sp_head.cc`
  - `sp_head::execute_procedure()` creates the stored-program runtime context
    and executes routine instructions.
- `packages/libmylite/src/database.cc`
  - MyLite's unsupported SQL policy is applied to the top-level SQL string
    before execution.
  - Prepared `CALL` is already rejected by `mylite_prepare()`.
  - Ownerless routine DDL is already rejected, but ownerless top-level `CALL`
    is not currently a separate policy check.

## Design

Add an ownerless-only top-level `CALL` policy:

1. Extend the unsupported ownerless SQL policy with a
   `CALL`-statement predicate.
2. Return a clear MyLite error before MariaDB executes the routine body.
3. Add an ownerless SQL selector, `routine-execution-policy`, that:
   - creates an InnoDB table and stored procedure in ordinary exclusive mode,
   - opens the same directory ownerless,
   - verifies `CALL app.ownerless_routine_execution_policy_proc(...)` is
     rejected,
   - verifies the procedure body did not update the base table,
   - verifies the existing routine metadata and base row survive
     ownerless/native reopen before and after forced `.shm` rebuild.

## Scope

In scope:

- Top-level `CALL` rejection for `MYLITE_OPEN_OWNERLESS_RW`.
- A regression test proving blocked `CALL` does not execute procedure-body DML.
- Compatibility and ownerless-concurrency documentation updates.

Out of scope:

- Stored function invocation inside expressions such as `SELECT app.fn()`.
- Stored routine execution support in ownerless mode.
- Parser-aware inspection of routine bodies.
- Prepared `CALL` support; it remains rejected by `mylite_prepare()`.
- Stored routine DDL support in ownerless mode.

## Compatibility Impact

This makes an ownerless unsupported surface explicit. Ordinary exclusive
embedded mode keeps inherited direct `CALL` behavior. Ownerless mode remains
partial for routines until routine metadata and routine-body execution are
coordinated and tested.

## Directory And Lifecycle Impact

The test creates routine metadata and an InnoDB table inside the MyLite-owned
directory in exclusive mode, then verifies ownerless rejection does not mutate
either. No new durable paths or directory layout changes are introduced.

## Native Storage Impact

Test and policy only. No InnoDB format, redo, page-version, or dictionary
metadata implementation changes are intended beyond preventing an unproven
execution path.

## Binary Size Impact

No new dependency or public API. The policy adds only a small token check and a
test selector.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run focused `routine-execution-policy` in `embedded-dev`.
- Run adjacent routine policy selectors in `embedded-dev`.
- Build and run focused/adjacent selectors in `ownerless-test-hooks`.
- Run the registered ownerless cross-process SQL CTest filters.
- Run `format-check`, `git diff --check`, and cached diff checks before
  commit.

## Acceptance Criteria

- Ownerless top-level `CALL` returns a MyLite policy error before MariaDB
  executes the routine body.
- The base table remains unchanged after the rejected call.
- Existing routine metadata remains visible through ownerless/native reopen
  before and after forced `.shm` rebuild.
- Existing stored-routine DDL rejection and prepared `CALL` rejection remain
  documented separately.

## Risks And Open Questions

- Stored function invocation inside expressions remains a broader routine
  execution gap because token-only SQL policy cannot reliably distinguish
  stored functions from built-in functions.
- Routines created by exclusive mode remain durable and callable again from
  exclusive mode; this slice only constrains ownerless execution.
