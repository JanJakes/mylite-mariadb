/* Copyright (c) 2026, MyLite contributors

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License. */

#include "mariadb.h"
#include "sql_priv.h"
#include "sql_class.h"
#include "sql_lex.h"
#include "sp.h"
#include "sp_cache.h"
#include "sp_head.h"
#include "sp_instr.h"
#include "sp_pcontext.h"
#include "sp_rcontext.h"
#include "field.h"
#include "sql_parse.h"
#include "mysqld_error.h"

static bool mylite_stored_program_runtime_unsupported();
static int mylite_stored_program_runtime_unsupported_int();
static void mylite_stored_program_runtime_unsupported_void();
static void mylite_stored_program_runtime_next(uint *nextp, uint next);

Sp_handler_function sp_handler_function;
Sp_handler_procedure sp_handler_procedure;
Sp_handler_package_spec sp_handler_package_spec;
Sp_handler_package_body sp_handler_package_body;
Sp_handler_package_function sp_handler_package_function;
Sp_handler_package_procedure sp_handler_package_procedure;
Sp_handler_trigger sp_handler_trigger;

Sp_rcontext_handler_local sp_rcontext_handler_local;
Sp_rcontext_handler_package_body sp_rcontext_handler_package_body;

PSI_statement_info sp_instr_stmt::psi_info= { 0, "stmt", 0 };
PSI_statement_info sp_instr_set::psi_info= { 0, "set", 0 };
PSI_statement_info sp_instr_set_trigger_field::psi_info=
  { 0, "set_trigger_field", 0 };
PSI_statement_info sp_instr_jump::psi_info= { 0, "jump", 0 };
PSI_statement_info sp_instr_jump_if_not::psi_info= { 0, "jump_if_not", 0 };
PSI_statement_info sp_instr_freturn::psi_info= { 0, "freturn", 0 };
PSI_statement_info sp_instr_preturn::psi_info= { 0, "preturn", 0 };
PSI_statement_info sp_instr_hpush_jump::psi_info= { 0, "hpush_jump", 0 };
PSI_statement_info sp_instr_hpop::psi_info= { 0, "hpop", 0 };
PSI_statement_info sp_instr_hreturn::psi_info= { 0, "hreturn", 0 };
PSI_statement_info sp_instr_cpush::psi_info= { 0, "cpush", 0 };
PSI_statement_info sp_instr_cpop::psi_info= { 0, "cpop", 0 };
PSI_statement_info sp_instr_copen::psi_info= { 0, "copen", 0 };
PSI_statement_info sp_instr_cclose::psi_info= { 0, "cclose", 0 };
PSI_statement_info sp_instr_cfetch::psi_info= { 0, "cfetch", 0 };
PSI_statement_info sp_instr_agg_cfetch::psi_info= { 0, "agg_cfetch", 0 };
PSI_statement_info sp_instr_cursor_copy_struct::psi_info=
  { 0, "cursor_copy_struct", 0 };
PSI_statement_info sp_instr_error::psi_info= { 0, "error", 0 };
PSI_statement_info sp_instr_set_case_expr::psi_info=
  { 0, "set_case_expr", 0 };

LEX_CSTRING Sp_handler_procedure::empty_body_lex_cstring(sql_mode_t) const
{
  static LEX_CSTRING empty_body= { STRING_WITH_LEN("BEGIN END") };
  return empty_body;
}

const Sp_handler *Sp_handler_procedure::package_routine_handler() const
{
  return &sp_handler_package_procedure;
}

sp_cache **Sp_handler_procedure::get_cache(THD *) const
{
  return nullptr;
}

ulong Sp_handler_procedure::recursion_depth(THD *) const
{
  return 0;
}

void Sp_handler_procedure::recursion_level_error(THD *,
                                                 const sp_head *) const
{
  mylite_stored_program_runtime_unsupported_void();
}

bool Sp_handler_procedure::add_instr_preturn(THD *, sp_head *,
                                             sp_pcontext *) const
{
  return mylite_stored_program_runtime_unsupported();
}

LEX_CSTRING Sp_handler_function::empty_body_lex_cstring(sql_mode_t) const
{
  static LEX_CSTRING empty_body= { STRING_WITH_LEN("RETURN NULL") };
  return empty_body;
}

const Sp_handler *Sp_handler_function::package_routine_handler() const
{
  return &sp_handler_package_function;
}

sp_cache **Sp_handler_function::get_cache(THD *) const
{
  return nullptr;
}

bool Sp_handler_function::add_instr_freturn(THD *, sp_head *, sp_pcontext *,
                                            Item *, sp_expr_lex *) const
{
  return mylite_stored_program_runtime_unsupported();
}

bool Sp_handler_package::show_create_sp(THD *, String *,
                                        const LEX_CSTRING &,
                                        const LEX_CSTRING &,
                                        const LEX_CSTRING &,
                                        const LEX_CSTRING &,
                                        const LEX_CSTRING &,
                                        const st_sp_chistics &,
                                        const AUTHID &,
                                        const DDL_options_st,
                                        sql_mode_t) const
{
  return mylite_stored_program_runtime_unsupported();
}

int Sp_handler_package_spec::sp_find_and_drop_routine(
    THD *, TABLE *, const Database_qualified_name *) const
{
  return SP_INTERNAL_ERROR;
}

sp_cache **Sp_handler_package_spec::get_cache(THD *) const
{
  return nullptr;
}

sp_cache **Sp_handler_package_body::get_cache(THD *) const
{
  return nullptr;
}

