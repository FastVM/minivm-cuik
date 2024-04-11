// Let's just explain the architecture of the optimizer here.
//
// # Peephole optimizations
//   These are the kind which work locally like 2+2=4 and in TB's design they're
//   performed incrementally which means that certain mutations must go through
//   functions to guarentee they update correctly. Let's go over those:
//
//   set_input(f, n, in, slot)
//     basically `n->inputs[slot] = in` except it correctly updates the user set
//
// # How to implement peepholes
//     TODO
//
#include "passes.h"
#include <log.h>

// helps us do some matching later
static void remove_user(TB_Function* f, TB_Node* n, int slot);
static void remove_input(TB_Function* f, TB_Node* n, size_t i);
static void violent_kill(TB_Function* f, TB_Node* n);
static bool alloc_types(TB_Function* f);

static void print_lattice(Lattice* l);

static Lattice* value_of(TB_Function* f, TB_Node* n);

// node creation helpers
TB_Node* make_poison(TB_Function* f, TB_DataType dt);
TB_Node* dead_node(TB_Function* f);
TB_Node* make_int_node(TB_Function* f, TB_DataType dt, uint64_t x);
TB_Node* make_proj_node(TB_Function* f, TB_DataType dt, TB_Node* src, int i);

////////////////////////////////
// TB_Worklist
////////////////////////////////
void worklist_alloc(TB_Worklist* restrict ws, size_t initial_cap) {
    ws->visited_cap = (initial_cap + 63) / 64;
    ws->visited = tb_platform_heap_alloc(ws->visited_cap * sizeof(uint64_t));
    ws->items = dyn_array_create(uint64_t, ws->visited_cap * 64);
    FOR_N(i, 0, ws->visited_cap) {
        ws->visited[i] = 0;
    }
}

void worklist_free(TB_Worklist* restrict ws) {
    tb_platform_heap_free(ws->visited);
    dyn_array_destroy(ws->items);
}

void worklist_clear_visited(TB_Worklist* restrict ws) {
    CUIK_TIMED_BLOCK("clear visited") {
        memset(ws->visited, 0, ws->visited_cap * sizeof(uint64_t));
    }
}

void worklist_clear(TB_Worklist* restrict ws) {
    CUIK_TIMED_BLOCK("clear worklist") {
        memset(ws->visited, 0, ws->visited_cap * sizeof(uint64_t));
        dyn_array_clear(ws->items);
    }
}

void worklist_remove(TB_Worklist* restrict ws, TB_Node* n) {
    uint64_t gvn_word = n->gvn / 64; // which word this ID is at
    if (gvn_word >= ws->visited_cap) return;

    uint64_t gvn_mask = 1ull << (n->gvn % 64);
    ws->visited[gvn_word] &= ~gvn_mask;
}

// checks if node is visited but doesn't push item
bool worklist_test(TB_Worklist* restrict ws, TB_Node* n) {
    uint64_t gvn_word = n->gvn / 64; // which word this ID is at
    if (gvn_word >= ws->visited_cap) return false;

    uint64_t gvn_mask = 1ull << (n->gvn % 64);
    return ws->visited[gvn_word] & gvn_mask;
}

bool worklist_test_n_set(TB_Worklist* restrict ws, TB_Node* n) {
    uint64_t gvn_word = n->gvn / 64; // which word this ID is at

    // resize?
    if (gvn_word >= ws->visited_cap) {
        size_t new_cap = gvn_word + 16;
        ws->visited = tb_platform_heap_realloc(ws->visited, new_cap * sizeof(uint64_t));

        // clear new space
        FOR_N(i, ws->visited_cap, new_cap) {
            ws->visited[i] = 0;
        }

        ws->visited_cap = new_cap;
    }

    uint64_t gvn_mask = 1ull << (n->gvn % 64);
    if (ws->visited[gvn_word] & gvn_mask) {
        return true;
    } else {
        ws->visited[gvn_word] |= gvn_mask;
        return false;
    }
}

void worklist_push(TB_Worklist* restrict ws, TB_Node* restrict n) {
    if (!worklist_test_n_set(ws, n)) { dyn_array_put(ws->items, n); }
}

TB_Node* worklist_pop(TB_Worklist* ws) {
    if (dyn_array_length(ws->items)) {
        TB_Node* n = dyn_array_pop(ws->items);
        uint64_t gvn_word = n->gvn / 64;
        uint64_t gvn_mask = 1ull << (n->gvn % 64);

        ws->visited[gvn_word] &= ~gvn_mask;
        return n;
    } else {
        return NULL;
    }
}

int worklist_count(TB_Worklist* ws) {
    return dyn_array_length(ws->items);
}

static int bits_in_data_type(int pointer_size, TB_DataType dt) {
    switch (dt.type) {
        case TB_TAG_INT: return dt.data;
        case TB_TAG_PTR: return pointer_size;
        case TB_TAG_F32: return 32;
        case TB_TAG_F64: return 64;
        default: return 0;
    }
}

static int bytes_in_data_type(int pointer_size, TB_DataType dt) {
    return (bits_in_data_type(pointer_size, dt) + 7) / 8;
}

static TB_Node* mem_user(TB_Function* f, TB_Node* n, int slot) {
    FOR_USERS(u, n) {
        if ((USERN(u)->type == TB_PROJ && USERN(u)->dt.type == TB_TAG_MEMORY) ||
            (USERI(u) == slot && is_mem_out_op(USERN(u)))) {
            return USERN(u);
        }
    }

    return NULL;
}

static bool is_empty_bb(TB_Function* f, TB_Node* end) {
    assert(end->type == TB_BRANCH || end->type == TB_UNREACHABLE);
    if (!cfg_is_bb_entry(end->inputs[0])) {
        return false;
    }

    TB_Node* bb = end->inputs[0];
    FOR_USERS(u, bb) {
        if (USERN(u) != end) { return false; }
    }

    return true;
}

static bool same_sorta_branch(TB_Node* n, TB_Node* n2) {
    TB_NodeBranchProj* br  = cfg_if_branch(n);
    TB_NodeBranchProj* br2 = cfg_if_branch(n2);
    return br && br2 && br->key == br2->key;
}

// incremental dominators, plays nice with peepholes and has
// a limited walk of 20 steps.
static TB_Node* fast_idom(TB_Node* bb) {
    int steps = 0;

    // note that "subtypes" of region like TB_NATURAL_LOOP and TB_AFFINE_LOOP are
    // valid for fast doms since they guarentee they're dominated by inputs[0]
    while (steps < FAST_IDOM_LIMIT && bb->type != TB_REGION && bb->type != TB_ROOT) {
        bb = bb->inputs[0];
        steps++;
    }

    return bb;
}

static bool fast_dommy(TB_Node* expected_dom, TB_Node* bb) {
    int steps = 0;

    // note that "subtypes" of region like TB_NATURAL_LOOP and TB_AFFINE_LOOP are
    // valid for fast doms since they guarentee they're dominated by inputs[0]
    while (steps < FAST_IDOM_LIMIT && bb != expected_dom && bb->type != TB_REGION && bb->type != TB_ROOT) {
        bb = bb->inputs[0];
        steps++;
    }

    return bb == expected_dom;
}

static void mark_node(TB_Function* f, TB_Node* n) {
    worklist_push(f->worklist, n);
}

static void mark_users_raw(TB_Function* f, TB_Node* n) {
    FOR_USERS(u, n) { mark_node(f, USERN(u)); }
}

static void mark_users(TB_Function* f, TB_Node* n) {
    FOR_USERS(u, n) {
        worklist_push(f->worklist, USERN(u));
        TB_NodeTypeEnum type = USERN(u)->type;

        // tuples changing means their projections did too.
        if (type == TB_PROJ || type == TB_MEMBER_ACCESS) {
            mark_users(f, USERN(u));
        }

        // (br (cmp a b)) => ...
        // (or (shl a 24) (shr a 40)) => ...
        // (trunc (mul a b)) => ...
        // (phi ...) => ... (usually converting into branchless ops)
        if ((type >= TB_CMP_EQ && type <= TB_CMP_FLE) ||
            type == TB_SHL || type == TB_SHR || type == TB_MUL ||
            type == TB_STORE || type == TB_PHI) {
            mark_users_raw(f, USERN(u));
        }
    }
}

