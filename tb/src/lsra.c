// Linear scan register allocator:
//   https://ssw.jku.at/Research/Papers/Wimmer04Master/Wimmer04Master.pdf
#include "codegen.h"

#define FOREACH_SET(it, set) \
FOR_N(_i, 0, ((set).capacity + 63) / 64) FOR_BIT(it, _i*64, (set).data[_i])

typedef struct {
    int id;
    RegMask* mask;
} Spill;

typedef struct {
    Ctx* ctx;
    TB_Arena* arena;

    int num_classes;
    size_t stack_slots;
    int* num_regs;
    uint64_t* callee_saved;

    int* free_until;
    int* block_pos;
    int* fixed;

    ArenaArray(Spill) spills; // [vreg]

    // Row numbers per node
    ArenaArray(int) time;   // [gvn]

    // waiting to get registers, sorted such that the top most item is the youngest
    DynArray(int) unhandled;
    DynArray(int) inactive;
    RegMask* normie_mask;

    Set active_set[MAX_REG_CLASSES];
    int* active[MAX_REG_CLASSES];
} LSRA;

#define BND(arr, i, limit) ((i) >= (limit) ? abort() : 0, arr)[i]

static Range NULL_RANGE = { .start = INT_MAX, .end = INT_MAX };

// Forward decls... yay
static void move_to_active(LSRA* restrict ra, VReg* vreg, int vreg_id);
static void update_intervals(Ctx* restrict ctx, LSRA* restrict ra, int time);
static void cuiksort_defs(Ctx* ctx, int* intervals, ptrdiff_t lo, ptrdiff_t hi);
static bool update_interval(Ctx* restrict ctx, LSRA* restrict ra, int vreg_id, bool is_active, int time, int inactive_index);
static void split_intersecting(Ctx* restrict ctx, LSRA* restrict ra, int pos, VReg* vreg, RegMask* new_mask);
static void spill_entire_life(Ctx* restrict ctx, LSRA* restrict ra, VReg* vreg, RegMask* new_mask);
static int allocate_free_reg(Ctx* restrict ctx, LSRA* restrict ra, VReg* vreg, int vreg_id);

// Helpers
static bool reg_mask_may_intersect(RegMask* a, RegMask* b) {
    if (a == b) {
        return true;
    } else if (a->class == REG_CLASS_STK) {
        return b->may_spill || (b->class == REG_CLASS_STK && b->mask[0] == 0);
    } else if (b->class == REG_CLASS_STK) {
        return a->may_spill || (a->class == REG_CLASS_STK && a->mask[0] == 0);
    } else if (a->class != b->class) {
        return false;
    }

    assert(a->count == b->count);
    FOR_N(i, 0, a->count) {
        if ((a->mask[i] & b->mask[i]) != 0) {
            return true;
        }
    }

    return false;
}

static int vreg_class(VReg* l) { return l->mask->class; }
static int vreg_start(Ctx* ctx, int id) { return ctx->vregs[id].active_range->start; }

static bool vreg_is_fixed(Ctx* restrict ctx, LSRA* restrict ra, int id) {
    int class = ctx->vregs[id].mask->class;
    uint32_t base = id - ra->fixed[class];
    return base < ctx->num_regs[class];
}

// static const char* GPR_NAMES[] = { "X0", "X1", "X2", "X3", "X4", "X5", "X6", "X7", "X8",  "X9", "X10", "X11", "X12", "X13", "X14", "X15" };
static const char* GPR_NAMES[] = { "RAX", "RCX", "RDX", "RBX", "RSP", "RBP", "RSI", "RDI", "R8",  "R9", "R10", "R11", "R12", "R13", "R14", "R15" };
static void print_reg_name(int rg, int num) {
    if (rg == 1) {
        printf("FLAGS");
    } else if (rg == 2) {
        // printf("R%d", num);
        printf("%s", GPR_NAMES[num]);
    } else if (rg == 3) {
        printf("XMM%d", num);
    } else if (rg == REG_CLASS_STK) {
        printf("[sp + %d]", num*8);
    } else {
        tb_todo();
    }
}

static void add_range(LSRA* restrict ra, VReg* vreg, int start, int end) {
    assert(start <= end);
    // printf("V%lld: add_range(%d, %d)\n", vreg - ra->ctx->vregs, start, end);

    Range* top = vreg->active_range;
    if (top == NULL) { vreg->active_range = top = &NULL_RANGE; }

    assert(top && "should've already placed the INT_MAX range");
    if (top->start <= end) {
        // coalesce
        top->start = TB_MIN(top->start, start);
        top->end   = TB_MAX(top->end,   end);
    } else {
        Range* rg = tb_arena_alloc(ra->arena, sizeof(Range));
        rg->next  = top;
        rg->start = start;
        rg->end   = end;
        vreg->active_range = rg;

        // end range will be the first range added past the NULL_RANGE
        if (top == &NULL_RANGE) { vreg->end_time = end; }
    }
}

static void dump_sched(Ctx* restrict ctx, LSRA* restrict ra) {
    FOR_N(i, 0, ctx->bb_count) {
        MachineBB* mbb = &ctx->machine_bbs[i];
        printf("BB %zu:\n", i);
        aarray_for(i, mbb->items) {
            printf("  T%d:  ", ra->time[mbb->items[i]->gvn]);
            tb_print_dumb_node(NULL, mbb->items[i]);
            printf("\n");
        }
    }
}

