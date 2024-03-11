#include "x64.h"
#include <tb_x64.h>
#include "x64_emitter.h"
#include "x64_disasm.c"

#ifdef TB_HAS_X64
enum {
    // register classes
    REG_CLASS_FLAGS = 1,
    REG_CLASS_GPR,
    REG_CLASS_XMM,
    REG_CLASS_COUNT,
};

#include "../codegen_impl.h"

// node with X86MemOp (mov, add, and...) will have this layout of inputs:
//   [1] mem
//   [2] base (or first src)
//   [3] idx
//   [4] val
typedef struct {
    enum {
        MODE_REG,
        MODE_LD, // reg <- mem
        MODE_ST, // mem <- reg
    } mode;
    Scale scale;
    int32_t disp;
    int32_t imm;
} X86MemOp;

typedef struct {
    TB_FunctionPrototype* proto;
    TB_Symbol* sym;
    uint32_t clobber_gpr;
    uint32_t clobber_xmm;
} X86Call;

// machine node types
typedef enum {
    x86_int3 = TB_FIRST_ARCH_MACHINE_OP,

    #define X(name) x86_ ## name,
    #include "x64_nodes.inc"
} X86NodeType;

const char* tb_node_x86_get_name(TB_Node* n) {
    switch (n->type) {
        case TB_MACH_COPY: return "mach_copy";
        case TB_MACH_MOVE: return "mach_move";
        case TB_MACH_LOCAL: return "mach_local";
        #define X(name) case x86_ ## name: return STR(x86_ ## name);
        #include "x64_nodes.inc"
        default: return NULL;
    }
}

void tb_node_x86_print_extra(TB_Node* n) {
    switch (n->type) {
        case x86_add: case x86_or:  case x86_and: case x86_sub:
        case x86_xor: case x86_cmp: case x86_mov: case x86_test:
        {
            static const char* modes[] = { "reg", "ld", "st" };
            X86MemOp* op = TB_NODE_GET_EXTRA(n);
            printf(", scale=%d, disp=%d, mode=%s", 1<<op->scale, op->disp, modes[op->mode]);
            break;
        }

        case x86_addimm: case x86_orimm:  case x86_andimm: case x86_subimm:
        case x86_xorimm: case x86_cmpimm: case x86_movimm: case x86_testimm: case x86_imulimm:
        case x86_shlimm: case x86_shrimm: case x86_sarimm: case x86_rolimm: case x86_rorimm:
        {
            X86MemOp* op = TB_NODE_GET_EXTRA(n);
            printf(", %d", op->imm);
            break;
        }
    }
}

typedef struct {
    int64_t min, max;
    bool if_chain;
} AuxBranch;

static const struct ParamDesc {
    int chkstk_limit;
    int gpr_count;
    int xmm_count;
    uint16_t caller_saved_xmms; // XMM0 - XMMwhatever
    uint16_t caller_saved_gprs; // bitfield

    GPR gprs[6];
} param_descs[] = {
    // win64
    { 4096,    4, 4, 6, WIN64_ABI_CALLER_SAVED,   { RCX, RDX, R8,  R9,  0,  0 } },
    // system v
    { INT_MAX, 6, 4, 5, SYSV_ABI_CALLER_SAVED,    { RDI, RSI, RDX, RCX, R8, R9 } },
    // syscall
    { INT_MAX, 6, 4, 5, SYSCALL_ABI_CALLER_SAVED, { RDI, RSI, RDX, R10, R8, R9 } },
};

enum {
    NO_RCX     = ~((1 << RCX)),
};

// *out_mask of 0 means no mask
static TB_X86_DataType legalize_int(TB_DataType dt, uint64_t* out_mask) {
    assert(dt.type == TB_INT || dt.type == TB_PTR);
    if (dt.type == TB_PTR) return *out_mask = 0, TB_X86_TYPE_QWORD;

    TB_X86_DataType t = TB_X86_TYPE_NONE;
    int bits = 0;

    if (dt.data <= 8) bits = 8, t = TB_X86_TYPE_BYTE;
    else if (dt.data <= 16) bits = 16, t = TB_X86_TYPE_WORD;
    else if (dt.data <= 32) bits = 32, t = TB_X86_TYPE_DWORD;
    else if (dt.data <= 64) bits = 64, t = TB_X86_TYPE_QWORD;

    assert(bits != 0 && "TODO: large int support");
    assert(dt.data != 0);
    uint64_t mask = ~UINT64_C(0) >> (64 - dt.data);

    *out_mask = (dt.data == bits) ? 0 : mask;
    return t;
}

static TB_X86_DataType legalize_int2(TB_DataType dt) {
    uint64_t m;
    return legalize_int(dt, &m);
}

static TB_X86_DataType legalize_float(TB_DataType dt) {
    assert(dt.type == TB_FLOAT);
    return (dt.data == TB_FLT_64 ? TB_X86_TYPE_SSE_SD : TB_X86_TYPE_SSE_SS);
}

static TB_X86_DataType legalize(TB_DataType dt) {
    if (dt.type == TB_FLOAT) {
        return legalize_float(dt);
    } else {
        uint64_t m;
        return legalize_int(dt, &m);
    }
}

static bool fits_into_int32(uint64_t x) {
    uint32_t hi = x >> 32ull;
    return hi == 0 || hi == 0xFFFFFFFF;
}

static bool try_for_imm32(int bits, TB_Node* n, int32_t* out_x) {
    if (n->type != TB_INTEGER_CONST) {
        return false;
    }

    TB_NodeInt* i = TB_NODE_GET_EXTRA(n);
    if (bits > 32) {
        bool sign = (i->value >> 31ull) & 1;
        uint64_t top = i->value >> 32ull;

        // if the sign matches the rest of the top bits, we can sign extend just fine
        if (top != (sign ? 0xFFFFFFFF : 0)) {
            return false;
        }
    }

    *out_x = i->value;
    return true;
}

static int node_2addr(TB_Node* n) {
    switch (n->type) {
        // ANY_GPR = OP(ANY_GPR, ANY_GPR)
        case x86_add: case x86_or:  case x86_and: case x86_sub:
        case x86_xor: case x86_cmp: case x86_mov: case x86_test:
        {
            X86MemOp* op = TB_NODE_GET_EXTRA(n);
            return op->mode == MODE_REG ? 4 : -1;
        }

        // ANY_GPR = OP(ANY_GPR, IMM)
        case x86_addimm: case x86_orimm:  case x86_andimm: case x86_subimm:
        case x86_xorimm: case x86_cmpimm: case x86_movimm: case x86_testimm: case x86_imulimm:
        case x86_shlimm: case x86_shrimm: case x86_sarimm: case x86_rolimm: case x86_rorimm:
        {
            X86MemOp* op = TB_NODE_GET_EXTRA(n);
            return op->mode == MODE_REG ? 1 : -1;
        }

        // ANY_GPR = OP(COND, shared: ANY_GPR, ANY_GPR)
        case TB_SELECT:
        return 2;

        // ANY_GPR = OP(ANY_GPR, CL)
        case TB_SHL: case TB_SHR: case TB_ROL: case TB_ROR: case TB_SAR:
        return 1;

        case TB_MACH_COPY:
        case TB_MACH_MOVE:
        return 1;
    }

    return n->type >= TB_AND && n->type <= TB_CMP_FLE;
}

static void init_ctx(Ctx* restrict ctx, TB_ABI abi) {
    ctx->abi_index = abi == TB_ABI_SYSTEMV ? 1 : 0;

    // currently only using 16 GPRs and 16 XMMs, AVX gives us
    // 32 YMMs (which double as XMMs) and later on APX will do
    // 32 GPRs.
    ctx->num_regs[REG_CLASS_FLAGS] = 1;
    ctx->num_regs[REG_CLASS_GPR] = 16;
    ctx->num_regs[REG_CLASS_XMM] = 16;

    uint16_t all_gprs = 0xFFFF & ~(1 << RSP);
    if (ctx->features.gen & TB_FEATURE_FRAME_PTR) {
        all_gprs &= ~(1 << RBP);
        ctx->stack_header = 16;
    } else {
        ctx->stack_header = 8;
    }

    ctx->normie_mask[REG_CLASS_FLAGS] = new_regmask(ctx->f, REG_CLASS_FLAGS, false, 1);
    ctx->normie_mask[REG_CLASS_GPR]   = new_regmask(ctx->f, REG_CLASS_GPR,   false, all_gprs);
    ctx->normie_mask[REG_CLASS_XMM]   = new_regmask(ctx->f, REG_CLASS_XMM,   false, 0xFFFF);

    // mark GPR callees (technically includes RSP but since it's
    // never conventionally allocated we should never run into issues)
    // ctx->callee_saved[REG_CLASS_GPR] = ~param_descs[ctx->abi_index].caller_saved_gprs;

    // mark XMM callees
    // ctx->callee_saved[REG_CLASS_XMM] = 0;
    /* FOREACH_N(i, param_descs[ctx->abi_index].caller_saved_xmms, 16) {
        ctx->callee_saved[REG_CLASS_XMM] |= (1ull << i);
    } */

    TB_FunctionPrototype* proto = ctx->f->prototype;
    TB_Node** params = ctx->f->params;
    TB_Node* root_ctrl = params[0];

    ctx->stack_usage += ctx->stack_header + (proto->param_count * 8);

    if (proto->has_varargs) {
        const GPR* parameter_gprs = param_descs[ctx->abi_index].gprs;

        // spill the rest of the parameters (assumes they're all in the GPRs)
        size_t gpr_count = param_descs[ctx->abi_index].gpr_count;
        size_t extra_param_count = proto->param_count > gpr_count ? 0 : gpr_count - proto->param_count;

        ctx->stack_usage += (extra_param_count * 8);
    }
}

static RegMask* normie_mask(Ctx* restrict ctx, TB_DataType dt) {
    return ctx->normie_mask[dt.type == TB_FLOAT ? REG_CLASS_XMM : REG_CLASS_GPR];
}

// returns true if it should split
static bool addr_split_heuristic(int arr_uses, int stride, int scale) {
    // doesn't matter if we do *1 *2 *4 *8, all
    // basically just an LEA. once we leave LEA
    // levels we need to do explicit ops with regs
    // which increases pressure.
    int cost = 0;
    if (stride != 1 << scale || scale >= 4) {
        cost = 3;
    } else {
        cost = 1;
    }

    return cost*arr_uses > 10;
}

// store(binop(load(a), b))
static bool can_folded_store(TB_Node* mem, TB_Node* addr, TB_Node* src) {
    switch (src->type) {
        case TB_AND:
        case TB_OR:
        case TB_XOR:
        case TB_ADD:
        case TB_SUB:
        return
            src->inputs[1]->type == TB_LOAD &&
            src->inputs[1]->inputs[1] == mem &&
            src->inputs[1]->inputs[2] == addr &&
            src->users->next == NULL &&
            src->inputs[1]->users->next == NULL;

        default: return false;
    }
}

// not TLS
static bool simple_symbol(TB_Node* n) {
    if (n->type != TB_SYMBOL) return false;

    TB_Symbol* sym = TB_NODE_GET_EXTRA_T(n, TB_NodeSymbol)->sym;
    if (sym->tag != TB_SYMBOL_GLOBAL) return true;

    TB_Global* g = (TB_Global*) sym;
    return (sym->module->sections[g->parent].flags & TB_MODULE_SECTION_TLS) == 0;
}

static bool is_tls_symbol(TB_Symbol* sym) {
    if (sym->tag == TB_SYMBOL_GLOBAL) {
        TB_Global* g = (TB_Global*) sym;
        return sym->module->sections[g->parent].flags & TB_MODULE_SECTION_TLS;
    } else {
        return false;
    }
}

static TB_Node* to_mach_local(Ctx* restrict ctx, TB_Function* f, TB_Node* n) {
    assert(n->type == TB_LOCAL);
    TB_NodeLocal* local = TB_NODE_GET_EXTRA(n);
    ctx->stack_usage = align_up(ctx->stack_usage + local->size, local->align);
    int disp = ctx->stack_usage;

    // machine address is effectively a MemberAccess on SP
    TB_Node* addr = tb_alloc_node(f, TB_MACH_LOCAL, n->dt, 2, sizeof(TB_NodeMachLocal));
    set_input(f, addr, n->inputs[0], 0); // root node
    TB_NODE_SET_EXTRA(addr, TB_NodeMachLocal, .name = local->name, .type = local->type, .disp = disp);
    return addr;
}