bool Sp_handler::add_instr_freturn(THD *, sp_head *, sp_pcontext *, Item *,
                                   sp_expr_lex *) const
{
  return mylite_stored_program_runtime_unsupported();
}

bool Sp_handler::add_instr_preturn(THD *, sp_head *, sp_pcontext *) const
{
  return mylite_stored_program_runtime_unsupported();
}

void Sp_handler::add_used_routine(Query_tables_list *, Query_arena *,
                                  const Database_qualified_name *) const
{
}

bool Sp_handler::sp_resolve_package_routine(
    THD *, sp_head *, sp_name *, const Sp_handler **pkg_routine_handler,
    Database_qualified_name *) const
{
  if (pkg_routine_handler)
    *pkg_routine_handler= nullptr;
  return mylite_stored_program_runtime_unsupported();
}

int Sp_handler::sp_cache_package_routine(THD *,
                                         const LEX_CSTRING &,
                                         const Database_qualified_name *,
                                         sp_head **sp) const
{
  if (sp)
    *sp= nullptr;
  mylite_stored_program_runtime_unsupported();
  return SP_INTERNAL_ERROR;
}

int Sp_handler::sp_cache_package_routine(THD *,
                                         const Database_qualified_name *,
                                         sp_head **sp) const
{
  if (sp)
    *sp= nullptr;
  mylite_stored_program_runtime_unsupported();
  return SP_INTERNAL_ERROR;
}

sp_head *Sp_handler::sp_find_package_routine(
    THD *, const LEX_CSTRING, const Database_qualified_name *, bool) const
{
  mylite_stored_program_runtime_unsupported();
  return nullptr;
}

sp_head *Sp_handler::sp_find_package_routine(
    THD *, const Database_qualified_name *, bool) const
{
  mylite_stored_program_runtime_unsupported();
  return nullptr;
}

sp_head *Sp_handler::sp_find_routine(THD *, const Database_qualified_name *,
                                     bool) const
{
  mylite_stored_program_runtime_unsupported();
  return nullptr;
}

int Sp_handler::sp_cache_routine(THD *, const Database_qualified_name *,
                                 sp_head **sp) const
{
  if (sp)
    *sp= nullptr;
  mylite_stored_program_runtime_unsupported();
  return SP_INTERNAL_ERROR;
}

int Sp_handler::sp_cache_routine_reentrant(THD *,
                                           const Database_qualified_name *,
                                           sp_head **sp) const
{
  if (sp)
    *sp= nullptr;
  mylite_stored_program_runtime_unsupported();
  return SP_INTERNAL_ERROR;
}

bool Sp_handler::sp_exist_routines(THD *, TABLE_LIST *) const
{
  return false;
}

bool Sp_handler::sp_show_create_routine(THD *,
                                        const Database_qualified_name *) const
{
  return mylite_stored_program_runtime_unsupported();
}

bool Sp_handler::sp_create_routine(THD *, const sp_head *) const
{
  return mylite_stored_program_runtime_unsupported();
}

int Sp_handler::sp_update_routine(THD *, const Database_qualified_name *,
                                  const st_sp_chistics *) const
{
  mylite_stored_program_runtime_unsupported();
  return SP_INTERNAL_ERROR;
}

int Sp_handler::sp_drop_routine(THD *, const Database_qualified_name *) const
{
  mylite_stored_program_runtime_unsupported();
  return SP_INTERNAL_ERROR;
}

sp_head *Sp_handler::sp_load_for_information_schema(
    THD *, TABLE *, const LEX_CSTRING &, const LEX_CSTRING &,
    const LEX_CSTRING &, const LEX_CSTRING &, sql_mode_t, bool *free_sp_head)
    const
{
  if (free_sp_head)
    *free_sp_head= false;
  return nullptr;
}

bool Sp_handler::show_create_sp(THD *, String *, const LEX_CSTRING &,
                                const LEX_CSTRING &, const LEX_CSTRING &,
                                const LEX_CSTRING &, const LEX_CSTRING &,
                                const st_sp_chistics &, const AUTHID &,
                                const DDL_options_st, sql_mode_t) const
{
  return mylite_stored_program_runtime_unsupported();
}

int Sp_handler::sp_find_and_drop_routine(THD *, TABLE *,
                                         const Database_qualified_name *) const
{
  return SP_INTERNAL_ERROR;
}

uint sp_get_flags_for_command(LEX *)
{
  return 0;
}

void init_sp_psi_keys()
{
}

void sp_cache_init()
{
}

void sp_cache_end()
{
}

void sp_cache_clear(sp_cache **)
{
}

void sp_cache_insert(sp_cache **, sp_head *)
{
}

sp_head *sp_cache_lookup(sp_cache **, const Database_qualified_name *)
{
  return nullptr;
}

void sp_cache_invalidate()
{
}

void sp_cache_remove(sp_cache **, sp_head **sp)
{
  if (sp)
    *sp= nullptr;
}

ulong sp_cache_version()
{
  return 0;
}

void sp_cache_enforce_limit(sp_cache *, ulong)
{
}

void Sp_caches::sp_caches_clear()
{
  sp_proc_cache= nullptr;
  sp_func_cache= nullptr;
  sp_package_spec_cache= nullptr;
  sp_package_body_cache= nullptr;
  m_sp_cache_version= 0;
}

void Sp_caches::sp_caches_empty()
{
}

int sp_drop_db_routines(THD *, const LEX_CSTRING &)
{
  return SP_OK;
}

bool lock_db_routines(THD *, const Lex_ident_db_normalized &)
{
  return false;
}

bool sp_add_used_routine(Query_tables_list *, Query_arena *, const MDL_key *,
                         const Sp_handler *, TABLE_LIST *)
{
  return false;
}