void tb__lsra(Ctx* restrict ctx, TB_Arena* arena) {
    LSRA ra = { .ctx = ctx, .arena = arena };
    TB_Function* f = ctx->f;
    size_t node_count = f->node_count;

    // creating fixed vregs which coalesce all fixed reg uses
    // so i can more easily tell when things are asking for them.
    int max_regs_in_class = 0;
    CUIK_TIMED_BLOCK("pre-pass on fixed intervals") {
        ra.fixed    = tb_arena_alloc(arena, ctx->num_classes * sizeof(int));

        FOR_N(i, 0, ctx->num_classes) {
            size_t count = ctx->num_regs[i];
            if (max_regs_in_class < count) {
                max_regs_in_class = count;
            }

            int base = aarray_length(ctx->vregs);
            FOR_N(j, 0, count) {
                RegMask* mask = intern_regmask(ctx, i, false, 1ull << j);
                aarray_push(ctx->vregs, (VReg){
                        .class = i,
                        .assigned = j,
                        .mask = mask,
                        .active_range = &NULL_RANGE
                    });
            }

            ra.fixed[i] = base;
            ra.active_set[i] = set_create_in_arena(arena, count);
            ra.active[i] = tb_arena_alloc(arena, count * sizeof(int));
            memset(ra.active[i], 0, count * sizeof(int));
        }

        // only need enough to store for the biggest register class
        ra.free_until = tb_arena_alloc(arena, max_regs_in_class * sizeof(int));
        ra.block_pos  = tb_arena_alloc(arena, max_regs_in_class * sizeof(int));
        ra.num_regs   = ctx->num_regs;
    }

    // probably gonna throw into the arena later but the important bit is that
    // new nodes inherit liveness from some other node.
    //
    // new GVN -> old GVN
    NL_Table fwd_table = nl_table_alloc(32);

    // create timeline & insert moves
    CUIK_TIMED_BLOCK("insert legalizing moves") {
        ra.time = aarray_create(arena, int, tb_next_pow2(node_count));
        ra.time[0] = 4;

        int timeline = 4;
        FOR_N(i, 0, ctx->bb_count) {
            MachineBB* mbb = &ctx->machine_bbs[i];
            mbb->start_t = timeline;

            size_t j = 0; // we do insert things while iterating
            for (; j < aarray_length(mbb->items); j++) {
                TB_Node* n = mbb->items[j];
                int tmp_count = ctx->tmp_count(ctx, n);

                // printf("  "), tb_print_dumb_node(NULL, n), printf("\n");

                RegMask** ins = ctx->ins;
                ctx->constraint(ctx, n, ins);

                // insert input copies (temporaries & clobbers never introduce
                // these so we're safe don't check those)
                size_t in_count = n->input_count;
                FOR_N(k, 1, in_count) if (n->inputs[k]) {
                    TB_Node* in = n->inputs[k];
                    RegMask* in_mask = ins[k];
                    if (in_mask == &TB_REG_EMPTY) continue;

                    VReg* in_def = node_vreg(ctx, in);
                    int hint = fixed_reg_mask(in_mask);
                    if (hint >= 0) { in_def->hint_vreg = ra.fixed[in_mask->class] + hint; }

                    // we resolve def-use conflicts with a spill move, either when:
                    //   * the use and def classes don't match.
                    //   * the use mask is more constrained than the def.
                    //   * it's on both ends to avoid stretching fixed intervals.
                    bool both_fixed = hint >= 0 && reg_mask_eq(in_def->mask, in_mask);
                    if (tb__reg_mask_less(ctx, in_def->mask, in_mask) || both_fixed) {
                        RegMask* in_def_mask = in_def->mask;
                        if (both_fixed) {
                            in_def_mask = ctx->normie_mask[in_def->mask->class];
                        }

                        // unless we're writing to a stack slot, we can basically always
                        // do the spill move as a load.
                        if (in_def_mask->class != REG_CLASS_STK) {
                            in_def_mask = intern_regmask(ctx, in_def->mask->class, true, in_def->mask->mask[0]);
                        }

                        TB_OPTDEBUG(REGALLOC)(printf("  TEMP "), tb__print_regmask(in_def_mask), printf(" -> "), tb__print_regmask(in_mask), printf("\n"));

                        // construct copy (either to a fixed interval or a new masked interval)
                        TB_Node* tmp = tb_alloc_node(f, TB_MACH_COPY, in->dt, 2, sizeof(TB_NodeMachCopy));
                        set_input(f, tmp, in, 1);
                        set_input(f, n, tmp,  k);
                        TB_NODE_SET_EXTRA(tmp, TB_NodeMachCopy, .def = in_mask, .use = in_def_mask);

                        // schedule the split right before use
                        tb__insert_before(ctx, f, tmp, n);
                        if (hint >= 0) {
                            int fixed_vreg = ra.fixed[in_mask->class] + hint;
                            aarray_insert(ctx->vreg_map, tmp->gvn, fixed_vreg);
                        } else {
                            VReg* tmp_vreg = tb__set_node_vreg(ctx, tmp);
                            tmp_vreg->mask = in_mask;
                            tmp_vreg->active_range = &NULL_RANGE;
                        }

                        aarray_insert(ra.time, tmp->gvn, timeline);
                        timeline += 2;
                        j += 1;
                    }
                }

                int vreg_id = ctx->vreg_map[n->gvn];
                if (tmp_count > 0) {
                    // used for clobbers/scratch but more importantly they're not bound to a node.
                    Tmps* tmps  = tb_arena_alloc(arena, sizeof(Tmps) + tmp_count*sizeof(int));
                    tmps->count = tmp_count;
                    nl_table_put(&ctx->tmps_map, n, tmps);

                    FOR_N(k, in_count, in_count + tmp_count) {
                        RegMask* in_mask = ins[k];
                        assert(in_mask != &TB_REG_EMPTY);

                        int fixed = fixed_reg_mask(in_mask);
                        if (fixed >= 0) {
                            // insert new range to the existing vreg
                            tmps->elems[k - in_count] = ra.fixed[in_mask->class] + fixed;
                        } else {
                            tmps->elems[k - in_count] = aarray_length(ctx->vregs);
                            aarray_push(ctx->vregs, (VReg){ .n = n, .mask = in_mask, .assigned = -1 });
                        }
                    }
                }

                int shared_edge = ctx->node_2addr(n);
                if (n->type == TB_PROJ || n->type == TB_MACH_PROJ) {
                    // projections share time with their tuple node
                    int tup_time = ra.time[n->inputs[0]->gvn];
                    aarray_insert(ra.time, n->gvn, tup_time);
                } else {
                    // place on timeline
                    aarray_insert(ra.time, n->gvn, timeline);
                    timeline += shared_edge >= 0 ? 4 : 2;
                }
                // printf("  T=%d\n", ra.time[n->gvn]);

                if (vreg_id > 0) {
                    VReg* vreg = &ctx->vregs[vreg_id];
                    RegMask* def_mask = vreg->mask;
                    vreg->active_range = &NULL_RANGE;

                    #if 0
                    // if we're writing to a fixed interval, insert copy
                    // such that we only guarentee a fixed location at the
                    // def site.
                    int reg = fixed_reg_mask(def_mask);
                    if (reg >= 0 && n->type != TB_MACH_COPY) {
                        int fixed_vreg = ra.fixed[def_mask->class] + reg;
                        aarray_insert(ctx->vreg_map, n->gvn, fixed_vreg);

                        // construct copy (either to a fixed interval or a new masked interval)
                        TB_Node* tmp = tb_alloc_node(f, TB_MACH_COPY, n->dt, 2, sizeof(TB_NodeMachCopy));
                        subsume_node2(f, n, tmp);
                        set_input(f, tmp, n, 1);
                        TB_NODE_SET_EXTRA(tmp, TB_NodeMachCopy, .def = ctx->normie_mask[def_mask->class], .use = def_mask);

                        // schedule the split right after def
                        tb__insert_after(ctx, f, tmp, n);
                        VReg* tmp_vreg = tb__set_node_vreg(ctx, tmp);
                        tmp_vreg->mask = ctx->normie_mask[def_mask->class];
                        tmp_vreg->active_range = &NULL_RANGE;

                        nl_table_put(&fwd_table, (void*) (uintptr_t) n->gvn, (void*) (uintptr_t) tmp->gvn);
                        aarray_insert(ra.time, tmp->gvn, timeline);
                        timeline += 2;
                        j += 1;
                    }
                    #endif
                }
            }

            mbb->end_t = timeline;
            timeline += 4;
        }
    }

    // build intervals from dataflow
    CUIK_TIMED_BLOCK("build intervals") {
        FOR_REV_N(i, 0, ctx->bb_count) {
            MachineBB* mbb = &ctx->machine_bbs[i];
            int bb_start   = mbb->start_t;
            int bb_end     = mbb->end_t + 2;

            // live outs define a full range across the BB (if they're defined
            // in the block, the later reverse walk will fix that up)
            TB_BasicBlock* bb = f->scheduled[mbb->n->gvn];
            Set* live_out = &bb->live_out;
            FOR_N(j, 0, (node_count + 63) / 64) {
                uint64_t bits = live_out->data[j];
                if (bits == 0) continue;
                FOR_N(k, 0, 64) if (bits & (1ull << k)) {
                    uintptr_t fwd = (uintptr_t) nl_table_get(&fwd_table, (void*) (j*64 + k));
                    uintptr_t gvn = fwd ? fwd : j*64 + k;

                    int vreg_id = ctx->vreg_map[gvn];
                    if (vreg_id > 0) {
                        assert(ctx->vregs[vreg_id].assigned < 0);
                        add_range(&ra, &ctx->vregs[vreg_id], bb_start, bb_end);
                    }
                }
            }

            size_t item_count = aarray_length(mbb->items);
            FOR_REV_N(j, 0, item_count) {
                TB_Node* n = mbb->items[j];

                int vreg_id = ctx->vreg_map[n->gvn];
                int time = ra.time[n->gvn];
                if (vreg_id > 0) {
                    assert(time > 0);
                    VReg* vreg = &ctx->vregs[vreg_id];

                    // mark output
                    RegMask* def_mask = vreg->mask;
                    if (def_mask != &TB_REG_EMPTY) {
                        // fixed regs & phi moves are the only ones which get coalesced
                        // so we don't place them here to avoid duplicates in the list.
                        if (vreg->assigned < 0 && n->type != TB_MACH_MOVE) {
                            dyn_array_put(ra.unhandled, vreg_id);
                        }

                        if (vreg->active_range->next == NULL) {
                            add_range(&ra, vreg, time, time);
                        } else {
                            vreg->active_range->start = time;
                        }
                    }

                    if (ctx->flags(ctx, n)) {
                        int reg = ra.fixed[1]; // we assume FLAGS is in class[1]
                        add_range(&ra, &ctx->vregs[reg], time, time + 1);
                    }
                }

                Tmps* tmps = nl_table_get(&ctx->tmps_map, n);
                if (tmps) FOR_N(k, 0, tmps->count) {
                    add_range(&ra, &ctx->vregs[tmps->elems[k]], time, time + 1);
                }

                // 2 address ops will interfere with their own inputs (except for
                // shared dst/src), it'll be -1 if there's no shared edge.
                int shared_edge = ctx->node_2addr(n);

                // mark inputs (unless it's a phi, the edges are eval'd on other basic blocks)
                if (n->type != TB_PHI) {
                    RegMask** ins = ctx->ins;
                    ctx->constraint(ctx, n, ins);

                    FOR_N(k, 1, n->input_count) if (n->inputs[k]) {
                        TB_Node* in = n->inputs[k];
                        RegMask* in_mask = ins[k];
                        if (in_mask == &TB_REG_EMPTY) continue;

                        int use_time = time;
                        VReg* in_def = node_vreg(ctx, in);
                        if (shared_edge >= 0) {
                            if (k != shared_edge) { use_time += 2; } // extend
                            else { ctx->vregs[vreg_id].hint_vreg = in_def - ctx->vregs; } // try to coalesce
                        }

                        add_range(&ra, in_def, bb_start, use_time);
                        // add_use_pos(&ra, in_def, use_time, in_mask->may_spill);
                    }
                }
            }
        }
    }

    CUIK_TIMED_BLOCK("post-pass on fixed intervals") {
        FOR_N(i, 0, ctx->num_classes) {
            // add range at beginning such that all fixed intervals are "awake"
            FOR_N(j, 0, ctx->num_regs[i]) {
                add_range(&ra, &ctx->vregs[ra.fixed[i]+j], 0, 1);
                dyn_array_put(ra.unhandled, ra.fixed[i]+j);
            }
        }
    }

    // sort intervals
    CUIK_TIMED_BLOCK("sort intervals") {
        cuiksort_defs(ctx, ra.unhandled, 0, dyn_array_length(ra.unhandled) - 1);
    }

    int total_spills = 0;
    CUIK_TIMED_BLOCK("linear scan") {
        ra.spills = aarray_create(arena, Spill, 100);

        int rounds = 0;
        for (;;) {
            total_spills = 0;

            rounds += 1;
            TB_OPTDEBUG(REGALLOC)(printf("  ###############################\n"));
            TB_OPTDEBUG(REGALLOC)(printf("  #  ROUND %-4d                 #\n", rounds));
            TB_OPTDEBUG(REGALLOC)(printf("  ###############################\n"));

            // run linear scan all the way through, we'll accumulate things to
            // split and handle them in bulk
            cuikperf_region_start("linear scan", NULL);

            int unhandled_i = dyn_array_length(ra.unhandled);
            while (unhandled_i--) {
                int vreg_id = ra.unhandled[unhandled_i];
                VReg* vreg  = &ctx->vregs[vreg_id];

                int start = vreg->active_range->start;
                int end   = vreg->end_time;
                assert(start != INT_MAX);

                #if TB_OPTDEBUG_REGALLOC
                printf("  # V%-4d t=[%-4d - %4d) ", vreg_id, start, end);
                tb__print_regmask(vreg->mask);
                printf("    ");
                if (vreg->n) { tb_print_dumb_node(NULL, vreg->n); }
                printf("\n");
                #endif

                if (vreg->saved_range == NULL) { vreg->saved_range = vreg->active_range; }
                update_intervals(ctx, &ra, start);

                int reg = vreg->assigned;
                if (reg >= 0) {
                    move_to_active(&ra, vreg, vreg_id);
                } else {
                    if (reg_mask_is_not_empty(vreg->mask)) {
                        // allocate free register
                        reg = allocate_free_reg(ctx, &ra, vreg, vreg_id);
                        vreg = &ctx->vregs[vreg_id];

                        // add to active set
                        if (reg >= 0) {
                            vreg->class = vreg->mask->class;
                            vreg->assigned = reg;
                            move_to_active(&ra, vreg, vreg_id);
                        }
                    } else if (vreg->mask->may_spill) {
                        // allocate stack slo t
                        TB_OPTDEBUG(REGALLOC)(printf("  #   assign to [BP - %d]\n", 8 + total_spills*8));
                        vreg->class    = REG_CLASS_STK;
                        vreg->assigned = STACK_BASE_REG_NAMES + total_spills++;
                    } else {
                        tb_todo();
                    }
                }

                // display active set
                #if TB_OPTDEBUG_REGALLOC
                static const char* classes[] = { "STK", "FLAGS", "GPR", "VEC" };
                FOR_N(rc, 1, ctx->num_classes) {
                    printf("  \x1b[32m%s { ", classes[rc]);
                    FOREACH_SET(reg, ra.active_set[rc]) {
                        int other_id = ra.active[rc][reg];
                        printf("V%d:", other_id);
                        print_reg_name(rc, reg);
                        printf(" ");
                    }
                    printf("}\x1b[0m\n");
                }
                #endif
            }
            cuikperf_region_end();

            size_t spills_accum = aarray_length(ra.spills);
            if (spills_accum == 0) { break; }

            cuikperf_region_start("alloc fail", NULL);
            // alloc failure? spill/split
            FOR_N(i, 0, spills_accum) {
                VReg* vreg = &ctx->vregs[ra.spills[i].id];
                spill_entire_life(ctx, &ra, vreg, ra.spills[i].mask);
            }

            // undo all non-fixed regs
            FOR_N(i, 0, dyn_array_length(ra.unhandled)) {
                int vreg_id = ra.unhandled[i];
                VReg* vreg  = &ctx->vregs[vreg_id];
                if (!vreg_is_fixed(ctx, &ra, vreg_id)) {
                    vreg->class    = vreg->mask->class;
                    vreg->assigned = -1;
                }

                if (vreg->saved_range) {
                    vreg->active_range = vreg->saved_range;
                }
            }

            FOR_N(rc, 1, ctx->num_classes) {
                set_clear(&ra.active_set[rc]);
            }
            dyn_array_clear(ra.inactive);
            aarray_clear(ra.spills);
            cuikperf_region_end();
        }

        ctx->num_spills = total_spills;
    }

    // dump_sched(ctx);
}