static void mark_node_n_users(TB_Function* f, TB_Node* n) {
    worklist_push(f->worklist, n);
    mark_node(f, n);
}

// unity build with all the passes
#include "properties.h"
#include "lattice.h"
#include "cfg.h"
#include "gvn.h"
#include "fold.h"
#include "mem_opt.h"
#include "sroa.h"
#include "loop.h"
#include "branches.h"
#include "print.h"
#include "verify.h"
#include "print_dumb.h"
#include "print_c.h"
#include "gcm.h"
#include "libcalls.h"
#include "mem2reg.h"
#include "scheduler.h"
#include "list_sched.h"
#include "legalizer.h"

void tb__gvn_remove(TB_Function* f, TB_Node* n) {
    nl_hashset_remove2(&f->gvn_nodes, n, gvn_hash, gvn_compare);
}

static void violent_kill(TB_Function* f, TB_Node* n) {
    // remove from GVN if we're murdering it
    size_t extra = extra_bytes(n);
    nl_hashset_remove2(&f->gvn_nodes, n, gvn_hash, gvn_compare);

    // remove users
    FOR_REV_N(i, 0, n->user_count) {
        remove_user(f, n, i);
        n->inputs[i] = NULL;
    }

    tb_arena_free(f->arena, n->users, n->user_cap * sizeof(TB_User));
    tb_arena_free(f->arena, n->inputs, n->input_cap * sizeof(TB_Node*));
    tb_arena_free(f->arena, n, sizeof(TB_Node) + extra);

    n->user_cap = n->user_count = 0;
    n->users = NULL;
    n->input_count = 0;
    n->type = TB_NULL;
}

static Lattice* value_f32(TB_Function* f, TB_Node* n) {
    assert(n->type == TB_F32CONST);
    TB_NodeFloat32* num = TB_NODE_GET_EXTRA(n);
    return lattice_intern(f, (Lattice){ LATTICE_FLTCON32, ._f32 = num->value });
}

static Lattice* value_f64(TB_Function* f, TB_Node* n) {
    assert(n->type == TB_F64CONST);
    TB_NodeFloat64* num = TB_NODE_GET_EXTRA(n);
    return lattice_intern(f, (Lattice){ LATTICE_FLTCON64, ._f64 = num->value });
}

static Lattice* value_int(TB_Function* f, TB_Node* n) {
    assert(n->type == TB_ICONST);
    TB_NodeInt* num = TB_NODE_GET_EXTRA(n);
    if (n->dt.type == TB_TAG_PTR) {
        return num->value ? &XNULL_IN_THE_SKY : &NULL_IN_THE_SKY;
    } else {
        uint64_t mask = tb__mask(n->dt.data);
        int64_t x = tb__sxt(num->value & mask, n->dt.data, 64);
        return lattice_intern(f, (Lattice){ LATTICE_INT, ._int = { x, x, ~x, x } });
    }
}

static Lattice* value_root(TB_Function* f, TB_Node* n) {
    return lattice_tuple_from_node(f, f->root_node);
}

static Lattice* value_proj(TB_Function* f, TB_Node* n) {
    assert(is_proj(n));
    Lattice* l = latuni_get(f, n->inputs[0]);
    if (l == &TOP_IN_THE_SKY) {
        return &TOP_IN_THE_SKY;
    } else if (l == &BOT_IN_THE_SKY) {
        return lattice_from_dt(f, n->dt);
    } else {
        assert(l->tag == LATTICE_TUPLE);
        int index = TB_NODE_GET_EXTRA_T(n, TB_NodeProj)->index;
        return l->elems[index];
    }
}

static Lattice* value_dead(TB_Function* f, TB_Node* n) {
    return &TOP_IN_THE_SKY;
}

static Lattice* value_ctrl(TB_Function* f, TB_Node* n) {
    return latuni_get(f, n->inputs[0]);
}

static Lattice* value_ptr_vals(TB_Function* f, TB_Node* n) {
    if (n->type == TB_LOCAL) {
        return &XNULL_IN_THE_SKY;
    } else {
        assert(n->type == TB_SYMBOL);
        return lattice_intern(f, (Lattice){ LATTICE_PTRCON, ._ptr = TB_NODE_GET_EXTRA_T(n, TB_NodeSymbol)->sym });
    }
}

static Lattice* value_lookup(TB_Function* f, TB_Node* n) {
    TB_NodeLookup* l = TB_NODE_GET_EXTRA(n);
    TB_DataType dt = n->dt;
    assert(dt.type == TB_TAG_INT);

    LatticeInt a = { l->entries[0].val, l->entries[0].val, l->entries[0].val, ~l->entries[0].val };
    FOR_N(i, 1, n->input_count) {
        TB_LookupEntry* e = &l->entries[i];
        a.min = TB_MIN(a.min, l->entries[i].val);
        a.max = TB_MAX(a.max, l->entries[i].val);
        a.known_zeros &=  l->entries[i].val;
        a.known_ones  &= ~l->entries[i].val;
    }

    return lattice_intern(f, (Lattice){ LATTICE_INT, ._int = a });
}

static Lattice* value_region(TB_Function* f, TB_Node* n) {
    assert(cfg_is_region(n));

    // technically just the MOP logic but folded out
    FOR_N(i, 0, n->input_count) {
        Lattice* edge = latuni_get(f, n->inputs[i]);
        if (edge == &CTRL_IN_THE_SKY) { return &CTRL_IN_THE_SKY; }
    }

    return &TOP_IN_THE_SKY;
}

static Lattice* value_phi(TB_Function* f, TB_Node* n) {
    // wait for region to check first
    TB_Node* r = n->inputs[0];
    if (latuni_get(f, r) == &TOP_IN_THE_SKY) return &TOP_IN_THE_SKY;

    if (r->type == TB_AFFINE_LOOP) {
        TB_Node* latch = affine_loop_latch(r);

        if (latch) {
            // when a loop is deleted, you end up with a latch that's constant
            // so we'll undo the affine loop allegations and just
            /*Lattice* latch_t = latuni_get(f, latch->inputs[1]);
            if (lattice_is_const(latch_t)) {
                bool exit_when_key = !TB_NODE_GET_EXTRA_T(r->inputs[1], TB_NodeProj)->index;
                TB_Node* exit_proj = USERN(proj_with_index(latch, exit_when_key));
                int64_t latch_key  = TB_NODE_GET_EXTRA_T(exit_proj, TB_NodeBranchProj)->key;

                if ((exit_when_key  && lattice_int_eq(latch_t, latch_key)) ||
                    (!exit_when_key && lattice_int_ne(latch_t, latch_key))) {
                    // this is a loop without a real backedge, it'll get packed up soon but
                    // until then we consider it 1 trip, i can walk through why i guess:
                    // * affine loops are natural loops (we've rotated them)
                    // * if you're seeing the phi we're past the ZTC (trips greater than 1)
                    // * thus a dead latch means 1 trip not 0.
                    Lattice* init = latuni_get(f, n->inputs[1]);
                    Lattice* init = latuni_get(f, n->inputs[1]);

                    return latuni_get(f, n->inputs[1]);
                }
            }*/

            TB_InductionVar var;
            if (find_indvar(r, latch, &var)) {
                if (var.phi == n) {
                    Lattice* init = latuni_get(f, n->inputs[1]);
                    Lattice* end = var.end_cond ? latuni_get(f, var.end_cond) : lattice_int_const(f, var.end_const);

                    if (lattice_is_const(init) && lattice_is_const(end)) {
                        int64_t trips = (end->_int.min - init->_int.min) / var.step;
                        int64_t rem   = (end->_int.min - init->_int.min) % var.step;
                        if (rem == 0 || var.pred != IND_NE) {
                            return lattice_gimme_int(f, init->_int.min, init->_int.min + trips*var.step);
                        }
                    } else {
                        // TODO(NeGate): we vaguely know the range, this is the common case
                        // so let's handle it. main things we wanna know are whether or not
                        // the number is ever negative.
                    }
                }
            } else {
                // affine loop missing latch? ok then it's 1 trips
                return latuni_get(f, n->inputs[1]);
            }
        }
    }

    Lattice* old = latuni_get(f, n);
    if (old->tag == LATTICE_INT && old->_int.widen >= INT_WIDEN_LIMIT) {
        return lattice_intern(f, (Lattice){ LATTICE_INT, ._int = { lattice_int_min(n->dt.data), lattice_int_max(n->dt.data), .widen = INT_WIDEN_LIMIT } });
    }

    Lattice* l = old;
    FOR_N(i, 1, n->input_count) {
        Lattice* ctrl = latuni_get(f, r->inputs[i - 1]);
        if (ctrl == &CTRL_IN_THE_SKY) {
            Lattice* edge = latuni_get(f, n->inputs[i]);
            l = lattice_meet(f, l, edge);
        }
    }

    // downward progress will widen...
    Lattice* glb = lattice_meet(f, old, l);
    if (old != l && glb == l) {
        Lattice new_l = *l;
        new_l._int.widen = TB_MAX(old->_int.widen, l->_int.widen) + 1;
        return lattice_intern(f, new_l);
    }
    return l;
}