void sp_remove_not_own_routines(Query_tables_list *)
{
}

bool sp_update_sp_used_routines(HASH *, HASH *)
{
  return false;
}

void sp_update_stmt_used_routines(THD *, Query_tables_list *, HASH *,
                                  TABLE_LIST *)
{
}

void sp_update_stmt_used_routines(THD *, Query_tables_list *,
                                  SQL_I_List<Sroutine_hash_entry> *,
                                  TABLE_LIST *)
{
}

const uchar *sp_sroutine_key(const void *, size_t *, my_bool)
{
  return nullptr;
}

TABLE *open_proc_table_for_read(THD *)
{
  mylite_stored_program_runtime_unsupported();
  return nullptr;
}

int Sroutine_hash_entry::sp_cache_routine(THD *, sp_head **sp) const
{
  if (sp)
    *sp= nullptr;
  return SP_INTERNAL_ERROR;
}

bool Lex_ident_routine::check_name_with_error(const LEX_CSTRING &ident)
{
  DBUG_ASSERT(ident.str);

  if (!ident.str[0] || ident.str[ident.length - 1] == ' ')
  {
    my_error(ER_SP_WRONG_NAME, MYF(0), ident.str);
    return true;
  }
  return check_ident_length(&ident);
}

bool Qualified_column_ident::resolve_type_ref(THD *,
                                              Column_definition *) const
{
  return mylite_stored_program_runtime_unsupported();
}

bool Table_ident::resolve_table_rowtype_ref(THD *, Row_definition_list &)
{
  return mylite_stored_program_runtime_unsupported();
}

Item *THD::sp_prepare_func_item(Item **, uint)
{
  mylite_stored_program_runtime_unsupported();
  return nullptr;
}

Item *THD::sp_fix_func_item(Item **)
{
  mylite_stored_program_runtime_unsupported();
  return nullptr;
}

Item *THD::sp_fix_func_item_for_assignment(const Field *, Item **)
{
  mylite_stored_program_runtime_unsupported();
  return nullptr;
}

sp_head::~sp_head() = default;

void sp_head::destroy(sp_head *)
{
}

ulong sp_head::sp_cache_version() const
{
  return 0;
}

sp_head *sp_head::create(sp_package *, const Sp_handler *,
                         enum_sp_aggregate_type, sql_mode_t, MEM_ROOT *)
{
  mylite_stored_program_runtime_unsupported();
  return nullptr;
}

void sp_head::init(LEX *)
{
}

bool sp_head::init_sp_name(const sp_name *)
{
  return mylite_stored_program_runtime_unsupported();
}

void sp_head::set_body_start(THD *, const char *)
{
}

void sp_head::set_stmt_end(THD *, const char *)
{
}

bool sp_head::execute_trigger(THD *, const LEX_CSTRING *,
                              const LEX_CSTRING *, GRANT_INFO *)
{
  return mylite_stored_program_runtime_unsupported();
}

bool sp_head::execute_function(THD *, Item **, uint, Field *, sp_rcontext **,
                               Query_arena *)
{
  return mylite_stored_program_runtime_unsupported();
}

bool sp_head::execute_procedure(THD *, List<Item> *)
{
  return mylite_stored_program_runtime_unsupported();
}

void sp_head::show_create_routine_get_fields(THD *, const Sp_handler *,
                                             List<Item> *)
{
}

bool sp_head::show_create_routine(THD *, const Sp_handler *)
{
  return mylite_stored_program_runtime_unsupported();
}

int sp_head::add_instr(sp_instr *)
{
  return 1;
}

bool sp_head::add_instr_jump(THD *, sp_pcontext *)
{
  return mylite_stored_program_runtime_unsupported();
}

bool sp_head::add_instr_jump(THD *, sp_pcontext *, uint)
{
  return mylite_stored_program_runtime_unsupported();
}

bool sp_head::add_instr_jump_forward_with_backpatch(THD *, sp_pcontext *,
                                                    sp_label *)
{
  return mylite_stored_program_runtime_unsupported();
}

bool sp_head::add_instr_freturn(THD *, sp_pcontext *, Item *, sp_expr_lex *)
{
  return mylite_stored_program_runtime_unsupported();
}

bool sp_head::add_instr_preturn(THD *, sp_pcontext *)
{
  return mylite_stored_program_runtime_unsupported();
}

Item *sp_head::adjust_assignment_source(THD *, Item *val, Item *)
{
  mylite_stored_program_runtime_unsupported();
  return val;
}

bool sp_head::set_local_variable(THD *, sp_pcontext *,
                                 const Sp_rcontext_handler *, sp_variable *,
                                 Item *, LEX *, bool,
                                 const LEX_CSTRING &)
{
  return mylite_stored_program_runtime_unsupported();
}

bool sp_head::set_local_variable_row_field(
    THD *, sp_pcontext *, const Sp_rcontext_handler *, sp_variable *, uint,
    Item *, LEX *, const LEX_CSTRING &)
{
  return mylite_stored_program_runtime_unsupported();
}

bool sp_head::set_local_variable_row_field_by_name(
    THD *, sp_pcontext *, const Sp_rcontext_handler *, sp_variable *,
    const LEX_CSTRING *, Item *, LEX *, const LEX_CSTRING &)
{
  return mylite_stored_program_runtime_unsupported();
}

bool sp_head::check_package_routine_end_name(const LEX_CSTRING &) const
{
  return mylite_stored_program_runtime_unsupported();
}