static TB_Node* node_isel(Ctx* restrict ctx, TB_Function* f, TB_Node* n) {
    if (n->type == TB_PROJ) {
        return n;
    } else if (n->type == TB_PHI) {
        if (n->dt.type == TB_FLOAT || n->dt.type == TB_INT || n->dt.type == TB_PTR) {
            // we just want some copies on the data edges which RA will coalesce, this way we
            // never leave SSA.
            FOREACH_N(i, 1, n->input_count) {
                TB_Node* cpy = tb_alloc_node(f, TB_MACH_MOVE, n->inputs[i]->dt, 2, 0);
                set_input(f, cpy, n->inputs[i], 1);
                set_input(f, n, cpy, i);
            }

            RegMask* rm = ctx->normie_mask[n->dt.type == TB_FLOAT ? REG_CLASS_XMM : REG_CLASS_GPR];

            // just in case we have some recursive phis, RA should be able to fold it away later.
            // we have to be a bit hacky since we can't subsume the node with something that's
            // referencing it (we'll get a cycle we didn't want).
            TB_Node* cpy = tb_alloc_node(f, TB_MACH_COPY, n->dt, 2, sizeof(TB_NodeMachCopy));
            TB_NODE_SET_EXTRA(cpy, TB_NodeMachCopy, .def = rm, .use = rm);

            subsume_node2(f, n, cpy);
            set_input(f, cpy, n, 1);

            // we did the subsumes for it
            return n;
        } else {
            return n;
        }
    } else if (n->type == TB_ZERO_EXT) {
        TB_DataType src_dt = n->inputs[1]->dt;
        int bits_in_type = src_dt.type == TB_PTR ? 64 : src_dt.data;
        if (bits_in_type == 8 || bits_in_type == 16 || bits_in_type == 32 || bits_in_type == 64) {
            tb__gvn_remove(f, n);
            n->type = x86_movzx;
            return n;
        } else {
            // uint64_t mask = UINT64_MAX >> (64 - bits_in_type);
            tb_todo();
        }
    } else if (n->type == TB_SIGN_EXT) {
        TB_DataType src_dt = n->inputs[1]->dt;
        int bits_in_type = src_dt.type == TB_PTR ? 64 : src_dt.data;
        if (bits_in_type == 8 || bits_in_type == 16 || bits_in_type == 32 || bits_in_type == 64) {
            tb__gvn_remove(f, n);
            n->type = x86_movsx;
            return n;
        } else {
            // unconventional sizes do:
            //   SHL dst, x
            //   SAR dst, x (or SHR if zero ext)
            //
            // where x is 'reg_width - val_width'
            // int dst_bits = dt == TB_X86_TYPE_QWORD ? 64 : 32;
            // int ext = is_signed ? SAR : SHR;
            // Val imm = val_imm(dst_bits - bits_in_type);
            tb_todo();
        }
    } else if (n->type == TB_LOCAL) {
        TB_Node* addr = to_mach_local(ctx, f, n);

        // we don't directly ref the MachLocal, this is the accessor op whenever we're
        // not folding into some other op nicely.
        TB_Node* op = tb_alloc_node(f, x86_lea, TB_TYPE_PTR, 5, sizeof(TB_NodeMachLocal));
        X86MemOp* op_extra = TB_NODE_GET_EXTRA(op);
        op_extra->mode = MODE_LD;
        set_input(f, op, addr, 2); // addr
        return op;
    } else if ((n->type >= TB_SHL && n->type <= TB_ROR) && n->inputs[2]->type == TB_INTEGER_CONST) {
        const static int ops[] = { x86_shlimm, x86_shrimm, x86_sarimm, x86_rolimm, x86_rorimm };
        X86NodeType type = ops[n->type - TB_SHL];
        uint64_t imm = TB_NODE_GET_EXTRA_T(n->inputs[2], TB_NodeInt)->value;

        TB_Node* op = tb_alloc_node(f, type, n->dt, 3, sizeof(X86MemOp));
        set_input(f, op, n->inputs[1], 2);
        TB_NODE_SET_EXTRA(op, X86MemOp, .imm = imm & 63);
        return op;
    } else if (n->type == TB_CALL) {
        TB_Node* op = tb_alloc_node(f, x86_call, n->dt, n->input_count, sizeof(X86Call));
        set_input(f, op, n->inputs[0], 0); // ctrl
        set_input(f, op, n->inputs[1], 1); // mem
        X86Call* op_extra = TB_NODE_GET_EXTRA(op);

        // check for static call
        if (n->inputs[2]->type == TB_SYMBOL) {
            op->type = x86_static_call;
            op_extra->sym = TB_NODE_GET_EXTRA_T(n->inputs[2], TB_NodeSymbol)->sym;
        } else {
            set_input(f, op, n->inputs[2], 2);
        }

        const struct ParamDesc* abi = &param_descs[ctx->abi_index];
        op_extra->clobber_gpr = abi->caller_saved_gprs;
        op_extra->clobber_xmm = ~0ull >> (64 - abi->caller_saved_xmms);

        int gprs_used = 0, xmms_used = 0;
        FOREACH_N(i, 3, n->input_count) {
            int param_num = i - 3;

            // on win64 we always have the XMMs and GPRs used match the param_num
            // so if XMM2 is used, it's always the 3rd parameter.
            if (ctx->abi_index == 0) { xmms_used = gprs_used = param_num; }

            if (n->inputs[i]->dt.type == TB_FLOAT) {
                if (xmms_used < abi->xmm_count) {
                    op_extra->clobber_xmm &= ~(1u << xmms_used);
                    xmms_used += 1;
                }
            } else {
                assert(n->inputs[i]->dt.type == TB_INT || n->inputs[i]->dt.type == TB_PTR);
                if (gprs_used < abi->gpr_count) {
                    op_extra->clobber_gpr &= ~(1u << abi->gprs[i]);
                    gprs_used += 1;
                }
            }

            set_input(f, op, n->inputs[i], i);
        }
        return op;
    } else if (n->type == TB_BRANCH) {
        // convert an if into a machine-if
        TB_NodeBranch* br = TB_NODE_GET_EXTRA(n);
        assert(br->succ_count == 2 && "TODO");

        TB_Node* cond = n->inputs[1];
        uint64_t falsey = br->keys[0].key;
        if (cond->type >= TB_CMP_EQ && cond->type <= TB_CMP_FLE) {
            TB_DataType cmp_dt = TB_NODE_GET_EXTRA_T(cond, TB_NodeCompare)->cmp_dt;
            assert(falsey == 0 || falsey == 1);

            // starts at 1 since the keys[0] maps to the "falsey" edge
            int flip = 1;

            TB_Node* a = cond->inputs[1];
            TB_Node* b = cond->inputs[2];
            if (a->type == TB_INTEGER_CONST && b->type != TB_INTEGER_CONST) {
                flip ^= 1;
                SWAP(TB_Node*, a, b);
            }

            int32_t x;
            TB_Node* mach_cond = NULL;
            if ((cmp_dt.type == TB_INT || cmp_dt.type == TB_PTR) && try_for_imm32(cmp_dt.type == TB_PTR ? 64 : cmp_dt.data, b, &x)) {
                // x86_cmpimm n[1]
                mach_cond = tb_alloc_node(f, x86_cmpimm, TB_TYPE_I8, 3, sizeof(X86MemOp));
                X86MemOp* op_extra = TB_NODE_GET_EXTRA(mach_cond);
                op_extra->imm = x;
            } else {
                mach_cond = tb_alloc_node(f, x86_cmp, TB_TYPE_I8, 5, sizeof(X86MemOp));
                set_input(f, mach_cond, b, 4);
            }
            set_input(f, mach_cond, a, 2);
            set_input(f, n, mach_cond, 1);

            Cond cc;
            switch (cond->type) {
                case TB_CMP_EQ:  cc = E;  break;
                case TB_CMP_NE:  cc = NE; break;
                case TB_CMP_SLT: cc = L;  break;
                case TB_CMP_SLE: cc = LE; break;
                case TB_CMP_ULT: cc = B;  break;
                case TB_CMP_ULE: cc = BE; break;
                case TB_CMP_FLT: cc = B;  break;
                case TB_CMP_FLE: cc = BE; break;
                default: tb_unreachable();
            }
            br->keys[0].key = cc ^ flip;
        } else {
            TB_Node* mach_cond = tb_alloc_node(f, x86_cmp, TB_TYPE_I8, 3, sizeof(X86MemOp));
            TB_NODE_SET_EXTRA(mach_cond, X86MemOp, .imm = 0);

            set_input(f, mach_cond, cond, 2);
            set_input(f, n, mach_cond,    1);
        }

        return n;
    }

    int32_t x;
    if (n->type == TB_MUL && try_for_imm32(n->dt.data, n->inputs[2], &x)) {
        TB_Node* op = tb_alloc_node(f, x86_imulimm, n->dt, 2, sizeof(X86MemOp));
        set_input(f, op, n->inputs[1], 1);
        TB_NODE_SET_EXTRA(op, X86MemOp, .imm = x);
        return op;
    }

    // any of these ops might be the starting point to complex addressing modes
    if ((n->type >= TB_AND && n->type <= TB_SUB) || n->type == TB_LOAD || n->type == TB_STORE || n->type == TB_MEMBER_ACCESS || n->type == TB_ARRAY_ACCESS) {
        const static int ops[] = { x86_and, x86_or, x86_xor, x86_add, x86_sub };

        // folded binop with immediate
        int32_t x;
        if (n->type >= TB_AND && n->type <= TB_SUB) {
            assert(n->dt.type == TB_INT);
            if (try_for_imm32(n->dt.data, n->inputs[2], &x)) {
                X86NodeType type = ops[n->type - TB_AND] + (x86_andimm - x86_and);

                TB_Node* op = tb_alloc_node(f, type, n->dt, 3, sizeof(X86MemOp));
                X86MemOp* op_extra = TB_NODE_GET_EXTRA(op);
                op_extra->imm = x;

                set_input(f, op, n->inputs[1], 2);
                return op;
            }
        }

        TB_Node* op = tb_alloc_node(f, x86_lea, n->dt, 5, sizeof(X86MemOp));
        X86MemOp* op_extra = TB_NODE_GET_EXTRA(op);
        op_extra->mode = MODE_LD;

        // folded load now
        if (n->type == TB_STORE) {
            op_extra->mode = MODE_ST;
            op->type = x86_mov;
            op->dt = TB_TYPE_MEMORY;

            set_input(f, op, n->inputs[0], 0); // ctrl in
            set_input(f, op, n->inputs[1], 1); // mem in

            if (can_folded_store(n->inputs[1], n->inputs[2], n->inputs[3])) {
                TB_Node* binop = n->inputs[3];
                assert(binop->type >= TB_AND && binop->type <= TB_SUB);

                op->type = ops[binop->type - TB_AND];
                set_input(f, op, binop->inputs[2], 4); // val
            } else {
                set_input(f, op, n->inputs[3], 4); // val
            }
            n = n->inputs[2];
        } else {
            // folded binop
            if (n->type >= TB_AND && n->type <= TB_SUB) {
                op_extra->mode = MODE_REG;
                op->type = ops[n->type - TB_AND];
                set_input(f, op, n->inputs[1], 4);
                n = n->inputs[2];
            }

            // folded load now
            if (n->type == TB_LOAD) {
                op_extra->mode = MODE_LD;
                if (op->type == x86_lea) {
                    op->type = x86_mov;
                }

                set_input(f, op, n->inputs[0], 0); // ctrl in
                set_input(f, op, n->inputs[1], 1); // mem in
                n = n->inputs[2];
            }
        }

        // [... + disp]
        if (n->type == TB_MEMBER_ACCESS) {
            op_extra->disp = TB_NODE_GET_EXTRA_T(n, TB_NodeMember)->offset;
            n = n->inputs[1];
        }

        if (n->type == TB_ARRAY_ACCESS) {
            int32_t stride = TB_NODE_GET_EXTRA_T(n, TB_NodeArray)->stride;
            int scale = tb_ffs(stride) - 1;

            // [... + index*scale] given scale is 1,2,4,8
            if (stride == (1<<scale) && scale <= 3) {
                set_input(f, op, n->inputs[2], 3);
                op_extra->scale = scale;
                n = n->inputs[1];
            }
        }

        if (n->type == x86_lea && n->inputs[2]->type == TB_MACH_LOCAL && n->inputs[3] == NULL) {
            // we're referring to a Local, let's just use the MachLocal directly
            set_input(f, op, n->inputs[2], 2);
        } else if (n->type == TB_LOCAL) {
            // if we found a Local first, convert it to the machine form without the lea op
            TB_Node* addr = to_mach_local(ctx, f, n);
            set_input(f, op, addr, 2);

            // we don't directly ref the MachLocal, this is the accessor op whenever we're
            // not folding into some other op nicely.
            TB_Node* op_lea = tb_alloc_node(f, x86_lea, TB_TYPE_PTR, 5, sizeof(TB_NodeMachLocal));
            X86MemOp* op_extra = TB_NODE_GET_EXTRA(op_lea);
            op_extra->mode = MODE_LD;
            set_input(f, op_lea, addr, 2); // addr
            subsume_node(f, n, op_lea);
        } else {
            set_input(f, op, n, 2);
        }
        return op;
    }

    return NULL;
}

static bool node_flags(Ctx* restrict ctx, TB_Node* n) {
    switch (n->type) {
        // regions & misc nodes don't even generate ops
        case TB_PHI:
        case TB_PROJ:
        case TB_REGION:
        case TB_AFFINE_LOOP:
        case TB_NATURAL_LOOP:
        // moves don't affect FLAGS
        case x86_mov:
        case x86_movimm:
        case x86_movsx:
        case x86_movzx:
        case TB_MACH_MOVE:
        case TB_MACH_COPY:
        case TB_INTEGER_CONST:
        // actually uses flags, that's handled in node_constraint
        case TB_BRANCH:
        case TB_RETURN:
        // actually produces FLAGS we care about
        case x86_cmp: case x86_cmpimm:
        return false;

        default:
        return true;
    }
}

static int node_tmp_count(Ctx* restrict ctx, TB_Node* n) {
    switch (n->type) {
        case x86_call: case x86_static_call: {
            X86Call* op_extra = TB_NODE_GET_EXTRA(n);
            return tb_popcount(op_extra->clobber_gpr) + tb_popcount(op_extra->clobber_xmm);
        }

        default: return 0;
    }
}