////////////////////////////////
// Allocating new registers
////////////////////////////////
static int range_intersect(Range* a, Range* b) {
    if (b->start <= a->end && a->start <= b->end) {
        return a->start > b->start ? a->start : b->start;
    } else {
        return -1;
    }
}

static int vreg_intersect(VReg* a, VReg* b) {
    for (Range* ar = a->active_range; ar != &NULL_RANGE; ar = ar->next) {
        for (Range* br = b->active_range; br != &NULL_RANGE; br = br->next) {
            int t = range_intersect(ar, br);
            if (t >= 0) { return t; }
        }
    }

    return -1;
}

static void compute_free_until(Ctx* restrict ctx, LSRA* restrict ra, VReg* vreg) {
    RegMask* mask = vreg->mask;
    int class = mask->class;

    size_t reg_count = ra->num_regs[class];
    uint64_t word_count = (ra->num_regs[class] + 63) / 64;
    FOR_N(i, 0, word_count) {
        uint64_t in_use = ra->active_set[class].data[i];
        // don't care about non-intersecting free/blocked regs
        in_use |= ~mask->mask[i];

        size_t j = i*64, k = j + 64;
        for (; j < k && j < reg_count; j++) {
            ra->free_until[j] = (in_use & 1) ? 0 : INT_MAX;
            in_use >>= 1;
        }
    }

    // for each inactive which intersects current
    dyn_array_for(i, ra->inactive) {
        VReg* other = &ctx->vregs[ra->inactive[i]];
        int fp = ra->free_until[other->assigned];

        // if their regmasks don't intersect, we don't care
        if (fp > 0 && tb__reg_mask_meet(ctx, mask, other->mask) != &TB_REG_EMPTY) {
            int p = vreg_intersect(vreg, other);
            if (p >= 0 && p < fp) { ra->free_until[other->assigned] = p; }
        }
    }
}

// returns -1 if no registers are available
static int allocate_free_reg(Ctx* restrict ctx, LSRA* restrict ra, VReg* vreg, int vreg_id) {
    // let's figure out how long
    compute_free_until(ctx, ra, vreg);

    int highest = -1;

    // it's better in the long run to aggressively split based on hints
    int hint = vreg->hint_vreg > 0 ? ctx->vregs[vreg->hint_vreg].assigned : -1;
    if (hint >= 0 && vreg->end_time <= ra->free_until[hint]) { highest = hint; }

    // pick highest free pos
    int rc = vreg->mask->class;
    if (highest < 0) {
        highest = 0;
        FOR_N(i, 1, ra->num_regs[rc]) if (ra->free_until[i] > ra->free_until[highest]) {
            highest = i;
        }
    }

    int pos = ra->free_until[highest];
    if (UNLIKELY(pos == 0)) {
        int reg = -1;
        FOR_N(i, 0, ra->num_regs[rc]) {
            if (set_get(&ra->active_set[rc], i) && !vreg_is_fixed(ctx, ra, ra->active[rc][i])) {
                reg = i;
                break;
            }
        }
        assert(reg >= 0 && "no way they're all in fixed-use lmao");

        int active_id = ra->active[rc][reg];
        TB_OPTDEBUG(REGALLOC)(printf("  #   spill v%u\n", vreg_id));

        // alloc failure, split any
        VReg* active_user = &ctx->vregs[active_id];
        set_remove(&ra->active_set[rc], reg);

        // HACK(NeGate): FLAGS can't spill to the stack so we'll just still to GPRs which we're
        // assuming to be in slot 2
        RegMask* spill_rm = ctx->has_flags && rc == 1 ? ctx->normie_mask[2] : intern_regmask(ctx, 1, true, 0);
        aarray_push(ra->spills, (Spill){ active_id, spill_rm });
        return reg;
    } else if (vreg->end_time <= pos) {
        // we can steal it completely
        TB_OPTDEBUG(REGALLOC)(printf("  #   assign to "), print_reg_name(rc, highest));

        if (hint >= 0) {
            if (highest == hint) {
                TB_OPTDEBUG(REGALLOC)(printf(" (HINTED)\n"));
            } else {
                TB_OPTDEBUG(REGALLOC)(printf(" (FAILED HINT "), print_reg_name(rc, hint), printf(")\n"));
            }
        } else {
            TB_OPTDEBUG(REGALLOC)(printf("\n"));
        }

        return highest;
    } else {
        // TODO(NeGate): split at optimal position before current
        vreg->class    = rc;
        vreg->assigned = highest;

        TB_OPTDEBUG(REGALLOC)(printf("  #   spill v%u\n", vreg_id));
        TB_OPTDEBUG(REGALLOC)(printf("  #   stole "), print_reg_name(rc, highest), printf("\n"));

        // steal the reg
        RegMask* spill_rm = ctx->has_flags && rc == 1 ? ctx->normie_mask[2] : intern_regmask(ctx, 1, true, 0);
        aarray_push(ra->spills, (Spill){ vreg_id, spill_rm });
        return highest;
    }
}

////////////////////////////////
// VReg splitting
////////////////////////////////
static void insert_before_time(Ctx* ctx, LSRA* restrict ra, TB_BasicBlock* bb, TB_Node* n, int time) {
    MachineBB* mbb = tb__insert(ctx, ctx->f, bb, n);

    size_t i = 0, cnt = aarray_length(mbb->items);
    while (i < cnt && ra->time[mbb->items[i]->gvn] <= time) { i++; }

    aarray_push(mbb->items, 0);
    memmove(&mbb->items[i + 1], &mbb->items[i], (cnt - i) * sizeof(TB_Node*));
    mbb->items[i] = n;
}