static Lattice* value_select(TB_Function* f, TB_Node* n) {
    Lattice* a = latuni_get(f, n->inputs[2]);
    Lattice* b = latuni_get(f, n->inputs[3]);
    return lattice_meet(f, a, b);
}

// this is where the vtable goes for all peepholes
#include "peeps.h"

static bool can_gvn(TB_Node* n) {
    switch (n->type) {
        case TB_LOCAL:
        return false;

        // control producing nodes can't really GVN, it doesn't make sense if
        // they're constructed from a CFG.
        case TB_ROOT:
        case TB_CALL:
        case TB_READ:
        case TB_REGION:
        case TB_WRITE:
        case TB_RETURN:
        case TB_BRANCH:
        case TB_AFFINE_LATCH:
        case TB_SYSCALL:
        case TB_TAILCALL:
        case TB_CALLGRAPH:
        case TB_NATURAL_LOOP:
        case TB_AFFINE_LOOP:
        case TB_ATOMIC_LOAD:
        case TB_ATOMIC_XCHG:
        case TB_ATOMIC_ADD:
        case TB_ATOMIC_SUB:
        case TB_ATOMIC_AND:
        case TB_ATOMIC_XOR:
        case TB_ATOMIC_OR:
        case TB_ATOMIC_CAS:
        case TB_SAFEPOINT_POLL:
        return false;

        default: {
            int family = n->type / 0x100;
            if (family == 0) {
                return true;
            } else {
                assert(family >= 0 && family < TB_ARCH_MAX);
                return tb_codegen_families[family].extra_bytes(n);
            }
        }
    }
}

TB_Node* tb_opt_gvn_node(TB_Function* f, TB_Node* n) {
    return tb__gvn(f, n, extra_bytes(n));
}

TB_Node* tb__gvn(TB_Function* f, TB_Node* n, size_t extra) {
    if (!can_gvn(n)) { return n; }

    // try GVN, if we succeed, just delete the node and use the old copy
    TB_Node* k = nl_hashset_put2(&f->gvn_nodes, n, gvn_hash, gvn_compare);
    if (k && k != n) {
        // remove users
        FOR_N(i, 0, n->input_count) {
            remove_user(f, n, i);
            n->inputs[i] = NULL;
        }

        tb_arena_free(f->arena, n->users, n->user_cap * sizeof(TB_User));
        tb_arena_free(f->arena, n->inputs, n->input_cap * sizeof(TB_Node*));
        tb_arena_free(f->arena, n, sizeof(TB_Node) + extra);
        n->type = TB_NULL;
        return k;
    } else {
        return n;
    }
}

TB_Node* make_poison(TB_Function* f, TB_DataType dt) {
    TB_Node* n = tb_alloc_node(f, TB_POISON, dt, 1, 0);
    set_input(f, n, f->root_node, 0);
    return tb__gvn(f, n, 0);
}

TB_Node* make_int_node(TB_Function* f, TB_DataType dt, uint64_t x) {
    uint64_t mask = tb__mask(dt.data);
    x &= mask;

    TB_Node* n = tb_alloc_node(f, TB_ICONST, dt, 1, sizeof(TB_NodeInt));
    TB_NodeInt* i = TB_NODE_GET_EXTRA(n);
    i->value = x;

    set_input(f, n, f->root_node, 0);

    latuni_set(f, n, value_int(f, n));
    return tb__gvn(f, n, sizeof(TB_NodeInt));
}

TB_Node* dead_node(TB_Function* f) {
    TB_Node* n = tb_alloc_node(f, TB_DEAD, TB_TYPE_VOID, 1, 0);
    set_input(f, n, f->root_node, 0);
    latuni_set(f, n, &TOP_IN_THE_SKY);
    return tb__gvn(f, n, 0);
}

TB_Node* make_proj_node(TB_Function* f, TB_DataType dt, TB_Node* src, int i) {
    TB_Node* n = tb_alloc_node(f, TB_PROJ, dt, 1, sizeof(TB_NodeProj));
    set_input(f, n, src, 0);
    TB_NODE_SET_EXTRA(n, TB_NodeProj, .index = i);
    return n;
}

static void remove_input(TB_Function* f, TB_Node* n, size_t i) {
    // remove swap
    n->input_count--;
    if (n->input_count > 0) {
        if (n->input_count != i) {
            set_input(f, n, n->inputs[n->input_count], i);
        }
        set_input(f, n, NULL, n->input_count);
    }
}

void tb_kill_node(TB_Function* f, TB_Node* n) {
    // remove from GVN if we're murdering it
    if (can_gvn(n)) {
        nl_hashset_remove2(&f->gvn_nodes, n, gvn_hash, gvn_compare);
    }

    FOR_N(i, 0, n->input_count) {
        remove_user(f, n, i);
        n->inputs[i] = NULL;
    }

    // assert(n->users == NULL && "we can't kill nodes with users, that's fucking rude");
    n->input_count = 0;
    n->type = TB_NULL;
}

static void remove_user(TB_Function* f, TB_Node* n, int slot) {
    // early out: there was no previous input
    if (n->inputs[slot] == NULL) { return; }

    TB_Node* old = n->inputs[slot];
    TB_User* old_use = old->users;

    // remove-swap
    FOR_N(i, 0, old->user_count) {
        if (USERN(&old_use[i]) == n && USERI(&old_use[i]) == slot) {
            old->user_count -= 1;
            old_use[i] = old_use[old->user_count];

            // push to worklist, we've got a dead node
            if (old->user_count == 0 && f->worklist) {
                worklist_push(f->worklist, old);
            }
            return;
        }
    }

    tb_panic("Failed to remove non-existent user %p from %p (slot %d)", old, n, slot);
}

void set_input(TB_Function* f, TB_Node* n, TB_Node* in, int slot) {
    // try to recycle the user
    // assert(slot < n->input_count);
    remove_user(f, n, slot);
    n->inputs[slot] = in;
    if (in != NULL) { add_user(f, n, in, slot); }
}

