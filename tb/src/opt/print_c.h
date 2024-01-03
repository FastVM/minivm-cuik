
#include <new_hash_map.h>
#include <hash_map.h>

typedef struct nl_buffer_t nl_buffer_t;

nl_buffer_t *nl_buffer_new(void);
void nl_buffer_format(nl_buffer_t *buf, const char *fmt, ...);
char *nl_buffer_get(nl_buffer_t *buf);

typedef struct {
    const char *name;
    TB_Passes* opt;
    TB_Function* f;
    TB_CFG cfg;
    TB_Scheduler sched;
    NL_HashSet completed_blocks;
    NL_HashSet needed_blocks;
    NL_HashSet declared_vars;
    NL_HashSet visited_blocks;
    nl_buffer_t *globals;
    nl_buffer_t *args;
    nl_buffer_t *pre;
    nl_buffer_t *buf;
    unsigned char depth: 8;
    bool has_ret: 1;
} CFmtState;

static void c_fmt_spaces(CFmtState *ctx) {
    for (unsigned char i = 0; i < ctx->depth; i++) {
        nl_buffer_format(ctx->buf, "  ");
    }
}

static size_t c_fmt_type_size(CFmtState* ctx, TB_DataType dt) {
    switch (dt.type) {
        case TB_INT: {
            if (dt.data == 0) return 0;
            if (dt.data == 1) return 1;
            if (dt.data == 8) return 1;
            if (dt.data == 16) return 2;
            if (dt.data == 32) return 4;
            if (dt.data == 64) return 8;
            else __builtin_trap();
            break;
        }
        case TB_PTR: {
            if (dt.data == 0) return sizeof(void *);
            else tb_todo();
            break;
        }
        case TB_FLOAT: {
            if (dt.data == TB_FLT_32) return sizeof(float);
            if (dt.data == TB_FLT_64) return sizeof(double);
            break;
        }
        case TB_TUPLE: {
            tb_todo();
            break;
        }
        case TB_CONTROL: {
            tb_todo();
            break;
        }
        case TB_MEMORY: {
            tb_todo();
            break;
        }
        default: {
            tb_todo();
            break;
        }
    }
    return 0;
}

static const char *c_fmt_type_name(CFmtState* ctx, TB_DataType dt) {
    switch (dt.type) {
        case TB_INT: {
            if (dt.data == 0) return  "void";
            if (dt.data == 1) return  "char";
            if (dt.data == 8) return  "uint8_t";
            if (dt.data == 16) return  "uint16_t";
            if (dt.data == 32) return  "uint32_t";
            if (dt.data == 64) return  "uint64_t";
            else __builtin_trap();
            break;
        }
        case TB_PTR: {
            if (dt.data == 0) return  "void *";
            else tb_todo();
            break;
        }
        case TB_FLOAT: {
            if (dt.data == TB_FLT_32) return  "float";
            if (dt.data == TB_FLT_64) return  "double";
            break;
        }
        case TB_TUPLE: {
            tb_todo();
            break;
        }
        case TB_CONTROL: {
            tb_todo();
            break;
        }
        case TB_MEMORY: {
            tb_todo();
            break;
        }
        default: {
            tb_todo();
            break;
        }
    }
    return "void *";
}

static void c_fmt_declare(CFmtState* ctx, TB_Node* n) {
    if (nl_hashset_put(&ctx->declared_vars, n)) {
        nl_buffer_format(ctx->pre, "  %s v%u;\n", c_fmt_type_name(ctx, n->dt), n->gvn);
    }
}

static void c_fmt_ref_to_node(CFmtState* ctx, TB_Node* n, bool def) {
    if (n == NULL) {
        nl_buffer_format(ctx->buf, "_");
        return;
    }
    // c_fmt_declare(ctx, n);;
    if (n->type == TB_ROOT) {
        if (def) {
            nl_buffer_format(ctx->buf, "\nbb%u:;\n", n->gvn);
            // nl_buffer_format(ctx->buf, "}\n", n->gvn);
        } else {
            nl_buffer_format(ctx->buf, "bb%u", n->gvn);
        }
    } else if (n->type == TB_PROJ && n->dt.type == TB_CONTROL) {
        if (def) {
            nl_buffer_format(ctx->buf, "\nbb%u:;\n", n->gvn);
            size_t count = 0;
            FOR_USERS(u, n) {
                if (u->n != NULL && u->n->type == TB_PHI && u->n->dt.type != TB_MEMORY && u->n->dt.type != TB_CONTROL && u->n->dt.type != TB_TUPLE) {
                    c_fmt_declare(ctx, u->n);
                    count += 1;
                }
            }
            // nl_buffer_format(ctx->buf, "}\n", n->gvn);
        } else {
            nl_buffer_format(ctx->buf, "bb%u", n->gvn);
        }
    } else if (n->type == TB_REGION || (n->type == TB_PROJ && n->dt.type == TB_CONTROL)) {
        TB_NodeRegion* r = TB_NODE_GET_EXTRA(n);
        if (def) {
            nl_buffer_format(ctx->buf, "\nbb%u:;\n", n->gvn);
            size_t count = 0;
            FOR_USERS(u, n) {
                if (u->n != NULL && u->n->type == TB_PHI && u->n->dt.type != TB_MEMORY && u->n->dt.type != TB_CONTROL && u->n->dt.type != TB_TUPLE) {
                    c_fmt_declare(ctx, u->n);
                    count += 1;
                }
            }
        } else {
            nl_buffer_format(ctx->buf, "bb%zu", n->gvn);
        }
    } else if (n->type == TB_FLOAT32_CONST) {
        TB_NodeFloat32* f = TB_NODE_GET_EXTRA(n);
        nl_buffer_format(ctx->buf, "%f", f->value);
    } else if (n->type == TB_FLOAT64_CONST) {
        TB_NodeFloat64* f = TB_NODE_GET_EXTRA(n);
        nl_buffer_format(ctx->buf, "%f", f->value);
    } else if (n->type == TB_SYMBOL) {
        TB_Symbol* sym = TB_NODE_GET_EXTRA_T(n, TB_NodeSymbol)->sym;
        if (sym->name[0]) {
            nl_buffer_format(ctx->buf, "(void *) %s", sym->name);
        } else {
            nl_buffer_format(ctx->buf, "(void *) %p", TB_NODE_GET_EXTRA_T(n, TB_NodeSymbol)->sym->address);
        }
    } else if (n->type == TB_ZERO_EXT) {
        // nl_buffer_format(ctx->buf, "(zxt.???");
        // c_fmt_type2(ctx, n->dt);
        // nl_buffer_format(ctx->buf, " ");
        c_fmt_ref_to_node(ctx, n->inputs[1], false);
        // nl_buffer_format(ctx->buf, ")");
    } else if (n->type == TB_SIGN_EXT) {
        // nl_buffer_format(ctx->buf, "(sxt.???");
        // c_fmt_type2(ctx, n->dt);
        // nl_buffer_format(ctx->buf, " ");
        c_fmt_ref_to_node(ctx, n->inputs[1], false);
        // nl_buffer_format(ctx->buf, ")");
    } else if (n->type == TB_INTEGER_CONST) {
        TB_NodeInt* num = TB_NODE_GET_EXTRA(n);

        // nl_buffer_format(ctx->buf, "(%s)", c_fmt_type_name(ctx, n->dt));
        if (num->value < 0xFFFF) {
            nl_buffer_format(ctx->buf, "%"PRId64, num->value);
        } else {
            nl_buffer_format(ctx->buf, "%#0"PRIx64, num->value);
        }
    } else {
        nl_buffer_format(ctx->buf, "v%u", n->gvn);
    }
}