static RegMask* node_constraint(Ctx* restrict ctx, TB_Node* n, RegMask** ins) {
    switch (n->type) {
        case TB_REGION:
        case TB_AFFINE_LOOP:
        case TB_NATURAL_LOOP:
        if (ins) {
            // region inputs are all control
            FOREACH_N(i, 1, n->input_count) { ins[i] = &TB_REG_EMPTY; }
        }
        return &TB_REG_EMPTY;

        case TB_MACH_LOCAL: return &TB_REG_EMPTY;
        case TB_MACH_COPY: {
            TB_NodeMachCopy* move = TB_NODE_GET_EXTRA(n);
            if (ins) { ins[1] = move->use; }
            return move->def;
        }

        case TB_MACH_MOVE: {
            RegMask* rm = ctx->normie_mask[n->dt.type == TB_FLOAT ? REG_CLASS_XMM : REG_CLASS_GPR];
            if (ins) { ins[1] = rm; }
            return rm;
        }

        case TB_PHI: {
            if (ins) {
                FOREACH_N(i, 1, n->input_count) { ins[i] = &TB_REG_EMPTY; }
            }

            if (n->dt.type == TB_MEMORY) return &TB_REG_EMPTY;
            if (n->dt.type == TB_FLOAT) return ctx->normie_mask[REG_CLASS_XMM];
            return ctx->normie_mask[REG_CLASS_GPR];
        }

        case TB_INTEGER_CONST:
        case TB_SYMBOL:
        return ctx->normie_mask[REG_CLASS_GPR];

        case TB_PROJ: {
            if (n->dt.type == TB_MEMORY || n->dt.type == TB_CONTROL) {
                return &TB_REG_EMPTY;
            }

            int i = TB_NODE_GET_EXTRA_T(n, TB_NodeProj)->index;
            if (n->inputs[0]->type == TB_ROOT) {
                const struct ParamDesc* params = &param_descs[ctx->abi_index];
                assert(i >= 2);
                if (i == 2) {
                    // RPC is inaccessible for now
                    return &TB_REG_EMPTY;
                } else if (n->dt.type == TB_FLOAT) {
                    return intern_regmask(ctx, REG_CLASS_XMM, false, 1u << (i - 3));
                } else {
                    return intern_regmask(ctx, REG_CLASS_GPR, false, 1u << params->gprs[i - 3]);
                }
            } else if (n->inputs[0]->type == x86_call || n->inputs[0]->type == x86_static_call) {
                assert(i == 2 || i == 3);
                if (n->dt.type == TB_FLOAT) {
                    if (i >= 2) { return intern_regmask(ctx, REG_CLASS_XMM, false, 1u << (i - 2)); }
                } else {
                    if (i >= 2) { return intern_regmask(ctx, REG_CLASS_GPR, false, 1u << (i == 2 ? RAX : RDX)); }
                }
            } else {
                tb_todo();
                return &TB_REG_EMPTY;
            }
        }

        case x86_lea:
        // ANY_GPR = OP(ANY_GPR, ANY_GPR)
        case x86_add: case x86_or:  case x86_and: case x86_sub:
        case x86_xor: case x86_cmp: case x86_mov: case x86_test:
        // ANY_GPR = OP(ANY_GPR, IMM)
        case x86_addimm: case x86_orimm:  case x86_andimm: case x86_subimm:
        case x86_xorimm: case x86_cmpimm: case x86_movimm: case x86_testimm: case x86_imulimm:
        case x86_shlimm: case x86_shrimm: case x86_sarimm: case x86_rolimm: case x86_rorimm:
        {
            RegMask* rm = ctx->normie_mask[REG_CLASS_GPR];
            if (ins) {
                ins[1] = &TB_REG_EMPTY;
                FOREACH_N(i, 2, n->input_count) {
                    ins[i] = n->inputs[i] ? rm : &TB_REG_EMPTY;
                }

                if (n->inputs[2] && n->inputs[2]->type == TB_MACH_LOCAL) {
                    ins[2] = &TB_REG_EMPTY;
                }
            }

            X86MemOp* op = TB_NODE_GET_EXTRA(n);
            if (op->mode == MODE_ST) {
                return &TB_REG_EMPTY;
            } else if (n->type == x86_cmp || n->type == x86_cmpimm) {
                return ctx->normie_mask[REG_CLASS_FLAGS];
            } else {
                return ctx->normie_mask[REG_CLASS_GPR];
            }
        }

        case TB_MUL:
        {
            RegMask* rm = ctx->normie_mask[REG_CLASS_GPR];
            if (ins) { ins[1] = ins[2] = rm; }
            return rm;
        }

        case TB_SHL: case TB_SHR: case TB_ROL: case TB_ROR: case TB_SAR:
        {
            RegMask* rm = ctx->normie_mask[REG_CLASS_GPR];
            if (ins) {
                ins[1] = rm;
                ins[2] = intern_regmask(ctx, REG_CLASS_GPR, false, 1u << RCX);
            }
            return rm;
        }

        case x86_movsx: case x86_movzx:
        {
            RegMask* rm = ctx->normie_mask[REG_CLASS_GPR];
            if (ins) { ins[1] = rm; }
            return rm;
        }

        case TB_SELECT:
        {
            RegMask* rm = ctx->normie_mask[REG_CLASS_GPR];
            if (ins) {
                ins[1] = rm;
                ins[2] = rm;
                ins[3] = rm;
            }
            return rm;
        }

        case TB_MEMSET:
        {
            if (ins) {
                ins[1] = &TB_REG_EMPTY;
                ins[2] = intern_regmask(ctx, REG_CLASS_GPR, false, 1u << RDI);
                ins[3] = intern_regmask(ctx, REG_CLASS_GPR, false, 1u << RAX);
                ins[4] = intern_regmask(ctx, REG_CLASS_GPR, false, 1u << RCX);
            }
            return &TB_REG_EMPTY;
        }

        case TB_BRANCH:
        {
            if (ins) { ins[1] = ctx->normie_mask[REG_CLASS_FLAGS]; }
            return &TB_REG_EMPTY;
        }

        case TB_RETURN:
        {
            if (ins) {
                static int ret_gprs[2] = { RAX, RDX };
                assert(n->input_count <= 5 && "At most 2 return values :(");

                ins[1] = &TB_REG_EMPTY; // mem
                ins[2] = &TB_REG_EMPTY; // rpc

                FOREACH_N(i, 3, n->input_count) {
                    TB_DataType dt = n->inputs[i]->dt;
                    if (dt.type == TB_FLOAT) {
                        ins[i] = intern_regmask(ctx, REG_CLASS_XMM, false, 1u << (i-3));
                    } else {
                        ins[i] = intern_regmask(ctx, REG_CLASS_GPR, false, 1u << ret_gprs[i-3]);
                    }
                }
            }
            return &TB_REG_EMPTY;
        }

        case x86_static_call:
        case x86_call:
        {
            if (ins) {
                const struct ParamDesc* abi = &param_descs[ctx->abi_index];

                int abi_index = ctx->abi_index;
                int gprs_used = 0, xmms_used = 0;

                ins[1] = &TB_REG_EMPTY;
                ins[2] = n->type == x86_static_call ? &TB_REG_EMPTY : ctx->normie_mask[REG_CLASS_GPR];

                FOREACH_N(i, 3, n->input_count) {
                    int param_num = i - 3;

                    // on win64 we always have the XMMs and GPRs used match the param_num
                    // so if XMM2 is used, it's always the 3rd parameter.
                    if (abi_index == 0) { xmms_used = gprs_used = param_num; }

                    if (n->inputs[i]->dt.type == TB_FLOAT) {
                        if (xmms_used < abi->xmm_count) {
                            ins[i] = intern_regmask(ctx, REG_CLASS_XMM, false, 1u << xmms_used);
                            xmms_used += 1;
                        } else {
                            tb_todo();
                        }
                    } else {
                        assert(n->inputs[i]->dt.type == TB_INT || n->inputs[i]->dt.type == TB_PTR);
                        if (gprs_used < abi->gpr_count) {
                            ins[i] = intern_regmask(ctx, REG_CLASS_GPR, false, 1u << abi->gprs[gprs_used]);
                            gprs_used += 1;
                        } else {
                            tb_todo();
                        }
                    }
                }

                size_t j = n->input_count;
                X86Call* op_extra = TB_NODE_GET_EXTRA(n);
                for (uint64_t bits = op_extra->clobber_gpr, k = 0; bits; bits >>= 1, k++) {
                    if (bits & 1) { ins[j++] = intern_regmask(ctx, REG_CLASS_GPR, false, 1u << k); }
                }

                for (uint64_t bits = op_extra->clobber_xmm, k = 0; bits; bits >>= 1, k++) {
                    if (bits & 1) { ins[j++] = intern_regmask(ctx, REG_CLASS_XMM, false, 1u << k); }
                }
            }

            // the tuple node doesn't itself produce the result
            return &TB_REG_EMPTY;
        }

        default:
        tb_todo();
        return &TB_REG_EMPTY;
    }
}

static int op_reg_at(Ctx* ctx, TB_Node* n, int class) {
    assert(ctx->vreg_map[n->gvn] > 0);
    VReg* vreg = &ctx->vregs[ctx->vreg_map[n->gvn]];
    assert(vreg->assigned >= 0);
    assert(vreg->class == class);
    return vreg->assigned;
}

static Val op_at(Ctx* ctx, TB_Node* n) {
    assert(ctx->vreg_map[n->gvn] > 0);
    VReg* vreg = &ctx->vregs[ctx->vreg_map[n->gvn]];
    if (vreg->class == REG_CLASS_STK) {
        tb_todo();
        return val_stack(vreg->assigned*8);
    } else {
        assert(vreg->assigned >= 0);
        return (Val) { .type = vreg->class == REG_CLASS_XMM ? VAL_XMM : VAL_GPR, .reg = vreg->assigned };
    }
}

static void emit_goto(Ctx* ctx, TB_CGEmitter* e, MachineBB* succ) {
    if (ctx->fallthrough != succ->id) {
        EMIT1(e, 0xE9); EMIT4(e, 0);
        tb_emit_rel32(e, &e->labels[succ->id], GET_CODE_POS(e) - 4);
    }
}

static void node_emit(Ctx* restrict ctx, TB_CGEmitter* e, TB_Node* n, VReg* vreg) {
    switch (n->type) {
        // some ops don't do shit lmao
        case TB_PHI:
        case TB_REGION:
        case TB_AFFINE_LOOP:
        case TB_NATURAL_LOOP:
        case TB_PROJ:
        case TB_MACH_LOCAL:
        break;

        case TB_BRANCH: {
            TB_NodeBranch* br = TB_NODE_GET_EXTRA(n);

            // the arena on the function should also be available at this time, we're
            // in the TB_Passes
            TB_Arena* arena = ctx->f->arena;
            TB_ArenaSavepoint sp = tb_arena_save(arena);
            int* succ = tb_arena_alloc(arena, br->succ_count * sizeof(int));

            // fill successors
            bool has_default = false;
            FOR_USERS(u, n) {
                if (USERN(u)->type == TB_PROJ) {
                    int index = TB_NODE_GET_EXTRA_T(USERN(u), TB_NodeProj)->index;
                    TB_Node* succ_n = cfg_next_bb_after_cproj(USERN(u));

                    if (index == 0) {
                        has_default = !cfg_is_unreachable(succ_n);
                    }

                    MachineBB* mbb = node_to_bb(ctx, succ_n);
                    succ[index] = mbb->id;
                }
            }

            if (br->succ_count == 2) {
                Val taken    = val_label(succ[0]);
                Val fallthru = val_label(succ[1]);
                Cond cc = br->keys[0].key;

                // if flipping avoids a jmp, do that
                if (ctx->fallthrough == taken.label) {
                    x86_jcc(e, cc ^ 1, fallthru);
                } else {
                    x86_jcc(e, cc, taken);
                    if (ctx->fallthrough != fallthru.label) {
                        x86_jmp(e, fallthru);
                    }
                }
            } else {
                tb_todo();
            }
            break;
        }

        case TB_SYMBOL: {
            TB_Symbol* sym = TB_NODE_GET_EXTRA_T(n, TB_NodeSymbol)->sym;

            Val dst = op_at(ctx, n);
            Val src = val_global(sym, 0);
            inst2(e, LEA, &dst, &src, TB_X86_TYPE_QWORD);
            break;
        }

        case TB_INTEGER_CONST: {
            uint64_t x = TB_NODE_GET_EXTRA_T(n, TB_NodeInt)->value;
            uint32_t hi = x >> 32ull;

            TB_X86_DataType dt = legalize_int2(n->dt);
            Val dst = op_at(ctx, n);
            if (x == 0) {
                // xor reg, reg
                inst2(e, XOR, &dst, &dst, dt);
            } else if (hi == 0 || dt == TB_X86_TYPE_QWORD) {
                Val src = val_abs(x);
                inst2(e, MOVABS, &dst, &src, dt);
            } else {
                Val src = val_imm(x);
                inst2(e, MOV, &dst, &src, dt);
            }
            break;
        }

        case TB_MACH_MOVE:
        case TB_MACH_COPY: {
            TB_X86_DataType dt = legalize_int2(n->dt);
            Val dst = op_at(ctx, n);
            Val src = op_at(ctx, n->inputs[1]);

            if (!is_value_match(&dst, &src)) {
                inst2(e, MOV, &dst, &src, dt);
            }
            break;
        }

        // epilogue
        case TB_RETURN: {
            size_t pos = e->count;
            // emit_epilogue(ctx, e, ctx->stack_usage);
            EMIT1(e, 0xC3);
            ctx->epilogue_length = e->count - pos;
            break;
        }

        case x86_lea:
        case x86_add: case x86_or:  case x86_and: case x86_sub:
        case x86_xor: case x86_cmp: case x86_mov: case x86_test:
        case x86_addimm: case x86_orimm:  case x86_andimm: case x86_subimm:
        case x86_xorimm: case x86_cmpimm: case x86_movimm: case x86_testimm:
        case x86_shlimm: case x86_shrimm: case x86_sarimm: case x86_rolimm: case x86_rorimm:
        {
            const static int ops[] = {
                // binop
                ADD, OR, AND, SUB, XOR, CMP, MOV, TEST,
                // binop with immediates
                ADD, OR, AND, SUB, XOR, CMP, MOV, TEST,
                // shifts
                SHL, SHR, SAR, ROL, ROR,
                // misc (except for imul because it's weird)
                LEA,
            };

            TB_X86_DataType dt;
            if (n->dt.type == TB_MEMORY || n->type == x86_cmp) {
                dt = legalize_int2(n->inputs[4]->dt);
            } else if (n->type == x86_cmpimm) {
                dt = legalize_int2(n->inputs[2]->dt);
            } else {
                dt = legalize_int2(n->dt);
            }

            X86MemOp* op = TB_NODE_GET_EXTRA(n);
            TB_Node* lhs_n = n->input_count == 3 ? n->inputs[2] : n->inputs[4];

            Val rhs = { 0 };
            if (n->type >= x86_addimm && n->type <= x86_rorimm) {
                rhs = val_imm(op->imm);
            } else if (op->mode == MODE_LD || op->mode == MODE_ST) {
                rhs.type = VAL_MEM;
                if (n->inputs[2]->type == TB_MACH_LOCAL) {
                    int disp = TB_NODE_GET_EXTRA_T(n->inputs[2], TB_NodeMachLocal)->disp;
                    rhs.reg = RSP;
                    rhs.imm = ctx->stack_usage - disp;
                } else {
                    rhs.reg = op_reg_at(ctx, n->inputs[2], REG_CLASS_GPR);
                }
                if (n->inputs[3]) {
                    rhs.index = op_at(ctx, n->inputs[3]).reg;
                } else {
                    rhs.index = -1;
                }
                rhs.imm   += op->disp;
                rhs.scale  = op->scale;
            } else {
                rhs.type = VAL_GPR;
                rhs.reg = op_reg_at(ctx, n->inputs[2], REG_CLASS_GPR);
            }

            if (op->mode == MODE_ST) {
                Val lhs = op_at(ctx, lhs_n);
                inst2(e, ops[n->type - x86_add], &rhs, &lhs, dt);
            } else if (n->type == x86_cmp || n->type == x86_cmpimm) {
                Val lhs = op_at(ctx, lhs_n);
                inst2(e, CMP, &lhs, &rhs, dt);
            } else {
                Val dst = op_at(ctx, n);
                if (lhs_n != NULL) {
                    assert(n->type != x86_lea);
                    Val lhs = op_at(ctx, lhs_n);
                    if (!is_value_match(&dst, &lhs)) {
                        inst2(e, MOV, &dst, &lhs, dt);
                    }
                }

                inst2(e, ops[n->type - x86_add], &dst, &rhs, dt);
            }
            break;
        }

        case x86_imulimm: {
            TB_X86_DataType dt = legalize_int2(n->dt);
            X86MemOp* op = TB_NODE_GET_EXTRA(n);

            Val dst = op_at(ctx, n);
            Val lhs = op_at(ctx, n->inputs[1]);

            inst2(e, IMUL3, &dst, &lhs, dt);
            if (dt == TB_X86_TYPE_WORD) {
                EMIT2(e, op->imm);
            } else {
                EMIT4(e, op->imm);
            }
            break;
        }

        case TB_MUL: {
            TB_X86_DataType dt = legalize_int2(n->dt);

            Val dst  = op_at(ctx, n);
            Val lhs  = op_at(ctx, n->inputs[1]);
            Val rhs  = op_at(ctx, n->inputs[2]);

            if (!is_value_match(&dst, &lhs)) {
                inst2(e, MOV, &dst, &lhs, dt);
            }
            inst2(e, IMUL, &dst, &rhs, dt);
            break;
        }

        case TB_SELECT: {
            TB_X86_DataType dt = legalize_int2(n->dt);
            X86MemOp* op = TB_NODE_GET_EXTRA(n);

            Val dst  = op_at(ctx, n);
            Val cond = op_at(ctx, n->inputs[1]);
            Val lhs  = op_at(ctx, n->inputs[2]);
            Val rhs  = op_at(ctx, n->inputs[3]);

            inst2(e, TEST, &cond, &cond, dt);
            if (!is_value_match(&dst, &lhs)) {
                inst2(e, MOV, &dst, &lhs, dt);
            }
            inst2(e, CMOVO+E, &dst, &rhs, dt);
            break;
        }

        case TB_SHL:
        case TB_SHR:
        case TB_ROL:
        case TB_ROR:
        case TB_SAR: {
            TB_X86_DataType dt = legalize_int2(n->dt);

            Val dst = op_at(ctx, n);
            Val lhs = op_at(ctx, n->inputs[1]);
            if (!is_value_match(&dst, &lhs)) {
                inst2(e, MOV, &dst, &lhs, dt);
            }

            InstType op;
            switch (n->type) {
                case TB_SHL: op = SHL; break;
                case TB_SHR: op = SHR; break;
                case TB_ROL: op = ROL; break;
                case TB_ROR: op = ROR; break;
                case TB_SAR: op = SAR; break;
                default: tb_todo();
            }

            Val rcx = val_gpr(RCX);
            inst2(e, op, &dst, &rcx, dt);
            break;
        }

        case x86_movsx: case x86_movzx:
        {
            bool is_signed = n->type == TB_SIGN_EXT;
            TB_DataType src_dt = n->inputs[1]->dt;
            int bits_in_type = src_dt.type == TB_PTR ? 64 : src_dt.data;

            int op = 0;
            TB_X86_DataType dt = legalize_int2(n->dt);
            switch (bits_in_type) {
                case 8:  op =  is_signed ? MOVSXB : MOVZXB; break;
                case 16: op =  is_signed ? MOVSXB : MOVZXW; break;
                case 32: op = (is_signed ? MOVSXD : MOV), dt = TB_X86_TYPE_DWORD; break;
                case 64: op = MOV;                          break;
            }

            Val dst = op_at(ctx, n);
            if (is_signed && dt <= TB_X86_TYPE_DWORD) {
                dt = TB_X86_TYPE_DWORD;
            }

            Val lhs  = op_at(ctx, n->inputs[1]);
            inst2(e, op ? op : MOV, &dst, &lhs, dt);
            break;
        }

        case TB_MEMSET: {
            EMIT1(e, 0xF3);
            EMIT1(e, 0xAA);
            break;
        }

        case x86_static_call: {
            X86Call* op_extra = TB_NODE_GET_EXTRA(n);
            Val sym = val_global(op_extra->sym, 0);

            inst1(e, CALL, &sym, TB_X86_TYPE_QWORD);
            break;
        }

        default:
        tb_todo();
        break;
    }
}

