
#include <libgccjit.h>

struct TB_GCCJIT_Module {
    TB_Module *tb;
    gcc_jit_context *ctx;
    size_t nfuncs;
};

struct TB_GCCJIT_Function {
    TB_GCCJIT_Module *mod;
    gcc_jit_function *func;
    const char *name;
};

typedef struct TB_GCCJIT_Context {
    TB_GCCJIT_Module *mod;
    NL_Table values;
    NL_Table blocks;
    NL_Table phi;
    TB_Function *tb_func;
    size_t gcc_func_num_return_fields;
    gcc_jit_field ** gcc_func_return_fields;
    gcc_jit_type *gcc_func_return;
    gcc_jit_function *gcc_func;
} TB_GCCJIT_Context;

static gcc_jit_rvalue *tb_gcc_get(NL_Table *table, TB_Node *key) {
    gcc_jit_rvalue *ret = nl_table_get(table, (void *) (size_t) key->gvn);
    if (ret == NULL) {
        fprintf(stderr, "gvn not found: %zu\n", key->gvn);
        tb_todo();
    }
    return ret;
}

static void tb_gcc_set(NL_Table *table, TB_Node *key, gcc_jit_rvalue *value) {
    nl_table_put(table, (void *) (size_t) key->gvn, value);
}

static gcc_jit_type *tb_gcc_type(TB_GCCJIT_Context *ctx, TB_DataType dt) {
    switch (dt.type) {
        case TB_INT: {
            if (dt.data == 0) {
                return gcc_jit_context_get_type(ctx->mod->ctx, GCC_JIT_TYPE_VOID);
            }
            if (dt.data == 1) {
                return gcc_jit_context_get_type(ctx->mod->ctx, GCC_JIT_TYPE_BOOL);
            }
            if (dt.data <= 8) {
                return gcc_jit_context_get_type(ctx->mod->ctx, GCC_JIT_TYPE_INT8_T);
            }
            if (dt.data <= 16) {
                return gcc_jit_context_get_type(ctx->mod->ctx, GCC_JIT_TYPE_INT16_T);
            }
            if (dt.data <= 32) {
                return gcc_jit_context_get_type(ctx->mod->ctx, GCC_JIT_TYPE_INT32_T);
            }
            if (dt.data <= 64) {
                return gcc_jit_context_get_type(ctx->mod->ctx, GCC_JIT_TYPE_INT64_T);
            }
            break;
        }
        case TB_PTR: {
            if (dt.data == 0) {
                return gcc_jit_context_get_type(ctx->mod->ctx, GCC_JIT_TYPE_VOID_PTR);
            }
            break;
        }
        case TB_FLOAT32: {
            return gcc_jit_context_get_type(ctx->mod->ctx, GCC_JIT_TYPE_FLOAT);
        }
        case TB_FLOAT64: {
            return gcc_jit_context_get_type(ctx->mod->ctx, GCC_JIT_TYPE_DOUBLE);
        }
        default: {
            break;
        }
    }
    tb_todo();
}

static gcc_jit_block *tb_gcc_branch_ext(TB_GCCJIT_Context *ctx, TB_Node *n, TB_Node *target) {
    gcc_jit_block *base = nl_table_get(&ctx->blocks, target);
    if (!cfg_is_region(target)) {
        return base;
    }

    char ret_name[24];
    snprintf(ret_name, 23, "bb_phi_%zu", (size_t) n->gvn);
    gcc_jit_block *ret = gcc_jit_function_new_block(ctx->gcc_func, ret_name);
    
    int phi_i = -1;
    FOR_USERS(u, n) {
        if (cfg_is_region(USERN(u))) {
            phi_i = 1 + USERI(u);
            break;
        }
    }

    FOR_USERS(u, target) {
        TB_Node *dest = USERN(u);
        if (dest->type == TB_PHI) {
            assert(phi_i >= 0);
            if (dest->inputs[phi_i] != NULL) {
                if (dest->inputs[phi_i]->dt.type != TB_CONTROL && dest->inputs[phi_i]->dt.type != TB_MEMORY) {
                    TB_Node *src = dest->inputs[phi_i];
                    gcc_jit_lvalue *lval = nl_table_get(&ctx->phi, dest);
                    if (lval == NULL) {
                        char dest_name[24];
                        snprintf(dest_name, 23, "phi_%zu", (size_t) dest->gvn);
                        lval = gcc_jit_function_new_local(
                            ctx->gcc_func,
                            NULL,
                            tb_gcc_type(ctx, dest->dt),
                            dest_name
                        );
                        nl_table_put(&ctx->phi, dest, lval);
                    }
                    gcc_jit_block_add_assignment(
                        ret,
                        NULL,
                        lval,
                        tb_gcc_get(&ctx->values, src)
                    );
                }
            }
        }
    }

    gcc_jit_block_end_with_jump(ret, NULL, base);

    return ret;
}