static void c_fmt_bb(CFmtState* ctx, TB_Node* bb_start);

// deals with printing BB params
static void c_fmt_branch_edge(CFmtState* ctx, TB_Node* n, bool fallthru) {
    TB_Node* target = fallthru ? cfg_next_control(n) : cfg_next_bb_after_cproj(n);

    // print phi args
    if (target->type == TB_REGION) {
        int phi_i = -1;
        FOR_USERS(u, n) {
            if (u->n->type == TB_REGION) {
                phi_i = 1 + u->slot;
                break;
            }
        }

        size_t pos = 0;
        FOR_USERS(u, target) {
            if (u->n->type == TB_PHI) {
                if (u->n->inputs[phi_i] != NULL) {
                    if (u->n->inputs[phi_i]->dt.type != TB_CONTROL && u->n->inputs[phi_i]->dt.type != TB_MEMORY) {
                        assert(phi_i >= 0);
                        c_fmt_declare(ctx, u->n);
                        c_fmt_spaces(ctx);
                        nl_buffer_format(ctx->buf, "v%u = ", u->n->gvn);
                        c_fmt_ref_to_node(ctx, u->n->inputs[phi_i], false);
                        nl_buffer_format(ctx->buf, ";\n");
                        pos += 1;
                    }
                }
            }
        }
    }

    if (nl_hashset_lookup(&ctx->visited_blocks, target) & NL_HASHSET_HIGH_BIT) {
        c_fmt_spaces(ctx);
        nl_buffer_format(ctx->buf, "goto ");
        c_fmt_ref_to_node(ctx, target, false);
        nl_buffer_format(ctx->buf, ";\n");
        nl_hashset_put(&ctx->needed_blocks, target);
        fprintf(stderr, "put: %u\n", target->gvn);
    } else {
        ctx->depth -= 1;
        c_fmt_bb(ctx, target);
        ctx->depth += 1;
    }
}