static int node_latency(TB_Function* f, TB_Node* n) {
    switch (n->type) {
        case x86_movsx: case x86_movzx: {
            X86MemOp* op = TB_NODE_GET_EXTRA(n);
            return 2 + (op->mode == MODE_LD ? 3 : 0);
        }

        // load/store ops should count as a bit slower
        case x86_add: case x86_or:  case x86_and: case x86_sub:
        case x86_xor: case x86_cmp: case x86_mov: case x86_test:
        case x86_addimm: case x86_orimm:  case x86_andimm: case x86_subimm:
        case x86_xorimm: case x86_cmpimm: case x86_movimm: case x86_testimm: case x86_imulimm:
        case x86_shlimm: case x86_shrimm: case x86_sarimm: case x86_rolimm: case x86_rorimm:
        {
            X86MemOp* op = TB_NODE_GET_EXTRA(n);

            int clk;
            switch (n->type) {
                case x86_imulimm: clk = 3; break;
                default:          clk = 1; break;
            }

            if (op->mode == MODE_LD) clk += 3;
            // every store op except for x86_mov will do both a ld(3 clks) + st(4 clks)
            if (op->mode == MODE_ST) clk += (n->type != x86_mov ? 7 : 4);
            return clk;
        }

        case TB_MACH_MOVE:
        return 0; // cheapest op so that it tries to schedule it later

        default: return 1;
    }
}