// we sometimes get the choice to recycle users because we just deleted something
void add_user(TB_Function* f, TB_Node* n, TB_Node* in, int slot) {
    if (in->user_count >= in->user_cap) {
        size_t new_cap = ((size_t) in->user_cap) * 2;
        assert(new_cap < UINT16_MAX);

        // resize
        TB_User* users = tb_arena_alloc(f->arena, new_cap * sizeof(TB_User));
        memcpy(users, in->users, in->user_count * sizeof(TB_User));

        // in debug builds we'll fill the old array if easily detectable garbage to notice
        // pointer invalidation issues
        #ifndef NDEBUG
        memset(in->users, 0xF0, in->user_cap * sizeof(TB_User));
        memset(users + in->user_count, 0xF0, (new_cap - in->user_count) * sizeof(TB_User));
        #endif

        in->user_cap = new_cap;
        in->users = users;
    }

    in->users[in->user_count]._n    = n;
    in->users[in->user_count]._slot = slot;
    in->user_count += 1;
}

void subsume_node2(TB_Function* f, TB_Node* n, TB_Node* new_n) {
    if (new_n->user_count + n->user_count >= new_n->user_cap) {
        size_t new_cap = tb_next_pow2(new_n->user_count + n->user_count + 4);
        assert(new_cap < UINT16_MAX);

        // resize
        TB_User* users = tb_arena_alloc(f->arena, new_cap * sizeof(TB_User));
        memcpy(users, new_n->users, new_n->user_count * sizeof(TB_User));

        // in debug builds we'll fill the old array if easily detectable garbage to notice
        // pointer invalidation issues
        #ifndef NDEBUG
        memset(new_n->users, 0xF0, new_n->user_cap * sizeof(TB_User));
        memset(users + new_n->user_count, 0xF0, (new_cap - new_n->user_count) * sizeof(TB_User));
        #endif

        new_n->user_cap = new_cap;
        new_n->users = users;
    }

    FOR_N(i, 0, n->user_count) {
        TB_Node* un = USERN(&n->users[i]);
        int ui      = USERI(&n->users[i]);
        tb_assert(un->inputs[ui] == n, "Mismatch between def-use and use-def data");

        tb__gvn_remove(f, un);
        un->inputs[ui] = new_n;
        new_n->users[new_n->user_count + i] = n->users[i];
    }
    new_n->user_count += n->user_count;
    n->user_count = 0;
}

void subsume_node(TB_Function* f, TB_Node* n, TB_Node* new_n) {
    subsume_node2(f, n, new_n);
    tb_kill_node(f, n);
}

// Returns NULL or a modified node (could be the same node, we can stitch it back into place)
static TB_Node* idealize(TB_Function* f, TB_Node* n) {
    NodeIdealize ideal = node_vtables[n->type].idealize;
    return ideal ? ideal(f, n) : NULL;
}

static TB_Node* identity(TB_Function* f, TB_Node* n) {
    NodeIdentity identity = node_vtables[n->type].identity;
    return identity ? identity(f, n) : n;
}

static Lattice* value_of(TB_Function* f, TB_Node* n) {
    NodeValueOf value = node_vtables[n->type].value;
    Lattice* type = value ? value(f, n) : NULL;

    // no type provided? just make a not-so-form fitting bottom type
    if (type == NULL) {
        Lattice* old_type = latuni_get(f, n);
        return n->dt.type == TB_TAG_TUPLE ? lattice_tuple_from_node(f, n) : lattice_from_dt(f, n->dt);
    } else {
        return type;
    }
}

// converts constant Lattice into constant node
static bool is_dead_ctrl(TB_Function* f, TB_Node* n) { return latuni_get(f, n) == &TOP_IN_THE_SKY; }
static TB_Node* try_as_const(TB_Function* f, TB_Node* n, Lattice* l) {
    // already a constant?
    if (n->type == TB_SYMBOL || n->type == TB_ICONST || n->type == TB_F32CONST || n->type == TB_F64CONST) {
        return NULL;
    }

    // Dead node? kill
    if (cfg_is_region(n)) {
        // remove dead predeccessors
        bool changes = false;

        size_t i = 0, extra_edges = 0;
        while (i < n->input_count) {
            if (is_dead_ctrl(f, n->inputs[i])) {
                changes = true;
                remove_input(f, n, i);

                FOR_USERS(u, n) {
                    if (USERN(u)->type == TB_PHI && USERI(u) == 0) {
                        remove_input(f, USERN(u), i + 1);
                    }
                }
            } else {
                i += 1;
            }
        }

        if (n->input_count == 0) {
            f->invalidated_loops = true;
            tb_kill_node(f, n);
            return dead_node(f);
        } else if (n->input_count == 1) {
            // remove phis, because we're single entry they're all degens
            for (size_t i = 0; i < n->user_count;) {
                TB_User* use = &n->users[i];
                if (USERN(use)->type == TB_PHI) {
                    assert(USERI(use) == 0);
                    assert(USERN(use)->input_count == 2);
                    subsume_node(f, USERN(use), USERN(use)->inputs[1]);
                } else {
                    i += 1;
                }
            }

            f->invalidated_loops = true;
            return n->inputs[0];
        } else if (changes) {
            f->invalidated_loops = true;
            return n;
        } else {
            return NULL;
        }
    } else if (n->type != TB_ROOT && n->inputs[0] && is_dead_ctrl(f, n->inputs[0])) {
        if (n->type == TB_BRANCH || n->type == TB_AFFINE_LATCH) {
            f->invalidated_loops = true;
        }

        // control-dependent nodes which become considered dead will also
        // have to be dead.
        if (n->dt.type == TB_TAG_TUPLE) {
            TB_Node* dead = dead_node(f);
            while (n->user_count > 0) {
                TB_Node* use_n = USERN(&n->users[n->user_count - 1]);
                int use_i      = USERI(&n->users[n->user_count - 1]);

                if (use_n->type == TB_CALLGRAPH) {
                    TB_Node* last = use_n->inputs[use_n->input_count - 1];
                    set_input(f, use_n, NULL, use_n->input_count - 1);
                    if (use_i != use_n->input_count - 1) {
                        set_input(f, use_n, last, use_i);
                    }
                    use_n->input_count--;
                } else if (is_proj(use_n)) {
                    TB_Node* replacement = use_n->dt.type == TB_TAG_CONTROL
                        ? dead
                        : make_poison(f, use_n->dt);

                    subsume_node(f, use_n, replacement);
                } else {
                    tb_todo();
                }
            }

            return dead;
        } else if (n->dt.type == TB_TAG_CONTROL) {
            return dead_node(f);
        } else {
            return make_poison(f, n->dt);
        }
    }

    switch (l->tag) {
        case LATTICE_INT: {
            // degenerate range
            if (l->_int.min == l->_int.max) {
                return make_int_node(f, n->dt, l->_int.max);
            }

            // all bits are known
            uint64_t mask = tb__mask(n->dt.data);
            if ((l->_int.known_zeros | l->_int.known_ones) == UINT64_MAX) {
                return make_int_node(f, n->dt, l->_int.known_ones);
            }

            return NULL;
        }

        case LATTICE_FLTCON32: {
            TB_Node* k = tb_alloc_node(f, TB_F32CONST, n->dt, 1, sizeof(TB_NodeFloat32));
            set_input(f, k, f->root_node, 0);
            TB_NODE_SET_EXTRA(k, TB_NodeFloat32, .value = l->_f32);
            latuni_set(f, k, l);
            return k;
        }

        case LATTICE_FLTCON64: {
            TB_Node* k = tb_alloc_node(f, TB_F64CONST, n->dt, 1, sizeof(TB_NodeFloat64));
            set_input(f, k, f->root_node, 0);
            TB_NODE_SET_EXTRA(n, TB_NodeFloat64, .value = l->_f64);
            latuni_set(f, n, l);
            return n;
        }

        case LATTICE_NULL:
        return make_int_node(f, n->dt, 0);

        case LATTICE_TUPLE: {
            if (n->type != TB_BRANCH && n->type != TB_AFFINE_LATCH) {
                return NULL;
            }

            // check if tuple is constant path
            int trues = 0;
            FOR_N(i, 0, l->_elem_count) {
                if (l->elems[i] == &CTRL_IN_THE_SKY) {
                    trues++;
                }
            }

            if (trues == 1) {
                TB_Node* dead = dead_node(f);
                TB_Node* ctrl = n->inputs[0];

                for (size_t i = 0; i < n->user_count;) {
                    TB_Node* un = USERN(&n->users[i]);
                    if (is_proj(un)) {
                        assert(USERI(&n->users[i]) == 0);
                        int index   = TB_NODE_GET_EXTRA_T(un, TB_NodeProj)->index;
                        TB_Node* in = l->elems[index] == &CTRL_IN_THE_SKY ? ctrl : dead;

                        set_input(f, un, NULL, 0);
                        subsume_node(f, un, in);
                    } else {
                        i += 1;
                    }
                }

                // no more projections, kill the branch
                tb_kill_node(f, n);
                mark_users(f, dead);
                return ctrl;
            } else {
                return NULL;
            }
        }

        default: return NULL;
    }
}