bool sp_head::check_standalone_routine_end_name(const sp_name *) const
{
  return mylite_stored_program_runtime_unsupported();
}

bool sp_head::check_group_aggregate_instructions_function() const
{
  return mylite_stored_program_runtime_unsupported();
}

bool sp_head::check_group_aggregate_instructions_forbid() const
{
  return mylite_stored_program_runtime_unsupported();
}

bool sp_head::check_group_aggregate_instructions_require() const
{
  return mylite_stored_program_runtime_unsupported();
}

void sp_head::sp_returns_type(THD *, String &) const
{
}

void sp_head::sp_returns_type_of(THD *, String &,
                                 const Qualified_column_ident &) const
{
}

void sp_head::sp_returns_rowtype_of(THD *, String &, const Table_ident &) const
{
}

bool sp_head::add_open_cursor(THD *, sp_pcontext *, uint, sp_pcontext *,
                              List<sp_assignment_lex> *)
{
  return mylite_stored_program_runtime_unsupported();
}

bool sp_head::add_for_loop_open_cursor(THD *, sp_pcontext *, sp_variable *,
                                       const sp_pcursor *, uint,
                                       sp_assignment_lex *, Item_args *)
{
  return mylite_stored_program_runtime_unsupported();
}

bool sp_head::replace_instr_to_nop(THD *, uint)
{
  return mylite_stored_program_runtime_unsupported();
}

bool sp_head::reset_lex(THD *)
{
  return mylite_stored_program_runtime_unsupported();
}

bool sp_head::reset_lex(THD *, sp_lex_local *)
{
  return mylite_stored_program_runtime_unsupported();
}

bool sp_head::merge_lex(THD *, LEX *, LEX *)
{
  return mylite_stored_program_runtime_unsupported();
}

void sp_head::unwind_aux_lexes_and_restore_original_lex()
{
}

int sp_head::push_backpatch(THD *, sp_instr *, sp_label *)
{
  return 1;
}

int sp_head::push_backpatch_goto(THD *, sp_pcontext *, sp_label *)
{
  return 1;
}

void sp_head::backpatch(sp_label *)
{
}

void sp_head::backpatch_goto(THD *, sp_label *, sp_label *)
{
}

bool sp_head::check_unresolved_goto()
{
  return mylite_stored_program_runtime_unsupported();
}

int sp_head::new_cont_backpatch(sp_instr_opt_meta *)
{
  return 1;
}

int sp_head::add_cont_backpatch(sp_instr_opt_meta *)
{
  return 1;
}

void sp_head::do_cont_backpatch()
{
}

bool sp_head::sp_add_instr_cpush_for_cursors(THD *, sp_pcontext *)
{
  return mylite_stored_program_runtime_unsupported();
}

char *sp_head::create_string(THD *, ulong *lenp)
{
  if (lenp)
    *lenp= 0;
  return nullptr;
}

Field *sp_head::create_result_field(uint, const LEX_CSTRING *,
                                    const Column_definition &, TABLE *) const
{
  mylite_stored_program_runtime_unsupported();
  return nullptr;
}

bool sp_head::spvar_fill_row(THD *, sp_variable *, Row_definition_list *)
{
  return mylite_stored_program_runtime_unsupported();
}

bool sp_head::spvar_fill_type_reference(THD *, sp_variable *,
                                        const LEX_CSTRING &,
                                        const LEX_CSTRING &)
{
  return mylite_stored_program_runtime_unsupported();
}

bool sp_head::spvar_fill_type_reference(THD *, sp_variable *,
                                        const LEX_CSTRING &,
                                        const LEX_CSTRING &,
                                        const LEX_CSTRING &)
{
  return mylite_stored_program_runtime_unsupported();
}

bool sp_head::spvar_fill_table_rowtype_reference(THD *, sp_variable *,
                                                 const LEX_CSTRING &)
{
  return mylite_stored_program_runtime_unsupported();
}

bool sp_head::spvar_fill_table_rowtype_reference(THD *, sp_variable *,
                                                 const LEX_CSTRING &,
                                                 const LEX_CSTRING &)
{
  return mylite_stored_program_runtime_unsupported();
}

bool sp_head::spvar_def_fill_type_reference(THD *, Spvar_definition *,
                                            const LEX_CSTRING &,
                                            const LEX_CSTRING &)
{
  return mylite_stored_program_runtime_unsupported();
}

bool sp_head::spvar_def_fill_type_reference(THD *, Spvar_definition *,
                                            const LEX_CSTRING &,
                                            const LEX_CSTRING &,
                                            const LEX_CSTRING &)
{
  return mylite_stored_program_runtime_unsupported();
}

void sp_head::set_c_chistics(const st_sp_chistics &)
{
}

void sp_head::set_info(longlong, longlong, const st_sp_chistics &, sql_mode_t)
{
}

void sp_head::reset_thd_mem_root(THD *)
{
}

void sp_head::restore_thd_mem_root(THD *)
{
}

void sp_head::optimize()
{
}

void sp_head::add_mark_lead(uint, List<sp_instr> *)
{
}

bool sp_head::add_used_tables_to_table_list(THD *, TABLE_LIST ***,
                                            TABLE_LIST *)
{
  return false;
}

bool sp_head::check_execute_access(THD *) const
{
  return mylite_stored_program_runtime_unsupported();
}

void sp_head::init_psi_share()
{
}

sp_package *sp_package::create(LEX *, const sp_name *, const Sp_handler *,
                               sql_mode_t, MEM_ROOT *)
{
  mylite_stored_program_runtime_unsupported();
  return nullptr;
}