#if 0
#define OUT1(m) (dst->outs[0]->dt = n->dt, dst->outs[0]->mask = (m))
static void isel_node(Ctx* restrict ctx, Tile* dst, TB_Node* n) {
    switch (n->type) {
        // no inputs
        case TB_REGION:
        case TB_NATURAL_LOOP:
        case TB_AFFINE_LOOP:
        case TB_ROOT:
        case TB_TRAP:
        case TB_CALLGRAPH:
        case TB_SPLITMEM:
        case TB_MERGEMEM:
        case TB_UNREACHABLE:
        case TB_DEBUGBREAK:
        case TB_INTEGER_CONST:
        case TB_FLOAT32_CONST:
        case TB_FLOAT64_CONST:
        case TB_POISON:
        break;

        case TB_SYMBOL: {
            TB_Symbol* sym = TB_NODE_GET_EXTRA_T(n, TB_NodeSymbol)->sym;
            if (is_tls_symbol(sym)) {
                // on windows we'll need one temporary, linux needs none
                if (ctx->abi_index == 0) {
                    dst->ins = tb_arena_alloc(tmp_arena, 1 * sizeof(TileInput));
                    dst->in_count = 1;
                    dst->ins[0].mask = ctx->normie_mask[REG_CLASS_GPR];
                    dst->ins[0].src = NULL;
                } else {
                    dst->ins = NULL;
                    dst->in_count = 0;
                }
            }
            break;
        }

        case TB_INLINE_ASM: {
            TB_NodeInlineAsm* a = TB_NODE_GET_EXTRA_T(n, TB_NodeInlineAsm);
            // a->ra(n, a->ctx, tmp_arena);
            tb_todo();
            break;
        }

        case TB_LOCAL: {
            TB_NodeLocal* local = TB_NODE_GET_EXTRA(n);
            isel_addr(ctx, dst, n, n, 0);
            break;
        }

        case TB_VA_START: {
            assert(ctx->module->target_abi == TB_ABI_WIN64 && "How does va_start even work on SysV?");

            // on Win64 va_start just means whatever is one parameter away from
            // the parameter you give it (plus in Win64 the parameters in the stack
            // are 8bytes, no fanciness like in SysV):
            // void printf(const char* fmt, ...) {
            //     va_list args;
            //     va_start(args, fmt); // args = ((char*) &fmt) + 8;
            //     ...
            // }
            break;
        }

        case TB_LOAD:
        case TB_READ:
        isel_addr(ctx, dst, n, n->inputs[2], 0);
        break;

        case TB_ARRAY_ACCESS:
        case TB_MEMBER_ACCESS:
        isel_addr(ctx, dst, n, n, 0);
        break;

        case TB_CYCLE_COUNTER: {
            dst->ins = tb_arena_alloc(tmp_arena, 2 * sizeof(TileInput));
            dst->in_count = 2;
            dst->ins[0].mask = REGMASK(GPR, 1 << RAX);
            dst->ins[1].mask = REGMASK(GPR, 1 << RDX);
            dst->ins[0].src = NULL;
            dst->ins[1].src = NULL;
            OUT1(REGMASK(GPR, 1 << RAX));
            return;
        }

        case TB_WRITE:
        case TB_STORE: {
            TileInput* ins = isel_addr(ctx, dst, n, n->inputs[2], 1);
            ins[0].src = get_interval(ctx, n->inputs[3], 0);
            ins[0].mask = normie_mask(ctx, n->inputs[3]->dt);
            break;
        }

        case TB_SIGN_EXT:
        case TB_ZERO_EXT:
        if (n->inputs[1]->type == TB_LOAD) {
            isel_addr(ctx, dst, n, n->inputs[1]->inputs[2], 0);
        } else {
            tile_broadcast_ins(ctx, dst, n, 1, 2, normie_mask(ctx, n->inputs[1]->dt));
        }
        break;

        case TB_BITCAST:
        case TB_TRUNCATE:
        case TB_FLOAT_EXT:
        case TB_INT2FLOAT:
        case TB_FLOAT2INT:
        case TB_UINT2FLOAT:
        case TB_FLOAT2UINT:
        tile_broadcast_ins(ctx, dst, n, 1, 2, normie_mask(ctx, n->inputs[1]->dt));
        break;

        case TB_PHI:
        if (n->dt.type == TB_INT || n->dt.type == TB_PTR || n->dt.type == TB_FLOAT) {
            RegMask rm = normie_mask(ctx, n->dt);
            rm.may_spill = true;
            OUT1(rm);
        }
        return;

        case TB_RETURN: {
            static int ret_gprs[2] = { RAX, RDX };

            int rets = n->input_count - 3;
            TileInput* ins = tile_set_ins(ctx, dst, n, 3, n->input_count);

            assert(rets <= 2 && "At most 2 return values :(");
            FOREACH_N(i, 0, rets) {
                TB_DataType dt = n->inputs[3+i]->dt;
                if (dt.type == TB_FLOAT) {
                    ins[i].mask = REGMASK(XMM, 1 << i);
                } else {
                    ins[i].mask = REGMASK(GPR, 1 << ret_gprs[i]);
                }
            }
            return;
        }

        case TB_PROJ: {
            if (dst->out_count) {
                RegMask rm = { 0 };
                int i = TB_NODE_GET_EXTRA_T(n, TB_NodeProj)->index;

                if (n->inputs[0]->type == TB_ROOT) {
                    // function params are ABI crap
                    const struct ParamDesc* params = &param_descs[ctx->abi_index];
                    if (i == 2) {
                        assert(0 && "tf are you doing with the RPC?");
                    } else if (i >= 3) {
                        if (n->dt.type == TB_FLOAT) {
                            rm = REGMASK(XMM, 1u << (i - 3));
                        } else {
                            rm = REGMASK(GPR, 1u << params->gprs[i - 3]);
                        }
                    }
                } else if (n->inputs[0]->type == TB_CALL || n->inputs[0]->type == TB_SYSCALL) {
                    if (n->dt.type == TB_FLOAT) {
                        if (i >= 2) rm = REGMASK(XMM, 1 << (i - 2));
                    } else {
                        if (i == 2) rm = REGMASK(GPR, 1 << RAX);
                        else if (i == 3) rm = REGMASK(GPR, 1 << RDX);
                    }
                } else {
                    tb_todo();
                }

                OUT1(rm);
            }
            return;
        }

        // unary ops
        case TB_NOT:
        tile_broadcast_ins(ctx, dst, n, 1, n->input_count, ctx->normie_mask[REG_CLASS_GPR]);
        break;

        case TB_CMP_EQ:
        case TB_CMP_NE:
        case TB_CMP_SLT:
        case TB_CMP_SLE:
        case TB_CMP_ULT:
        case TB_CMP_ULE:
        case TB_CMP_FLT:
        case TB_CMP_FLE: {
            TB_Node* cmp = n->inputs[1];
            TB_DataType cmp_dt = TB_NODE_GET_EXTRA_T(n->inputs[1], TB_NodeCompare)->cmp_dt;

            int cap = 1;
            if (cmp->type >= TB_CMP_EQ && cmp->type <= TB_CMP_FLE) {
                dst->flags |= TILE_FOLDED_CMP;

                int32_t x;
                if (!try_for_imm32(cmp_dt.type == TB_PTR ? 64 : cmp_dt.data, cmp->inputs[2], &x)) {
                    cap += 1;
                } else {
                    dst->flags |= TILE_HAS_IMM;
                }
            }

            RegMask rm = normie_mask(ctx, n->dt);
            dst->ins = tb_arena_alloc(tmp_arena, cap * sizeof(TileInput));
            dst->in_count = cap;

            int in_count = 0;
            if (dst->flags & TILE_FOLDED_CMP) {
                RegMask rm = normie_mask(ctx, cmp_dt);
                dst->ins[0].src = get_interval(ctx, cmp->inputs[1], 0);
                dst->ins[0].mask = rm;
                in_count++;

                if ((dst->flags & TILE_HAS_IMM) == 0) {
                    dst->ins[1].src  = get_interval(ctx, cmp->inputs[2], 0);
                    dst->ins[1].mask = rm;
                    in_count++;
                }
            } else {
                dst->ins[0].src = get_interval(ctx, cmp, 0);
                dst->ins[0].mask = ctx->normie_mask[REG_CLASS_GPR];
                in_count++;
            }
            break;
        }

        case TB_SELECT: {
            TB_Node* cmp = n->inputs[1];
            TB_DataType cmp_dt = TB_NODE_GET_EXTRA_T(n->inputs[1], TB_NodeCompare)->cmp_dt;

            int cap = 3;
            if (cmp->type >= TB_CMP_EQ && cmp->type <= TB_CMP_FLE) {
                dst->flags |= TILE_FOLDED_CMP;

                int32_t x;
                if (!try_for_imm32(cmp_dt.type == TB_PTR ? 64 : cmp_dt.data, cmp->inputs[2], &x)) {
                    cap += 1;
                } else {
                    dst->flags |= TILE_HAS_IMM;
                }
            }

            RegMask rm = normie_mask(ctx, n->dt);
            dst->ins = tb_arena_alloc(tmp_arena, cap * sizeof(TileInput));
            dst->in_count = cap;

            int in_count = 0;
            if (dst->flags & TILE_FOLDED_CMP) {
                RegMask rm = normie_mask(ctx, cmp_dt);
                dst->ins[0].src = get_interval(ctx, cmp->inputs[1], 0);
                dst->ins[0].mask = rm;
                in_count++;

                if ((dst->flags & TILE_HAS_IMM) == 0) {
                    dst->ins[1].src  = get_interval(ctx, cmp->inputs[2], 0);
                    dst->ins[1].mask = rm;
                    in_count++;
                }
            } else {
                dst->ins[0].src = get_interval(ctx, cmp, 0);
                dst->ins[0].mask = ctx->normie_mask[REG_CLASS_GPR];
                in_count++;
            }

            dst->ins[in_count].src  = get_interval(ctx, n->inputs[2], 0);
            dst->ins[in_count].mask = rm;
            in_count++;

            dst->ins[in_count].src  = get_interval(ctx, n->inputs[3], 0);
            dst->ins[in_count].mask = rm;
            in_count++;
            break;
        }

        // binary ops
        case TB_AND:
        case TB_OR:
        case TB_XOR:
        case TB_ADD:
        case TB_SUB:
        case TB_MUL: {
            int32_t x;
            if (try_for_imm32(n->dt.data, n->inputs[2], &x)) {
                tile_broadcast_ins(ctx, dst, n, 1, 2, ctx->normie_mask[REG_CLASS_GPR]);
                dst->flags |= TILE_HAS_IMM;
            } else {
                TileInput* ins = tile_set_ins(ctx, dst, n, 1, 3);
                ins[0].mask = ctx->normie_mask[REG_CLASS_GPR];
                ins[1].mask = ctx->normie_mask[REG_CLASS_GPR];
                ins[1].mask.may_spill = true;
            }
            break;
        }

        case TB_SHL:
        case TB_SHR:
        case TB_ROL:
        case TB_ROR:
        case TB_SAR: {
            int32_t x;
            if (try_for_imm32(n->inputs[2]->dt.data, n->inputs[2], &x) && x >= 0 && x < 64) {
                tile_broadcast_ins(ctx, dst, n, 1, 2, ctx->normie_mask[REG_CLASS_GPR]);
                dst->flags |= TILE_HAS_IMM;
            } else {
                TileInput* ins = tile_set_ins(ctx, dst, n, 1, 3);
                ins[0].mask = REGMASK(GPR, ctx->normie_mask[REG_CLASS_GPR].mask & NO_RCX);
                ins[1].mask = REGMASK(GPR, 1 << RCX);
            }
            break;
        }

        case TB_UDIV:
        case TB_SDIV:
        case TB_UMOD:
        case TB_SMOD: {
            dst->ins = tb_arena_alloc(tmp_arena, 3 * sizeof(TileInput));
            dst->in_count = 3;
            dst->ins[0].mask = REGMASK(GPR, 1 << RAX);
            dst->ins[1].mask = ctx->normie_mask[REG_CLASS_GPR];
            dst->ins[2].mask = REGMASK(GPR, 1 << RDX);
            dst->ins[0].src = get_interval(ctx, n->inputs[1], 0);
            dst->ins[1].src = get_interval(ctx, n->inputs[2], 0);
            dst->ins[2].src = NULL;

            if (n->type == TB_UDIV || n->type == TB_SDIV) {
                OUT1(REGMASK(GPR, 1 << RAX));
            } else {
                OUT1(REGMASK(GPR, 1 << RDX));
            }
            break;
        }

        case TB_FADD:
        case TB_FSUB:
        case TB_FMUL:
        case TB_FDIV:
        case TB_FMIN:
        case TB_FMAX:
        tile_broadcast_ins(ctx, dst, n, 1, n->input_count, ctx->normie_mask[REG_CLASS_XMM]);
        break;

        case TB_BRANCH: {
            TB_Node* cmp = n->inputs[1];
            TB_NodeBranch* br = TB_NODE_GET_EXTRA(n);

            AuxBranch* aux = NULL;
            int ins = 1, tmps = 0;
            if (br->succ_count > 2) {
                // try for jump tables or if-chains
                //
                // check if there's at most only one space between entries
                int64_t last = br->keys[0].key;
                int64_t min = last, max = last;

                double dist_avg = 0;
                double inv_succ_count = 1.0 / (br->succ_count - 2);

                bool large_num = false;
                FOREACH_N(i, 2, br->succ_count) {
                    int64_t key = br->keys[i - 1].key;
                    if (!fits_into_int32(key)) {
                        large_num = true;
                    }

                    min = (min > key) ? key : min;
                    max = (max > key) ? max : key;

                    dist_avg += (key - last) * inv_succ_count;
                    last = key;
                }

                // if there's no default case we can skew heuristics around the lack of range check
                bool has_default = false;
                FOR_USERS(u, n) {
                    if (u->n->type != TB_PROJ) continue;
                    int index = TB_NODE_GET_EXTRA_T(u->n, TB_NodeProj)->index;
                    if (index == 0) {
                        has_default = cfg_next_control(u->n)->type != TB_UNREACHABLE;
                        break;
                    }
                }

                int64_t range = (max - min) + 1;

                // if we do if-else chains we'll do 1+2c ops (c is the number of cases).
                int64_t if_chain_cost  = 1 + 2*range;
                // if we do jump table it's 6 ops + a table that's got [max-min] entries but cost
                // wise the issue is slots which are missed (go to fallthru).
                int64_t jmp_table_cost = has_default ? 6 : 4;
                jmp_table_cost += (range - (range / dist_avg));

                aux = tb_arena_alloc(tmp_arena, sizeof(AuxBranch));
                aux->min = min;
                aux->max = max;
                aux->if_chain = if_chain_cost < jmp_table_cost;

                if (aux->if_chain) {
                    // large numbers require a temporary to store the immediate
                    tmps += large_num;
                } else {
                    // we need tmp for the key (either offset or casted)
                    tmps += 3;
                }
            } else {
                if (cmp->type >= TB_CMP_EQ && cmp->type <= TB_CMP_FLE) {
                    TB_DataType cmp_dt = TB_NODE_GET_EXTRA_T(n->inputs[1], TB_NodeCompare)->cmp_dt;
                    dst->flags |= TILE_FOLDED_CMP;

                    int32_t x;
                    if (!try_for_imm32(cmp_dt.type == TB_PTR ? 64 : cmp_dt.data, cmp->inputs[2], &x)) {
                        ins += 1;
                    } else {
                        dst->flags |= TILE_HAS_IMM;
                    }
                }
            }

            dst->ins = tb_arena_alloc(tmp_arena, (ins+tmps) * sizeof(TileInput));
            dst->in_count = ins+tmps;
            dst->aux = aux;

            if (dst->flags & TILE_FOLDED_CMP) {
                TB_DataType cmp_dt = TB_NODE_GET_EXTRA_T(cmp, TB_NodeCompare)->cmp_dt;

                RegMask rm = normie_mask(ctx, cmp_dt);
                dst->ins[0].src = get_interval(ctx, cmp->inputs[1], 0);
                dst->ins[0].mask = rm;

                if ((dst->flags & TILE_HAS_IMM) == 0) {
                    dst->ins[1].src = get_interval(ctx, cmp->inputs[2], 0);
                    dst->ins[1].mask = rm;
                }
            } else {
                dst->ins[0].src = get_interval(ctx, cmp, 0);
                dst->ins[0].mask = normie_mask(ctx, cmp->dt);
            }

            FOREACH_N(i, ins, ins+tmps) {
                dst->ins[i].src = NULL;
                dst->ins[i].mask = ctx->normie_mask[REG_CLASS_GPR];
            }

            break;
        }

        case TB_SYSCALL: {
            const struct ParamDesc* abi = &param_descs[2];
            uint32_t caller_saved_gprs = abi->caller_saved_gprs;

            int param_count = n->input_count - 3;
            if (n->type == TB_TAILCALL) {
                caller_saved_gprs &= ~(1u << RAX);
            }

            FOREACH_N(i, 0, param_count > 4 ? 4 : param_count) {
                caller_saved_gprs &= ~(1u << abi->gprs[i]);
            }

            size_t clobber_count = tb_popcount(caller_saved_gprs);
            size_t input_count = (n->input_count - 2) + clobber_count;

            // SYSCALL
            TileInput* ins = dst->ins = tb_arena_alloc(tmp_arena, input_count * sizeof(TileInput));
            dst->in_count = input_count;

            ins[0].src = get_interval(ctx, n->inputs[2], 0);
            ins[0].mask = REGMASK(GPR, 1u << RAX);

            assert(param_count < abi->gpr_count);
            FOREACH_N(i, 0, param_count) {
                ins[i].src = get_interval(ctx, n->inputs[i + 3], 0);
                ins[i].mask = REGMASK(GPR, 1u << abi->gprs[i]);
            }

            int j = param_count;
            FOREACH_N(i, 0, ctx->num_regs[REG_CLASS_GPR]) {
                if (caller_saved_gprs & (1u << i)) {
                    ins[j].src = NULL;
                    ins[j].mask = REGMASK(GPR, 1u << i);
                    j++;
                }
            }
            break;
        }

        case TB_CALL:
        case TB_TAILCALL: {
            const struct ParamDesc* abi = &param_descs[ctx->abi_index];
            uint32_t caller_saved_gprs = abi->caller_saved_gprs;
            uint32_t caller_saved_xmms = ~0ull >> (64 - abi->caller_saved_xmms);

            int param_count = n->input_count - 3;
            if (ctx->num_regs[0] < param_count) {
                ctx->num_regs[0] = param_count;
                ctx->call_usage = param_count;
            }

            if (n->type == TB_TAILCALL) {
                caller_saved_gprs &= ~(1u << RAX);
            }

            FOREACH_N(i, 0, param_count > 4 ? 4 : param_count) {
                caller_saved_gprs &= ~(1u << abi->gprs[i]);
            }

            size_t clobber_count = tb_popcount(caller_saved_gprs);
            size_t input_start = n->inputs[2]->type == TB_SYMBOL ? 3 : 2;
            size_t input_count = (n->input_count - input_start) + clobber_count;

            TileInput* ins;
            if (n->inputs[2]->type == TB_SYMBOL) {
                // CALL symbol
                ins = dst->ins = tb_arena_alloc(tmp_arena, input_count * sizeof(TileInput));
                dst->in_count = input_count;
            } else {
                // CALL r/m
                ins = dst->ins = tb_arena_alloc(tmp_arena, input_count * sizeof(TileInput));
                dst->in_count = input_count;

                ins[0].src = get_interval(ctx, n->inputs[2], 0);
                if (n->type == TB_TAILCALL) {
                    ins[0].mask = REGMASK(GPR, 1u << RAX);
                } else {
                    ins[0].mask = ctx->normie_mask[REG_CLASS_GPR];
                }
                ins += 1;
            }

            FOREACH_N(i, 0, param_count) {
                ins[i].src = get_interval(ctx, n->inputs[i + 3], 0);

                if (i < abi->gpr_count) {
                    if (TB_IS_FLOAT_TYPE(n->inputs[i + 3]->dt)) {
                        ins[i].mask = REGMASK(XMM, 1u << i);
                    } else {
                        ins[i].mask = REGMASK(GPR, 1u << abi->gprs[i]);
                    }
                } else {
                    // stack slots go into [RSP + 8i]
                    ins[i].mask = REGMASK(STK, i);
                }
            }

            int j = param_count;
            FOREACH_N(i, 0, 16) {
                if (caller_saved_gprs & (1u << i)) {
                    ins[j].src = NULL;
                    ins[j].mask = REGMASK(GPR, 1u << i);
                    j++;
                }
            }

            assert(j == input_count - (n->inputs[2]->type != TB_SYMBOL));
            return;
        }

        default:
        tb_todo();
        break;
    }

    if (dst->out_count == 1) {
        dst->outs[0]->dt = n->dt;
        dst->outs[0]->mask = normie_mask(ctx, n->dt);
    } else if (dst->out_count != 0) {
        tb_todo();
    }
}

static Val op_at(Ctx* ctx, LiveInterval* l) {
    if (l->class == REG_CLASS_STK) {
        return val_stack(stk_offset(ctx, l->assigned));
    } else {
        assert(l->assigned >= 0);
        return (Val) { .type = l->class == REG_CLASS_XMM ? VAL_XMM : VAL_GPR, .reg = l->assigned };
    }
}

static GPR op_gpr_at(LiveInterval* l) {
    assert(l->class == REG_CLASS_GPR);
    return l->assigned;
}
#endif

static int stk_offset(Ctx* ctx, int reg) {
    int pos = reg*8;
    if (reg >= ctx->num_regs[0]) {
        return ctx->stack_usage - (pos + 8);
    } else {
        return pos;
    }
}

static void emit_epilogue(Ctx* restrict ctx, TB_CGEmitter* e, int stack_usage) {
    TB_FunctionPrototype* proto = ctx->f->prototype;
    bool needs_stack = stack_usage > ctx->stack_header + (proto->param_count * 8);

    // add rsp, N
    if (stack_usage) {
        if (stack_usage == (int8_t)stack_usage) {
            EMIT1(&ctx->emit, rex(true, 0x00, RSP, 0));
            EMIT1(&ctx->emit, 0x83);
            EMIT1(&ctx->emit, mod_rx_rm(MOD_DIRECT, 0x00, RSP));
            EMIT1(&ctx->emit, (int8_t) stack_usage);
        } else {
            EMIT1(&ctx->emit, rex(true, 0x00, RSP, 0));
            EMIT1(&ctx->emit, 0x81);
            EMIT1(&ctx->emit, mod_rx_rm(MOD_DIRECT, 0x00, RSP));
            EMIT4(&ctx->emit, stack_usage);
        }
    }

    // pop rbp (if we even used the frameptr)
    if ((ctx->features.gen & TB_FEATURE_FRAME_PTR) && stack_usage > 0) {
        EMIT1(&ctx->emit, 0x58 + RBP);
    }
}

