
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

    TB_Function *tb_func;

    gcc_jit_function *gcc_func;
} TB_GCCJIT_Context;

static gcc_jit_type *tb_gcc_type(TB_GCCJIT_Module *mod, TB_DataType dt) {
    switch (dt.type) {
        case TB_INT: {
            if (dt.data == 0) {
                return gcc_jit_context_get_type(mod->ctx, GCC_JIT_TYPE_VOID);
            }
            if (dt.data == 1) {
                return gcc_jit_context_get_type(mod->ctx, GCC_JIT_TYPE_BOOL);
            }
            if (dt.data <= 8) {
                return gcc_jit_context_get_type(mod->ctx, GCC_JIT_TYPE_INT8_T);
            }
            if (dt.data <= 16) {
                return gcc_jit_context_get_type(mod->ctx, GCC_JIT_TYPE_INT16_T);
            }
            if (dt.data <= 32) {
                return gcc_jit_context_get_type(mod->ctx, GCC_JIT_TYPE_INT32_T);
            }
            if (dt.data <= 64) {
                return gcc_jit_context_get_type(mod->ctx, GCC_JIT_TYPE_INT64_T);
            }
            break;
        }
        case TB_PTR: {
            if (dt.data == 0) {
                return gcc_jit_context_get_type(mod->ctx, GCC_JIT_TYPE_VOID_PTR);
            }
            break;
        }
        case TB_FLOAT32: {
            return gcc_jit_context_get_type(mod->ctx, GCC_JIT_TYPE_FLOAT);
        }
        case TB_FLOAT64: {
            return gcc_jit_context_get_type(mod->ctx, GCC_JIT_TYPE_DOUBLE);
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

    FOR_N(i, foreach_start, foreach_end) {
        TB_Node* n = ws->items[i];

        if (n->type == TB_MERGEMEM || n->type == TB_SPLITMEM || n->type == TB_NULL
            || n->type == TB_PHI || n->type == TB_PROJ || n->type == TB_BRANCH_PROJ
            || n->type == TB_REGION) {
            continue;
        }
        switch (n->type) {
            case TB_LOCAL: {
                nl_table_put
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
    };

    size_t num_returns = f->prototype->return_count;
    gcc_jit_field **returns = tb_platform_heap_alloc(sizeof(gcc_jit_type *) * num_returns);
    for (size_t i = 0; i < num_returns; i++) {
        char name[24];
        snprintf(name, 23, "ret_%zu", i);
        returns[i] = gcc_jit_context_new_field(
            mod->ctx,
            NULL,
            tb_gcc_type(mod, f->prototype->params[f->prototype->param_count + i].dt),
            name
        );
    }
    gcc_jit_type *return_type = gcc_jit_struct_as_type(gcc_jit_context_new_struct_type(mod->ctx, NULL, "return", num_returns, returns));
    size_t num_params = 0;
    gcc_jit_param **params = tb_platform_heap_alloc(sizeof(gcc_jit_type *) * num_params);
    for (size_t i = 0; i < num_params; i++) {
        // params[i] = tb_gcc_type();
        char name[24];
        snprintf(name, 23, "param_%zu", i);
        params[i] = gcc_jit_context_new_param(
            mod->ctx,
            NULL,
            tb_gcc_type(mod, f->prototype->params[i].dt),
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