static RegMask* constraint_in(Ctx* ctx, TB_Node* n, int i) {
    ctx->constraint(ctx, n, ctx->ins);
    return ctx->ins[i];
}

static void add_to_unhandled(Ctx* restrict ctx, LSRA* restrict ra, int vreg_id, int pos) {
    // since the split is starting at pos and pos is at the top of the
    // unhandled list... we can push this to the top wit no problem
    size_t i = 0, count = dyn_array_length(ra->unhandled);
    for (; i < count; i++) {
        if (pos > vreg_start(ctx, ra->unhandled[i])) break;
    }

    // we know where to insert
    dyn_array_put(ra->unhandled, 0);
    memmove(&ra->unhandled[i + 1], &ra->unhandled[i], (count - i) * sizeof(int));
    ra->unhandled[i] = vreg_id;
}

// spills for the entire lifetime, we don't wanna be doing this but it's a fast compiling low-quality option
static void spill_entire_life(Ctx* restrict ctx, LSRA* restrict ra, VReg* vreg, RegMask* new_mask) {
    cuikperf_region_start("spill", NULL);
    int class = vreg->class;
    int vreg_id = vreg - ctx->vregs;

    if (vreg->assigned >= 0) {
        TB_OPTDEBUG(REGALLOC)(printf("  \x1b[33m#   v%d: spilled ", vreg_id), print_reg_name(class, vreg->assigned), printf("\x1b[0m\n"));
    } else {
        TB_OPTDEBUG(REGALLOC)(printf("  \x1b[33m#   v%d: spilled *undecided*\x1b[0m\n", vreg_id));
    }

    // insert spill move
    TB_Function* f = ctx->f;
    TB_Node* n = vreg->n;
    assert(n != NULL);

    TB_Node* spill_n = tb_alloc_node(f, TB_MACH_COPY, vreg->n->dt, 2, sizeof(TB_NodeMachCopy));
    subsume_node2(f, n, spill_n);
    set_input(f, spill_n, n, 1);
    TB_NODE_SET_EXTRA(spill_n, TB_NodeMachCopy, .def = new_mask, .use = vreg->mask);

    TB_BasicBlock* bb = f->scheduled[n->gvn];
    int pos = ra->time[n->gvn] + 1;

    // might invalidate vreg ptr
    aarray_insert(ra->time, spill_n->gvn, pos);
    tb__insert_after(ctx, f, spill_n, n);
    VReg* spill_vreg = tb__set_node_vreg(ctx, spill_n);
    vreg = &ctx->vregs[vreg_id];

    *spill_vreg = (VReg){
        .class = class,
        .assigned = -1,
        .mask = new_mask,
        .n = spill_n,
        .end_time = vreg->end_time,
    };
    vreg->end_time = pos;
    add_to_unhandled(ctx, ra, spill_vreg - ctx->vregs, pos);

    // pre-spill range is just a tiny piece
    Range* rg = tb_arena_alloc(ra->arena, sizeof(Range));
    rg->next  = &NULL_RANGE;
    rg->start = pos - 1;
    rg->end   = pos;
    spill_vreg->active_range = vreg->active_range;
    vreg->saved_range = vreg->active_range = rg;

    // it's probably gonna get invalidated by the new vreg adds so
    // i'd rather NULL it before using it
    vreg = NULL;

    // we'll just split at every use site... not the best
    // move (pun intended) but we'll improve it later
    User* u = spill_n->users;
    while (u != NULL) {
        TB_Node* use_n = USERN(u);
        int use_i      = USERI(u);

        RegMask* in_mask = constraint_in(ctx, use_n, use_i);
        RegMask* intersect = tb__reg_mask_meet(ctx, in_mask, new_mask);
        if (intersect == &TB_REG_EMPTY) {
            // reload per use site
            TB_Node* reload_n = tb_alloc_node(f, TB_MACH_COPY, spill_n->dt, 2, sizeof(TB_NodeMachCopy));
            set_input(f, use_n, reload_n, use_i);
            set_input(f, reload_n, spill_n, 1);
            TB_NODE_SET_EXTRA(reload_n, TB_NodeMachCopy, .def = in_mask, .use = new_mask);

            int use_t = ra->time[use_n->gvn];
            TB_OPTDEBUG(REGALLOC)(printf("  \x1b[33m#   v%d: reload at t=%d\x1b[0m\n", vreg_id, use_t));

            // schedule the split right before use
            tb__insert_before(ctx, ctx->f, reload_n, use_n);
            VReg* reload_vreg = tb__set_node_vreg(ctx, reload_n);
            reload_vreg->mask = in_mask;

            int good_before_spot = use_t - 1;
            assert(good_before_spot > pos);

            // insert small range
            Range* rg = tb_arena_alloc(ra->arena, sizeof(Range));
            rg->next  = &NULL_RANGE;
            rg->start = good_before_spot;
            rg->end   = use_t;
            reload_vreg->end_time = use_t;
            reload_vreg->active_range = rg;

            aarray_insert(ra->time, reload_n->gvn, good_before_spot);
            add_to_unhandled(ctx, ra, reload_vreg - ctx->vregs, good_before_spot);
            u = spill_n->users;
        } else {
            u = u->next;
        }
    }

    cuikperf_region_end();
}

#if 0
static void split_intersecting(Ctx* restrict ctx, LSRA* restrict ra, int pos, VReg* vreg, RegMask* new_mask) {
    cuikperf_region_start("split", NULL);
    int class = vreg->class;
    int vreg_id = vreg - ctx->vregs;

    if (vreg->assigned >= 0) {
        TB_OPTDEBUG(REGALLOC)(printf("  \x1b[33m#   v%d: split ", vreg_id), print_reg_name(class, vreg->assigned), printf(" at t=%d\x1b[0m\n", pos));
    } else {
        TB_OPTDEBUG(REGALLOC)(printf("  \x1b[33m#   v%d: split *undecided* at t=%d\x1b[0m\n", vreg_id, pos));
    }

    // insert spill move
    TB_Function* f = ctx->f;
    TB_Node* n = vreg->n;
    assert(n != NULL);

    TB_Node* spill_n = tb_alloc_node(f, TB_MACH_COPY, vreg->n->dt, 2, sizeof(TB_NodeMachCopy));
    subsume_node2(f, n, spill_n);
    set_input(f, spill_n, n, 1);
    TB_NODE_SET_EXTRA(spill_n, TB_NodeMachCopy, .def = new_mask, .use = vreg->mask);

    TB_BasicBlock* bb = NULL;
    FOR_N(i, 0, ctx->bb_count) {
        MachineBB* mbb = &ctx->machine_bbs[i];
        if (pos <= mbb->end_t) { bb = f->scheduled[mbb->n->gvn]; break; }
    }

    // might invalidate vreg ptr
    assert(bb != NULL);
    aarray_insert(ra->time, spill_n->gvn, pos);
    insert_before_time(ctx, ra, bb, spill_n, pos);
    VReg* spill_vreg = tb__set_node_vreg(ctx, spill_n);
    vreg = &ctx->vregs[vreg_id];

    *spill_vreg = (VReg){
        .class = class,
        .assigned = -1,
        .mask = new_mask,
        .n = spill_n,
        .end_time = vreg->end_time,
    };
    vreg->end_time = pos;
    add_to_unhandled(ctx, ra, spill_vreg - ctx->vregs, pos);

    // split ranges
    Range* prev = NULL;
    for (Range* r = vreg->active_range; r; prev = r, r = r->next) {
        if (r->end > pos) {
            bool clean_split = pos < r->start;

            // spill_vreg will keep r, if the split's unclean we'll hand a split copy to
            // spill_vreg (and then that piece will point to NULL_RANGE)
            if (clean_split) {
                assert(prev);
                prev->next = &NULL_RANGE;
            } else {
                Range* rg = tb_arena_alloc(ra->arena, sizeof(Range));
                rg->next  = &NULL_RANGE;
                rg->start = r->start;
                rg->end   = pos;

                if (prev) { prev->next = rg; }
                else { vreg->active_range = rg; }

                // spill_vreg's new start position is split
                r->start = pos;
            }
            spill_vreg->active_range = r;
            break;
        }
    }

    // it's probably gonna get invalidated by the new vreg adds so
    // i'd rather NULL it before using it
    vreg = NULL;

    // insert phis to guarentee SSA-form:
    // * just find all user's blocks and see if they're
    //   dominated by the spill block.
    for (User* u = spill_n->users; u; u = u->next) {
        TB_Node* use_n = USERN(u);
        int use_i      = USERI(u);
        TB_BasicBlock* use_bb = f->scheduled[use_n->gvn];

        TB_Node* use_bb_node = use_bb->start;
        size_t pred_count = cfg_is_region(use_bb_node) ? use_bb_node->input_count : 1;

        TB_ArenaSavepoint sp = tb_arena_save(f->tmp_arena);

        // build up the bitset for each predecessor: 0 for og_def, 1 for spill_def
        uint64_t* ins = tb_arena_alloc(f->tmp_arena, ((pred_count + 63) / 64) * sizeof(uint64_t));
        FOR_N(i, 0, (pred_count + 63) / 64) { ins[i] = 0; }

        FOR_N(i, 0, pred_count) {
            TB_BasicBlock* pred_bb = f->scheduled[use_bb_node->inputs[i]->gvn];
            if (slow_dommy2(bb, pred_bb)) { ins[i / 64] |= 1ull << (i % 64); }
        }

        // we only need a phi if they disagree
        bool needs_phi = false;
        bool path = ins[0] & 1;
        FOR_N(i, 1, pred_count) {
            bool v = (ins[i / 64] >> (i % 64)) & 1;
            if (v != path) { needs_phi = true; }
        }

        if (needs_phi) {
            assert(pred_count > 1);

            // reload per use site
            TB_Node* phi_n = tb_alloc_node(f, TB_PHI, n->dt, 1 + pred_count, 0);
            set_input(f, phi_n, use_bb_node, 0);

            FOR_N(i, 0, pred_count) {
                bool v = (ins[i / 64] >> (i % 64)) & 1;
                set_input(f, phi_n, v ? spill_n : n, 1 + i);
            }

            __debugbreak();
        } else {
            // doesn't require a phi but might have a hard-split
        }
        tb_arena_restore(f->tmp_arena, sp);
    }

    dump_sched(ctx, ra);
    __debugbreak();

    // we'll just split at every use site... not the best
    // move (pun intended) but we'll improve it later
    #if 0
    for (User* u = spill_n->users; u; u = u->next) {
        TB_Node* use_n = USERN(u);
        int use_i      = USERI(u);

        RegMask* in_mask = constraint_in(ctx, use_n, use_i);
        RegMask* intersect = tb__reg_mask_meet(ctx, in_mask, new_mask);
        if (intersect == &TB_REG_EMPTY) {
            // reload per use site
            TB_Node* reload_n = tb_alloc_node(f, TB_MACH_COPY, spill_n->dt, 2, sizeof(TB_NodeMachCopy));
            set_input(f, use_n, reload_n, use_i);
            TB_NODE_SET_EXTRA(reload_n, TB_NodeMachCopy, .def = in_mask, .use = new_mask);

            // we'll inplace replace the user edge to avoid
            // iterator invalidation problems.
            #if TB_PACKED_USERS
            #error todo
            #else
            u->_n = reload_n;
            u->_slot = 1;
            #endif
            reload_n->inputs[1] = spill_n;

            int use_t = ra->time[use_n->gvn];
            TB_OPTDEBUG(REGALLOC)(printf("  \x1b[33m#   v%d: reload at t=%d\x1b[0m\n", vreg_id, use_t));

            // schedule the split right before use
            tb__insert_before(ctx, ctx->f, reload_n, use_n);
            VReg* reload_vreg = tb__set_node_vreg(ctx, reload_n);
            reload_vreg->mask = in_mask;

            // insert small range
            Range* rg = tb_arena_alloc(ra->arena, sizeof(Range));
            rg->next  = &NULL_RANGE;
            rg->start = use_t - 1;
            rg->end   = use_t;
            reload_vreg->active_range = rg;

            aarray_insert(ra->time, reload_n->gvn, use_t - 1);
            add_to_unhandled(ctx, ra, reload_vreg - ctx->vregs, use_t - 1);
        }
    }
    #endif

    cuikperf_region_end();
}
#endif