static void pre_emit(Ctx* restrict ctx, TB_CGEmitter* e, TB_Node* root) {
    size_t call_usage = ctx->call_usage;
    if (ctx->abi_index == 0 && call_usage > 0 && call_usage < 4) {
        call_usage = 4;
    }

    ctx->stack_usage -= ctx->initial_spills * 8;
    ctx->stack_usage += call_usage * 8;

    TB_FunctionPrototype* proto = ctx->f->prototype;
    size_t stack_usage = 0;
    if (ctx->stack_usage > ctx->stack_header + (proto->param_count * 8)) {
        // Align stack usage to 16bytes + 8 to accommodate for the RIP being pushed by CALL
        stack_usage = align_up(ctx->stack_usage + ctx->stack_header, 16) - ctx->stack_header;
    }
    ctx->stack_usage = stack_usage;

    FOR_USERS(u, root) {
        TB_Node* n = USERN(u);
        if (n->type != TB_MACH_LOCAL) continue;
        TB_NodeMachLocal* l = TB_NODE_GET_EXTRA(n);
        if (l->type == NULL) continue;
        TB_StackSlot s = {
            .name = l->name,
            .type = l->type,
            .storage = { l->disp },
        };
        dyn_array_put(ctx->debug_stack_slots, s);
    }

    // save frame pointer (if applies)
    if ((ctx->features.gen & TB_FEATURE_FRAME_PTR) && stack_usage > 0) {
        EMIT1(e, 0x50 + RBP);

        // mov rbp, rsp
        EMIT1(e, rex(true, RSP, RBP, 0));
        EMIT1(e, 0x89);
        EMIT1(e, mod_rx_rm(MOD_DIRECT, RSP, RBP));
    }

    // inserts a chkstk call if we use too much stack
    if (stack_usage >= param_descs[ctx->abi_index].chkstk_limit) {
        assert(ctx->f->super.module->chkstk_extern);
        ctx->f->super.module->uses_chkstk++;

        Val sym = val_global(ctx->f->super.module->chkstk_extern, 0);
        Val imm = val_imm(stack_usage);
        Val rax = val_gpr(RAX);
        Val rsp = val_gpr(RSP);

        inst2(e, MOV, &rax, &imm, TB_X86_TYPE_DWORD);
        inst1(e, CALL, &sym, TB_X86_TYPE_QWORD);
        inst2(e, SUB, &rsp, &rax, TB_X86_TYPE_QWORD);
    } else if (stack_usage) {
        if (stack_usage == (int8_t)stack_usage) {
            // sub rsp, stack_usage
            EMIT1(e, rex(true, 0x00, RSP, 0));
            EMIT1(e, 0x83);
            EMIT1(e, mod_rx_rm(MOD_DIRECT, 0x05, RSP));
            EMIT1(e, stack_usage);
        } else {
            // sub rsp, stack_usage
            EMIT1(e, rex(true, 0x00, RSP, 0));
            EMIT1(e, 0x81);
            EMIT1(e, mod_rx_rm(MOD_DIRECT, 0x05, RSP));
            EMIT4(e, stack_usage);
        }
    }

    // handle unknown parameters (if we have varargs)
    if (proto->has_varargs) {
        const GPR* parameter_gprs = param_descs[ctx->abi_index].gprs;

        // spill the rest of the parameters (assumes they're all in the GPRs)
        size_t gpr_count = param_descs[ctx->abi_index].gpr_count;
        size_t extra_param_count = proto->param_count > gpr_count ? 0 : gpr_count - proto->param_count;

        FOREACH_N(i, proto->param_count, gpr_count) {
            int dst_pos = ctx->stack_header + (i * 8);
            Val src = val_gpr(parameter_gprs[i]);

            Val dst = val_base_disp(RSP, stack_usage + dst_pos);
            inst2(e, MOV, &dst, &src, TB_X86_TYPE_QWORD);
        }
    }

    ctx->prologue_length = e->count;
}

static void on_basic_block(Ctx* restrict ctx, TB_CGEmitter* e, int bb) {
    tb_resolve_rel32(e, &e->labels[bb], e->count);
}

#if 0
static Val parse_memory_op(Ctx* restrict ctx, TB_CGEmitter* e, Tile* t, TB_Node* addr) {
    Val ptr;
    if (addr->type == TB_LOCAL) {
        int pos = get_stack_slot(ctx, addr);
        ptr = val_stack(pos);
    } else if (addr->type == TB_SYMBOL) {
        TB_Symbol* sym = TB_NODE_GET_EXTRA_T(addr, TB_NodeSymbol)->sym;
        ptr = val_global(sym, 0);
    } else {
        tb_todo();
    }

    return ptr;
}

// compute effective address operand
static Val emit_addr(Ctx* restrict ctx, TB_CGEmitter* e, Tile* t) {
    bool use_tmp = t->out_count == 0 || t->outs[0]->mask->class == REG_CLASS_XMM;

    int in_count = 0;
    Val ea = { .type = VAL_MEM, .index = GPR_NONE };
    AuxAddress* aux = t->aux;
    if (t->flags & TILE_FOLDED_BASE) {
        if (aux->base->type == TB_LOCAL) {
            int pos = get_stack_slot(ctx, aux->base);
            ea.reg = RSP;
            ea.imm = pos;
        } else {
            assert(aux->base->type == TB_SYMBOL);
            ea.type = VAL_GLOBAL;
            ea.symbol = TB_NODE_GET_EXTRA_T(aux->base, TB_NodeSymbol)->sym;
        }
    } else {
        ea.reg = op_at(ctx, t->ins[in_count].src).reg;
        in_count++;
    }

    if (t->flags & TILE_INDEXED) {
        int index = op_gpr_at(t->ins[in_count++].src);

        int stride = aux->stride;
        if (tb_is_power_of_two(stride)) {
            int scale = tb_ffs(stride) - 1;
            if (scale > 3) {
                Val tmp = val_gpr(op_gpr_at(use_tmp ? t->ins[in_count++].src : t->outs[0]));
                if (tmp.reg != index) {
                    Val index_op = val_gpr(index);
                    inst2(e, MOV, &tmp, &index_op, TB_X86_TYPE_QWORD);
                }

                Val imm = val_imm(scale);
                inst2(e, SHL, &tmp, &imm, TB_X86_TYPE_QWORD);
                index = tmp.reg;
            } else {
                ea.scale = scale;
            }
        } else {
            tb_todo();
        }

        ea.index = index;
    }

    ea.imm += aux->offset;
    return ea;
}

static Cond emit_cmp(Ctx* restrict ctx, TB_CGEmitter* e, TB_Node* cmp, Tile* t, int64_t falsey) {
    Val a = op_at(ctx, t->ins[0].src);
    if (t->flags & TILE_FOLDED_CMP) {
        TB_DataType cmp_dt = TB_NODE_GET_EXTRA_T(cmp, TB_NodeCompare)->cmp_dt;
        assert(cmp->type >= TB_CMP_EQ && cmp->type <= TB_CMP_FLE);
        assert(falsey == 0 || falsey == 1);

        Cond cc;
        if (TB_IS_FLOAT_TYPE(cmp_dt)) {
            Val b = op_at(ctx, t->ins[1].src);
            inst2sse(e, FP_UCOMI, &a, &b, legalize_float(cmp_dt));

            switch (cmp->type) {
                case TB_CMP_EQ:  cc = E;  break;
                case TB_CMP_NE:  cc = NE; break;
                case TB_CMP_FLT: cc = B;  break;
                case TB_CMP_FLE: cc = BE; break;
                default: tb_unreachable();
            }
        } else {
            if (t->flags & TILE_HAS_IMM) {
                assert(cmp->inputs[2]->type == TB_INTEGER_CONST);
                TB_NodeInt* i = TB_NODE_GET_EXTRA(cmp->inputs[2]);

                Val b = val_imm(i->value);
                inst2(e, CMP, &a, &b, legalize_int2(cmp_dt));
            } else {
                Val b = op_at(ctx, t->ins[1].src);
                inst2(e, CMP, &a, &b, legalize_int2(cmp_dt));
            }

            switch (cmp->type) {
                case TB_CMP_EQ:  cc = E;  break;
                case TB_CMP_NE:  cc = NE; break;
                case TB_CMP_SLT: cc = L;  break;
                case TB_CMP_SLE: cc = LE; break;
                case TB_CMP_ULT: cc = B;  break;
                case TB_CMP_ULE: cc = BE; break;
                default: tb_unreachable();
            }
        }

        if (falsey == 1) { cc ^= 1; }
        return cc;
    } else {
        if (falsey == 0) {
            inst2(e, TEST, &a, &a, legalize_int2(cmp->dt));
        } else {
            assert(fits_into_int32(falsey));
            Val imm = val_imm(falsey);
            inst2(e, CMP, &a, &imm, legalize_int2(cmp->dt));
        }
        return NE;
    }
}