static void print_lattice(Lattice* l) {
    switch (l->tag) {
        case LATTICE_BOT:      printf("bot");                       break;
        case LATTICE_TOP:      printf("top");                       break;
        case LATTICE_CTRL:     printf("ctrl");                      break;
        case LATTICE_FLT32:    printf("f32");                       break;
        case LATTICE_FLT64:    printf("f64");                       break;
        case LATTICE_FLTCON32: printf("[f32: %f]", l->_f32);        break;
        case LATTICE_FLTCON64: printf("[f64: %f]", l->_f64);        break;
        case LATTICE_NULL:     printf("null");                      break;
        case LATTICE_XNULL:    printf("~null");                     break;
        case LATTICE_NAN32:    printf("NaN32");                     break;
        case LATTICE_XNAN32:   printf("~NaN32");                    break;
        case LATTICE_NAN64:    printf("NaN64");                     break;
        case LATTICE_XNAN64:   printf("~NaN64");                    break;
        case LATTICE_ALLPTR:   printf("allptr");                    break;
        case LATTICE_ANYPTR:   printf("anyptr");                    break;
        case LATTICE_PTRCON:   printf("%s", l->_ptr->name);         break;
        case LATTICE_ANYMEM:   printf("anymem");                    break;
        case LATTICE_ALLMEM:   printf("allmem");                    break;
        case LATTICE_MEM_SLICE: {
            printf("[mem:");
            bool comma = false;
            FOR_N(i, 0, l->_alias_n) {
                uint64_t bits = l->alias[i], j = 0;
                while (bits) {
                    if (bits & 1) {
                        if (!comma) { comma = true; } else { printf(","); }
                        printf("%"PRIu64, i*64 + j);
                    }
                    bits >>= 1, j++;
                }
            }
            printf("]");
            break;
        }

        case LATTICE_TUPLE: {
            printf("[");
            FOR_N(i, 0, l->_elem_count) {
                if (i) printf(", ");
                print_lattice(l->elems[i]);
            }
            printf("]");
            break;
        }

        case LATTICE_INT: {
            printf("[");
            if (l->_int.min == l->_int.max) {
                printf("%"PRId64, l->_int.min);
            } else if (l->_int.min == INT16_MIN && l->_int.max == INT16_MAX) {
                printf("i8");
            } else if (l->_int.min == INT16_MIN && l->_int.max == INT16_MAX) {
                printf("i16");
            } else if (l->_int.min == INT32_MIN && l->_int.max == INT32_MAX) {
                printf("i32");
            } else if (l->_int.min == INT64_MIN && l->_int.max == INT64_MAX) {
                printf("i64");
            } else if (l->_int.min > l->_int.max) {
                printf("%"PRIu64",%"PRIu64, l->_int.min, l->_int.max);
            } else {
                printf("%"PRId64",%"PRId64, l->_int.min, l->_int.max);
            }

            uint64_t known = l->_int.known_zeros | l->_int.known_ones;
            if (known && known != UINT64_MAX) {
                printf("; zeros=%#"PRIx64", ones=%#"PRIx64, l->_int.known_zeros, l->_int.known_ones);
            }
            if (l->_int.widen) {
                printf(", widen=%"PRIu64, l->_int.widen);
            }
            printf("]");
            break;
        }

        default:
        break;
    }
}

static int node_sort_cmp(const void* a, const void* b) {
    TB_Node* const* aa = a;
    TB_Node* const* bb = b;
    return aa[0]->gvn - bb[0]->gvn;
}

static void migrate_type(TB_Function* f, TB_Node* n, TB_Node* k) {
    // if both nodes are the same datatype, we should join the elements to avoid
    // weird backtracking when dealing with the pessimistic solver.
    if (k->dt.raw == n->dt.raw) {
        Lattice* new_t = latuni_get(f, k);
        Lattice* old_t = latuni_get(f, n);
        Lattice* merged = lattice_join(f, old_t, new_t);
        latuni_set(f, k, merged);
    }
}

// because certain optimizations apply when things are the same
// we mark ALL users including the ones who didn't get changed
// when subsuming.
static TB_Node* peephole(TB_Function* f, TB_Node* n) {
    DO_IF(TB_OPTDEBUG_PEEP)(printf("PEEP t=%d? ", ++f->stats.time), tb_print_dumb_node(NULL, n));

    bool progress = false;

    // idealize can modify the node, make sure it's not in the GVN pool at the time
    if (can_gvn(n)) {
        nl_hashset_remove2(&f->gvn_nodes, n, gvn_hash, gvn_compare);
    }

    // idealize node (this can technically run an arbitrary number of times
    // but in practice we should only hit a node like once or twice)
    TB_Node* k = idealize(f, n);
    DO_IF(TB_OPTDEBUG_PEEP)(int loop_count=0);
    while (k != NULL) {
        DO_IF(TB_OPTDEBUG_STATS)(f->stats.rewrites++);
        DO_IF(TB_OPTDEBUG_PEEP)(printf(" => \x1b[32m"), tb_print_dumb_node(NULL, k), printf("\x1b[0m"));

        // transfer users from n -> k
        if (n != k) {
            migrate_type(f, n, k);
            subsume_node(f, n, k);
            n = k;
        }

        // mark post subsume since previous users of n might have
        // name equality based opts.
        progress = true;
        mark_users(f, n);

        // try again, maybe we get another transformation
        k = idealize(f, n);
        DO_IF(TB_OPTDEBUG_PEEP)(if (++loop_count > 5) { log_warn("%p: we looping a lil too much dawg...", n); });
    }

    // pessimistic constant prop
    {
        #ifndef NDEBUG
        Lattice* old_type = latuni_get(f, n);
        Lattice* new_type = value_of(f, n);

        // monotonic moving up
        Lattice* glb = lattice_meet(f, old_type, new_type);
        if (glb != old_type) {
            TB_OPTDEBUG(PEEP)(printf("\n\nFORWARD PROGRESS ASSERT!\n"));
            TB_OPTDEBUG(PEEP)(printf("  "), print_lattice(old_type), printf("  =//=>  "), print_lattice(new_type), printf(", MEET: "), print_lattice(glb), printf("\n\n"));
            assert(0 && "forward progress assert!");
        }
        #else
        Lattice* new_type = value_of(f, n);
        #endif

        // print fancy type
        DO_IF(TB_OPTDEBUG_PEEP)(printf(" => \x1b[93m"), print_lattice(new_type), printf("\x1b[0m"));

        TB_Node* k = try_as_const(f, n, new_type);
        if (k != NULL) {
            DO_IF(TB_OPTDEBUG_STATS)(f->stats.constants++);
            DO_IF(TB_OPTDEBUG_PEEP)(printf(" => \x1b[96m"), tb_print_dumb_node(NULL, k), printf("\x1b[0m\n"));

            migrate_type(f, n, k);
            subsume_node(f, n, k);
            mark_users(f, k);
            return k;
        } else if (latuni_set_progress(f, n, new_type)) {
            mark_users(f, n);
        }
    }

    // convert into matching identity
    k = identity(f, n);
    if (n != k) {
        DO_IF(TB_OPTDEBUG_STATS)(f->stats.identities++);
        DO_IF(TB_OPTDEBUG_PEEP)(printf(" => \x1b[33m"), tb_print_dumb_node(NULL, k), printf("\x1b[0m\n"));

        migrate_type(f, n, k);
        subsume_node(f, n, k);
        mark_users(f, k);
        return k;
    }

    // global value numbering
    #if TB_OPTDEBUG_GVN
    DynArray(TB_Node*) arr = dyn_array_create(TB_Node*, 64);
    nl_hashset_for(p, &f->gvn_nodes) {
        dyn_array_put(arr, *p);
    }
    qsort(arr, dyn_array_length(arr), sizeof(TB_Node*), node_sort_cmp);
    dyn_array_for(i, arr) {
        printf("  * ");
        tb_print_dumb_node(NULL, arr[i]);
        if (gvn_compare(arr[i], n)) {
            printf(" <-- HERE");
        }
        printf(" (hash=%#x)\n", gvn_hash(arr[i]));
    }
    #endif

    if (can_gvn(n)) {
        DO_IF(TB_OPTDEBUG_STATS)(f->stats.gvn_tries++);
        k = nl_hashset_put2(&f->gvn_nodes, n, gvn_hash, gvn_compare);
        if (k && (k != n)) {
            DO_IF(TB_OPTDEBUG_STATS)(f->stats.gvn_hit++);
            DO_IF(TB_OPTDEBUG_PEEP)(printf(" => \x1b[95mGVN v%u\x1b[0m\n", k->gvn));

            migrate_type(f, n, k);
            subsume_node(f, n, k);
            mark_users(f, k);
            return k;
        }
    }

    DO_IF(TB_OPTDEBUG_PEEP)(printf("\n"));
    return progress ? n : NULL;
}