static gcc_jit_block *tb_gcc_branch(TB_GCCJIT_Context *ctx, TB_Node *n) {
    return tb_gcc_branch_ext(ctx, n, cfg_next_bb_after_cproj(n));
}

static gcc_jit_block *tb_gcc_branch_fall(TB_GCCJIT_Context *ctx, TB_Node *n) {
    return tb_gcc_branch_ext(ctx, n, cfg_next_control(n));
}

static void tb_gcc_block(TB_GCCJIT_Context *ctx, gcc_jit_block *block, TB_CFG *cfg, TB_Worklist *ws, TB_Node* start) {
    TB_BasicBlock* bb = ctx->tb_func->scheduled[start->gvn];

    size_t foreach_start = dyn_array_length(ws->items);
    tb_greedy_scheduler(ctx->tb_func, cfg, ws, NULL, bb);
    size_t foreach_end = dyn_array_length(ws->items);

    FOR_USERS(u, start) {
        TB_Node *dest = USERN(u);
        if (dest->dt.type == TB_CONTROL || dest->dt.type == TB_MEMORY || dest->dt.type == TB_TUPLE) {
            continue;
        }
        gcc_jit_lvalue *lval = nl_table_get(&ctx->phi, dest);
        if (lval == NULL) {
            char dest_name[24];
            snprintf(dest_name, 23, "phi_%zu", (size_t) dest->gvn);
            lval = gcc_jit_function_new_local(
                ctx->gcc_func,
                NULL,
                tb_gcc_type(ctx, dest->dt),
                dest_name
            );
            nl_table_put(&ctx->phi, dest, lval);
        }
        tb_gcc_set(
            &ctx->values,
            dest,
            gcc_jit_lvalue_as_rvalue(lval)
        );
    }

    FOR_N(i, foreach_start, foreach_end) {
        TB_Node* n = ws->items[i];

        if (n->type == TB_MERGEMEM || n->type == TB_SPLITMEM || n->type == TB_NULL
            || n->type == TB_PHI || n->type == TB_PROJ || n->type == TB_BRANCH_PROJ
            || n->type == TB_REGION) {
            continue;
        }

        switch (n->type) {
            case TB_SYMBOL: {
                TB_Symbol* sym = TB_NODE_GET_EXTRA_T(n, TB_NodeSymbol)->sym;
                if (ctx->mod->tb->is_jit) {
                    tb_gcc_set(
                        &ctx->values,
                        n,
                        gcc_jit_context_new_rvalue_from_ptr(
                            ctx->mod->ctx,
                            gcc_jit_context_get_type(ctx->mod->ctx, GCC_JIT_TYPE_VOID_PTR),
                            sym->address
                        )
                    );
                } else {
                    goto fail;
                }

                break;
            }
            case TB_INTEGER_CONST: {
                TB_NodeInt* num = TB_NODE_GET_EXTRA(n);

                if (n->dt.type == TB_PTR) {
                    tb_gcc_set(
                        &ctx->values,
                        n,
                        gcc_jit_context_new_rvalue_from_ptr(
                            ctx->mod->ctx,
                            tb_gcc_type(ctx, n->dt),
                            (void *) num->value
                        )
                    );
                } else {
                    tb_gcc_set(
                        &ctx->values,
                        n,
                        gcc_jit_context_new_rvalue_from_long(
                            ctx->mod->ctx,
                            tb_gcc_type(ctx, n->dt),
                            (long) num->value
                        )
                    );
                }
                break;
            }
            case TB_ADD:
            case TB_SUB:
            case TB_MUL:
            case TB_SDIV:
            case TB_UDIV:
            case TB_FDIV:
            case TB_SMOD:
            case TB_UMOD: {
                static const char table[] = {
                    [TB_ADD] = GCC_JIT_BINARY_OP_PLUS,
                    [TB_SUB] = GCC_JIT_BINARY_OP_MINUS,
                    [TB_MUL] = GCC_JIT_BINARY_OP_MULT,
                    [TB_SDIV] = GCC_JIT_BINARY_OP_DIVIDE,
                    [TB_UDIV] = GCC_JIT_BINARY_OP_DIVIDE,
                    [TB_FDIV] = GCC_JIT_BINARY_OP_DIVIDE,
                    [TB_SMOD] = GCC_JIT_BINARY_OP_MODULO,
                    [TB_UMOD] = GCC_JIT_BINARY_OP_MODULO,
                };

                TB_Node *lhs = n->inputs[n->input_count-2];
                TB_Node *rhs = n->inputs[n->input_count-1];

                tb_gcc_set(
                    &ctx->values,
                    n,
                    gcc_jit_context_new_binary_op(
                        ctx->mod->ctx,
                        NULL,
                        table[n->type],
                        tb_gcc_type(ctx, n->dt),
                        tb_gcc_get(&ctx->values, lhs),
                        tb_gcc_get(&ctx->values, rhs)
                    )
                );

                break;
            }

            case TB_CMP_EQ:
            case TB_CMP_NE:
            case TB_CMP_SLT:
            case TB_CMP_SLE:
            case TB_CMP_ULT:
            case TB_CMP_ULE:
            case TB_CMP_FLT:
            case TB_CMP_FLE: {
                static const char table[] = {
                    [TB_CMP_EQ] = GCC_JIT_COMPARISON_EQ,
                    [TB_CMP_NE] = GCC_JIT_COMPARISON_NE,
                    [TB_CMP_SLT] = GCC_JIT_COMPARISON_LT,
                    [TB_CMP_SLE] = GCC_JIT_COMPARISON_LE,
                    [TB_CMP_ULT] = GCC_JIT_COMPARISON_LT,
                    [TB_CMP_ULE] = GCC_JIT_COMPARISON_LE,
                    [TB_CMP_FLT] = GCC_JIT_COMPARISON_LT,
                    [TB_CMP_FLE] = GCC_JIT_COMPARISON_LE,
                };

                TB_Node *lhs = n->inputs[n->input_count-2];
                TB_Node *rhs = n->inputs[n->input_count-1];

                tb_gcc_set(
                    &ctx->values,
                    n,
                    gcc_jit_context_new_comparison(
                        ctx->mod->ctx,
                        NULL,
                        table[n->type],
                        tb_gcc_get(&ctx->values, lhs),
                        tb_gcc_get(&ctx->values, rhs)
                    )
                );

                break;
            }
            case TB_ZERO_EXT: {
                TB_Node *src = n->inputs[n->input_count-1];
                
                gcc_jit_rvalue *rval = tb_gcc_get(&ctx->values, src);
                if (n->dt.type == TB_PTR && src->dt.type != TB_PTR) {
                    rval = gcc_jit_context_new_bitcast(
                        ctx->mod->ctx,
                        NULL,
                        gcc_jit_context_new_cast(
                            ctx->mod->ctx,
                            NULL,
                            rval,
                            gcc_jit_context_get_type(ctx->mod->ctx, GCC_JIT_TYPE_SIZE_T)
                        ),
                        gcc_jit_context_get_type(ctx->mod->ctx, GCC_JIT_TYPE_VOID_PTR)
                    );
                }
                if (n->dt.type != TB_PTR && src->dt.type == TB_PTR) {
                    rval = gcc_jit_context_new_bitcast(
                        ctx->mod->ctx,
                        NULL,
                        rval,
                        gcc_jit_context_get_type(ctx->mod->ctx, GCC_JIT_TYPE_SIZE_T)
                    );
                }

                tb_gcc_set(
                    &ctx->values,
                    n,
                    gcc_jit_context_new_cast(
                        ctx->mod->ctx,
                        NULL,
                        rval,
                        tb_gcc_type(ctx, n->dt)
                    )
                );
                break;
            }
            case TB_MEMBER_ACCESS: {
                TB_NodeMember *extra = TB_NODE_GET_EXTRA(n);

                TB_Node *src = n->inputs[n->input_count-1];
                
                tb_gcc_set(
                    &ctx->values,
                    n,
                    gcc_jit_lvalue_get_address(
                        gcc_jit_context_new_array_access(
                            ctx->mod->ctx,
                            NULL,
                            gcc_jit_context_new_cast(
                                ctx->mod->ctx,
                                NULL,
                                tb_gcc_get(&ctx->values, src),
                                gcc_jit_context_get_type(ctx->mod->ctx, GCC_JIT_TYPE_CONST_CHAR_PTR)
                            ),
                            gcc_jit_context_new_rvalue_from_int(
                                ctx->mod->ctx,
                                gcc_jit_context_get_type(ctx->mod->ctx, GCC_JIT_TYPE_SIZE_T),
                                extra->offset
                            )
                        ),
                        NULL
                    )
                );
                break;
            }
            case TB_ARRAY_ACCESS: {
                TB_NodeArray *extra = TB_NODE_GET_EXTRA(n);

                TB_Node *array = n->inputs[n->input_count-2];
                TB_Node *nth = n->inputs[n->input_count-1];
                
                tb_gcc_set(
                    &ctx->values,
                    n,
                    gcc_jit_lvalue_get_address(
                        gcc_jit_context_new_array_access(
                            ctx->mod->ctx,
                            NULL,
                            gcc_jit_context_new_cast(
                                ctx->mod->ctx,
                                NULL,
                                tb_gcc_get(&ctx->values, array),
                                gcc_jit_context_get_type(ctx->mod->ctx, GCC_JIT_TYPE_CONST_CHAR_PTR)
                            ),
                            gcc_jit_context_new_binary_op(
                                ctx->mod->ctx,
                                NULL,
                                GCC_JIT_BINARY_OP_MULT,
                                gcc_jit_context_get_type(ctx->mod->ctx, GCC_JIT_TYPE_SIZE_T),
                                gcc_jit_context_new_cast(
                                    ctx->mod->ctx,
                                    NULL,
                                    tb_gcc_get(&ctx->values, nth),
                                    gcc_jit_context_get_type(ctx->mod->ctx, GCC_JIT_TYPE_SIZE_T)
                                ),
                                gcc_jit_context_new_rvalue_from_int(
                                    ctx->mod->ctx,
                                    gcc_jit_context_get_type(ctx->mod->ctx, GCC_JIT_TYPE_SIZE_T),
                                    extra->stride
                                )
                            )
                        ),
                        NULL
                    )
                );

                // \n", gcc_jit_object_get_debug_string(gcc_jit_rvalue_as_object(tb_gcc_get(&ctx->values, n))));

                break;
            }
            case TB_BITCAST: {
                TB_Node *src = n->inputs[n->input_count-1];

                gcc_jit_field *src_field = gcc_jit_context_new_field(
                    ctx->mod->ctx,
                    NULL,
                    tb_gcc_type(ctx, src->dt),
                    "value_in"
                );

                gcc_jit_field *out_field = gcc_jit_context_new_field(
                    ctx->mod->ctx,
                    NULL,
                    tb_gcc_type(ctx, n->dt),
                    "value_out"
                );

                gcc_jit_field *fields[2] = { src_field, out_field };

                char type_name[24];
                snprintf(type_name, 23, "local_%zu_t", (size_t) n->gvn);
                gcc_jit_type *join = gcc_jit_context_new_union_type(
                    ctx->mod->ctx,
                    NULL,
                    type_name,
                    2,
                    fields
                );

                char local_name[24];
                snprintf(local_name, 23, "local_%zu", (size_t) n->gvn);
                gcc_jit_lvalue *local = gcc_jit_function_new_local(ctx->gcc_func, NULL, join, local_name);

                gcc_jit_block_add_assignment(
                    block,
                    NULL,
                    gcc_jit_lvalue_access_field(local, NULL, src_field),
                    tb_gcc_get(&ctx->values, src)
                );

                tb_gcc_set(
                    &ctx->values,
                    n,
                    gcc_jit_lvalue_as_rvalue(
                        gcc_jit_lvalue_access_field(local, NULL, out_field)
                    )
                );

                break;
            }
            case TB_STORE: {
                TB_Node *dest = n->inputs[n->input_count-2];
                TB_Node *src = n->inputs[n->input_count-1];

                gcc_jit_block_add_assignment(
                    block,
                    NULL,
                    gcc_jit_rvalue_dereference(
                        gcc_jit_context_new_bitcast(
                            ctx->mod->ctx,
                            NULL,
                            tb_gcc_get(&ctx->values, dest),
                            gcc_jit_type_get_pointer(
                                tb_gcc_type(ctx, src->dt)
                            )
                        ),
                        NULL
                    ),
                    tb_gcc_get(&ctx->values, src)
                );
                break;
            }
            case TB_LOAD: {
                TB_Node *src = n->inputs[n->input_count-1];
                char name[24];
                snprintf(name, 23, "local_%zu", (size_t) n->gvn);

                gcc_jit_lvalue *local = gcc_jit_function_new_local(
                    ctx->gcc_func,
                    NULL,
                    tb_gcc_type(ctx, n->dt),
                    name
                );
                
                gcc_jit_rvalue *v1;
                
                gcc_jit_block_add_assignment(
                    block,
                    NULL,
                    local,
                    v1 = gcc_jit_lvalue_as_rvalue(
                        gcc_jit_rvalue_dereference(
                            gcc_jit_context_new_cast(
                                ctx->mod->ctx,
                                NULL,
                                tb_gcc_get(&ctx->values, src),
                                gcc_jit_type_get_pointer(
                                    tb_gcc_type(ctx, n->dt)
                                )
                            ),
                            NULL
                        )
                    )
                );

                // printf("%s %s = %s\n", gcc_jit_object_get_debug_string(gcc_jit_type_as_object(tb_gcc_type(ctx, n->dt))), gcc_jit_object_get_debug_string(gcc_jit_lvalue_as_object(local)), gcc_jit_object_get_debug_string(gcc_jit_rvalue_as_object(v1)));

                tb_gcc_set(
                    &ctx->values,
                    n,
                    gcc_jit_lvalue_as_rvalue(
                        local
                    )
                );

                break;
            }
            case TB_LOCAL: {
                char name[24];
                snprintf(name, 23, "local_%zu", (size_t) n->gvn);
                TB_NodeLocal* l = TB_NODE_GET_EXTRA(n);
                tb_gcc_set(
                    &ctx->values,
                    n,
                    gcc_jit_lvalue_get_address(
                        gcc_jit_function_new_local(
                            ctx->gcc_func,
                            NULL,
                            gcc_jit_context_new_array_type(
                                ctx->mod->ctx,
                                NULL,
                                gcc_jit_context_get_type(ctx->mod->ctx, GCC_JIT_TYPE_CHAR),
                                l->size
                            ),
                            name
                        ),
                        NULL
                    )
                );
                break;
            }
            case TB_RETURN: {
                size_t count = n->input_count - 3;

                // printf("%zu == %zu\n", ctx->gcc_func_num_return_fields, count);

                if (ctx->gcc_func_num_return_fields == 0) {
                    gcc_jit_block_end_with_void_return(block, NULL);
                } else if (ctx->gcc_func_num_return_fields == 1) {
                    TB_Node *src = n->inputs[n->input_count - 1];
                    
                    gcc_jit_block_end_with_return(
                        block,
                        NULL, 
                        tb_gcc_get(&ctx->values, src)
                    );
                } else if (ctx->gcc_func_num_return_fields == 2) {
                    TB_Node *car = n->inputs[n->input_count - 2];
                    TB_Node *cdr = n->inputs[n->input_count - 1];

                    gcc_jit_rvalue *values[2] = {
                        tb_gcc_get(&ctx->values, car),
                        tb_gcc_get(&ctx->values, cdr),
                    };

                    gcc_jit_block_end_with_return(
                        block,
                        NULL, 
                        gcc_jit_context_new_struct_constructor(
                            ctx->mod->ctx,
                            NULL,
                            ctx->gcc_func_return,
                            ctx->gcc_func_num_return_fields,
                            ctx->gcc_func_return_fields,
                            values
                        )
                    );
                }
                break;
            }
            case TB_CALL: {
                TB_Node *func = n->inputs[2];

                TB_Node* projs[2] = { NULL, NULL };
                FOR_USERS(use, n) {
                    if (USERN(use)->type == TB_PROJ) {
                        int index = TB_NODE_GET_EXTRA_T(USERN(use), TB_NodeProj)->index - 2;
                        if (0 <= index && index < 2) {
                            projs[index] = USERN(use);
                        }
                    }
                }
                gcc_jit_type *ret = NULL;
                
                gcc_jit_field *fields[2];
                
                if (projs[0] == NULL) {
                    ret = gcc_jit_context_get_type(ctx->mod->ctx, GCC_JIT_TYPE_VOID);
                } else if (projs[1] == NULL) {
                    ret = tb_gcc_type(ctx, projs[0]->dt);
                } else {
                    char buf[24];
                    snprintf(buf, 23, "ret_%zu", (size_t) n->gvn);
                    fields[0] = gcc_jit_context_new_field(ctx->mod->ctx, NULL, tb_gcc_type(ctx, projs[0]->dt), "member_1");
                    fields[1] = gcc_jit_context_new_field(ctx->mod->ctx, NULL, tb_gcc_type(ctx, projs[1]->dt), "member_2");
                    gcc_jit_struct *s = gcc_jit_context_new_struct_type(ctx->mod->ctx, NULL, buf, 2, fields);
                    ret = gcc_jit_struct_as_type(s);
                    // printf("%s %s\n", gcc_jit_object_get_debug_string(gcc_jit_type_as_object(tb_gcc_type(ctx, projs[0]->dt))), gcc_jit_object_get_debug_string(gcc_jit_field_as_object(fields[0])));
                    // printf("%s %s\n", gcc_jit_object_get_debug_string(gcc_jit_type_as_object(tb_gcc_type(ctx, projs[1]->dt))), gcc_jit_object_get_debug_string(gcc_jit_field_as_object(fields[1])));
                }
                
                size_t num_params = n->input_count - 3;
                gcc_jit_type **params = tb_platform_heap_alloc(sizeof(gcc_jit_type *) * num_params);

                size_t num_args = num_params;
                gcc_jit_rvalue **args = tb_platform_heap_alloc(sizeof(gcc_jit_rvalue *) * num_args);

                FOR_N(i, 0, num_params) {
                    char buf[24];
                    snprintf(buf, 23, "arg_%zu", (size_t) i);
                    params[i] = tb_gcc_type(ctx, n->inputs[i + 3]->dt);

                    args[i] = tb_gcc_get(&ctx->values, n->inputs[i + 3]);
                }

                if (projs[0] == NULL) {
                    gcc_jit_block_add_eval(
                        block,
                        NULL,
                        gcc_jit_context_new_call_through_ptr(
                            ctx->mod->ctx,
                            NULL,
                            gcc_jit_context_new_bitcast(
                                ctx->mod->ctx,
                                NULL,
                                tb_gcc_get(&ctx->values, func),
                                gcc_jit_context_new_function_ptr_type(ctx->mod->ctx, NULL, ret, num_params, params, false)
                            ),
                            num_args,
                            args
                        )
                    );
                } else {
                    char ret_name[24];
                    snprintf(ret_name, 23, "local_%zu", (size_t) n->gvn);
                    gcc_jit_lvalue *local = gcc_jit_function_new_local(ctx->gcc_func, NULL, ret, ret_name);

                    gcc_jit_rvalue *v1;

                    gcc_jit_block_add_assignment(
                        block,
                        NULL,
                        local,
                        v1 = gcc_jit_context_new_call_through_ptr(
                            ctx->mod->ctx,
                            NULL,
                            gcc_jit_context_new_bitcast(
                                ctx->mod->ctx,
                                NULL,
                                tb_gcc_get(&ctx->values, func),
                                gcc_jit_context_new_function_ptr_type(ctx->mod->ctx, NULL, ret, num_params, params, false)
                            ),
                            num_args,
                            args
                        )
                    );

                    // printf("%s\n", gcc_jit_object_get_debug_string(gcc_jit_block_as_object(block)));
                    // printf("%s %s = %s\n", gcc_jit_object_get_debug_string(gcc_jit_type_as_object(ret)), gcc_jit_object_get_debug_string(gcc_jit_lvalue_as_object(local)), gcc_jit_object_get_debug_string(gcc_jit_rvalue_as_object(v1)));

                    if (projs[1] == NULL) {
                        tb_gcc_set(
                            &ctx->values,
                            projs[0],
                            gcc_jit_lvalue_as_rvalue(local)
                        );
                    } else {
                        for (size_t i = 0; i < 2; i++) {
                            tb_gcc_set(
                                &ctx->values,
                                projs[i],
                                gcc_jit_lvalue_as_rvalue(
                                    gcc_jit_lvalue_access_field(
                                        local,
                                        NULL,
                                        fields[i]
                                    )
                                )
                            );
                        }
                    }
                }

                break;
            }

            case TB_BRANCH: {
                TB_NodeBranch* br = TB_NODE_GET_EXTRA(n);
                TB_Node** succ = tb_platform_heap_alloc(br->succ_count * sizeof(TB_Node**));

                size_t succ_count = 0;
                FOR_USERS(u, n) {
                    if (USERN(u)->type == TB_BRANCH_PROJ) {
                        int index = TB_NODE_GET_EXTRA_T(USERN(u), TB_NodeBranchProj)->index;
                        succ[index] = USERN(u);
                        succ_count += 1;
                    }
                }

                if (succ_count == 2) {
                    gcc_jit_block_end_with_conditional(
                        block,
                        NULL,
                        tb_gcc_get(&ctx->values, n->inputs[n->input_count-1]),
                        tb_gcc_branch(ctx, succ[0]),
                        tb_gcc_branch(ctx, succ[1])
                    );
                } else {
                    goto fail;
                }

                break;
            }

        fail:;
            default: {
                fprintf(stderr, "internal unimplemented node type: %s\n", tb_node_get_name(n));
                tb_todo();
                break;
            } 
        }
    }

    if (!cfg_is_terminator(bb->end)) {
        gcc_jit_block_end_with_jump(
            block,
            NULL,
            tb_gcc_branch_fall(ctx, bb->end)
        );
    }
}