static void emit_tile(Ctx* restrict ctx, TB_CGEmitter* e, Tile* t) {
    if (t->tag == TILE_SPILL_MOVE) {
        Val dst = op_at(ctx, t->outs[0]);
        Val src = op_at(ctx, t->ins[0].src);
        if (!is_value_match(&dst, &src)) {
            COMMENT("move v%d -> v%d", t->outs[0]->id, t->ins[0].src->id);

            TB_DataType dt = t->spill_dt;
            if (dt.type == TB_FLOAT) {
                inst2sse(e, FP_MOV, &dst, &src, legalize_float(dt));
            } else {
                inst2(e, MOV, &dst, &src, legalize_int2(dt));
            }
        } else {
            COMMENT("folded move v%d -> v%d", t->outs[0]->id, t->ins[0].src->id);
        }
    } else if (t->tag == TILE_GOTO) {
        MachineBB* mbb = node_to_bb(ctx, t->succ);
        if (ctx->fallthrough != mbb->id) {
            EMIT1(e, 0xE9); EMIT4(e, 0);
            tb_emit_rel32(e, &e->labels[mbb->id], GET_CODE_POS(e) - 4);
        }
    } else {
        TB_Node* n = t->n;
        switch (n->type) {
            // epilogue
            case TB_RETURN: {
                size_t pos = e->count;
                emit_epilogue(ctx, e, ctx->stack_usage);
                EMIT1(e, 0xC3);
                ctx->epilogue_length = e->count - pos;
                break;
            }
            case TB_TRAP: {
                EMIT1(e, 0x0F);
                EMIT1(e, 0x0B);
                break;
            }
            case TB_DEBUGBREAK: {
                EMIT1(e, 0xCC);
                break;
            }
            // projections don't manage their own work, that's the
            // TUPLE node's job.
            case TB_PROJ:
            case TB_REGION:
            case TB_NATURAL_LOOP:
            case TB_AFFINE_LOOP:
            case TB_PHI:
            case TB_POISON:
            case TB_UNREACHABLE:
            case TB_SPLITMEM:
            case TB_MERGEMEM:
            case TB_CALLGRAPH:
            case TB_ROOT:
            break;

            case TB_INLINE_ASM: {
                TB_NodeInlineAsm* a = TB_NODE_GET_EXTRA_T(n, TB_NodeInlineAsm);

                size_t count = a->emit(n, a->ctx, e->capacity, e->data);
                assert(e->count + count < e->capacity);
                e->count += count;
                break;
            }

            // rdtsc
            // shl rdx, 32
            // or rax, rdx
            case TB_CYCLE_COUNTER: {
                Val rax = val_gpr(RAX);
                Val rdx = val_gpr(RDX);
                Val imm = val_imm(32);
                EMIT1(e, 0x0F); EMIT1(e, 0x31);
                inst2(e, SHL, &rdx, &imm, TB_X86_TYPE_QWORD);
                inst2(e, OR,  &rax, &rdx, TB_X86_TYPE_QWORD);
                break;
            }

            case TB_READ: {
                TB_Node* proj1 = proj_with_index(n, 1)->n;

                Val dst = op_at(ctx, val_at(ctx, proj1)->tile->outs[0]);
                Val ea = emit_addr(ctx, e, t);
                inst2(e, MOV, &dst, &ea, legalize_int2(proj1->dt));
                break;
            }

            case TB_LOAD: {
                Val dst = op_at(ctx, t->outs[0]);
                Val ea = emit_addr(ctx, e, t);
                if (n->dt.type == TB_FLOAT) {
                    inst2sse(e, FP_MOV, &dst, &ea, legalize_float(n->dt));
                } else {
                    inst2(e, MOV, &dst, &ea, legalize_int2(n->dt));
                }
                break;
            }
            case TB_WRITE:
            case TB_STORE: {
                TB_Node* val = n->inputs[3];

                Val ea = emit_addr(ctx, e, t);
                Val src;
                if (val->dt.type == TB_FLOAT) {
                    src = op_at(ctx, t->ins[t->in_count - 1].src);
                    inst2sse(e, FP_MOV, &ea, &src, legalize_float(val->dt));
                } else {
                    if (t->flags & TILE_HAS_IMM) {
                        assert(val->type == TB_INTEGER_CONST);
                        TB_NodeInt* i = TB_NODE_GET_EXTRA(val);
                        src = val_imm(i->value);
                    } else {
                        src = op_at(ctx, t->ins[t->in_count - 1].src);
                    }

                    inst2(e, MOV, &ea, &src, legalize_int2(val->dt));
                }
                break;
            }
            case TB_LOCAL:
            case TB_MEMBER_ACCESS:
            case TB_ARRAY_ACCESS: {
                Val dst = op_at(ctx, t->outs[0]);
                Val ea = emit_addr(ctx, e, t);
                inst2(e, LEA, &dst, &ea, TB_X86_TYPE_QWORD);
                break;
            }
            case TB_VA_START: {
                TB_FunctionPrototype* proto = ctx->f->prototype;

                Val dst = op_at(ctx, t->outs[0]);
                Val ea = val_stack(ctx->stack_usage + ctx->stack_header + proto->param_count*8);
                inst2(e, LEA, &dst, &ea, TB_X86_TYPE_QWORD);
                break;
            }

            case TB_INTEGER_CONST: {
                uint64_t x = TB_NODE_GET_EXTRA_T(n, TB_NodeInt)->value;
                uint32_t hi = x >> 32ull;

                TB_X86_DataType dt = legalize_int2(n->dt);
                Val dst = op_at(ctx, t->outs[0]);
                if (x == 0) {
                    // xor reg, reg
                    inst2(e, XOR, &dst, &dst, dt);
                } else if (hi == 0 || dt == TB_X86_TYPE_QWORD) {
                    Val src = val_abs(x);
                    inst2(e, MOVABS, &dst, &src, dt);
                } else {
                    Val src = val_imm(x);
                    inst2(e, MOV, &dst, &src, dt);
                }
                break;
            }
            case TB_FLOAT32_CONST: {
                uint32_t imm = (Cvt_F32U32){ .f = TB_NODE_GET_EXTRA_T(n, TB_NodeFloat32)->value }.i;
                Val dst = op_at(ctx, t->outs[0]);

                if (imm == 0) {
                    inst2sse(e, FP_XOR, &dst, &dst, TB_X86_TYPE_SSE_PS);
                } else {
                    TB_Symbol* sym = &tb__small_data_intern(ctx->module, sizeof(float), &imm)->super;
                    Val src = val_global(sym, 0);
                    inst2sse(e, FP_MOV, &dst, &src, TB_X86_TYPE_SSE_PS);
                }
                break;
            }
            case TB_FLOAT64_CONST: {
                uint64_t imm = (Cvt_F64U64){ .f = TB_NODE_GET_EXTRA_T(n, TB_NodeFloat64)->value }.i;
                Val dst = op_at(ctx, t->outs[0]);

                if (imm == 0) {
                    inst2sse(e, FP_XOR, &dst, &dst, TB_X86_TYPE_SSE_PS);
                } else {
                    TB_Symbol* sym = &tb__small_data_intern(ctx->module, sizeof(double), &imm)->super;
                    Val src = val_global(sym, 0);
                    inst2sse(e, FP_MOV, &dst, &src, TB_X86_TYPE_SSE_PS);
                }
                break;
            }
            case TB_FADD:
            case TB_FSUB:
            case TB_FMUL:
            case TB_FDIV:
            case TB_FMIN:
            case TB_FMAX: {
                const static InstType ops[] = { FP_ADD, FP_SUB, FP_MUL, FP_DIV, FP_MIN, FP_MAX };
                TB_X86_DataType dt = legalize_float(n->dt);

                Val dst = op_at(ctx, t->outs[0]);
                Val lhs = op_at(ctx, t->ins[0].src);
                if (!is_value_match(&dst, &lhs)) {
                    inst2sse(e, FP_MOV, &dst, &lhs, dt);
                }

                Val rhs = op_at(ctx, t->ins[1].src);
                inst2sse(e, ops[n->type - TB_FADD], &dst, &rhs, dt);
                break;
            }
            case TB_SIGN_EXT:
            case TB_ZERO_EXT: {
                bool is_signed = n->type == TB_SIGN_EXT;
                TB_DataType src_dt = n->inputs[1]->dt;
                int bits_in_type = src_dt.type == TB_PTR ? 64 : src_dt.data;

                int op = 0;
                TB_X86_DataType dt = legalize_int2(n->dt);
                switch (bits_in_type) {
                    case 8:  op = is_signed ? MOVSXB : MOVZXB; break;
                    case 16: op = is_signed ? MOVSXB : MOVZXW; break;
                    case 32: if (is_signed) {
                        op = MOVSXD;
                    } else {
                        op = MOV, dt = TB_X86_TYPE_DWORD;
                    }
                    break;
                    case 64: op = MOV; break;
                    default: tb_todo();
                }

                Val dst = op_at(ctx, t->outs[0]);
                if (is_signed && dt <= TB_X86_TYPE_DWORD) {
                    dt = TB_X86_TYPE_DWORD;
                }

                if (n->inputs[1]->type == TB_LOAD) {
                    Val ea = emit_addr(ctx, e, t);
                    inst2(e, op ? op : MOV, &dst, &ea, dt);
                } else {
                    Val lhs = op_at(ctx, t->ins[0].src);
                    inst2(e, op ? op : MOV, &dst, &lhs, dt);
                }

                if (op == 0) {
                    if (!is_signed && bits_in_type < 32) {
                        // chop bits with a mask
                        Val imm = val_imm(UINT64_MAX >> (64 - bits_in_type));
                        inst2(e, AND, &dst, &imm, dt);
                    } else {
                        // unconventional sizes do:
                        //   SHL dst, x
                        //   SAR dst, x (or SHR if zero ext)
                        //
                        // where x is 'reg_width - val_width'
                        int dst_bits = dt == TB_X86_TYPE_QWORD ? 64 : 32;
                        int ext = is_signed ? SAR : SHR;
                        Val imm = val_imm(dst_bits - bits_in_type);
                        inst2(e, SHL, &dst, &imm, dt);
                        inst2(e, ext, &dst, &imm, dt);
                    }
                }
                break;
            }
            case TB_TRUNCATE: {
                if (TB_IS_FLOAT_TYPE(n->dt)) {
                    Val dst = op_at(ctx, t->outs[0]);
                    Val lhs = op_at(ctx, t->ins[0].src);
                    inst2sse(e, FP_CVT, &dst, &lhs, legalize_float(n->inputs[1]->dt));
                } else {
                    TB_X86_DataType dt = legalize_int2(n->dt);

                    Val dst = op_at(ctx, t->outs[0]);
                    Val lhs = op_at(ctx, t->ins[0].src);
                    if (!is_value_match(&dst, &lhs)) {
                        inst2(e, MOV, &dst, &lhs, dt);
                    }
                }
                break;
            }
            case TB_FLOAT_EXT: {
                TB_X86_DataType src_dt = legalize_float(n->inputs[1]->dt);
                Val dst = op_at(ctx, t->outs[0]);
                Val lhs = op_at(ctx, t->ins[0].src);
                inst2sse(e, FP_CVT, &dst, &lhs, src_dt);
                break;
            }
            case TB_UINT2FLOAT:
            case TB_INT2FLOAT: {
                TB_DataType src_dt = n->inputs[1]->dt;
                assert(src_dt.type == TB_INT);

                // it's either 32bit or 64bit conversion
                //   CVTSI2SS r/m32, xmm1
                //   CVTSI2SD r/m64, xmm1
                bool is_64bit = src_dt.data > 32;

                TB_X86_DataType dt = legalize_float(n->dt);
                Val dst = op_at(ctx, t->outs[0]);
                Val lhs = op_at(ctx, t->ins[0].src);
                inst2sse(e, is_64bit ? FP_CVT64 : FP_CVT32, &dst, &lhs, dt);
                break;
            }

            case TB_FLOAT2INT:
            case TB_FLOAT2UINT: {
                TB_DataType src_dt = n->inputs[1]->dt;
                assert(src_dt.type == TB_FLOAT);

                // it's either 32bit or 64bit conversion
                // F3 0F 2C /r            CVTTSS2SI xmm1, r/m32
                // F3 REX.W 0F 2C /r      CVTTSS2SI xmm1, r/m64
                // F2 0F 2C /r            CVTTSD2SI xmm1, r/m32
                // F2 REX.W 0F 2C /r      CVTTSD2SI xmm1, r/m64
                Val dst = op_at(ctx, t->outs[0]);
                Val lhs = op_at(ctx, t->ins[0].src);
                inst2sse(e, FP_CVTT, &dst, &lhs, legalize_float(src_dt));
                break;
            }
            case TB_BITCAST: {
                TB_X86_DataType dst_dt = legalize_int2(n->dt);
                TB_X86_DataType src_dt = legalize_int2(n->inputs[1]->dt);

                Val dst = op_at(ctx, t->outs[0]);
                Val src = op_at(ctx, t->ins[0].src);

                if (dst_dt >= TB_X86_TYPE_BYTE && dst_dt <= TB_X86_TYPE_QWORD &&
                    src_dt >= TB_X86_TYPE_BYTE && src_dt <= TB_X86_TYPE_QWORD) {
                    if (dst_dt != src_dt || !is_value_match(&dst, &src)) {
                        inst2(e, MOV, &dst, &src, dst_dt);
                    }
                } else {
                    tb_todo();
                }
                break;
            }
            case TB_SYMBOL: {
                TB_Symbol* sym = TB_NODE_GET_EXTRA_T(n, TB_NodeSymbol)->sym;
                Val dst = op_at(ctx, t->outs[0]);

                assert(sym);
                if (is_tls_symbol(sym)) {
                    if (ctx->abi_index == 0) {
                        Val tmp = op_at(ctx, t->ins[0].src);
                        Val tls_index = val_global(ctx->module->tls_index_extern, 0);

                        // mov tmp, dword [_tls_index]
                        inst2(e, MOV, &tmp, &tls_index, TB_X86_TYPE_DWORD);
                        // mov dst, qword gs:[58h]
                        EMIT1(e, 0x65);
                        EMIT1(e, tmp.reg >= 8 ? 0x4C : 0x48);
                        EMIT1(e, 0x8B);
                        EMIT1(e, mod_rx_rm(MOD_INDIRECT, tmp.reg, RSP));
                        EMIT1(e, mod_rx_rm(SCALE_X1, RSP, RBP));
                        EMIT4(e, 0x58);
                        // mov dst, qword [dst+tmp*8]
                        Val mem = val_base_index_disp(dst.reg, tmp.reg, SCALE_X8, 0);
                        INST2(MOV, &dst, &mem, TB_X86_TYPE_QWORD);
                        // add dst, relocation
                        EMIT1(e, rex(true, 0, dst.reg, 0)), EMIT1(e, 0x81);
                        EMIT1(e, mod_rx_rm(MOD_DIRECT, 0, dst.reg));
                        EMIT4(e, 0);
                        tb_emit_symbol_patch(e->output, sym, e->count - 4);
                    } else {
                        tb_todo();
                    }
                } else {
                    Val src = val_global(sym, 0);
                    inst2(e, LEA, &dst, &src, TB_X86_TYPE_QWORD);
                }
                break;
            }
            case TB_NOT: {
                TB_X86_DataType dt = legalize_int2(n->dt);
                Val dst = op_at(ctx, t->outs[0]);
                Val src = op_at(ctx, t->ins[0].src);
                if (!is_value_match(&dst, &src)) {
                    inst2(e, MOV, &dst, &src, dt);
                }

                inst1(e, NOT, &dst, dt);
                break;
            }
            case TB_AND:
            case TB_OR:
            case TB_XOR:
            case TB_ADD:
            case TB_SUB: {
                const static InstType ops[] = { AND, OR, XOR, ADD, SUB };
                InstType op = ops[n->type - TB_AND];
                TB_X86_DataType dt = legalize_int2(n->dt);

                Val dst = op_at(ctx, t->outs[0]);
                Val lhs = op_at(ctx, t->ins[0].src);

                if (!is_value_match(&dst, &lhs)) {
                    // we'd rather do LEA addition than mov+add, but if it's add by itself it's fine
                    if (n->type == TB_ADD && (dt == TB_X86_TYPE_DWORD || dt == TB_X86_TYPE_QWORD)) {
                        if (t->flags & TILE_HAS_IMM) {
                            assert(n->inputs[2]->type == TB_INTEGER_CONST);
                            TB_NodeInt* i = TB_NODE_GET_EXTRA(n->inputs[2]);

                            // lea dst, [lhs + imm]
                            Val ea = val_base_disp(lhs.reg, i->value);
                            inst2(e, LEA, &dst, &ea, dt);
                            break;
                        }
                    }

                    inst2(e, MOV, &dst, &lhs, dt);
                }

                if (t->flags & TILE_HAS_IMM) {
                    assert(n->inputs[2]->type == TB_INTEGER_CONST);
                    TB_NodeInt* i = TB_NODE_GET_EXTRA(n->inputs[2]);

                    Val rhs = val_imm(i->value);
                    inst2(e, op, &dst, &rhs, dt);
                } else {
                    Val rhs = op_at(ctx, t->ins[1].src);
                    inst2(e, op, &dst, &rhs, dt);
                }
                break;
            }
            case TB_MUL: {
                TB_X86_DataType dt = legalize_int2(n->dt);

                Val dst = op_at(ctx, t->outs[0]);
                Val lhs = op_at(ctx, t->ins[0].src);

                if (t->flags & TILE_HAS_IMM) {
                    assert(n->inputs[2]->type == TB_INTEGER_CONST);
                    TB_NodeInt* i = TB_NODE_GET_EXTRA(n->inputs[2]);

                    inst2(e, IMUL3, &dst, &lhs, dt);
                    if (dt == TB_X86_TYPE_WORD) {
                        EMIT2(e, i->value);
                    } else {
                        EMIT4(e, i->value);
                    }
                } else {
                    if (!is_value_match(&dst, &lhs)) {
                        inst2(e, MOV, &dst, &lhs, dt);
                    }

                    Val rhs = op_at(ctx, t->ins[1].src);
                    inst2(e, IMUL, &dst, &rhs, dt);
                }
                break;
            }
            case TB_SHL:
            case TB_SHR:
            case TB_ROL:
            case TB_ROR:
            case TB_SAR: {
                TB_X86_DataType dt = legalize_int2(n->dt);

                Val dst = op_at(ctx, t->outs[0]);
                Val lhs = op_at(ctx, t->ins[0].src);
                if (!is_value_match(&dst, &lhs)) {
                    inst2(e, MOV, &dst, &lhs, dt);
                }

                InstType op;
                switch (n->type) {
                    case TB_SHL: op = SHL; break;
                    case TB_SHR: op = SHR; break;
                    case TB_ROL: op = ROL; break;
                    case TB_ROR: op = ROR; break;
                    case TB_SAR: op = SAR; break;
                    default: tb_todo();
                }

                if (t->flags & TILE_HAS_IMM) {
                    assert(n->inputs[2]->type == TB_INTEGER_CONST);
                    TB_NodeInt* i = TB_NODE_GET_EXTRA(n->inputs[2]);

                    Val rhs = val_imm(i->value);
                    inst2(e, op, &dst, &rhs, dt);
                } else {
                    Val rcx = val_gpr(RCX);
                    inst2(e, op, &dst, &rcx, dt);
                }
                break;
            }
            case TB_UDIV:
            case TB_SDIV:
            case TB_UMOD:
            case TB_SMOD: {
                bool is_signed = (n->type == TB_SDIV || n->type == TB_SMOD);
                bool is_div    = (n->type == TB_UDIV || n->type == TB_SDIV);

                TB_DataType dt = n->dt;

                // if signed:
                //   cqo/cdq (sign extend RAX into RDX)
                // else:
                //   xor rdx, rdx
                if (is_signed) {
                    if (n->dt.data > 32) {
                        EMIT1(e, 0x48);
                    }
                    EMIT1(e, 0x99);
                } else {
                    Val rdx = val_gpr(RDX);
                    inst2(e, XOR, &rdx, &rdx, TB_X86_TYPE_DWORD);
                }

                Val rhs = op_at(ctx, t->ins[1].src);
                inst1(e, is_signed ? IDIV : DIV, &rhs, legalize_int2(dt));
                break;
            }
            case TB_SYSCALL: {
                inst0(e, SYSCALL, TB_X86_TYPE_QWORD);
                break;
            }
            case TB_CALL:
            case TB_TAILCALL: {
                int op = CALL;
                if (n->type == TB_TAILCALL) {
                    op = JMP;
                    emit_epilogue(ctx, e, ctx->stack_usage);
                }

                if (n->inputs[2]->type == TB_SYMBOL) {
                    TB_Symbol* sym = TB_NODE_GET_EXTRA_T(n->inputs[2], TB_NodeSymbol)->sym;

                    Val target = val_global(sym, 0);
                    inst1(e, op, &target, TB_X86_TYPE_QWORD);
                } else {
                    Val target = op_at(ctx, t->ins[0].src);
                    inst1(e, op, &target, TB_X86_TYPE_QWORD);
                }
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
                TB_X86_DataType dt = legalize_int2(n->dt);
                Val dst = op_at(ctx, t->outs[0]);

                Cond cc = emit_cmp(ctx, e, n, t, 0);
                inst1(e, SETO+(cc^1), &dst, dt);
                break;
            }
            case TB_SELECT: {
                TB_X86_DataType dt = legalize_int2(n->dt);
                Val dst = op_at(ctx, t->outs[0]);

                Cond cc = emit_cmp(ctx, e, n->inputs[1], t, 0);

                int ops = 1;
                if ((t->flags & TILE_HAS_IMM) == 0) {
                    ops += 1;
                }

                Val a = op_at(ctx, t->ins[ops+0].src);
                if (!is_value_match(&dst, &a)) {
                    inst2(e, MOV, &dst, &a, dt);
                }

                Val b = op_at(ctx, t->ins[ops+1].src);
                inst2(e, CMOVO+(cc^1), &dst, &b, dt);
                break;
            }
            case TB_BRANCH: {
                TB_NodeBranch* br = TB_NODE_GET_EXTRA(n);

                // the arena on the function should also be available at this time, we're
                // in the TB_Passes
                TB_Arena* arena = ctx->f->arena;
                TB_ArenaSavepoint sp = tb_arena_save(arena);
                int* succ = tb_arena_alloc(arena, br->succ_count * sizeof(int));

                // fill successors
                bool has_default = false;
                FOR_USERS(u, n) {
                    if (u->n->type == TB_PROJ) {
                        int index = TB_NODE_GET_EXTRA_T(u->n, TB_NodeProj)->index;
                        TB_Node* succ_n = cfg_next_bb_after_cproj(u->n);

                        if (index == 0) {
                            has_default = !cfg_is_unreachable(succ_n);
                        }

                        MachineBB* mbb = node_to_bb(ctx, succ_n);
                        succ[index] = mbb->id;
                    }
                }

                TB_DataType dt = n->inputs[1]->dt;
                if (br->succ_count == 1) {
                    assert(0 && "degenerate branch? that's odd");
                } else if (br->succ_count == 2) {
                    Val naw = val_label(succ[1]);
                    Val yea = val_label(succ[0]);
                    Cond cc = emit_cmp(ctx, e, n->inputs[1], t, br->keys[0].key);

                    // if flipping avoids a jmp, do that
                    if (ctx->fallthrough == yea.label) {
                        x86_jcc(e, cc ^ 1, naw);
                    } else {
                        x86_jcc(e, cc, yea);
                        if (ctx->fallthrough != naw.label) {
                            x86_jmp(e, naw);
                        }
                    }
                } else {
                    AuxBranch* aux = t->aux;
                    TB_X86_DataType cmp_dt = legalize_int2(dt);
                    Val key = op_at(ctx, t->ins[0].src);

                    if (aux->if_chain) {
                        // Basic if-else chain
                        FOREACH_N(i, 1, br->succ_count) {
                            uint64_t curr_key = br->keys[i-1].key;

                            if (fits_into_int32(curr_key)) {
                                Val imm = val_imm(curr_key);
                                inst2(e, CMP, &key, &imm, cmp_dt);
                            } else {
                                Val tmp = op_at(ctx, t->ins[1].src);
                                Val imm = val_abs(curr_key);

                                inst2(e, MOV, &key, &imm, cmp_dt);
                                inst2(e, CMP, &key, &imm, cmp_dt);
                            }
                            x86_jcc(e, E, val_label(succ[i]));
                        }
                        x86_jmp(e, val_label(succ[0]));
                    } else {
                        int64_t min = aux->min;
                        int64_t max = aux->max;
                        int64_t range = (aux->max - aux->min) + 1;

                        // make a jump table with 4 byte relative pointers for each target
                        TB_Function* f = ctx->f;
                        TB_Global* jump_table = tb_global_create(f->super.module, -1, "jumptbl", NULL, TB_LINKAGE_PRIVATE);
                        tb_global_set_storage(f->super.module, tb_module_get_rdata(f->super.module), jump_table, range*4, 4, 1);

                        // generate patches for later
                        uint32_t* jump_entries = tb_global_add_region(f->super.module, jump_table, 0, range*4);

                        Set entries_set = set_create_in_arena(arena, range);
                        FOREACH_N(i, 1, br->succ_count) {
                            uint64_t key_idx = br->keys[i - 1].key - min;
                            assert(key_idx < range);

                            JumpTablePatch p;
                            p.pos = &jump_entries[key_idx];
                            p.target = succ[i];
                            dyn_array_put(ctx->jump_table_patches, p);
                            set_put(&entries_set, key_idx);
                        }

                        // handle default cases
                        FOREACH_N(i, 0, range) {
                            if (!set_get(&entries_set, i)) {
                                JumpTablePatch p;
                                p.pos = &jump_entries[i];
                                p.target = succ[0];
                                dyn_array_put(ctx->jump_table_patches, p);
                            }
                        }

                        /*int tmp = DEF(NULL, dt);
                        hint_reg(ctx, tmp, key);
                        if (dt.data >= 32) {
                            SUBMIT(inst_move(dt, tmp, key));
                        } else if (dt.data == 16) {
                            dt = TB_TYPE_I32;
                            SUBMIT(inst_op_rr(MOVZXW, dt, tmp, key));
                        } else if (dt.data == 8) {
                            dt = TB_TYPE_I32;
                            SUBMIT(inst_op_rr(MOVZXB, dt, tmp, key));
                        } else {
                            dt = TB_TYPE_I32;
                            uint64_t mask = tb__mask(dt.data);

                            SUBMIT(inst_move(dt, tmp, key));
                            SUBMIT(inst_op_rri(AND, dt, tmp, tmp, mask));
                        }*/

                        // copy key into temporary
                        {
                            Val tmp = op_at(ctx, t->ins[1].src);
                            inst2(e, MOV, &tmp, &key, TB_X86_TYPE_QWORD);
                            key = tmp;
                        }

                        int ins = 1;
                        Val target = op_at(ctx, t->ins[2].src);
                        Val table = op_at(ctx, t->ins[3].src);

                        // Simple range check:
                        //   if ((key - min) >= (max - min)) goto default
                        if (has_default) {
                            if (min != 0) {
                                Val imm = val_imm(min);
                                inst2(e, SUB, &key, &imm, cmp_dt);
                            }
                            // cmp key, range
                            Val imm = val_imm(range);
                            inst2(e, CMP, &key, &imm, cmp_dt);
                            // jnb fallthru
                            jcc(e, NB, succ[0]);
                        }
                        //   lea target, [rip + f]
                        Val fn_sym = val_global((TB_Symbol*) f, 0);
                        inst2(e, LEA, &target, &fn_sym, TB_X86_TYPE_QWORD);
                        //   lea table, [rip + JUMP_TABLE]
                        Val table_sym = val_global((TB_Symbol*) jump_table, 0);
                        inst2(e, LEA, &table, &table_sym, TB_X86_TYPE_QWORD);
                        //   movsxd table, [table + key*4]
                        Val addr = val_base_index_disp(table.reg, key.reg, SCALE_X4, 0);
                        inst2(e, MOVSXD, &table, &addr, TB_X86_TYPE_QWORD);
                        //   add target, table
                        inst2(e, ADD, &target, &table, TB_X86_TYPE_QWORD);
                        //   jmp target
                        __(jmp, target);
                    }
                }

                tb_arena_restore(arena, sp);
                break;
            }

            default: tb_todo();
        }
    }
}
#endif