TB_Node* tb_opt_peep_node(TB_Function* f, TB_Node* n) {
    TB_Node* k = peephole(f, n);
    return k ? k : n;
}

void tb_opt_dump_stats(TB_Function* f) {
    #if TB_OPTDEBUG_STATS
    int total_constants = f->stats.constants + f->stats.opto_constants;
    int final_count = f->node_count;
    double factor = ((double) final_count / (double) f->stats.initial) * 100.0;

    printf("%s: stats:\n", f->super.name);
    printf("  %4d   -> %4d nodes (%.2f%%)\n", f->stats.initial, final_count, factor);
    printf("  %4d GVN hit    %4d GVN attempts\n", f->stats.gvn_hit, f->stats.gvn_tries);
    printf("  %4d peepholes  %4d rewrites    %4d identities\n", f->stats.peeps, f->stats.rewrites, f->stats.identities);
    printf("  %4d constants  %4d of which were optimistic\n", total_constants, f->stats.opto_constants);

    #if TB_OPTDEBUG_PEEP || TB_OPTDEBUG_SCCP
    printf("  %4d ticks\n", f->stats.time);
    #endif
    #endif
}

void tb_pass_sroa(TB_Function* f) {
    CUIK_TIMED_BLOCK("sroa") {
        TB_Worklist* ws = f->worklist;
        int pointer_size = f->super.module->codegen->pointer_size;
        TB_Node* root = f->root_node;

        // write initial locals
        FOR_USERS(u, root) {
            if (USERN(u)->type == TB_LOCAL) { worklist_push(ws, USERN(u)); }
        }

        // i think the SROA'd pieces can't themselves split more? that should something we check
        size_t local_count = dyn_array_length(ws->items);
        for (size_t i = 0; i < local_count; i++) {
            assert(ws->items[i]->type == TB_LOCAL);
            sroa_rewrite(f, pointer_size, root, ws->items[i]);
        }
    }
}

static void push_non_bottoms(TB_Function* f, TB_Node* n) {
    // if it's a bottom there's no more steps it can take, don't recompute it
    Lattice* l = latuni_get(f, n);
    if (l != lattice_from_dt(f, n->dt)) { mark_node(f, n); }
}

static void tb_opt_cprop_node(TB_Function* f, TB_Node* n) {
    DO_IF(TB_OPTDEBUG_SCCP)(printf("TYPE t=%d? ", ++f->stats.time), tb_print_dumb_node(NULL, n));

    Lattice* old_type = latuni_get(f, n);
    Lattice* new_type = value_of(f, n);

    DO_IF(TB_OPTDEBUG_SCCP)(printf(" => \x1b[93m"), print_lattice(new_type), printf("\x1b[0m\n"));
    if (old_type != new_type) {
        #ifndef NDEBUG
        Lattice* glb = lattice_meet(f, old_type, new_type);
        if (glb != new_type) {
            TB_OPTDEBUG(PEEP)(printf("\n\nFORWARD PROGRESS ASSERT!\n"));
            TB_OPTDEBUG(PEEP)(printf("  "), print_lattice(old_type), printf("  =//=>  "), print_lattice(new_type), printf(", MEET: "), print_lattice(glb), printf("\n\n"));
            assert(0 && "forward progress assert!");
        }
        #endif

        latuni_set(f, n, new_type);

        // push affected users (handling one-input shit immediately)
        FOR_USERS(u, n) {
            TB_Node* un = USERN(u);
            if (un->input_count == 1) {
                tb_opt_cprop_node(f, un);
            } else {
                push_non_bottoms(f, un);
                if (cfg_is_region(un)) {
                    FOR_USERS(phi, un) if (USERN(phi)->type == TB_PHI) {
                        push_non_bottoms(f, USERN(phi));
                    }
                }
            }
        }
    }
}

void tb_opt_cprop(TB_Function* f) {
    assert(worklist_count(f->worklist) == 0);

    alloc_types(f);
    //   reset all types into TOP
    FOR_N(i, 0, f->node_count) { f->types[i] = &TOP_IN_THE_SKY; }
    //   anything unallocated should stay as NULL tho
    FOR_N(i, f->node_count, f->type_cap) { f->types[i] = NULL; }
    // except for ROOT
    worklist_push(f->worklist, f->root_node);

    // Pass 1: find constants.
    CUIK_TIMED_BLOCK("sccp") {
        TB_Node* n;
        while (n = worklist_pop(f->worklist), n) {
            tb_opt_cprop_node(f, n);
        }
    }

    // Pass 2: ok replace with constants now
    //   fills up the entire worklist again
    worklist_push(f->worklist, f->root_node);
    for (size_t i = 0; i < dyn_array_length(f->worklist->items); i++) {
        TB_Node* n = f->worklist->items[i];
        TB_Node* k = try_as_const(f, n, latuni_get(f, n));
        DO_IF(TB_OPTDEBUG_SCCP)(printf("CONST t=%d? ", ++f->stats.time), tb_print_dumb_node(NULL, n));
        if (k != NULL) {
            DO_IF(TB_OPTDEBUG_STATS)(f->stats.opto_constants++);
            DO_IF(TB_OPTDEBUG_SCCP)(printf(" => \x1b[96m"), tb_print_dumb_node(NULL, k), printf("\x1b[0m"));

            mark_users(f, k);
            mark_node(f, k);
            subsume_node(f, n, k);
            n = k;
        }
        DO_IF(TB_OPTDEBUG_SCCP)(printf("\n"));
        FOR_USERS(u, n) { mark_node(f, USERN(u)); }
    }
}