bool sp_package::validate_after_parser(THD *)
{
  return mylite_stored_program_runtime_unsupported();
}

bool sp_package::instantiate_if_needed(THD *)
{
  return mylite_stored_program_runtime_unsupported();
}

void sp_package::LexList::cleanup()
{
}

LEX *sp_package::LexList::find(const LEX_CSTRING &, enum_sp_type)
{
  return nullptr;
}

LEX *sp_package::LexList::find_qualified(const LEX_CSTRING &, enum_sp_type)
{
  return nullptr;
}

bool check_show_routine_access(THD *, sp_head *, bool *full_access)
{
  if (full_access)
    *full_access= false;
  return mylite_stored_program_runtime_unsupported();
}

bool check_db_routine_access(THD *, privilege_t, const char *, const char *,
                             const Sp_handler *, bool)
{
  return mylite_stored_program_runtime_unsupported();
}

TABLE_LIST *sp_add_to_query_tables(THD *, LEX *, const LEX_CSTRING *,
                                   const LEX_CSTRING *, thr_lock_type,
                                   enum_mdl_type)
{
  return nullptr;
}

sp_pcontext::sp_pcontext()
  : Sql_alloc(),
    m_max_var_index(0), m_max_cursor_index(0),
    m_parent(nullptr), m_var_offset(0), m_cursor_offset(0),
    m_pboundary(0), m_num_case_exprs(0),
    m_vars(PSI_INSTRUMENT_MEM), m_case_expr_ids(PSI_INSTRUMENT_MEM),
    m_conditions(PSI_INSTRUMENT_MEM), m_cursors(PSI_INSTRUMENT_MEM),
    m_handlers(PSI_INSTRUMENT_MEM), m_records(PSI_INSTRUMENT_MEM),
    m_children(PSI_INSTRUMENT_MEM), m_scope(REGULAR_SCOPE)
{
}

sp_pcontext::~sp_pcontext()
{
}

sp_pcontext *sp_pcontext::push_context(THD *, enum_scope)
{
  mylite_stored_program_runtime_unsupported();
  return nullptr;
}

sp_pcontext *sp_pcontext::pop_context()
{
  return parent_context();
}

uint sp_pcontext::diff_handlers(const sp_pcontext *, bool) const
{
  return 0;
}

uint sp_pcontext::diff_cursors(const sp_pcontext *, bool) const
{
  return 0;
}

uint sp_pcontext::default_context_var_count() const
{
  return 0;
}

sp_variable *sp_pcontext::add_variable(THD *, const LEX_CSTRING *)
{
  mylite_stored_program_runtime_unsupported();
  return nullptr;
}

void sp_pcontext::retrieve_field_definitions(List<Spvar_definition> *) const
{
}

sp_variable *sp_pcontext::find_variable(const LEX_CSTRING *, bool) const
{
  return nullptr;
}

sp_variable *sp_pcontext::find_variable(uint) const
{
  return nullptr;
}

sp_label *sp_pcontext::push_label(THD *, const LEX_CSTRING *, uint,
                                  sp_label::enum_type, List<sp_label> *)
{
  mylite_stored_program_runtime_unsupported();
  return nullptr;
}

sp_label *sp_pcontext::find_label(const LEX_CSTRING *)
{
  return nullptr;
}

sp_label *sp_pcontext::find_goto_label(const LEX_CSTRING *, bool)
{
  return nullptr;
}

sp_label *sp_pcontext::find_label_current_loop_start()
{
  return nullptr;
}

bool sp_pcontext::add_condition(THD *, const Lex_ident_column &,
                                sp_condition_value *)
{
  return mylite_stored_program_runtime_unsupported();
}

sp_condition_value *sp_pcontext::find_condition(const LEX_CSTRING *,
                                                bool) const
{
  return nullptr;
}

sp_condition_value *sp_pcontext::find_declared_or_predefined_condition(
    THD *, const LEX_CSTRING *) const
{
  mylite_stored_program_runtime_unsupported();
  return nullptr;
}

sp_handler *sp_pcontext::add_handler(THD *, sp_handler::enum_type)
{
  mylite_stored_program_runtime_unsupported();
  return nullptr;
}

bool sp_pcontext::check_duplicate_handler(
    const sp_condition_value *) const
{
  return false;
}

sp_handler *sp_pcontext::find_handler(
    const Sql_condition_identity &) const
{
  return nullptr;
}

bool sp_pcontext::add_cursor(const LEX_CSTRING *, sp_pcontext *,
                             sp_lex_cursor *)
{
  return mylite_stored_program_runtime_unsupported();
}

const sp_pcursor *sp_pcontext::find_cursor(const LEX_CSTRING *, uint *,
                                           bool) const
{
  return nullptr;
}

const sp_pcursor *sp_pcontext::find_cursor(uint) const
{
  return nullptr;
}

bool sp_pcontext::add_record(THD *, const Lex_ident_column &,
                             Row_definition_list *)
{
  return mylite_stored_program_runtime_unsupported();
}

sp_record *sp_pcontext::find_record(const LEX_CSTRING *, bool) const
{
  return nullptr;
}

const Spvar_definition *sp_variable::find_row_field(const LEX_CSTRING *,
                                                    const LEX_CSTRING *,
                                                    uint *)
{
  return nullptr;
}

bool Row_definition_list::append_uniq(MEM_ROOT *mem_root,
                                      Spvar_definition *var)
{
  return push_back(var, mem_root);
}

bool Row_definition_list::adjust_formal_params_to_actual_params(THD *,
                                                                List<Item> *)
{
  return false;
}