static void c_fmt_bb(CFmtState* ctx, TB_Node* bb_start) {

    nl_hashset_put(&ctx->visited_blocks, bb_start);
    ctx->depth += 1;

    TB_BasicBlock* bb = ctx->opt->scheduled[bb_start->gvn];
    Worklist* ws = &ctx->opt->worklist;

    size_t foreach_start = dyn_array_length(ws->items);

    #ifndef NDEBUG
    TB_BasicBlock* expected = &nl_map_get_checked(ctx->cfg.node_to_block, bb_start);
    assert(expected == bb);
    #endif

    ctx->sched(ctx->opt, &ctx->cfg, ws, NULL, bb, bb->end);

    TB_Node* prev_effect = NULL;
    FOREACH_N(i, foreach_start, dyn_array_length(ws->items)) {
        TB_Node* n = ws->items[i];

        // skip these
        if (n->type == TB_INTEGER_CONST || n->type == TB_FLOAT32_CONST ||
            n->type == TB_FLOAT64_CONST || n->type == TB_SYMBOL ||
            n->type == TB_SIGN_EXT || n->type == TB_ZERO_EXT ||
            n->type == TB_PROJ || n->type == TB_REGION ||
            n->type == TB_NULL ||
            n->type == TB_PHI) {
            continue;
        }

        TB_NodeLocation* v;
        if (v = nl_table_get(&ctx->f->locations, n), v) {
            c_fmt_spaces(ctx);
            nl_buffer_format(ctx->buf, "# location %s:%d\n", v->file->path, v->line);
        }

        switch (n->type) {
            case TB_DEBUGBREAK: {
                c_fmt_spaces(ctx);
                nl_buffer_format(ctx->buf, "throw new Error(\"debug\");\n");
                break;
            }
            case TB_UNREACHABLE: {
                c_fmt_spaces(ctx);
                nl_buffer_format(ctx->buf, "throw new Error(\"unreachable\");\n");
                break;
            }

            case TB_BRANCH: {
                TB_NodeBranch* br = TB_NODE_GET_EXTRA(n);
                TB_ArenaSavepoint sp = tb_arena_save(tmp_arena);
                TB_Node** restrict succ = tb_arena_alloc(tmp_arena, br->succ_count * sizeof(TB_Node**));

                // fill successors
                FOR_USERS(u, n) {
                    if (u->n->type == TB_PROJ) {
                        int index = TB_NODE_GET_EXTRA_T(u->n, TB_NodeProj)->index;
                        succ[index] = u->n;
                    }
                }

                if (br->succ_count == 1) {
                    c_fmt_branch_edge(ctx, succ[0], false);
                } else if (br->succ_count == 2) {
                    c_fmt_spaces(ctx);
                    nl_buffer_format(ctx->buf, "if (");
                    FOREACH_N(i, 1, n->input_count) {
                        if (i != 1) nl_buffer_format(ctx->buf, ", ");
                        c_fmt_ref_to_node(ctx, n->inputs[i], false);
                    }
                    if (br->keys[0] == 0) {
                        nl_buffer_format(ctx->buf, " != 0");
                    } else {
                        nl_buffer_format(ctx->buf, " != %"PRId64, br->keys[0]);
                    }
                    nl_buffer_format(ctx->buf, ") {\n");
                    ctx->depth += 1;
                    c_fmt_branch_edge(ctx, succ[0], false);
                    ctx->depth -= 1;
                    c_fmt_spaces(ctx);
                    nl_buffer_format(ctx->buf, "} else {\n");
                    ctx->depth += 1;
                    c_fmt_branch_edge(ctx, succ[1], false);
                    ctx->depth -= 1;
                    c_fmt_spaces(ctx);
                    nl_buffer_format(ctx->buf, "}\n");
                } else {
                    c_fmt_spaces(ctx);
                    nl_buffer_format(ctx->buf, "/* TODO: branch/%zu */ ", (size_t) br->succ_count);
                }
                nl_buffer_format(ctx->buf, "\n");
                break;
            }

            case TB_TRAP: {
                c_fmt_spaces(ctx);
                nl_buffer_format(ctx->buf, "throw new Error(\"trap\");\n");
                break;
            }

            case TB_ROOT: {
                if (ctx->has_ret) {
                    ctx->globals = nl_buffer_new();
                }
                size_t count = 0;
                FOREACH_N(i, 1, n->input_count) {
                    if (i >= 4) {
                        count += 1;
                    }
                }
                if (count == 0) {
                    nl_buffer_format(ctx->globals, "typedef void tb2c_%s_ret_t;\n", ctx->name);
                    c_fmt_spaces(ctx);
                    nl_buffer_format(ctx->buf, "return;\n");
                } else if (count == 1) {
                    c_fmt_spaces(ctx);
                    nl_buffer_format(ctx->buf, "return ");
                    FOREACH_N(i, 1, n->input_count) {
                        if (i >= 4) {
                            if (n->inputs[i]->dt.type == TB_INT && n->inputs[i]->dt.data == 32 && !strcmp(ctx->name, "main")) {
                                nl_buffer_format(ctx->globals, "typedef int tb2c_%s_ret_t;\n", ctx->name);
                            } else {
                                nl_buffer_format(ctx->globals, "typedef %s tb2c_%s_ret_t;\n", c_fmt_type_name(ctx, n->inputs[i]->dt), ctx->name);
                            }
                            c_fmt_ref_to_node(ctx, n->inputs[i], false);
                        }
                    }
                    nl_buffer_format(ctx->buf, ";\n");
                } else {
                    nl_buffer_format(ctx->globals, "typedef struct {\n");
                    c_fmt_spaces(ctx);
                    nl_buffer_format(ctx->buf, "{\n");
                    c_fmt_spaces(ctx);
                    nl_buffer_format(ctx->buf, "  tb2c_%s_ret_t ret;\n", ctx->name);
                    
                    bool index = 0;
                    FOREACH_N(i, 1, n->input_count) {
                        if (i >= 4) {
                            nl_buffer_format(ctx->globals, "  %s v%zu;\n", c_fmt_type_name(ctx, n->inputs[i]->dt), index);
                            c_fmt_spaces(ctx);
                            nl_buffer_format(ctx->buf, "  ret.v%zu = ", index);
                            c_fmt_ref_to_node(ctx, n->inputs[i], false);
                            nl_buffer_format(ctx->buf, ";\n");
                            index += 1;
                        }
                    }
                    nl_buffer_format(ctx->globals, "} tb2c_%s_ret_t;\n", ctx->name);
                    c_fmt_spaces(ctx);
                    nl_buffer_format(ctx->buf, "  return ret;\n");
                    c_fmt_spaces(ctx);
                    nl_buffer_format(ctx->buf, "}\n");
                }
                break;
            }

            case TB_CALLGRAPH: {
                break;
            }

            case TB_STORE: {
                TB_Node *dest = n->inputs[n->input_count-2];
                TB_Node *src = n->inputs[n->input_count-1];
                c_fmt_spaces(ctx);
                nl_buffer_format(ctx->buf, "*(%s*) ", c_fmt_type_name(ctx, src->dt));
                c_fmt_ref_to_node(ctx, dest, false);
                nl_buffer_format(ctx->buf, " = ", c_fmt_type_name(ctx, src->dt));
                c_fmt_ref_to_node(ctx, src, false);
                nl_buffer_format(ctx->buf, ";\n");
                break;
            }

            case TB_LOAD: {
                TB_Node *src = n->inputs[n->input_count-1];
                c_fmt_declare(ctx, n);;
                c_fmt_spaces(ctx);
                nl_buffer_format(ctx->buf, "v%u = *(%s*) ", n->gvn, c_fmt_type_name(ctx, n->dt));
                c_fmt_ref_to_node(ctx, src, false);
                nl_buffer_format(ctx->buf, ";\n");
                break;
            }
            
            case TB_LOCAL: {
                TB_NodeLocal* l = TB_NODE_GET_EXTRA(n);
                nl_buffer_format(ctx->pre, "  uint8_t v%u[0x%x];\n", n->gvn, l->size);
                // nl_buffer_format(ctx->pre, "  %s v%u = &v%ud[0];\n", c_fmt_type_name(ctx, n->dt), n->gvn, n->gvn);
                break;
            }
            
            case TB_BITCAST: {
                TB_Node *src = n->inputs[n->input_count-1];
                size_t src_size = c_fmt_type_size(ctx, src->dt);
                size_t dest_size = c_fmt_type_size(ctx, n->dt);
                c_fmt_declare(ctx, n);;
                // if (dest_size >= src_size) {
                //     c_fmt_ref_to_node(ctx, src, false);
                //     nl_buffer_format(ctx->buf, ";\n");
                // } else {
                    c_fmt_spaces(ctx);
                    nl_buffer_format(ctx->buf, "if (1) {\n");
                    c_fmt_spaces(ctx);
                    nl_buffer_format(ctx->buf, "  union {%s src; %s dest;} tmp;\n", c_fmt_type_name(ctx, src->dt), c_fmt_type_name(ctx, n->dt));
                    c_fmt_spaces(ctx);
                    nl_buffer_format(ctx->buf, "  tmp.src = ");
                    c_fmt_ref_to_node(ctx, src, false);
                    nl_buffer_format(ctx->buf, ";\n");
                    c_fmt_spaces(ctx);
                    nl_buffer_format(ctx->buf, "  v%u = tmp.dest;\n", n->gvn);
                    c_fmt_spaces(ctx);
                    nl_buffer_format(ctx->buf, "}\n");
                // }
                break;
            }

            case TB_OR: {
                TB_Node *lhs = n->inputs[n->input_count-2];
                TB_Node *rhs = n->inputs[n->input_count-1];
                c_fmt_declare(ctx, n);;
                c_fmt_spaces(ctx);
                nl_buffer_format(ctx->buf, "v%u = ", n->gvn);
                c_fmt_ref_to_node(ctx, lhs, false);
                nl_buffer_format(ctx->buf, " | ");
                c_fmt_ref_to_node(ctx, rhs, false);
                nl_buffer_format(ctx->buf, ";\n");
                break;
            }

            case TB_XOR: {
                TB_Node *lhs = n->inputs[n->input_count-2];
                TB_Node *rhs = n->inputs[n->input_count-1];
                c_fmt_declare(ctx, n);;
                c_fmt_spaces(ctx);
                nl_buffer_format(ctx->buf, "v%u = ", n->gvn);
                c_fmt_ref_to_node(ctx, lhs, false);
                nl_buffer_format(ctx->buf, " ^ ");
                c_fmt_ref_to_node(ctx, rhs, false);
                nl_buffer_format(ctx->buf, ";\n");
                break;
            }

            case TB_AND: {
                TB_Node *lhs = n->inputs[n->input_count-2];
                TB_Node *rhs = n->inputs[n->input_count-1];
                c_fmt_declare(ctx, n);;
                c_fmt_spaces(ctx);
                nl_buffer_format(ctx->buf, "v%u = ", n->gvn);
                c_fmt_ref_to_node(ctx, lhs, false);
                nl_buffer_format(ctx->buf, " & ");
                c_fmt_ref_to_node(ctx, rhs, false);
                nl_buffer_format(ctx->buf, ";\n");
                break;
            }

            case TB_FADD:
            case TB_ADD: {
                TB_Node *lhs = n->inputs[n->input_count-2];
                TB_Node *rhs = n->inputs[n->input_count-1];
                c_fmt_declare(ctx, n);;
                c_fmt_spaces(ctx);
                nl_buffer_format(ctx->buf, "v%u = ", n->gvn);
                c_fmt_ref_to_node(ctx, lhs, false);
                nl_buffer_format(ctx->buf, " + ");
                c_fmt_ref_to_node(ctx, rhs, false);
                nl_buffer_format(ctx->buf, ";\n");
                break;
            }
            case TB_FSUB:
            case TB_SUB: {
                TB_Node *lhs = n->inputs[n->input_count-2];
                TB_Node *rhs = n->inputs[n->input_count-1];
                c_fmt_declare(ctx, n);;
                c_fmt_spaces(ctx);
                nl_buffer_format(ctx->buf, "v%u = ", n->gvn);
                c_fmt_ref_to_node(ctx, lhs, false);
                nl_buffer_format(ctx->buf, " - ");
                c_fmt_ref_to_node(ctx, rhs, false);
                nl_buffer_format(ctx->buf, ";\n");
                break;
            }
            case TB_FMUL:
            case TB_MUL: {
                TB_Node *lhs = n->inputs[n->input_count-2];
                TB_Node *rhs = n->inputs[n->input_count-1];
                c_fmt_declare(ctx, n);;
                c_fmt_spaces(ctx);
                nl_buffer_format(ctx->buf, "v%u = ", n->gvn);
                c_fmt_ref_to_node(ctx, lhs, false);
                nl_buffer_format(ctx->buf, " * ");
                c_fmt_ref_to_node(ctx, rhs, false);
                nl_buffer_format(ctx->buf, ";\n");
                break;
            }
            case TB_FDIV: {
                TB_Node *lhs = n->inputs[n->input_count-2];
                TB_Node *rhs = n->inputs[n->input_count-1];
                c_fmt_declare(ctx, n);;
                c_fmt_spaces(ctx);
                nl_buffer_format(ctx->buf, "v%u = ", n->gvn);
                c_fmt_ref_to_node(ctx, lhs, false);
                nl_buffer_format(ctx->buf, " / ");
                c_fmt_ref_to_node(ctx, rhs, false);
                nl_buffer_format(ctx->buf, ";\n");
                break;
            }
            case TB_SDIV: {
                TB_Node *lhs = n->inputs[n->input_count-2];
                TB_Node *rhs = n->inputs[n->input_count-1];
                c_fmt_declare(ctx, n);;
                c_fmt_spaces(ctx);
                nl_buffer_format(ctx->buf, "v%u = (int64_t) ", n->gvn);
                c_fmt_ref_to_node(ctx, lhs, false);
                nl_buffer_format(ctx->buf, " / (int64_t) ");
                c_fmt_ref_to_node(ctx, rhs, false);
                nl_buffer_format(ctx->buf, ";\n");
                break;
            }
            case TB_UDIV: {
                TB_Node *lhs = n->inputs[n->input_count-2];
                TB_Node *rhs = n->inputs[n->input_count-1];
                c_fmt_declare(ctx, n);;
                c_fmt_spaces(ctx);
                nl_buffer_format(ctx->buf, "v%u = (uint64_t) ", n->gvn);
                c_fmt_ref_to_node(ctx, lhs, false);
                nl_buffer_format(ctx->buf, " / (uint64_t) ");
                c_fmt_ref_to_node(ctx, rhs, false);
                nl_buffer_format(ctx->buf, ";\n");
                break;
            }
            case TB_SMOD: {
                TB_Node *lhs = n->inputs[n->input_count-2];
                TB_Node *rhs = n->inputs[n->input_count-1];
                c_fmt_declare(ctx, n);;
                c_fmt_spaces(ctx);
                nl_buffer_format(ctx->buf, "v%u = (int64_t) ", n->gvn);
                c_fmt_ref_to_node(ctx, lhs, false);
                nl_buffer_format(ctx->buf, " %% (int64_t) ");
                c_fmt_ref_to_node(ctx, rhs, false);
                nl_buffer_format(ctx->buf, ";\n");
                break;
            }
            case TB_UMOD: {
                TB_Node *lhs = n->inputs[n->input_count-2];
                TB_Node *rhs = n->inputs[n->input_count-1];
                c_fmt_declare(ctx, n);;
                c_fmt_spaces(ctx);
                nl_buffer_format(ctx->buf, "v%u = (uint64_t) ", n->gvn);
                c_fmt_ref_to_node(ctx, lhs, false);
                nl_buffer_format(ctx->buf, " %% (uint64_t) ");
                c_fmt_ref_to_node(ctx, rhs, false);
                nl_buffer_format(ctx->buf, ";\n");
                break;
            }

            case TB_CMP_EQ: {
                TB_Node *lhs = n->inputs[n->input_count-2];
                TB_Node *rhs = n->inputs[n->input_count-1];
                c_fmt_declare(ctx, n);;
                c_fmt_spaces(ctx);
                nl_buffer_format(ctx->buf, "v%u = ", n->gvn);
                c_fmt_ref_to_node(ctx, lhs, false);
                nl_buffer_format(ctx->buf, " == ");
                c_fmt_ref_to_node(ctx, rhs, false);
                nl_buffer_format(ctx->buf, ";\n");
                break;
            }

            case TB_CMP_NE: {
                TB_Node *lhs = n->inputs[n->input_count-2];
                TB_Node *rhs = n->inputs[n->input_count-1];
                c_fmt_declare(ctx, n);;
                c_fmt_spaces(ctx);
                nl_buffer_format(ctx->buf, "v%u = ", n->gvn);
                c_fmt_ref_to_node(ctx, lhs, false);
                nl_buffer_format(ctx->buf, " != ");
                c_fmt_ref_to_node(ctx, rhs, false);
                nl_buffer_format(ctx->buf, ";\n");
                break;
            }

            case TB_POISON: {
                c_fmt_declare(ctx, n);;
                break;
            }

            case TB_CMP_FLT: {
                TB_Node *lhs = n->inputs[n->input_count-2];
                TB_Node *rhs = n->inputs[n->input_count-1];
                c_fmt_declare(ctx, n);;
                c_fmt_spaces(ctx);
                nl_buffer_format(ctx->buf, "v%u = ", n->gvn);
                c_fmt_ref_to_node(ctx, lhs, false);
                nl_buffer_format(ctx->buf, " < ");
                c_fmt_ref_to_node(ctx, rhs, false);
                nl_buffer_format(ctx->buf, ";\n");
                break;
            }

            case TB_CMP_FLE: {
                TB_Node *lhs = n->inputs[n->input_count-2];
                TB_Node *rhs = n->inputs[n->input_count-1];
                c_fmt_declare(ctx, n);;
                c_fmt_spaces(ctx);
                nl_buffer_format(ctx->buf, "v%u = ", n->gvn);
                c_fmt_ref_to_node(ctx, lhs, false);
                nl_buffer_format(ctx->buf, " <= ");
                c_fmt_ref_to_node(ctx, rhs, false);
                nl_buffer_format(ctx->buf, ";\n");
                break;
            }
            

            case TB_CMP_SLT: {
                TB_Node *lhs = n->inputs[n->input_count-2];
                TB_Node *rhs = n->inputs[n->input_count-1];
                c_fmt_declare(ctx, n);;
                c_fmt_spaces(ctx);
                nl_buffer_format(ctx->buf, "v%u = (int64_t) ", n->gvn);
                c_fmt_ref_to_node(ctx, lhs, false);
                nl_buffer_format(ctx->buf, " < (int64_t) ");
                c_fmt_ref_to_node(ctx, rhs, false);
                nl_buffer_format(ctx->buf, ";\n");
                break;
            }

            case TB_CMP_SLE: {
                TB_Node *lhs = n->inputs[n->input_count-2];
                TB_Node *rhs = n->inputs[n->input_count-1];
                c_fmt_declare(ctx, n);;
                c_fmt_spaces(ctx);
                nl_buffer_format(ctx->buf, "v%u = (int64_t) ", n->gvn);
                c_fmt_ref_to_node(ctx, lhs, false);
                nl_buffer_format(ctx->buf, " <= (int64_t) ");
                c_fmt_ref_to_node(ctx, rhs, false);
                nl_buffer_format(ctx->buf, ";\n");
                break;
            }
            

            case TB_CMP_ULT: {
                TB_Node *lhs = n->inputs[n->input_count-2];
                TB_Node *rhs = n->inputs[n->input_count-1];
                c_fmt_declare(ctx, n);;
                c_fmt_spaces(ctx);
                nl_buffer_format(ctx->buf, "v%u = (uint64_t) ", n->gvn);
                c_fmt_ref_to_node(ctx, lhs, false);
                nl_buffer_format(ctx->buf, " < (uint64_t) ");
                c_fmt_ref_to_node(ctx, rhs, false);
                nl_buffer_format(ctx->buf, ";\n");
                break;
            }

            case TB_CMP_ULE: {
                TB_Node *lhs = n->inputs[n->input_count-2];
                TB_Node *rhs = n->inputs[n->input_count-1];
                c_fmt_declare(ctx, n);;
                c_fmt_spaces(ctx);
                nl_buffer_format(ctx->buf, "v%u = (uint64_t) ", n->gvn);
                c_fmt_ref_to_node(ctx, lhs, false);
                nl_buffer_format(ctx->buf, " <= (uint64_t) ");
                c_fmt_ref_to_node(ctx, rhs, false);
                nl_buffer_format(ctx->buf, ";\n");
                break;
            }
            
            case TB_MEMBER_ACCESS: {
                TB_Node *ptr = n->inputs[n->input_count-1];
                c_fmt_declare(ctx, n);;
                c_fmt_spaces(ctx);
                nl_buffer_format(ctx->buf, "v%u = (void*) ((size_t) ", n->gvn);
                c_fmt_ref_to_node(ctx, ptr, false);
                nl_buffer_format(ctx->buf, " + %"PRId64, TB_NODE_GET_EXTRA_T(n, TB_NodeMember)->offset);
                nl_buffer_format(ctx->buf, ");\n");
                break;
            }

            
            case TB_SELECT: {
                TB_Node *cond = n->inputs[n->input_count-1];
                TB_Node *then = n->inputs[n->input_count-1];
                TB_Node *els = n->inputs[n->input_count-1];
                c_fmt_declare(ctx, n);;
                c_fmt_spaces(ctx);
                nl_buffer_format(ctx->buf, "v%u = ", n->gvn);
                c_fmt_ref_to_node(ctx, cond, false);
                nl_buffer_format(ctx->buf, " ? ");
                c_fmt_ref_to_node(ctx, then, false);
                nl_buffer_format(ctx->buf, " : ");
                c_fmt_ref_to_node(ctx, els, false);
                nl_buffer_format(ctx->buf, ";\n");
                break;
            }

            case TB_FLOAT2INT:
            case TB_INT2FLOAT:
            case TB_TRUNCATE: {
                TB_Node *input = n->inputs[n->input_count-1];
                c_fmt_declare(ctx, n);;
                c_fmt_spaces(ctx);
                nl_buffer_format(ctx->buf, "v%u = (%s) ", n->gvn, c_fmt_type_name(ctx, n->dt));
                c_fmt_ref_to_node(ctx, input, false);
                nl_buffer_format(ctx->buf, ";\n");
                break;
            }

            case TB_ARRAY_ACCESS: {
                TB_Node *ptr = n->inputs[n->input_count-2];
                TB_Node *index = n->inputs[n->input_count-1];
                c_fmt_declare(ctx, n);;
                c_fmt_spaces(ctx);
                nl_buffer_format(ctx->buf, "v%u = (void*) ((size_t) ", n->gvn);
                c_fmt_ref_to_node(ctx, ptr, false);
                nl_buffer_format(ctx->buf, " + ");
                c_fmt_ref_to_node(ctx, index, false);
                nl_buffer_format(ctx->buf, " * %"PRId64, TB_NODE_GET_EXTRA_T(n, TB_NodeArray)->stride);
                nl_buffer_format(ctx->buf, ");\n");
                break;
            }
            case TB_CALL: {
                TB_Node *func = n->inputs[2];
                
                TB_Node* projs[4] = { 0 };
                FOR_USERS(use, n) {
                    if (use->n->type == TB_PROJ) {
                        int index = TB_NODE_GET_EXTRA_T(use->n, TB_NodeProj)->index;
                        projs[index] = use->n;
                    }
                }

                if (projs[2] == NULL) {
                    nl_buffer_format(ctx->globals, "typedef void(*v%u_t)(", ctx->name, n->gvn);
                } else if (projs[3] == NULL) {
                    nl_buffer_format(ctx->globals, "typedef %s(*tb2c_%s_v%u_t)(", c_fmt_type_name(ctx, projs[2]->dt), ctx->name, n->gvn);
                } else {
                    nl_buffer_format(ctx->globals, "typedef struct {\n");
                    FOREACH_N(i, 2, 4) {
                        if (projs[i] == NULL) break;
                        nl_buffer_format(ctx->globals, "  %s v%u;\n", c_fmt_type_name(ctx, projs[i]->dt), projs[i]->gvn);
                    }
                    nl_buffer_format(ctx->globals, "} tb2c_%s_v%u_ret_t;\n", ctx->name, n->gvn);
                    nl_buffer_format(ctx->globals, "typedef tb2c_%s_v%u_ret_t(*tb2c_%s_vv%u_t)(", ctx->name, n->gvn, ctx->name, n->gvn);
                }
                {
                    bool first = true;
                    FOREACH_N(i, 3, n->input_count) {
                        if (n->inputs[i]->dt.type != TB_CONTROL && n->inputs[i]->dt.type != TB_MEMORY) {
                            if (!first) {
                                nl_buffer_format(ctx->globals, ", ");
                            }
                            nl_buffer_format(ctx->globals, "%s", c_fmt_type_name(ctx, n->inputs[i]->dt));
                            first = false;
                        }
                    }
                    if (first) {
                            nl_buffer_format(ctx->globals, "void");
                    }
                }
                nl_buffer_format(ctx->globals, ");\n");
                if (projs[2] == NULL) {
                } else if (projs[3] == NULL) {
                    nl_buffer_format(ctx->pre, "  %s v%u;\n", c_fmt_type_name(ctx, projs[2]->dt), projs[2]->gvn);
                    c_fmt_spaces(ctx);
                    nl_buffer_format(ctx->buf, "v%u = ", projs[2]->gvn);
                    nl_buffer_format(ctx->buf, "((tb2c_%s_v%u_t) ", ctx->name, n->gvn);
                    c_fmt_ref_to_node(ctx, func, false);
                    nl_buffer_format(ctx->buf, ")(");
                    {
                        bool first = true;
                        FOREACH_N(i, 3, n->input_count) {
                            if (n->inputs[i]->dt.type != TB_CONTROL && n->inputs[i]->dt.type != TB_MEMORY) {
                                if (!first) {
                                    nl_buffer_format(ctx->buf, ", ");
                                }
                                c_fmt_ref_to_node(ctx, n->inputs[i], false);
                                first = false;
                            }
                        }
                    }
                    nl_buffer_format(ctx->buf, ");\n");
                } else {
                    c_fmt_spaces(ctx);
                    nl_buffer_format(ctx->buf, "{\n");
                    c_fmt_spaces(ctx);
                    nl_buffer_format(ctx->buf, "  tb2c_%s_v%u_ret_t ret = ", ctx->name, n->gvn);
                    nl_buffer_format(ctx->buf, "((v%u_t) ", n->gvn);
                    c_fmt_ref_to_node(ctx, func, false);
                    nl_buffer_format(ctx->buf, ")(");
                    {
                        bool first = true;
                        FOREACH_N(i, 3, n->input_count) {
                            if (n->inputs[i]->dt.type != TB_CONTROL && n->inputs[i]->dt.type != TB_MEMORY) {
                                if (!first) {
                                    nl_buffer_format(ctx->buf, ", ");
                                }
                                c_fmt_ref_to_node(ctx, n->inputs[i], false);
                                first = false;
                            }
                        }
                    }
                    nl_buffer_format(ctx->buf, ");\n");
                    if (projs[2] != NULL) {
                        FOREACH_N(i, 2, 4) {
                            if (projs[i] == NULL) break;
                            nl_buffer_format(ctx->pre, "  %s v%u;\n", c_fmt_type_name(ctx, projs[i]->dt), projs[i]->gvn);
                            c_fmt_spaces(ctx);
                            nl_buffer_format(ctx->buf, "  v%u = ret.v%u;\n", projs[i]->gvn, projs[i]->gvn);
                        }
                    }
                    c_fmt_spaces(ctx);
                    nl_buffer_format(ctx->buf, "}\n");
                }
                break;
            }

            // case TB_MEMSET: {
            //     break;
            // }

            case TB_MEMCPY: {
                TB_Node *dest = n->inputs[n->input_count-3];
                TB_Node *src = n->inputs[n->input_count-2];
                TB_Node *len = n->inputs[n->input_count-1];
                c_fmt_spaces(ctx);
                nl_buffer_format(ctx->buf, "memcpy(");
                c_fmt_ref_to_node(ctx, dest, false);
                nl_buffer_format(ctx->buf, ", ");
                c_fmt_ref_to_node(ctx, src, false);
                nl_buffer_format(ctx->buf, ", ");
                c_fmt_ref_to_node(ctx, len, false);
                nl_buffer_format(ctx->buf, ");\n");
                break;
            }

            default: {
                fprintf(stderr, "internal unimplemented node type: %s\n", tb_node_get_name(n));
                fflush(stderr);
                __builtin_trap();
                // if (n->dt.type == TB_TUPLE) {
                //     // print with multiple returns
                //     TB_Node* projs[4] = { 0 };
                //     FOR_USERS(use, n) {
                //         if (use->n->type == TB_PROJ) {
                //             int index = TB_NODE_GET_EXTRA_T(use->n, TB_NodeProj)->index;
                //             projs[index] = use->n;
                //         }
                //     }

                //     c_fmt_spaces(ctx);
     nl_buffer_format(ctx->buf, "");

                //     size_t first = projs[0] && projs[0]->dt.type == TB_CONTROL ? 1 : 0;
                //     FOREACH_N(i, first, 4) {
                //         if (projs[i] == NULL) break;
                //         if (i > first) nl_buffer_format(ctx->buf, ", ");
                //         nl_buffer_format(ctx->buf, "v%u", projs[i]->gvn);
                //     }
                //     nl_buffer_format(ctx->buf, " = %s.(", tb_node_get_name(n));
                //     FOREACH_N(i, first, 4) {
                //         if (projs[i] == NULL) break;
                //         if (i > first) nl_buffer_format(ctx->buf, ", ");
                //         // c_fmt_type2(ctx, projs[i]->dt);
                //     }
                //     nl_buffer_format(ctx->buf, ")");
                // } else {
                    // print as normal instruction
                    TB_DataType dt = n->dt;
                    if (n->type >= TB_CMP_EQ && n->type <= TB_CMP_FLE) {
                        dt = TB_NODE_GET_EXTRA_T(n, TB_NodeCompare)->cmp_dt;
                    }
                    // c_fmt_type2(ctx, dt);
                // }

                nl_buffer_format(ctx->pre, "  %s v%u;\n", c_fmt_type_name(ctx, dt), n->gvn);
                c_fmt_spaces(ctx);
                nl_buffer_format(ctx->buf, "v%u = %s(", n->gvn, tb_node_get_name(n));

                // c_fmt_type_name(ctx, dt);
                // print extra data
                switch (n->type) {
                    case TB_CMP_FLT:
                    case TB_CMP_FLE:
                    case TB_SELECT:
                        // nl_buffer_format(ctx->buf, ", ");
                        // c_fmt_type_name(ctx, n->inputs[1]->dt);
                        break;

                    case TB_PROJ: {
                        // nl_buffer_format(ctx->buf, ", %d", TB_NODE_GET_EXTRA_T(n, TB_NodeProj)->index);
                        break;
                    }

                    case TB_AND:
                    case TB_OR:
                    case TB_XOR:
                    case TB_SHL:
                    case TB_SHR:
                    case TB_ROL:
                    case TB_ROR:
                    case TB_SAR:
                    {
                        // TB_NodeBinopInt* b = TB_NODE_GET_EXTRA(n);
                        // if (b->ab & TB_ARITHMATIC_NSW) nl_buffer_format(ctx->buf, " !nsw");
                        // if (b->ab & TB_ARITHMATIC_NUW) nl_buffer_format(ctx->buf, " !nuw");
                        break;
                    }

                    case TB_MEMSET:
                    case TB_MEMCPY: {
                        // TB_NodeMemAccess* mem = TB_NODE_GET_EXTRA(n);
                        // nl_buffer_format(ctx->buf, " !align(%d)", mem->align);
                        break;
                    }

                    case TB_ATOMIC_LOAD:
                    case TB_ATOMIC_XCHG:
                    case TB_ATOMIC_ADD:
                    case TB_ATOMIC_SUB:
                    case TB_ATOMIC_AND:
                    case TB_ATOMIC_XOR:
                    case TB_ATOMIC_OR:
                    case TB_ATOMIC_CAS: {
                        // static const char* order_names[] = {
                        //     "relaxed", "consume", "acquire",
                        //     "release", "acqrel", "seqcst"
                        // };

                        // TB_NodeAtomic* atomic = TB_NODE_GET_EXTRA(n);
                        // nl_buffer_format(ctx->buf, " !order(%s)", order_names[atomic->order]);
                        // if (n->type == TB_ATOMIC_CAS) {
                        //     nl_buffer_format(ctx->buf, " !fail_order(%s)", order_names[atomic->order2]);
                        // }
                        break;
                    }

                    case TB_CALL:
                    case TB_TAILCALL:
                    case TB_SYSCALL:
                    case TB_SAFEPOINT_POLL:
                    break;

                    case TB_LOOKUP: {
                        // TB_NodeLookup* l = TB_NODE_GET_EXTRA(n);

                        // nl_buffer_format(ctx->buf, " { default: %"PRId64, l->entries[0].val);
                        // FOREACH_N(i, 1, l->entry_count) {
                        //     nl_buffer_format(ctx->buf, ", %"PRId64": %"PRId64, l->entries[i].key, l->entries[i].val);
                        // }
                        // nl_buffer_format(ctx->buf, "}");
                        break;
                    }

                    default: tb_assert(extra_bytes(n) == 0, "TODO");
                }

                FOREACH_N(i, 1, n->input_count) {
                    if (n->inputs[i]->dt.type != TB_CONTROL && n->inputs[i]->dt.type != TB_MEMORY) {
                        nl_buffer_format(ctx->buf, ", ");
                        c_fmt_ref_to_node(ctx, n->inputs[i], false);
                    }
                }
                nl_buffer_format(ctx->buf, ");\n");
                break;
            }
        }
    }

    dyn_array_set_length(ws->items, ctx->cfg.block_count);

    if (!cfg_is_terminator(bb->end)) {
        c_fmt_branch_edge(ctx, bb->end, true);
    }

    ctx->depth -= 1;
    nl_hashset_remove(&ctx->visited_blocks, bb_start);
}