void tb_opt(TB_Function* f, TB_Worklist* ws, TB_Arena* ir, TB_Arena* tmp, bool preserve_types) {
    assert(f->root_node && "missing root node");
    f->arena     = ir;
    f->tmp_arena = tmp;
    f->worklist  = ws;

    TB_ArenaSavepoint sp = tb_arena_save(tmp);

    assert(worklist_count(ws) == 0);
    CUIK_TIMED_BLOCK("push_all_nodes") {
        // generate work list (put everything)
        worklist_test_n_set(ws, f->root_node);
        dyn_array_put(ws->items, f->root_node);

        for (size_t i = 0; i < dyn_array_length(ws->items); i++) {
            TB_Node* n = ws->items[i];
            FOR_USERS(u, n) { worklist_push(ws, USERN(u)); }
        }

        // just leads to getting to the important bits first in practice (RPO would be better but
        // more work to perform)
        CUIK_TIMED_BLOCK("reversing") {
            size_t last = dyn_array_length(ws->items) - 1;
            FOR_N(i, 0, dyn_array_length(ws->items) / 2) {
                SWAP(TB_Node*, ws->items[i], ws->items[last - i]);
            }
        }

        TB_OPTDEBUG(STATS)(f->stats.initial = worklist_count(ws));
    }
    TB_OPTDEBUG(PEEP)(log_debug("%s: pushed %d nodes (out of %d)", f->super.name, worklist_count(f->worklist), f->node_count));

    f->invalidated_loops = true;
    f->node2loop = nl_table_alloc(20);

    TB_OPTDEBUG(PASSES)(printf("FUNCTION %s:\n", f->super.name));

    int k;
    int rounds = 0;
    while (worklist_count(f->worklist) > 0) {
        TB_OPTDEBUG(PASSES)(printf("  * ROUND %d:\n", ++rounds));
        TB_OPTDEBUG(PASSES)(printf("    * Minor rewrites\n"));

        tb_verify(f, f->tmp_arena);

        // minor opts
        while (worklist_count(f->worklist) > 0) {
            TB_OPTDEBUG(PASSES)(printf("      * Peeps (%d nodes)\n", worklist_count(f->worklist)));
            // combined pessimistic solver
            if (k = tb_opt_peeps(f), k > 0) {
                TB_OPTDEBUG(PASSES)(printf("        * Rewrote %d nodes\n", k));
            }

            // locals scans the TB_LOCAL nodes, it might introduce peephole
            // work when it returns true.
            TB_OPTDEBUG(PASSES)(printf("      * Locals\n"));
            if (k = tb_opt_locals(f), k > 0) {
                TB_OPTDEBUG(PASSES)(printf("        * Folded %d locals into SSA\n", k));
            }
        }

        tb_verify(f, f->tmp_arena);

        // const prop leaves work for the peephole optimizer and
        // sometimes might invalidate the loop tree so we should
        // track when it makes CFG changes.
        TB_OPTDEBUG(PASSES)(printf("    * Optimistic solver\n"));
        tb_opt_cprop(f);

        TB_OPTDEBUG(PASSES)(printf("      * Peeps (%d nodes)\n", worklist_count(f->worklist)));
        if (k = tb_opt_peeps(f), k > 0) {
            TB_OPTDEBUG(PASSES)(printf("        * Rewrote %d nodes\n", k));
        }

        // only wanna build a loop tree if there's
        // major changes to the CFG, most rounds of peeps
        // wouldn't invalidate it.
        if (f->invalidated_loops) {
            nl_table_clear(&f->node2loop);
            f->invalidated_loops = false;

            tb_verify(f, f->tmp_arena);

            TB_OPTDEBUG(PASSES)(printf("    * Update loop tree\n"));
            tb_opt_build_loop_tree(f);

            tb_verify(f, f->tmp_arena);
        }

        // mostly just detecting loops and upcasting indvars
        TB_OPTDEBUG(PASSES)(printf("    * Loops\n"));
        tb_opt_loops(f);
    }

    tb_print(f, f->tmp_arena);

    nl_table_free(f->node2loop);
    // if we're doing IPO then it's helpful to keep these
    if (!preserve_types) {
        tb_opt_free_types(f);
    }
    // avoids bloating up my arenas with freed nodes
    tb_renumber_nodes(f, ws);

    #ifdef TB_OPTDEBUG_STATS
    tb_opt_dump_stats(f);
    #endif

    tb_arena_restore(tmp, sp);
    f->worklist = NULL;
}

TB_API TB_Worklist* tb_worklist_alloc(void) {
    TB_Worklist* ws = tb_platform_heap_alloc(sizeof(TB_Worklist));
    worklist_alloc(ws, 500);
    return ws;
}

TB_API void tb_worklist_free(TB_Worklist* ws) {
    worklist_free(ws);
    tb_platform_heap_free(ws);
}

static bool alloc_types(TB_Function* f) {
    if (f->types != NULL) { return false; }

    CUIK_TIMED_BLOCK("allocate type array") {
        size_t count = (f->node_count + 63ull) & ~63ull;
        f->type_cap = count;
        f->types = tb_platform_heap_alloc(count * sizeof(Lattice*));
        // when latuni_get sees a NULL, it'll replace it with the correct bottom type
        FOR_N(i, 0, count) { f->types[i] = NULL; }

        // place ROOT type
        f->root_mem = lattice_alias(f, 0);
        f->alias_n  = 1;
    }
    return true;
}

TB_API void tb_opt_free_types(TB_Function* f) {
    if (f->types != NULL) {
        tb_platform_heap_free(f->types);
        f->types = NULL;
    }
}

int tb_opt_peeps(TB_Function* f) {
    if (alloc_types(f)) {
        FOR_N(i, 0, dyn_array_length(f->worklist->items)) {
            TB_Node* n = f->worklist->items[i];
            f->types[n->gvn] = lattice_from_dt(f, n->dt);
        }
        f->types[f->root_node->gvn] = lattice_tuple_from_node(f, f->root_node);
    }

    int changes = 0;
    CUIK_TIMED_BLOCK("peephole") {
        TB_Node* n;
        while ((n = worklist_pop(f->worklist))) {

            // must've dead sometime between getting scheduled and getting here.
            if (!is_proj(n) && n->user_count == 0) {
                DO_IF(TB_OPTDEBUG_STATS)(f->stats.peeps++);
                DO_IF(TB_OPTDEBUG_PEEP)(printf("PEEP t=%d? ", ++f->stats.time), tb_print_dumb_node(NULL, n));
                DO_IF(TB_OPTDEBUG_PEEP)(printf(" => \x1b[196mKILL\x1b[0m\n"));
                tb_kill_node(f, n);
            } else if (n->type != TB_NULL && peephole(f, n)) {
                changes += 1;
            }
        }
    }

    return changes;
}

typedef struct {
    bool on_stack;
    int index, low_link;
} SCCNode;

typedef struct {
    TB_Arena* arena;
    size_t fn_count;
    NL_Table nodes;

    size_t stk_cnt;
    TB_Function** stk;

    int index;
} SCC;

static TB_Function* static_call_site(TB_Node* n) {
    // is this call site a static function call
    assert(n->type == TB_CALL || n->type == TB_TAILCALL);
    if (n->inputs[2]->type != TB_SYMBOL) return NULL;

    TB_Symbol* target = TB_NODE_GET_EXTRA_T(n->inputs[2], TB_NodeSymbol)->sym;
    if (atomic_load_explicit(&target->tag, memory_order_relaxed) != TB_SYMBOL_FUNCTION) return NULL;

    return (TB_Function*) target;
}

static SCCNode* scc_walk(SCC* restrict scc, IPOSolver* ipo, TB_Function* f) {
    SCCNode* n = tb_arena_alloc(scc->arena, sizeof(SCCNode));
    n->index = scc->index;
    n->low_link = scc->index;
    n->on_stack = true;
    scc->index += 1;
    nl_table_put(&scc->nodes, f, n);

    scc->stk[scc->stk_cnt++] = f;

    // consider the successors
    TB_Node* callgraph = f->root_node->inputs[0];
    assert(callgraph->type == TB_CALLGRAPH);
    FOR_N(i, 1, callgraph->input_count) {
        TB_Node* call = callgraph->inputs[i];
        TB_Function* target = static_call_site(call);
        if (target != NULL) {
            SCCNode* succ = nl_table_get(&scc->nodes, target);
            if (succ == NULL) {
                succ = scc_walk(scc, ipo, target);
                if (n->low_link > succ->low_link) { n->low_link = succ->low_link; }
            } else if (succ->on_stack) {
                if (n->low_link > succ->index) { n->low_link = succ->index; }
            }
        }
    }

    // we're the root, construct an SCC
    if (n->low_link == n->index) {
        TB_Function* kid_f;
        do {
            assert(scc->stk_cnt > 0);
            kid_f = scc->stk[--scc->stk_cnt];

            SCCNode* kid_n = nl_table_get(&scc->nodes, kid_f);
            kid_n->on_stack = false;
            ipo->ws[ipo->ws_cnt++] = kid_f;
        } while (kid_f != f);
    }

    return n;
}