bool Row_definition_list::adjust_formal_params_to_actual_params(THD *,
                                                                Item **,
                                                                uint)
{
  return false;
}

bool sp_condition_value::equals(const sp_condition_value *cv) const
{
  return this == cv;
}

bool sp_condition_value::matches(const Sql_condition_identity &,
                                 const sp_condition_value *) const
{
  return false;
}

bool sp_pcursor::check_param_count_with_error(uint) const
{
  return mylite_stored_program_runtime_unsupported();
}

sp_rcontext *Sp_rcontext_handler_local::get_rcontext(sp_rcontext *ctx) const
{
  return ctx;
}

sp_rcontext *Sp_rcontext_handler_package_body::get_rcontext(
    sp_rcontext *) const
{
  mylite_stored_program_runtime_unsupported();
  return nullptr;
}

const LEX_CSTRING *Sp_rcontext_handler_local::get_name_prefix() const
{
  return &empty_clex_str;
}

const LEX_CSTRING *Sp_rcontext_handler_package_body::get_name_prefix() const
{
  static const LEX_CSTRING prefix= { STRING_WITH_LEN("PACKAGE_BODY.") };
  return &prefix;
}

sp_rcontext::~sp_rcontext()
{
}

sp_rcontext *sp_rcontext::create(THD *, sp_head *, const sp_pcontext *,
                                 Field *, Row_definition_list &)
{
  mylite_stored_program_runtime_unsupported();
  return nullptr;
}

int sp_rcontext::set_variable(THD *, uint, Item **)
{
  return mylite_stored_program_runtime_unsupported_int();
}

int sp_rcontext::set_variable_row_field(THD *, uint, uint, Item **)
{
  return mylite_stored_program_runtime_unsupported_int();
}

int sp_rcontext::set_variable_row_field_by_name(THD *, uint,
                                                const LEX_CSTRING &, Item **)
{
  return mylite_stored_program_runtime_unsupported_int();
}

int sp_rcontext::set_variable_row(THD *, uint, List<Item> &)
{
  return mylite_stored_program_runtime_unsupported_int();
}

bool sp_rcontext::find_row_field_by_name_or_error(uint *, uint,
                                                  const LEX_CSTRING &)
{
  return mylite_stored_program_runtime_unsupported();
}

bool sp_rcontext::set_return_value(THD *, Item **)
{
  return mylite_stored_program_runtime_unsupported();
}

bool sp_rcontext::push_handler(sp_instr_hpush_jump *)
{
  return mylite_stored_program_runtime_unsupported();
}

void sp_rcontext::pop_handlers(size_t)
{
}

bool sp_rcontext::handle_sql_condition(THD *, uint *, const sp_instr *)
{
  return false;
}

uint sp_rcontext::exit_handler(Diagnostics_area *)
{
  return 0;
}

void sp_rcontext::push_cursor(sp_cursor *)
{
}

void sp_rcontext::pop_cursor(THD *)
{
}

void sp_rcontext::pop_cursors(THD *, size_t)
{
}

bool sp_rcontext::set_case_expr(THD *, int, Item **)
{
  return mylite_stored_program_runtime_unsupported();
}

bool sp_rcontext::alloc_arrays(THD *)
{
  return mylite_stored_program_runtime_unsupported();
}

bool sp_rcontext::init_var_table(THD *, List<Spvar_definition> &)
{
  return mylite_stored_program_runtime_unsupported();
}

bool sp_rcontext::init_var_items(THD *, List<Spvar_definition> &)
{
  return mylite_stored_program_runtime_unsupported();
}

Item_cache *sp_rcontext::create_case_expr_holder(THD *, const Item *) const
{
  mylite_stored_program_runtime_unsupported();
  return nullptr;
}

Virtual_tmp_table *sp_rcontext::virtual_tmp_table_for_row(uint)
{
  return nullptr;
}

int sp_cursor::open(THD *)
{
  return mylite_stored_program_runtime_unsupported_int();
}

int sp_cursor::close(THD *)
{
  return mylite_stored_program_runtime_unsupported_int();
}

void sp_cursor::destroy()
{
}

int sp_cursor::fetch(THD *, List<sp_fetch_target> *, bool)
{
  return mylite_stored_program_runtime_unsupported_int();
}

bool sp_cursor::export_structure(THD *, Row_definition_list *)
{
  return mylite_stored_program_runtime_unsupported();
}

bool sp_cursor::Select_fetch_into_spvars::send_data_to_variable_list(
    List<sp_fetch_target> &, List<Item> &)
{
  return mylite_stored_program_runtime_unsupported();
}

int sp_cursor::Select_fetch_into_spvars::prepare(List<Item> &,
                                                 SELECT_LEX_UNIT *)
{
  return mylite_stored_program_runtime_unsupported_int();
}

int sp_cursor::Select_fetch_into_spvars::send_data(List<Item> &)
{
  return mylite_stored_program_runtime_unsupported_int();
}

bool Item_splocal::append_value_for_log(THD *, String *)
{
  return mylite_stored_program_runtime_unsupported();
}

bool Item_splocal::append_for_log(THD *, String *)
{
  return mylite_stored_program_runtime_unsupported();
}

bool Item_splocal_row_field::append_for_log(THD *, String *)
{
  return mylite_stored_program_runtime_unsupported();
}

int sp_instr::exec_open_and_lock_tables(THD *, TABLE_LIST *)
{
  return mylite_stored_program_runtime_unsupported_int();
}

uint sp_instr::get_cont_dest() const
{
  return 0;
}

int sp_instr::exec_core(THD *, uint *nextp)
{
  mylite_stored_program_runtime_next(nextp, m_ip + 1);
  return mylite_stored_program_runtime_unsupported_int();
}