static void post_emit(Ctx* restrict ctx, TB_CGEmitter* e) {
    // pad to 16bytes
    static const uint8_t nops[8][8] = {
        { 0x90 },
        { 0x66, 0x90 },
        { 0x0F, 0x1F, 0x00 },
        { 0x0F, 0x1F, 0x40, 0x00 },
        { 0x0F, 0x1F, 0x44, 0x00, 0x00 },
        { 0x66, 0x0F, 0x1F, 0x44, 0x00, 0x00 },
        { 0x0F, 0x1F, 0x80, 0x00, 0x00, 0x00, 0x00 },
        { 0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00 },
    };

    size_t pad = 16 - (ctx->emit.count & 15);
    if (pad < 16) {
        ctx->nop_pads = pad;

        uint8_t* dst = tb_cgemit_reserve(&ctx->emit, pad);
        tb_cgemit_commit(&ctx->emit, pad);

        if (pad > 8) {
            size_t rem = pad - 8;
            memset(dst, 0x66, rem);
            pad -= rem, dst += rem;
        }
        memcpy(dst, nops[pad - 1], pad);
    }
}

static void emit_win64eh_unwind_info(TB_Emitter* e, TB_FunctionOutput* out_f, uint64_t stack_usage) {
    size_t patch_pos = e->count;
    UnwindInfo unwind = {
        .version = 1,
        .flags = 0, // UNWIND_FLAG_EHANDLER,
        .prolog_length = out_f->prologue_length,
        .code_count = 0,
    };
    tb_outs(e, sizeof(UnwindInfo), &unwind);

    size_t code_count = 0;
    if (stack_usage > 0) {
        UnwindCode codes[] = {
            // sub rsp, stack_usage
            { .code_offset = 4, .unwind_op = UNWIND_OP_ALLOC_SMALL, .op_info = (stack_usage / 8) - 1 },
        };
        tb_outs(e, sizeof(codes), codes);
        code_count += 1;
    }

    tb_patch1b(e, patch_pos + offsetof(UnwindInfo, code_count), code_count);
}

#define E(fmt, ...) tb_asm_print(e, fmt, ## __VA_ARGS__)
// #define E(fmt, ...) printf(fmt, ## __VA_ARGS__)
static void our_print_memory_operand(TB_CGEmitter* e, Disasm* restrict d, TB_X86_Inst* restrict inst, size_t pos) {
    uint8_t base = inst->regs & 0xFF;
    uint8_t index = (inst->regs >> 8) & 0xFF;

    if (inst->flags & TB_X86_INSTR_INDIRECT) {
        if ((inst->regs & 0xFFFF) == 0xFFFF) {
            E("[rip");
        } else {
            E("%s [", tb_x86_type_name(inst->dt));
            if (base != 0xFF) {
                E("%s", tb_x86_reg_name(base, TB_X86_TYPE_QWORD));
            }

            if (index != 0xFF) {
                E(" + %s*%d", tb_x86_reg_name(index, TB_X86_TYPE_QWORD), 1 << inst->scale);
            }
        }

        if (inst->disp > 0) {
            E(" + %#x", inst->disp);
        } else if (inst->disp < 0) {
            E(" - %#x", -inst->disp);
        }
        E("]");
    } else if (base != 0xFF) {
        E("%s", tb_x86_reg_name(base, inst->dt));
    }
}

static void our_print_rip32(TB_CGEmitter* e, Disasm* restrict d, TB_X86_Inst* restrict inst, size_t pos, int64_t imm) {
    if (d->patch && d->patch->pos == pos - 4) {
        const TB_Symbol* target = d->patch->target;

        if (target->name[0] == 0) {
            E("sym%p", target);
        } else {
            E("%s", target->name);
        }

        if (imm > 0) {
            E(" + %"PRId64, imm);
        } else if (imm < 0) {
            E(" - %"PRId64, imm);
        }

        d->patch = d->patch->next;
    } else {
        uint32_t target = pos + imm;
        int bb = tb_emit_get_label(e, target);
        uint32_t landed = e->labels[bb] & 0x7FFFFFFF;

        if (landed != target) {
            E(".bb%d + %d", bb, (int)target - (int)landed);
        } else {
            E(".bb%d", bb);
        }
    }
}

static void disassemble(TB_CGEmitter* e, Disasm* restrict d, int bb, size_t pos, size_t end) {
    if (bb >= 0) {
        E(".bb%d:\n", bb);
    }

    while (pos < end) {
        while (d->loc != d->end && d->loc->pos == pos) {
            E("  // %s : line %d\n", d->loc->file->path, d->loc->line);
            d->loc++;
        }

        TB_X86_Inst inst;
        if (!tb_x86_disasm(&inst, end - pos, &e->data[pos])) {
            E("  ERROR\n");
            pos += 1; // skip ahead once... cry
            continue;
        }

        uint64_t line_start = e->total_asm;
        const char* mnemonic = tb_x86_mnemonic(&inst);
        E("  ");
        if (inst.flags & TB_X86_INSTR_REP) {
            E("rep ");
        }
        if (inst.flags & TB_X86_INSTR_LOCK) {
            E("lock ");
        }
        E("%s", mnemonic);
        if (inst.dt >= TB_X86_TYPE_SSE_SS && inst.dt <= TB_X86_TYPE_SSE_PD) {
            static const char* strs[] = { "ss", "sd", "ps", "pd" };
            E("%s", strs[inst.dt - TB_X86_TYPE_SSE_SS]);
        }
        E(" ");

        uint8_t rx = (inst.regs >> 16) & 0xFF;
        if (inst.flags & TB_X86_INSTR_DIRECTION) {
            if (rx != 255) {
                E("%s", tb_x86_reg_name(rx, inst.dt2));
                E(", ");
            }
            our_print_memory_operand(e, d, &inst, pos);
        } else {
            our_print_memory_operand(e, d, &inst, pos);
            if (rx != 255) {
                E(", ");
                E("%s", tb_x86_reg_name(rx, inst.dt2));
            }
        }

        if (inst.flags & TB_X86_INSTR_IMMEDIATE) {
            if (inst.regs != 0xFFFFFF) {
                E(", ");
            }

            if (inst.opcode == 0xE8 || inst.opcode == 0xE9 || inst.opcode == 0xEB || (inst.opcode >= 0x180 && inst.opcode <= 0x18F)) {
                our_print_rip32(e, d, &inst, pos + inst.length, inst.imm);
            } else {
                E("%#"PRIx64, inst.imm);
            }
        }

        int offset = e->total_asm - line_start;
        if (d->comment && d->comment->pos == pos) {
            TB_OPTDEBUG(ANSI)(E("\x1b[32m"));
            E("  // ");
            bool out_of_line = false;
            do {
                if (out_of_line) {
                    // tack on a newline
                    E("%*s  // ", offset, "");
                }

                E("%.*s\n", d->comment->line_len, d->comment->line);
                d->comment = d->comment->next;
                out_of_line = true;
            } while  (d->comment && d->comment->pos == pos);
            TB_OPTDEBUG(ANSI)(E("\x1b[0m"));
        } else {
            E("\n");
        }

        pos += inst.length;
    }
}
#undef E

static size_t emit_call_patches(TB_Module* restrict m, TB_FunctionOutput* out_f) {
    size_t r = 0;
    uint32_t src_section = out_f->section;

    for (TB_SymbolPatch* patch = out_f->first_patch; patch; patch = patch->next) {
        if (patch->target->tag == TB_SYMBOL_FUNCTION) {
            uint32_t dst_section = ((TB_Function*) patch->target)->output->section;

            // you can't do relocations across sections
            if (src_section == dst_section) {
                assert(patch->pos < out_f->code_size);

                // x64 thinks of relative addresses as being relative
                // to the end of the instruction or in this case just
                // 4 bytes ahead hence the +4.
                size_t actual_pos = out_f->code_pos + patch->pos + 4;

                uint32_t p = ((TB_Function*) patch->target)->output->code_pos - actual_pos;
                memcpy(&out_f->code[patch->pos], &p, sizeof(uint32_t));

                r += 1;
                patch->internal = true;
            }
        }
    }

    return out_f->patch_count - r;
}

ICodeGen tb__x64_codegen = {
    .minimum_addressable_size = 8,
    .pointer_size = 64,
    .emit_win64eh_unwind_info = emit_win64eh_unwind_info,
    .emit_call_patches  = emit_call_patches,
    .get_data_type_size = get_data_type_size,
    .compile_function   = compile_function,
};
#else
ICodeGen tb__x64_codegen;
#endif