////////////////////////////////
// VReg state transitions
////////////////////////////////
// lifetime holes solved with active & inactive sets, basically the live interval is a conservative
// estimate of the lifetime and can include ranges where it's not alive but still within some "start"
// and "end" point.
//
// > if (x) goto err; # x is used during the "err" block, and if the err block is scheduled
// >                  # significantly later in the code we would've stretched the lifetime of x.
// > foo(x);
// >
// > ...              # not used/preserved at all for the next (let's say) 300 instructions, we've now
// > return ...;      # lost a register for a major period of time.
// >
// > err: leave(x);   # uses x so it's lifetime is extended
//
// the solution is that after foo(x) we can say x is inactive for some period of time. all we need to
// do is make sure that any new allocations do not intersect the ranges of the inactive ones. Impl wise
// we have a range per BB where a vreg is alive and if we're ever in a timespan where it's not
// intersecting we'll move to the inactive state.
static void update_intervals(Ctx* restrict ctx, LSRA* restrict ra, int time) {
    // update intervals (inactive <-> active along with expiring)
    FOR_N(rc, 0, ctx->num_classes) {
        FOREACH_SET(reg, ra->active_set[rc]) {
            update_interval(ctx, ra, ra->active[rc][reg], true, time, -1);
        }
    }

    for (size_t i = 0; i < dyn_array_length(ra->inactive);) {
        if (update_interval(ctx, ra, ra->inactive[i], false, time, i)) {
            continue;
        }
        i++;
    }
}

// update active range to match where the position is currently
static bool update_interval(Ctx* restrict ctx, LSRA* restrict ra, int vreg_id, bool is_active, int time, int inactive_index) {
    VReg* vreg = &ctx->vregs[vreg_id];

    // get to the right range first
    while (time >= vreg->active_range->end) {
        vreg->active_range = vreg->active_range->next;
        assert(vreg->active_range != NULL);
    }

    int hole_end = vreg->active_range->start;
    int active_end = vreg->active_range->end;
    bool is_now_active = time >= hole_end;

    int rc  = vreg_class(vreg);
    int reg = vreg->assigned;

    if (vreg->active_range == &NULL_RANGE) { // expired
        if (is_active) {
            TB_OPTDEBUG(REGALLOC)(printf("  #   active "), print_reg_name(rc, reg), printf(" has expired at t=%d (v%d)\n", vreg->end_time, vreg_id));
            set_remove(&ra->active_set[rc], reg);
        } else {
            TB_OPTDEBUG(REGALLOC)(printf("  #   inactive "), print_reg_name(rc, reg), printf(" has expired at t=%d (v%d)\n", vreg->end_time, vreg_id));
            dyn_array_remove(ra->inactive, inactive_index);
            return true;
        }
    } else if (is_now_active != is_active) { // if we moved, change which list we're in
        if (is_now_active) { // inactive -> active
            TB_OPTDEBUG(REGALLOC)(printf("  #   inactive "), print_reg_name(rc, reg), printf(" is active again (until t=%d, v%d)\n", active_end, vreg_id));

            move_to_active(ra, vreg, vreg_id);
            dyn_array_remove(ra->inactive, inactive_index);
            return true;
        } else { // active -> inactive
            TB_OPTDEBUG(REGALLOC)(printf("  #   active "), print_reg_name(rc, reg), printf(" is going quiet for now (until t=%d, v%d)\n", active_end, vreg_id));

            set_remove(&ra->active_set[rc], reg);
            dyn_array_put(ra->inactive, vreg_id);
        }
    }

    return false;
}

static void move_to_active(LSRA* restrict ra, VReg* vreg, int vreg_id) {
    int rc = vreg_class(vreg), reg = vreg->assigned;
    /* if (set_get(&ra->active_set[rc], reg)) {
        tb_panic("v%d: interval v%d should never be forced out, we should've accomodated them in the first place", vreg_id, ra->active[rc][reg]);
    } */

    set_put(&ra->active_set[rc], reg);
    ra->active[rc][reg] = vreg_id;
}

////////////////////////////////
// Sorting unhandled list
////////////////////////////////
static size_t partition(Ctx* ctx, int* intervals, ptrdiff_t lo, ptrdiff_t hi) {
    int pivot = vreg_start(ctx, intervals[(hi - lo) / 2 + lo]); // middle

    ptrdiff_t i = lo - 1, j = hi + 1;
    for (;;) {
        // Move the left index to the right at least once and while the element at
        // the left index is less than the pivot
        do { i += 1; } while (vreg_start(ctx, intervals[i]) > pivot);

        // Move the right index to the left at least once and while the element at
        // the right index is greater than the pivot
        do { j -= 1; } while (vreg_start(ctx, intervals[j]) < pivot);

        // If the indices crossed, return
        if (i >= j) return j;

        // Swap the elements at the left and right indices
        SWAP(int, intervals[i], intervals[j]);
    }
}

static void cuiksort_defs(Ctx* ctx, int* intervals, ptrdiff_t lo, ptrdiff_t hi) {
    if (lo >= 0 && hi >= 0 && lo < hi) {
        // get pivot
        size_t p = partition(ctx, intervals, lo, hi);

        // sort both sides
        cuiksort_defs(ctx, intervals, lo, p);
        cuiksort_defs(ctx, intervals, p + 1, hi);
    }
}

#if 0
static void insert_split_move(LSRA* restrict ra, int t, LiveInterval* old_it, LiveInterval* new_it);
static ptrdiff_t allocate_free_reg(LSRA* restrict ra, LiveInterval* interval);
static LiveInterval* split_intersecting(LSRA* restrict ra, int pos, LiveInterval* interval, RegMask new_mask);

static const char* GPR_NAMES[] = { "RAX", "RCX", "RDX", "RBX", "RSP", "RBP", "RSI", "RDI", "R8",  "R9", "R10", "R11", "R12", "R13", "R14", "R15" };