static void inline_into(TB_Arena* arena, TB_Function* f, TB_Node* call_site, TB_Function* kid);
bool tb_module_ipo(TB_Module* m) {
    // fill initial worklist with all external function calls :)
    //
    // two main things we wanna know are if something is alive and when to inline (eventually
    // we can incorporate IPSCCP)
    SCC scc = { 0 };
    scc.arena    = get_temporary_arena(m);
    scc.fn_count = m->symbol_count[TB_SYMBOL_FUNCTION];

    IPOSolver ipo = { 0 };
    ipo.ws_cap = scc.fn_count;
    ipo.ws = tb_arena_alloc(scc.arena, scc.fn_count * sizeof(TB_Function*));

    CUIK_TIMED_BLOCK("build SCC") {
        TB_ArenaSavepoint sp = tb_arena_save(scc.arena);
        scc.stk      = tb_arena_alloc(scc.arena, scc.fn_count * sizeof(TB_Function*));
        scc.nodes    = nl_table_arena_alloc(scc.arena, scc.fn_count);

        // build strongly connected components
        TB_ThreadInfo* info = atomic_load_explicit(&m->first_info_in_module, memory_order_relaxed);
        while (info != NULL) {
            TB_Symbol** syms = (TB_Symbol**) info->symbols.data;
            if (syms != NULL) {
                FOR_N(i, 0, 1ull << info->symbols.exp) {
                    TB_Symbol* s = syms[i];
                    if (s == NULL || s == NL_HASHSET_TOMB) continue;
                    if (atomic_load_explicit(&s->tag, memory_order_relaxed) != TB_SYMBOL_FUNCTION) continue;

                    if (nl_table_get(&scc.nodes, s) == NULL) {
                        scc_walk(&scc, &ipo, (TB_Function*) s);
                    }
                }
            }
            info = info->next_in_module;
        }
        tb_arena_restore(scc.arena, sp);
    }

    // we've got our bottom up ordering on the worklist... start trying to inline callsites
    bool progress = false;

    TB_OPTDEBUG(INLINE)(printf("BOTTOM-UP ORDER:\n"));
    FOR_N(i, 0, ipo.ws_cnt) {
        TB_Function* f = ipo.ws[i];

        TB_OPTDEBUG(INLINE)(printf("* FUNCTION: %s\n", f->super.name));

        TB_Node* callgraph = f->root_node->inputs[0];
        assert(callgraph->type == TB_CALLGRAPH);

        size_t i = 1;
        while (i < callgraph->input_count) {
            TB_Node* call = callgraph->inputs[i];
            TB_Function* target = static_call_site(call);

            // really simple getter/setter kind of heuristic
            if (target && target->node_count < 15) {
                TB_OPTDEBUG(INLINE)(printf("  -> %s (from v%u)\n", target->super.name, call->gvn));
                inline_into(scc.arena, f, call, target);
                progress = true;
            } else {
                i++;
            }
        }
    }

    return progress;
}

static TB_Node* inline_clone_node(TB_Function* f, TB_Node* call_site, TB_Node** clones, TB_Node* n) {
    // special cases
    if (n->type == TB_PROJ && n->inputs[0]->type == TB_ROOT) {
        // this is a parameter, just hook it directly to the inputs of
        // the callsite.
        //
        // 0:ctrl, 1:mem, 2:rpc, 3... params
        int index = TB_NODE_GET_EXTRA_T(n, TB_NodeProj)->index;
        clones[n->gvn] = call_site->inputs[index];

        assert(clones[n->gvn]);
        return clones[n->gvn];
    } else if (clones[n->gvn] != NULL) {
        return clones[n->gvn];
    }

    size_t extra = extra_bytes(n);
    TB_Node* cloned = tb_alloc_node(f, n->type, n->dt, n->input_count, extra);

    // clone extra data (i hope it's that easy lol)
    memcpy(cloned->extra, n->extra, extra);
    clones[n->gvn] = cloned;

    // fill cloned edges
    FOR_N(i, 0, n->input_count) if (n->inputs[i]) {
        TB_Node* in = inline_clone_node(f, call_site, clones, n->inputs[i]);

        cloned->inputs[i] = in;
        add_user(f, cloned, in, i);
    }

    #if TB_OPTDEBUG_INLINE
    printf("CLONE "), tb_print_dumb_node(NULL, n), printf(" => "), tb_print_dumb_node(NULL, cloned), printf("\n");
    #endif

    return cloned;

    /* TB_Node* k = tb__gvn(f, cloned, extra);
    if (k != cloned) {
        #if TB_OPTDEBUG_INLINE
        printf(" => GVN");
        #endif
    }
    printf("\n");
    return  = cloned; */
}

static void inline_into(TB_Arena* arena, TB_Function* f, TB_Node* call_site, TB_Function* kid) {
    TB_ArenaSavepoint sp = tb_arena_save(arena);
    TB_Node** clones = tb_arena_alloc(arena, kid->node_count * sizeof(TB_Node*));
    memset(clones, 0, kid->node_count * sizeof(TB_Node*));

    // find all nodes
    TB_Worklist ws = { 0 };
    worklist_alloc(&ws, kid->node_count);
    {
        worklist_push(&ws, kid->root_node);
        for (size_t i = 0; i < dyn_array_length(ws.items); i++) {
            TB_Node* n = ws.items[i];
            FOR_USERS(u, n) { worklist_push(&ws, USERN(u)); }
        }
    }

    // clone all nodes in kid into f (GVN while we're at it)
    FOR_REV_N(i, 0, dyn_array_length(ws.items)) {
        inline_clone_node(f, call_site, clones, ws.items[i]);
    }
    worklist_free(&ws);

    {
        // TODO(NeGate): region-ify the exit point
        TB_Node* kid_root = clones[kid->root_node->gvn];
        assert(kid_root->type == TB_ROOT);
        assert(kid_root->input_count == 2);

        TB_Node* ret = kid_root->inputs[1];
        assert(ret->type == TB_RETURN);

        for (size_t i = 0; i < call_site->user_count;) {
            TB_Node* un = USERN(&call_site->users[i]);
            if (is_proj(un)) {
                int index = TB_NODE_GET_EXTRA_T(un, TB_NodeProj)->index;
                if (index >= 2) { index += 1; }

                subsume_node(f, un, ret->inputs[index]);
            } else {
                i += 1;
            }
        }

        subsume_node(f, kid_root, f->root_node);
        tb_kill_node(f, call_site);
    }

    // kill edge in callgraph
    TB_Node* callgraph = f->root_node->inputs[0];
    assert(callgraph->type == TB_CALLGRAPH);

    FOR_N(i, 1, callgraph->input_count) {
        if (callgraph->inputs[i] == call_site) {
            set_input(f, callgraph, callgraph->inputs[callgraph->input_count - 1], i);
            set_input(f, callgraph, NULL, callgraph->input_count - 1);
            callgraph->input_count--;
            break;
        }
    }

    // append all callee callgraph edges to caller
    TB_Node* kid_callgraph = clones[kid->root_node->inputs[0]->gvn];
    FOR_N(i, 1, kid_callgraph->input_count) {
        add_input_late(f, callgraph, kid_callgraph->inputs[i]);
    }
    tb_kill_node(f, kid_callgraph);
    tb_arena_restore(arena, sp);
}