TB_API char *tb_pass_c_fmt(TB_Passes* opt, const char *name) {
    TB_Function* f = opt->f;
    cuikperf_region_start("print", NULL);

    Worklist old = opt->worklist;
    Worklist tmp_ws = { 0 };
    worklist_alloc(&tmp_ws, f->node_count);

    CFmtState ctx = { name, opt, f };
    ctx.has_ret = false;
    ctx.globals = nl_buffer_new();
    ctx.args = nl_buffer_new();
    ctx.pre = nl_buffer_new();
    ctx.buf = nl_buffer_new();

    ctx.visited_blocks = nl_hashset_alloc(8);
    ctx.declared_vars = nl_hashset_alloc(8);
    ctx.needed_blocks = nl_hashset_alloc(4);
    ctx.completed_blocks = nl_hashset_alloc(ctx.cfg.block_count);

    // nl_hashset_clear(&ctx.visited_blocks);

    opt->worklist = tmp_ws;
    ctx.cfg = tb_compute_rpo(f, opt);

    // does the IR printing need smart scheduling lol (yes... when we're testing things)
    ctx.sched = greedy_scheduler;

    TB_ArenaSavepoint sp = tb_arena_save(tmp_arena);

    // schedule nodes
    tb_pass_schedule(opt, ctx.cfg, false);
    worklist_clear_visited(&opt->worklist);

    bool first = true;
    TB_Node* end_bb = NULL;
    nl_hashset_put(&ctx.needed_blocks, opt->worklist.items[0]);
    while (true) {
        bool any = false;
        FOREACH_N(i, 0, ctx.cfg.block_count) {
            TB_Node* end = nl_map_get_checked(ctx.cfg.node_to_block, opt->worklist.items[i]).end;
            if (end == f->root_node) {
                end_bb = opt->worklist.items[i];
                continue;
            }

            if (nl_hashset_lookup(&ctx.needed_blocks, opt->worklist.items[i]) & NL_HASHSET_HIGH_BIT) {
                if (!nl_hashset_put(&ctx.completed_blocks, opt->worklist.items[i])) {
                    continue;
                }
                if (!first) {
                    c_fmt_ref_to_node(&ctx, opt->worklist.items[i], true);
                } else {
                    first = false;
                }
                c_fmt_bb(&ctx, opt->worklist.items[i]);
                any = true;
            }
        }
        if (!any) {
            break;
        }
    }

    if (nl_hashset_lookup(&ctx.needed_blocks, end_bb) & NL_HASHSET_HIGH_BIT) {
        c_fmt_ref_to_node(&ctx, end_bb, true);
        c_fmt_bb(&ctx, end_bb);
    }

    tb_arena_restore(tmp_arena, sp);
    worklist_free(&opt->worklist);
    // tb_free_cfg(&ctx.cfg);
    opt->worklist = old;
    opt->scheduled = NULL;
    opt->error_n = NULL;
    cuikperf_region_end();

    nl_buffer_t *buf = nl_buffer_new();

    // nl_buffer_format(buf, "typedef unsigned char uint8_t;\n");
    // nl_buffer_format(buf, "typedef unsigned short uint16_t;\n");
    // nl_buffer_format(buf, "typedef unsigned int uint32_t;\n");
    // nl_buffer_format(buf, "typedef unsigned long long uint64_t;\n");
    // nl_buffer_format(buf, "typedef signed char int8_t;\n");
    // nl_buffer_format(buf, "typedef signed short int16_t;\n");
    // nl_buffer_format(buf, "typedef signed int int32_t;\n");
    // nl_buffer_format(buf, "typedef signed long long int64_t;\n");
    // nl_buffer_format(buf, "typedef __SIZE_TYPE__ size_t;\n");
    nl_buffer_format(buf, "%s\n", nl_buffer_get(ctx.globals));
    nl_buffer_format(buf, "tb2c_%s_ret_t %s(", name, name);
    TB_Node** params = f->params;
    size_t count = 0;
    FOREACH_N(i, 3, 3 + f->param_count) {
        if (params[i] != NULL && params[i]->dt.type != TB_MEMORY && params[i]->dt.type != TB_CONTROL && params[i]->dt.type != TB_TUPLE) {
            if (count != 0) {
                nl_buffer_format(buf, ", ");
            }
            nl_buffer_format(buf, "%s v%u", c_fmt_type_name(&ctx, params[i]->dt), params[i]->gvn);
            count += 1;
        }
    }
    if (count == 0) {
        nl_buffer_format(buf, "void");
    }
    nl_buffer_format(buf, ") {\n");
    nl_buffer_format(buf, "%s", nl_buffer_get(ctx.pre));
    nl_buffer_format(buf, "%s", nl_buffer_get(ctx.buf));
    nl_buffer_format(buf, "}\n");

    nl_hashset_free(ctx.visited_blocks);

    return nl_buffer_get(buf);
}