static LiveInterval* split_vreg_at(LiveInterval* interval, int pos) {
    if (interval == NULL) {
        return interval;
    }

    // skip past previous intervals
    while (interval->split_kid && pos > vreg_end(interval)) {
        interval = interval->split_kid;
    }

    // assert(interval->reg >= 0 || pos <= vreg_end(interval));
    return interval;
}

LiveInterval* gimme_interval_for_mask(Ctx* restrict ctx, TB_Arena* arena, LSRA* restrict ra, RegMask mask, TB_DataType dt) {
    // not so fixed interval? we need a unique interval then
    int reg = fixed_reg_mask(mask);
    if (reg >= 0) {
        return &ctx->fixed[mask.class][reg];
    } else {
        LiveInterval* interval = tb_arena_alloc(arena, sizeof(LiveInterval));
        *interval = (LiveInterval){
            .id = ctx->interval_count++,
            .mask = mask,
            .dt   = dt,
            .hint = NULL,
            .reg  = -1,
            .assigned = -1,
            .range_cap = 4, .range_count = 1,
            .ranges = tb_arena_alloc(arena, 4 * sizeof(LiveRange))
        };
        interval->ranges[0] = (LiveRange){ INT_MAX, INT_MAX };
        return interval;
    }
}

static Tile* tile_at_time(LSRA* restrict ra, int t) {
    // find which BB
    MachineBB* mbb = NULL;
    FOR_N(i, 0, ra->ctx->bb_count) {
        mbb = &ra->ctx->machine_bbs[i];
        if (t <= mbb->end->time) break;
    }

    Tile* curr = mbb->start;
    while (curr != NULL) {
        if (curr->time > t) {
            return curr;
        }
        curr = curr->next;
    }

    return NULL;
}

