
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
    TB_Function *tb_func;
    gcc_jit_function *gcc_func;
} TB_GCCJIT_Context;

#define tb_gcc_get(t, k) ((gcc_jit_rvalue *) nl_table_get((t), (void *) (size_t) (k)->gvn))
#define tb_gcc_set(t, k, v) (nl_table_put((t), (void *) (size_t) (k)->gvn, (gcc_jit_rvalue *) (v)))

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

static void tb_gcc_block(TB_GCCJIT_Context *ctx, TB_CFG *cfg, TB_Worklist *ws, TB_Node* start) {
    TB_BasicBlock* bb = ctx->tb_func->scheduled[start->gvn];

    size_t foreach_start = dyn_array_length(ws->items);
    tb_greedy_scheduler(ctx->tb_func, cfg, ws, NULL, bb);
    size_t foreach_end = dyn_array_length(ws->items);

    gcc_jit_block *block = gcc_jit_function_new_block(ctx->gcc_func, NULL);

    FOR_N(i, foreach_start, foreach_end) {
        TB_Node* n = ws->items[i];

        if (n->type == TB_MERGEMEM || n->type == TB_SPLITMEM || n->type == TB_NULL
            || n->type == TB_PHI || n->type == TB_PROJ || n->type == TB_BRANCH_PROJ
            || n->type == TB_REGION) {
            continue;
        }
        switch (n->type) {
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
                                gcc_jit_context_get_type(ctx->mod->ctx, GCC_JIT_TYPE_VOID_PTR)
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
                            tb_gcc_get(&ctx->values, src),
                            gcc_jit_type_get_pointer(
                                tb_gcc_type(ctx, dest->dt)
                            )
                        ),
                        NULL
                    ),
                    tb_gcc_get(&ctx->values, dest)
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
                
                gcc_jit_block_add_assignment(
                    block,
                    NULL,
                    local,
                    gcc_jit_lvalue_as_rvalue(
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
            case TB_CALL: {
                TB_Node *func = n->inputs[2];

                TB_Node* projs[2] = { NULL, NULL };
                FOR_USERS(use, n) {
                    if (USERN(use)->type == TB_PROJ) {
                        int index = TB_NODE_GET_EXTRA_T(USERN(use), TB_NodeProj)->index;
                        projs[index - 2] = USERN(use);
                    }
                }

                gcc_jit_type *ret = NULL;
                gcc_jit_struct *s = NULL;

                if (projs[0] == NULL) {
                    ret = gcc_jit_context_get_type(ctx->mod->ctx, GCC_JIT_TYPE_VOID);
                } else if (projs[1] == NULL) {
                    ret = tb_gcc_type(ctx, projs[0]->dt);
                } else {
                    char buf[24];
                    snprintf(buf, 23, "ret_%zu", (size_t) n->gvn);
                    gcc_jit_field *fields[2] = {
                        gcc_jit_context_new_field(ctx->mod->ctx, NULL, tb_gcc_type(ctx, projs[0]->dt), "member_1"),
                        gcc_jit_context_new_field(ctx->mod->ctx, NULL, tb_gcc_type(ctx, projs[1]->dt), "member_2"),
                    };
                    s = gcc_jit_context_new_struct_type(ctx->mod->ctx, NULL, buf, 2, fields);
                    ret = gcc_jit_struct_as_type(s);
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

                gcc_jit_block_add_eval(
                    block,
                    NULL,
                    gcc_jit_context_new_call_through_ptr(
                        ctx->mod->ctx,
                        NULL,
                        gcc_jit_context_new_cast(
                            ctx->mod->ctx,
                            NULL,
                             tb_gcc_get(&ctx->values, n->inputs[2]),
                            gcc_jit_context_new_function_ptr_type(ctx->mod->ctx, NULL, ret, num_params, params, false)
                        ),
                        num_args,
                        args
                    )
                );

                break;
            }
            default: {
                fprintf(stderr, "internal unimplemented node type: %s\n", tb_node_get_name(n));
                tb_todo();
                break;
            } 
        }
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
    };

    size_t num_returns = f->prototype->return_count;
    gcc_jit_field **returns = tb_platform_heap_alloc(sizeof(gcc_jit_type *) * num_returns);
    for (size_t i = 0; i < num_returns; i++) {
        char name[24];
        snprintf(name, 23, "ret_%zu", i);
        returns[i] = gcc_jit_context_new_field(
            mod->ctx,
            NULL,
            tb_gcc_type(&ctx, f->prototype->params[f->prototype->param_count + i].dt),
            name
        );
    }
    gcc_jit_type *return_type = gcc_jit_struct_as_type(gcc_jit_context_new_struct_type(mod->ctx, NULL, "func_ret", num_returns, returns));
    size_t num_params = 0;
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

    ctx.gcc_func = gcc_jit_context_new_function(mod->ctx, NULL, GCC_JIT_FUNCTION_EXPORTED, return_type, fname, num_params, params, false);

    f->tmp_arena = tmp;
    f->worklist  = ws;

    TB_CFG cfg = tb_compute_rpo(f, ws);

    // schedule nodes
    tb_global_schedule(f, ws, cfg, false, NULL);

    // TB_Node* end_bb = NULL;
    FOR_N(i, 0, cfg.block_count) {
        tb_gcc_block(&ctx, &cfg, ws, ws->items[i]);
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
    gcc_jit_result *res = gcc_jit_context_compile(func->mod->ctx);
    return gcc_jit_result_get_code(res, func->name);
}