void sp_lex_instr::get_query(String *) const
{
}

LEX *sp_lex_instr::parse_expr(THD *, sp_head *, LEX *)
{
  mylite_stored_program_runtime_unsupported();
  return nullptr;
}

void sp_lex_instr::put_back_item_params(THD *, LEX *,
                                        const List<Item_param> &)
{
}

int sp_lex_keeper::reset_lex_and_exec_core(THD *, uint *nextp, bool,
                                           sp_instr *instr, bool)
{
  mylite_stored_program_runtime_next(nextp, instr ? instr->m_ip + 1 : 0);
  return mylite_stored_program_runtime_unsupported_int();
}

int sp_lex_keeper::validate_lex_and_exec_core(THD *, uint *nextp, bool,
                                              sp_lex_instr *instr)
{
  mylite_stored_program_runtime_next(nextp, instr ? instr->m_ip + 1 : 0);
  return mylite_stored_program_runtime_unsupported_int();
}

int sp_lex_keeper::cursor_reset_lex_and_exec_core(THD *, uint *nextp,
                                                  bool, sp_lex_instr *instr)
{
  mylite_stored_program_runtime_next(nextp, instr ? instr->m_ip + 1 : 0);
  return mylite_stored_program_runtime_unsupported_int();
}

LEX *sp_lex_keeper::parse_expr(THD *, const sp_head *)
{
  mylite_stored_program_runtime_unsupported();
  return nullptr;
}

int sp_instr_stmt::execute(THD *, uint *nextp)
{
  mylite_stored_program_runtime_next(nextp, m_ip + 1);
  return mylite_stored_program_runtime_unsupported_int();
}

int sp_instr_stmt::exec_core(THD *, uint *nextp)
{
  mylite_stored_program_runtime_next(nextp, m_ip + 1);
  return mylite_stored_program_runtime_unsupported_int();
}

void sp_instr_stmt::print(String *)
{
}

int sp_instr_set::execute(THD *, uint *nextp)
{
  mylite_stored_program_runtime_next(nextp, m_ip + 1);
  return mylite_stored_program_runtime_unsupported_int();
}

int sp_instr_set::exec_core(THD *, uint *nextp)
{
  mylite_stored_program_runtime_next(nextp, m_ip + 1);
  return mylite_stored_program_runtime_unsupported_int();
}

void sp_instr_set::print(String *)
{
}

sp_rcontext *sp_instr_set::get_rcontext(THD *) const
{
  mylite_stored_program_runtime_unsupported();
  return nullptr;
}

int sp_instr_set_default_param::execute(THD *, uint *nextp)
{
  mylite_stored_program_runtime_next(nextp, m_ip + 1);
  return mylite_stored_program_runtime_unsupported_int();
}

void sp_instr_set_default_param::print(String *)
{
}

int sp_instr_set_row_field::exec_core(THD *, uint *nextp)
{
  mylite_stored_program_runtime_next(nextp, m_ip + 1);
  return mylite_stored_program_runtime_unsupported_int();
}

void sp_instr_set_row_field::print(String *)
{
}

int sp_instr_set_row_field_by_name::exec_core(THD *, uint *nextp)
{
  mylite_stored_program_runtime_next(nextp, m_ip + 1);
  return mylite_stored_program_runtime_unsupported_int();
}

void sp_instr_set_row_field_by_name::print(String *)
{
}

int sp_instr_set_trigger_field::execute(THD *, uint *nextp)
{
  mylite_stored_program_runtime_next(nextp, m_ip + 1);
  return mylite_stored_program_runtime_unsupported_int();
}

int sp_instr_set_trigger_field::exec_core(THD *, uint *nextp)
{
  mylite_stored_program_runtime_next(nextp, m_ip + 1);
  return mylite_stored_program_runtime_unsupported_int();
}

void sp_instr_set_trigger_field::print(String *)
{
}

bool sp_instr_set_trigger_field::on_after_expr_parsing(THD *)
{
  return mylite_stored_program_runtime_unsupported();
}

int sp_instr_jump::execute(THD *, uint *nextp)
{
  mylite_stored_program_runtime_next(nextp, m_dest);
  return mylite_stored_program_runtime_unsupported_int();
}

void sp_instr_jump::print(String *)
{
}

uint sp_instr_jump::opt_mark(sp_head *, List<sp_instr> *)
{
  marked= 1;
  return m_dest;
}

uint sp_instr_jump::opt_shortcut_jump(sp_head *, sp_instr *)
{
  return m_dest;
}

void sp_instr_jump::opt_move(uint dst, List<sp_instr_opt_meta> *)
{
  m_ip= dst;
}

int sp_instr_jump_if_not::execute(THD *, uint *nextp)
{
  mylite_stored_program_runtime_next(nextp, m_dest);
  return mylite_stored_program_runtime_unsupported_int();
}

int sp_instr_jump_if_not::exec_core(THD *, uint *nextp)
{
  mylite_stored_program_runtime_next(nextp, m_dest);
  return mylite_stored_program_runtime_unsupported_int();
}

void sp_instr_jump_if_not::print(String *)
{
}

uint sp_instr_jump_if_not::opt_mark(sp_head *, List<sp_instr> *)
{
  marked= 1;
  return m_dest;
}

void sp_instr_jump_if_not::opt_move(uint dst, List<sp_instr_opt_meta> *)
{
  m_ip= dst;
}

int sp_instr_preturn::execute(THD *, uint *nextp)
{
  mylite_stored_program_runtime_next(nextp, UINT_MAX);
  return mylite_stored_program_runtime_unsupported_int();
}