void tb__lsra(Ctx* restrict ctx, TB_Arena* arena) {
    LSRA ra = { .ctx = ctx, .arena = arena, .spills = ctx->num_regs[0] };
    int initial_spills = ctx->num_regs[0];

    // create timeline & insert moves
    CUIK_TIMED_BLOCK("insert legalizing moves") {
        int timeline = 4;
        FOR_N(i, 0, ctx->bb_count) {
            MachineBB* mbb = &ctx->machine_bbs[i];

            FOR_N(j, 0, mbb->item_count) {
                TB_Node* n = mbb->items[j];
                LiveInterval* li = ctx->intervals[n->gvn];

                // insert input copies (temporaries & clobbers never introduce
                // these so we're safe don't check those)
                FOR_N(k, 1, n->input_count) if (n->inputs[k]) {
                    TB_Node* in = n->inputs[k];
                    RegMask* in_mask = c->ins[k];

                    int hint = fixed_reg_mask(in_mask);
                    RegMask* in_def_mask = ctx->intervals[in->gvn].out;
                    if (hint >= 0) {
                        in_def_mask->hint = c;
                    }

                    bool both_fixed = hint >= 0 && in_def_mask.mask == in_mask.mask;
                }

                set_put(kill, n->gvn);
                // ctx.id2interval[n->gvn] = n;
            }

            for (Tile* t = mbb->start; t; t = t->next) {
                // insert input copies
                FOR_N(j, 0, t->in_count) {
                    LiveInterval* in_def = t->ins[j].src;
                    RegMask in_mask = t->ins[j].mask;

                    if (in_def == NULL) {
                        continue;
                    }

                    RegMask in_def_mask = in_def->mask;
                    if (hint >= 0) {
                        in_def->hint = &ctx->fixed[in_mask.class][hint];
                    }

                    bool both_fixed = hint >= 0 && in_def_mask.mask == in_mask.mask;

                    // we resolve def-use conflicts with a spill move, either when:
                    //   * the use and def classes don't match.
                    //   * the use mask is more constrained than the def.
                    //   * it's on both ends to avoid stretching fixed intervals.
                    if (in_def_mask.class != in_mask.class || (in_def_mask.mask & in_mask.mask) != in_def_mask.mask || both_fixed) {
                        if (both_fixed) {
                            in_def_mask = ctx->normie_mask[in_def_mask.class];
                        }

                        TB_OPTDEBUG(REGALLOC)(printf("  TEMP "), tb__print_regmask(in_def_mask), printf(" -> "), tb__print_regmask(in_mask), printf("\n"));

                        // construct copy (either to a fixed interval or a new masked interval)
                        Tile* tmp = tb_arena_alloc(arena, sizeof(Tile) + sizeof(LiveInterval*));
                        *tmp = (Tile){
                            .prev = t->prev,
                            .next = t,
                            .tag = TILE_SPILL_MOVE,
                            .time = timeline
                        };
                        assert(in_def->dt.raw);
                        tmp->spill_dt = in_def->dt;
                        tmp->ins = tb_arena_alloc(arena, sizeof(Tile*));
                        tmp->in_count = 1;
                        tmp->ins[0].src  = in_def;
                        tmp->ins[0].mask = in_def_mask;
                        if (t->prev == NULL) {
                            mbb->start = tmp;
                        } else {
                            t->prev->next = tmp;
                        }
                        t->prev = tmp;

                        // replace use site with temporary that legalized the constraint
                        tmp->out_count = 1;
                        tmp->outs[0] = gimme_interval_for_mask(ctx, arena, &ra, in_mask, in_def->dt);
                        t->ins[j].src = tmp->outs[0];

                        timeline += 2;
                    }
                }

                // place on timeline
                t->time = timeline;
                // 2addr ops will have a bit of room for placing the hypothetical copy
                timeline += t->n && ctx->_2addr(t->n) ? 4 : 2;

                // insert copy if we're writing to a fixed interval
                FOR_N(j, 0, t->out_count) if (t->outs[j]) {
                    LiveInterval* interval = t->outs[j];

                    // if we're writing to a fixed interval, insert copy
                    // such that we only guarentee a fixed location at the
                    // def site.
                    int reg = fixed_reg_mask(interval->mask);
                    if (reg >= 0 && t->tag != TILE_SPILL_MOVE) {
                        RegMask rm = interval->mask;
                        LiveInterval* fixed = &ctx->fixed[rm.class][reg];

                        // interval: FIXED(t) => NORMIE_MASK
                        interval->mask = ctx->normie_mask[rm.class];
                        interval->hint = fixed;

                        // insert copy such that the def site is the only piece which "requires"
                        // the fixed range.
                        Tile* tmp = tb_arena_alloc(arena, sizeof(Tile) + sizeof(LiveInterval*));
                        *tmp = (Tile){
                            .prev = t,
                            .next = t->next,
                            .tag = TILE_SPILL_MOVE,
                            .time = timeline,
                        };
                        assert(interval->dt.raw);
                        tmp->spill_dt = interval->dt;
                        tmp->ins = tb_arena_alloc(tmp_arena, sizeof(Tile*));
                        tmp->in_count = 1;
                        tmp->ins[0].src  = fixed;
                        tmp->ins[0].mask = rm;
                        t->next->prev = tmp;
                        t->next = tmp;

                        // replace def site with fixed interval
                        tmp->out_count = 1;
                        tmp->outs[0] = t->outs[j];
                        t->outs[j] = fixed;

                        // skip this move op
                        t = tmp;
                        timeline += 2;
                    }
                }
            }

            timeline += 2;
        }
    }

    // build intervals from dataflow
    CUIK_TIMED_BLOCK("build intervals") {
        Set visited = set_create_in_arena(arena, ctx->interval_count);

        FOR_REV_N(i, 0, ctx->bb_count) {
            MachineBB* mbb = &ctx->machine_bbs[i];

            int bb_start = mbb->start->time;
            int bb_end = mbb->end->time + 2;

            // live outs define a full range across the BB (if they're defined
            // in the block, the later reverse walk will fix that up)
            Set* live_out = &mbb->live_out;
            FOR_N(j, 0, (ctx->interval_count + 63) / 64) {
                uint64_t bits = live_out->data[j];
                if (bits == 0) continue;

                FOR_N(k, 0, 64) if (bits & (1ull << k)) {
                    add_range(&ra, ctx->id2interval[j*64 + k], bb_start, bb_end);
                }
            }

            for (Tile* t = mbb->end; t; t = t->prev) {
                int time = t->time;

                // mark output
                FOR_N(j, 0, t->out_count) {
                    LiveInterval* interval = t->outs[j];
                    assert(interval->mask.mask);

                    if (!set_get(&visited, interval->id)) {
                        set_put(&visited, interval->id);
                        if (interval->reg < 0) {
                            dyn_array_put(ra.unhandled, interval);
                        }
                    }

                    if (interval->range_count == 1) {
                        add_range(&ra, interval, time, time);
                    } else {
                        interval->ranges[interval->range_count - 1].start = time;
                    }
                }

                // 2 address ops will interfere with their own inputs (except for
                // shared dst/src)
                bool _2addr = t->tag == TILE_SPILL_MOVE || (t->n && ctx->_2addr(t->n));

                // mark inputs
                FOR_N(j, 0, t->in_count) {
                    LiveInterval* in_def = t->ins[j].src;
                    RegMask in_mask = t->ins[j].mask;
                    int hint = fixed_reg_mask(in_mask);

                    // clobber fixed input
                    if (in_def == NULL) {
                        LiveInterval* tmp;
                        if (hint >= 0) {
                            tmp = &ctx->fixed[in_mask.class][hint];
                        } else {
                            tmp = gimme_interval_for_mask(ctx, arena, &ra, in_mask, TB_TYPE_I64);
                            dyn_array_put(ra.unhandled, tmp);
                            t->ins[j].src = tmp;
                        }

                        add_range(&ra, tmp, time, time + 1);
                        continue;
                    }

                    RegMask in_def_mask = in_def->mask;
                    if (hint >= 0) {
                        in_def->hint = &ctx->fixed[in_mask.class][hint];
                    }

                    int use_time = time;
                    if (_2addr) {
                        if (j != 0) {
                            // extend
                            use_time += 2;
                        } else {
                            assert(t->out_count >= 1 && "2addr ops need destinations");

                            // hint as copy
                            if (t->outs[0]->hint == NULL) {
                                t->outs[0]->hint = in_def;
                            }
                        }
                    }

                    add_range(&ra, in_def, bb_start, use_time);
                    // add_use_pos(&ra, in_def, use_time, in_mask.may_spill);
                }
            }
        }
    }

    int max_regs_in_class = 0;
    CUIK_TIMED_BLOCK("pre-pass on fixed intervals") {
        ra.num_classes = ctx->num_classes;
        ra.num_regs = ctx->num_regs;
        ra.normie_mask = ctx->normie_mask;
        ra.callee_saved = ctx->callee_saved;

        FOR_N(i, 0, ctx->num_classes) {
            if (max_regs_in_class < ctx->num_regs[i]) {
                max_regs_in_class = ctx->num_regs[i];
            }

            // add range at beginning such that all fixed intervals are "awake"
            FOR_N(j, 0, ctx->num_regs[i]) {
                add_range(&ra, &ctx->fixed[i][j], 0, 1);
                dyn_array_put(ra.unhandled, &ctx->fixed[i][j]);
            }

            size_t cap = ctx->num_regs[i];
            if (i == REG_CLASS_STK) {
                // stack slots have infinite colors we so need to make sure we can resize things
                if (cap < 128) cap = 128;
                else cap = tb_next_pow2(cap + 1);

                ra.stack_slot_cap = cap;
            }

            ra.active_set[i] = set_create_in_arena(arena, cap);
            ra.active[i] = tb_arena_alloc(arena, cap * sizeof(LiveInterval*));
            ra.fixed[i] = ctx->fixed[i];
            memset(ra.active[i], 0, ctx->num_regs[i] * sizeof(LiveInterval*));
        }

        // only need enough to store for the biggest register class
        ra.free_pos  = TB_ARENA_ARR_ALLOC(tmp_arena, max_regs_in_class, int);
        ra.block_pos = TB_ARENA_ARR_ALLOC(tmp_arena, max_regs_in_class, int);
    }

    // sort intervals:
    CUIK_TIMED_BLOCK("sort intervals") {
        cuiksort_defs(ra.unhandled, 0, dyn_array_length(ra.unhandled) - 1);
    }

    // linear scan:
    //   expire old => allocate free or spill/split => rinse & repeat.
    CUIK_TIMED_BLOCK("linear scan") {
        while (dyn_array_length(ra.unhandled)) {
            LiveInterval* interval = dyn_array_pop(ra.unhandled);

            int time = interval_start(interval);
            int end = interval_end(interval);

            assert(time != INT_MAX);

            #if TB_OPTDEBUG_REGALLOC
            printf("  # v%-4d t=[%-4d - %4d) ", interval->id, time, end);
            tb__print_regmask(interval->mask);
            printf("    ");
            if (interval->tile && interval->tile->n) {
                print_node_sexpr(interval->tile->n, 0);
            }
            printf("\n");
            #endif

            // update intervals (inactive <-> active along with expiring)
            FOR_N(rc, 0, ctx->num_classes) {
                FOREACH_SET(reg, ra.active_set[rc]) {
                    update_interval(&ra, ra.active[rc][reg], true, time, -1);
                }
            }

            for (size_t i = 0; i < dyn_array_length(ra.inactive);) {
                LiveInterval* inactive = ra.inactive[i];
                if (update_interval(&ra, inactive, false, time, i)) {
                    continue;
                }
                i++;
            }

            // allocate free register (or stack slot)
            ptrdiff_t reg = interval->reg;
            if (reg < 0) {
                if (interval->mask.class == REG_CLASS_STK) {
                    assert(interval->mask.mask == 0);

                    // add new stack slot
                    if (reg < 0) {
                        int next_stk_regs = ra.num_regs[0]++;
                        if (next_stk_regs == ra.stack_slot_cap) {
                            size_t old_cap = ra.stack_slot_cap;
                            ra.stack_slot_cap *= 2;

                            Set new_set = set_create_in_arena(arena, ra.stack_slot_cap);
                            set_copy(&new_set, &ra.active_set[REG_CLASS_STK]);

                            LiveInterval** new_active = tb_arena_alloc(arena, ra.stack_slot_cap * sizeof(LiveInterval*));
                            memcpy(new_active, ra.active, ra.stack_slot_cap * sizeof(LiveInterval*));

                            ra.active_set[REG_CLASS_STK] = new_set;
                            ra.active[REG_CLASS_STK] = new_active;
                            ra.active[REG_CLASS_STK][next_stk_regs] = NULL;
                        }

                        reg = ra.spills++;
                    }

                    TB_OPTDEBUG(REGALLOC)(printf("  #   assign to [SP + %"PRId64"]\n", reg*8));
                } else {
                    reg = allocate_free_reg(&ra, interval);
                    assert(reg >= 0 && "despair");
                }
            }

            // add to active set
            if (reg >= 0) {
                interval->class = interval->mask.class;
                interval->assigned = reg;
                move_to_active(&ra, interval);
            }

            // display active set
            #if TB_OPTDEBUG_REGALLOC
            static const char* classes[] = { "STK", "GPR", "VEC" };
            FOR_N(rc, 0, ctx->num_classes) {
                printf("  \x1b[32m%s { ", classes[rc]);
                FOREACH_SET(reg, ra.active_set[rc]) {
                    LiveInterval* l = ra.active[rc][reg];
                    printf("v%d:", l->id);
                    print_reg_name(rc, reg);
                    printf(" ");
                }
                printf("}\x1b[0m\n");
            }
            #endif
        }
    }

    // move resolver:
    //   when a split happens, all indirect paths that cross the split will have
    //   moves inserted.
    CUIK_TIMED_BLOCK("move resolver") {
        FOR_N(i, 0, ctx->bb_count) {
            MachineBB* mbb = &ctx->machine_bbs[i];
            TB_Node* end_node = mbb->end_n;
            int terminator = mbb->end->time;

            for (User* u = end_node->users; u; u = u->next) {
                if (cfg_is_control(u->n) && !cfg_is_endpoint(u->n)) {
                    TB_Node* succ = end_node->type == TB_BRANCH ? cfg_next_bb_after_cproj(u->n) : u->n;
                    MachineBB* target = node_to_bb(ctx, succ);
                    int start_time = target->start->time;

                    // for all live-ins, we should check if we need to insert a move
                    FOREACH_SET(k, target->live_in) {
                        LiveInterval* interval = ctx->id2interval[k];

                        // if the value changes across the edge, insert move
                        LiveInterval* start = split_interval_at(interval, terminator);
                        LiveInterval* end = split_interval_at(interval, start_time);

                        if (start != end) {
                            tb_todo();

                            /* if (start->spill > 0) {
                                assert(end->spill == start->spill && "TODO: both can't be spills yet");
                                insert_split_move(&ra, start_time, start, end);
                            } else {
                                insert_split_move(&ra, terminator - 1, start, end);
                            } */
                        }
                    }
                }
            }
        }
    }

    // resolve all split interval references
    CUIK_TIMED_BLOCK("split resolver") {
        FOR_REV_N(i, 0, ctx->bb_count) {
            MachineBB* mbb = &ctx->machine_bbs[i];

            for (Tile* t = mbb->start; t; t = t->next) {
                int pos = t->time;

                FOR_N(j, 0, t->out_count) {
                    t->outs[j] = split_interval_at(t->outs[j], pos - 1);
                }

                FOR_N(i, 0, t->in_count) {
                    t->ins[i].src = split_interval_at(t->ins[i].src, pos);
                }
            }
        }
    }

    ctx->stack_usage += (ra.spills - initial_spills) * 8;
    ctx->initial_spills = initial_spills;
    ctx->callee_spills  = ra.callee_spills;
}