TB_API TB_GCCJIT_Module *tb_gcc_module_new(TB_Module *tb_mod) {
    TB_GCCJIT_Module *mod = tb_platform_heap_alloc(sizeof(TB_GCCJIT_Module));
    *mod = (TB_GCCJIT_Module) {
        .tb = tb_mod,
        .ctx = gcc_jit_context_acquire(),
        .nfuncs = 0,
    };
    return mod;
}


TB_API TB_GCCJIT_Function *tb_gcc_module_function(TB_GCCJIT_Module *mod, TB_Function* f, TB_Worklist* ws, TB_Arena* tmp) {
    TB_GCCJIT_Context ctx = (TB_GCCJIT_Context) {
        .mod = mod,
        .tb_func = f,
        .values = nl_table_alloc(128),
        .blocks = nl_table_alloc(8),
        .phi = nl_table_alloc(32),
    };

    ctx.gcc_func_num_return_fields = f->prototype->return_count;
    if (ctx.gcc_func_num_return_fields == 0) {
        ctx.gcc_func_return = gcc_jit_context_get_type(mod->ctx, GCC_JIT_TYPE_VOID);
    } else if (ctx.gcc_func_num_return_fields == 1) {
        ctx.gcc_func_return = tb_gcc_type(&ctx, f->prototype->params[f->prototype->param_count].dt);
    } else {
        ctx.gcc_func_return_fields = tb_platform_heap_alloc(sizeof(gcc_jit_type *) * ctx.gcc_func_num_return_fields);
        for (size_t i = 0; i < ctx.gcc_func_num_return_fields; i++) {
            char name[24];
            snprintf(name, 23, "ret_%zu", i);
            ctx.gcc_func_return_fields[i] = gcc_jit_context_new_field(
                mod->ctx,
                NULL,
                tb_gcc_type(&ctx, f->prototype->params[f->prototype->param_count + i].dt),
                name
            );
        }
        ctx.gcc_func_return = gcc_jit_struct_as_type(
            gcc_jit_context_new_struct_type(mod->ctx, NULL, "func_ret", ctx.gcc_func_num_return_fields, ctx.gcc_func_return_fields)
        );
    }
    size_t num_params = f->param_count;
    gcc_jit_param **params = tb_platform_heap_alloc(sizeof(gcc_jit_type *) * num_params);
    for (size_t i = 0; i < num_params; i++) {
        // params[i] = tb_gcc_type();
        char name[24];
        snprintf(name, 23, "param_%zu", i);
        params[i] = gcc_jit_context_new_param(
            mod->ctx,
            NULL,
            tb_gcc_type(&ctx, f->prototype->params[i].dt),
            name
        );
    }
    char *fname = tb_platform_heap_alloc(sizeof(char) * 24);
    snprintf(fname, 23, "func_%zu", ++ mod->nfuncs);

    ctx.gcc_func = gcc_jit_context_new_function(mod->ctx, NULL, GCC_JIT_FUNCTION_EXPORTED, ctx.gcc_func_return, fname, num_params, params, false);

    FOR_N(i, 0, num_params) {
        TB_Node *param = f->params[i + 3];
        tb_gcc_set(
            &ctx.values,
            param,
            gcc_jit_param_as_rvalue(params[i])
        );
    }

    f->tmp_arena = tmp;
    f->worklist  = ws;

    TB_CFG cfg = tb_compute_rpo(f, ws);

    // schedule nodes
    tb_global_schedule(f, ws, cfg, false, NULL);

    gcc_jit_block **blocks = tb_platform_heap_alloc(sizeof(gcc_jit_block *) * cfg.block_count);

    FOR_N(i, 0, cfg.block_count) {
        TB_Node *start = ws->items[i];
        char block_name[64] = {0};
        if (start->type == TB_REGION) {
            TB_NodeRegion *node = TB_NODE_GET_EXTRA(start);
            if (node->tag != NULL) {
                snprintf(block_name, 63, "bb_%zu_%s", (size_t) start->gvn, node->tag);
            }
        }
        if (block_name[0] == '\0') {
            snprintf(block_name, 63, "bb_%zu", (size_t) start->gvn);
        }
        blocks[i] = gcc_jit_function_new_block(ctx.gcc_func, block_name);
        nl_table_put(&ctx.blocks, start, blocks[i]);
    }

    FOR_N(i, 0, cfg.block_count) {
        TB_Node *start = ws->items[i];
        tb_gcc_block(&ctx, blocks[i], &cfg, ws, start);
    }

    tb_free_cfg(&cfg);

    TB_GCCJIT_Function *ret = tb_platform_heap_alloc(sizeof(TB_GCCJIT_Function));
    *ret = (TB_GCCJIT_Function) {
        .mod = mod,
        .func = ctx.gcc_func,
        .name = fname,
    };

    return ret;
}

TB_API void *tb_gcc_function_ptr(TB_GCCJIT_Function *func) {
    // gcc_jit_context_set_bool_option(func->mod->ctx, GCC_JIT_BOOL_OPTION_DUMP_INITIAL_TREE, true);
    // gcc_jit_context_set_bool_option(func->mod->ctx, GCC_JIT_BOOL_OPTION_DUMP_INITIAL_GIMPLE, true);
    // gcc_jit_context_set_bool_option(func->mod->ctx, GCC_JIT_BOOL_OPTION_DUMP_GENERATED_CODE, true);
    gcc_jit_context_set_int_option(func->mod->ctx, GCC_JIT_INT_OPTION_OPTIMIZATION_LEVEL, 2);
    gcc_jit_result *res = gcc_jit_context_compile(func->mod->ctx);
    return gcc_jit_result_get_code(res, func->name);
}