void sp_instr_preturn::print(String *)
{
}

int sp_instr_freturn::execute(THD *, uint *nextp)
{
  mylite_stored_program_runtime_next(nextp, UINT_MAX);
  return mylite_stored_program_runtime_unsupported_int();
}

int sp_instr_freturn::exec_core(THD *, uint *nextp)
{
  mylite_stored_program_runtime_next(nextp, UINT_MAX);
  return mylite_stored_program_runtime_unsupported_int();
}

void sp_instr_freturn::print(String *)
{
}

int sp_instr_hpush_jump::execute(THD *, uint *nextp)
{
  mylite_stored_program_runtime_next(nextp, m_dest);
  return mylite_stored_program_runtime_unsupported_int();
}

void sp_instr_hpush_jump::print(String *)
{
}

uint sp_instr_hpush_jump::opt_mark(sp_head *, List<sp_instr> *)
{
  marked= 1;
  return m_dest;
}

int sp_instr_hpop::execute(THD *, uint *nextp)
{
  mylite_stored_program_runtime_next(nextp, m_ip + 1);
  return mylite_stored_program_runtime_unsupported_int();
}

void sp_instr_hpop::print(String *)
{
}

int sp_instr_hreturn::execute(THD *, uint *nextp)
{
  mylite_stored_program_runtime_next(nextp, m_dest);
  return mylite_stored_program_runtime_unsupported_int();
}

void sp_instr_hreturn::print(String *)
{
}

uint sp_instr_hreturn::opt_mark(sp_head *, List<sp_instr> *)
{
  marked= 1;
  return m_dest;
}

int sp_instr_cpush::execute(THD *, uint *nextp)
{
  mylite_stored_program_runtime_next(nextp, m_ip + 1);
  return mylite_stored_program_runtime_unsupported_int();
}

int sp_instr_cpush::exec_core(THD *, uint *nextp)
{
  mylite_stored_program_runtime_next(nextp, m_ip + 1);
  return mylite_stored_program_runtime_unsupported_int();
}

void sp_instr_cpush::print(String *)
{
}

int sp_instr_cpop::execute(THD *, uint *nextp)
{
  mylite_stored_program_runtime_next(nextp, m_ip + 1);
  return mylite_stored_program_runtime_unsupported_int();
}

void sp_instr_cpop::print(String *)
{
}

int sp_instr_copen::execute(THD *, uint *nextp)
{
  mylite_stored_program_runtime_next(nextp, m_ip + 1);
  return mylite_stored_program_runtime_unsupported_int();
}

void sp_instr_copen::print(String *)
{
}

int sp_instr_cursor_copy_struct::execute(THD *, uint *nextp)
{
  mylite_stored_program_runtime_next(nextp, m_ip + 1);
  return mylite_stored_program_runtime_unsupported_int();
}

int sp_instr_cursor_copy_struct::exec_core(THD *, uint *nextp)
{
  mylite_stored_program_runtime_next(nextp, m_ip + 1);
  return mylite_stored_program_runtime_unsupported_int();
}

void sp_instr_cursor_copy_struct::print(String *)
{
}

int sp_instr_cclose::execute(THD *, uint *nextp)
{
  mylite_stored_program_runtime_next(nextp, m_ip + 1);
  return mylite_stored_program_runtime_unsupported_int();
}

void sp_instr_cclose::print(String *)
{
}

int sp_instr_cfetch::execute(THD *, uint *nextp)
{
  mylite_stored_program_runtime_next(nextp, m_ip + 1);
  return mylite_stored_program_runtime_unsupported_int();
}

void sp_instr_cfetch::print(String *)
{
}

int sp_instr_agg_cfetch::execute(THD *, uint *nextp)
{
  mylite_stored_program_runtime_next(nextp, m_ip + 1);
  return mylite_stored_program_runtime_unsupported_int();
}

void sp_instr_agg_cfetch::print(String *)
{
}

int sp_instr_error::execute(THD *, uint *nextp)
{
  mylite_stored_program_runtime_next(nextp, UINT_MAX);
  return mylite_stored_program_runtime_unsupported_int();
}

void sp_instr_error::print(String *)
{
}

int sp_instr_set_case_expr::execute(THD *, uint *nextp)
{
  mylite_stored_program_runtime_next(nextp, m_cont_dest);
  return mylite_stored_program_runtime_unsupported_int();
}

int sp_instr_set_case_expr::exec_core(THD *, uint *nextp)
{
  mylite_stored_program_runtime_next(nextp, m_cont_dest);
  return mylite_stored_program_runtime_unsupported_int();
}

void sp_instr_set_case_expr::print(String *)
{
}

uint sp_instr_set_case_expr::opt_mark(sp_head *, List<sp_instr> *)
{
  marked= 1;
  return m_ip + 1;
}

void sp_instr_set_case_expr::opt_move(uint dst, List<sp_instr_opt_meta> *)
{
  m_ip= dst;
}

static bool mylite_stored_program_runtime_unsupported()
{
  my_error(ER_NOT_SUPPORTED_YET, MYF(0),
           "stored program runtime in MyLite embedded profile");
  return true;
}

static int mylite_stored_program_runtime_unsupported_int()
{
  return mylite_stored_program_runtime_unsupported() ? 1 : 0;
}

static void mylite_stored_program_runtime_unsupported_void()
{
  mylite_stored_program_runtime_unsupported();
}

static void mylite_stored_program_runtime_next(uint *nextp, uint next)
{
  if (nextp)
    *nextp= next;
}