// returns -1 if no registers are available
static int allocate_free_reg(Ctx* restrict ctx, LSRA* restrict ra, VReg* vreg, int vreg_id) {
    int rc = interval->mask.class;
    compute_free_pos(ra, interval, interval->mask.class, interval->mask.mask);

    int highest = -1;
    int hint_reg = interval->hint ? interval->hint->assigned : -1;

    // it's better in the long run to aggressively split based on hints
    if (hint_reg >= 0 && interval_end(interval) <= ra->free_pos[hint_reg]) {
        highest = hint_reg;
    }

    // pick highest free pos
    if (highest < 0) {
        highest = 0;
        FOR_N(i, 1, ra->num_regs[rc]) if (ra->free_pos[i] > ra->free_pos[highest]) {
            highest = i;
        }
    }

    int pos = ra->free_pos[highest];
    if (UNLIKELY(pos == 0)) {
        int reg = -1;
        FOR_N(i, 0, ra->num_regs[rc]) {
            if (set_get(&ra->active_set[rc], i) && ra->active[rc][i]->reg < 0) {
                reg = i;
                break;
            }
        }
        assert(reg >= 0 && "no way they're all in fixed-use lmao");

        // alloc failure, split any
        LiveInterval* active_user = ra->active[rc][reg];
        set_remove(&ra->active_set[rc], reg);

        // split whatever is using the interval right now
        split_intersecting(ra, interval_start(interval) - 3, active_user, REGMASK(STK, 0));
        return reg;
    } else {
        if (UNLIKELY(ra->callee_saved[rc] & (1ull << highest))) {
            ra->callee_saved[rc] &= ~(1ull << highest);

            TB_OPTDEBUG(REGALLOC)(printf("  #   spill callee saved register "), print_reg_name(rc, highest), printf("\n"));
            LiveInterval* fixed = &ra->fixed[rc][highest];

            // mark callee move
            CalleeSpill c = { fixed->class, fixed->reg, ra->spills++ };
            dyn_array_put(ra->callee_spills, c);
        }

        if (interval_end(interval) <= pos) {
            // we can steal it completely
            TB_OPTDEBUG(REGALLOC)(printf("  #   assign to "), print_reg_name(rc, highest));

            if (hint_reg >= 0) {
                if (highest == hint_reg) {
                    TB_OPTDEBUG(REGALLOC)(printf(" (HINTED)\n"));
                } else {
                    TB_OPTDEBUG(REGALLOC)(printf(" (FAILED HINT "), print_reg_name(rc, hint_reg), printf(")\n"));
                }
            } else {
                TB_OPTDEBUG(REGALLOC)(printf("\n"));
            }
        } else {
            // TODO(NeGate): split at optimal position before current
            interval->class = interval->mask.class;
            interval->assigned = highest;
            split_intersecting(ra, pos - 3, interval, REGMASK(STK, 0));
            TB_OPTDEBUG(REGALLOC)(printf("  #   stole "), print_reg_name(rc, highest), printf("\n"));
        }

        return highest;
    }
}

static void insert_split_move(LSRA* restrict ra, int t, LiveInterval* old_it, LiveInterval* new_it) {
    // find which BB
    MachineBB* mbb = NULL;
    FOR_N(i, 0, ra->ctx->bb_count) {
        mbb = &ra->ctx->machine_bbs[i];
        if (t <= mbb->end->time) break;
    }

    Tile *prev = NULL, *curr = mbb->start;
    while (curr != NULL) {
        if (curr->time > t) {
            break;
        }
        prev = curr, curr = curr->next;
    }

    Tile* move = tb_arena_alloc(ra->arena, sizeof(Tile) + sizeof(LiveInterval*));
    *move = (Tile){
        .tag = TILE_SPILL_MOVE,
    };
    assert(old_it->dt.raw);
    move->spill_dt = old_it->dt;
    move->ins = tb_arena_alloc(ra->arena, sizeof(Tile*));
    move->in_count = 1;
    move->ins[0].src  = old_it;
    move->ins[0].mask = old_it->mask;
    move->out_count = 1;
    move->outs[0] = new_it;
    if (prev) {
        move->time = prev->time + 1;
        move->prev = prev;
        move->next = prev->next;
        if (prev->next == NULL) {
            prev->next = move;
            mbb->end = move;
        } else {
            prev->next->prev = move;
            prev->next = move;
        }
    } else {
        move->time = t;
        move->next = mbb->start;
        mbb->start->prev = move;
        mbb->start = move;
    }
}

static LiveInterval* split_intersecting(LSRA* restrict ra, int pos, LiveInterval* interval, RegMask new_mask) {
    cuikperf_region_start("split", NULL);
    int rc = interval->class;

    LiveInterval* restrict new_it = TB_ARENA_ALLOC(ra->arena, LiveInterval);
    *new_it = *interval;

    assert(interval->mask.class != new_mask.class);
    new_it->mask = new_mask;

    if (interval->assigned >= 0) {
        TB_OPTDEBUG(REGALLOC)(printf("  \x1b[33m#   v%d: split ", interval->id), print_reg_name(rc, interval->assigned), printf(" at t=%d\x1b[0m\n", pos));
    } else {
        TB_OPTDEBUG(REGALLOC)(printf("  \x1b[33m#   v%d: split *undecided* at t=%d\x1b[0m\n", interval->id, pos));
    }

    // split lifetime
    new_it->assigned = new_it->reg = -1;
    new_it->uses = NULL;
    new_it->split_kid = NULL;
    new_it->range_count = 0;

    assert(interval->split_kid == NULL && "cannot spill while spilled");
    interval->split_kid = new_it;

    {
        // since the split is starting at pos and pos is at the top of the
        // unhandled list... we can push this to the top wit no problem
        size_t i = 0, count = dyn_array_length(ra->unhandled);
        for (; i < count; i++) {
            if (pos > interval_start(ra->unhandled[i])) break;
        }

        // we know where to insert
        dyn_array_put(ra->unhandled, NULL);
        memmove(&ra->unhandled[i + 1], &ra->unhandled[i], (count - i) * sizeof(LiveInterval*));
        ra->unhandled[i] = new_it;
    }

    // split ranges
    size_t end = interval->range_count;
    FOR_REV_N(i, 1, end) {
        LiveRange* range = &interval->ranges[i];
        if (range->end > pos) {
            bool clean_split = pos < range->start;

            LiveRange old = interval->ranges[interval->active_range];

            new_it->range_count = new_it->range_cap = i + 1;
            new_it->active_range = new_it->range_count - 1;
            new_it->ranges = interval->ranges;

            // move interval up, also insert INT_MAX and potentially
            size_t start = new_it->range_count - !clean_split;

            interval->range_count = interval->range_cap = (end - start) + 1;
            interval->ranges = tb_arena_alloc(ra->arena, interval->range_count * sizeof(LiveRange));
            interval->active_range -= start - 1;
            interval->ranges[0] = (LiveRange){ INT_MAX, INT_MAX };

            FOR_N(j, start, end) {
                assert(j - start + 1 < interval->range_count);
                interval->ranges[j - start + 1] = new_it->ranges[j];
            }

            // assert(interval->ranges[interval->active_range].start == old.start);
            // assert(interval->ranges[interval->active_range].end == old.end);

            if (range->start <= pos) {
                interval->ranges[1].end = pos;
                new_it->ranges[new_it->range_count - 1].start = pos;
            }
            break;
        }
    }
    assert(new_it->range_count != 0);

    // split uses
    size_t i = 0, use_count = interval->use_count;
    while (i < use_count + 1) {
        size_t split_count = use_count - i;
        if (i == use_count || interval->uses[i].pos < pos) {
            new_it->use_count = new_it->use_cap = i;
            new_it->uses = interval->uses;

            interval->use_count = interval->use_cap = split_count;
            interval->uses = &interval->uses[i];
            break;
        }
        i++;
    }

    // insert move (the control flow aware moves are inserted later)
    insert_split_move(ra, pos, interval, new_it);

    // reload before next use that requires the original regclass
    if (new_mask.class == REG_CLASS_STK) {
        FOR_REV_N(i, 0, new_it->use_count) {
            if (!new_it->uses[i].may_spill) {
                split_intersecting(ra, new_it->uses[i].pos - 3, new_it, interval->mask);
                break;
            }
        }
    }

    cuikperf_region_end();
    return new_it;
}

// update active range to match where the position is currently
static bool update_interval(LSRA* restrict ra, LiveInterval* interval, bool is_active, int time, int inactive_index) {
    // get to the right range first
    while (time >= interval->ranges[interval->active_range].end) {
        assert(interval->active_range > 0);
        interval->active_range -= 1;
    }

    int hole_end = interval->ranges[interval->active_range].start;
    int active_end = interval->ranges[interval->active_range].end;
    bool is_now_active = time >= hole_end;

    int rc = interval_class(interval);
    int reg = interval->assigned;

    if (interval->active_range == 0) { // expired
        if (is_active) {
            TB_OPTDEBUG(REGALLOC)(printf("  #   active "), print_reg_name(rc, reg), printf(" has expired at t=%d (v%d)\n", interval_end(interval), interval->id));
            set_remove(&ra->active_set[rc], reg);
        } else {
            TB_OPTDEBUG(REGALLOC)(printf("  #   inactive "), print_reg_name(rc, reg), printf(" has expired at t=%d (v%d)\n", interval_end(interval), interval->id));
            dyn_array_remove(ra->inactive, inactive_index);
            return true;
        }
    } else if (is_now_active != is_active) { // if we moved, change which list we're in
        if (is_now_active) { // inactive -> active
            TB_OPTDEBUG(REGALLOC)(printf("  #   inactive "), print_reg_name(rc, reg), printf(" is active again (until t=%d, v%d)\n", active_end, interval->id));

            move_to_active(ra, interval);
            dyn_array_remove(ra->inactive, inactive_index);
            return true;
        } else { // active -> inactive
            TB_OPTDEBUG(REGALLOC)(printf("  #   active "), print_reg_name(rc, reg), printf(" is going quiet for now (until t=%d, v%d)\n", active_end, interval->id));

            set_remove(&ra->active_set[rc], reg);
            dyn_array_put(ra->inactive, interval);
        }
    }

    return false;
}
#endif
