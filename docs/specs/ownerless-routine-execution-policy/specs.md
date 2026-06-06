# Ownerless Routine Execution Policy

## Problem Statement

Ownerless mode rejects stored-routine DDL because routine definitions mutate
`mysql.proc` and `mysql.procs_priv` through metadata paths that are not yet
coordinated for cross-process ownerless writers. Existing routines created in
ordinary exclusive mode can still be invoked with top-level `CALL`, stored
functions in expressions, or trigger bodies that call procedures/functions.
Those routine bodies can execute DML or DDL without the nested body text passing
through MyLite's top-level ownerless SQL policy.

Until routine execution has a coordinated ownerless design, ownerless mode must
reject stored procedure and stored function execution at the MariaDB routine
entry points, not only at the top-level SQL string.

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
  - `sp_head::execute_function()` creates the stored-function runtime context
    when `Item_func_sp` evaluates a stored function in an expression, including
    trigger expressions.
  - `sp_head::execute_trigger()` executes trigger bodies through
    `sp_head::execute()` and is intentionally not rejected; only nested
    stored procedure/function entry points fail in ownerless mode.
- `packages/libmylite/src/database.cc`
  - MyLite's unsupported SQL policy is applied to the top-level SQL string
    before execution.
  - Prepared `CALL` is rejected by `mylite_prepare()` before statement
    allocation.
  - Ownerless routine DDL and top-level `CALL` are already rejected by the
    top-level policy; stored functions and nested trigger-body routine calls
    need the MariaDB routine-entry guard.

## Design

Add an ownerless stored-routine execution policy:

1. Keep the unsupported ownerless SQL policy for top-level `CALL`.
2. Guard `sp_head::execute_procedure()` and `sp_head::execute_function()` when
   the MyLite ownerless runtime hooks are installed.
3. Return a clear MariaDB/MyLite error before MariaDB executes any stored
   procedure or stored function body reached by `CALL`, expression evaluation,
   or trigger body execution.
4. Add an ownerless SQL selector, `routine-execution-policy`, that:
   - creates InnoDB tables, stored procedures, a stored function, and triggers
     in ordinary exclusive mode,
   - opens the same directory ownerless,
   - verifies `CALL app.ownerless_routine_execution_policy_proc(...)` is
     rejected,
   - verifies prepared `CALL app.ownerless_routine_execution_policy_proc(...)`
     is rejected before statement allocation,
   - verifies direct and prepared stored-function calls are rejected before
     result delivery,
   - verifies trigger bodies that would call a stored function or stored
     procedure fail without inserting rows or audit records,
   - verifies no routine body updated the base tables,
   - verifies the existing routine metadata and base row survive
     ownerless/native reopen before and after forced `.shm` rebuild.

## Scope

In scope:

- Top-level `CALL` rejection for `MYLITE_OPEN_OWNERLESS_RW`.
- Stored function execution rejection for `MYLITE_OPEN_OWNERLESS_RW`.
- Nested stored procedure/function execution rejection when trigger bodies reach
  the MariaDB routine entry points.
- A regression test proving blocked direct and prepared `CALL` forms do not
  execute procedure-body DML.
- A regression test proving blocked direct/prepared stored-function and
  trigger-body routine paths do not mutate InnoDB tables.
- Compatibility and ownerless-concurrency documentation updates.

Out of scope:

- Stored routine execution support in ownerless mode.
- Parser-aware inspection of routine bodies before execution.
- Prepared `CALL` support; this slice only covers explicit rejection.
- Stored routine DDL support in ownerless mode.

## Compatibility Impact

This makes an ownerless unsupported surface explicit. Ordinary exclusive
embedded mode keeps inherited direct `CALL`, stored-function, and trigger-body
routine behavior. Ownerless mode remains partial for routines until routine
metadata and routine-body execution are coordinated and tested.

## Directory And Lifecycle Impact

The test creates routine metadata, trigger metadata, and InnoDB tables inside
the MyLite-owned directory in exclusive mode, then verifies ownerless rejection
does not mutate those tables. No new durable paths or directory layout changes
are introduced.

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
- Prepared ownerless `CALL` fails before statement allocation and leaves the
  routine body unexecuted.
- Ownerless stored-function expression execution fails before result delivery.
- Ownerless trigger-body routine execution fails before its routine body mutates
  base or audit tables.
- The base table remains unchanged after the rejected call.
- Existing routine metadata remains visible through ownerless/native reopen
  before and after forced `.shm` rebuild.
- Existing stored-routine DDL rejection remains documented separately.

## Risks And Open Questions

- Routines created by exclusive mode remain durable and callable again from
  exclusive mode; this slice only constrains ownerless execution.
- Parser-aware routine-body classification is still needed before ownerless can
  selectively support routine execution.
