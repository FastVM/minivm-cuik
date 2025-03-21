
enum { SROA_LIMIT = 1024 };

typedef struct {
    TB_Node* old_n;
    int64_t offset;
    TB_CharUnits size;
    TB_DataType dt;
} AggregateConfig;

static ptrdiff_t find_config(size_t config_count, AggregateConfig* configs, int64_t offset) {
    FOR_N(i, 0, config_count) {
        if (configs[i].offset == offset) return i;
    }

    tb_unreachable();
    return -1;
}

// -1 is a bad match
// -2 is no match, so we can add a new config
static ptrdiff_t compatible_with_configs(size_t config_count, AggregateConfig* configs, int64_t offset, TB_CharUnits size, TB_DataType dt) {
    int64_t max = offset + size;

    FOR_N(i, 0, config_count) {
        int64_t max2 = configs[i].offset + configs[i].size;

        if (offset >= configs[i].offset && max <= max2) {
            // they overlap... but is it a clean overlap?
            if (offset == configs[i].offset && max == max2 && TB_DATA_TYPE_EQUALS(dt, configs[i].dt)) {
                return i;
            }

            return -1;
        }
    }

    return -2;
}

// false means failure to SROA
static bool add_configs(TB_Function* f, TB_Node* addr, TB_Node* base_address, size_t base_offset, size_t* config_count, AggregateConfig* configs, int pointer_size) {
    FOR_USERS(use, addr) {
        TB_Node* n = USERN(use);

        if (n->type == TB_PTR_OFFSET && n->inputs[2]->type == TB_ICONST && USERI(use) == 1) {
            // same rules, different offset
            int64_t offset = TB_NODE_GET_EXTRA_T(n->inputs[2], TB_NodeInt)->value;
            if (!add_configs(f, n, base_address, base_offset + offset, config_count, configs, pointer_size)) {
                return false;
            }
            continue;
        }

        // we can only SROA if we know we're not using the
        // address for anything but direct memory ops or TB_MEMBERs.
        if (USERI(use) != 2) {
            return false;
        }

        // find direct memory op
        if (n->type != TB_LOAD && n->type != TB_STORE) {
            return false;
        }

        TB_DataType dt = n->type == TB_LOAD ? n->dt : n->inputs[3]->dt;
        TB_Node* address = n->inputs[2];
        int size = (bits_in_data_type(pointer_size, dt) + 7) / 8;

        // see if it's a compatible configuration
        int match = compatible_with_configs(*config_count, configs, base_offset, size, dt);
        if (match == -1) {
            return false;
        } else if (match == -2) {
            // add new config
            if (*config_count == SROA_LIMIT) {
                return false;
            }
            configs[(*config_count)++] = (AggregateConfig){ address, base_offset, size, dt };
        } else if (configs[match].old_n != address) {
            log_warn("%s: v%u SROA config matches but reaches so via a different node, please idealize nodes before mem2reg", f->super.name, address->gvn);
            return false;
        }
    }

    return true;
}

static size_t sroa_rewrite(TB_Function* f, int pointer_size, TB_Node* start, TB_Node* n) {
    TB_ArenaSavepoint sp = tb_arena_save(f->tmp_arena);

    size_t config_count = 0;
    AggregateConfig* configs = tb_arena_alloc(f->tmp_arena, SROA_LIMIT * sizeof(AggregateConfig));
    if (!add_configs(f, n, n, 0, &config_count, configs, pointer_size)) {
        return 1;
    }

    // split allocation into pieces
    if (config_count > 1) {
        DO_IF(TB_OPTDEBUG_SROA)(printf("sroa v%u => SROA to %zu pieces", n->gvn, config_count));

        uint32_t alignment = TB_NODE_GET_EXTRA_T(n, TB_NodeLocal)->align;
        FOR_N(i, 0, config_count) {
            TB_Node* new_n = tb_alloc_node(f, TB_LOCAL, TB_TYPE_PTR, 1, sizeof(TB_NodeLocal));
            set_input(f, new_n, start, 0);
            TB_NODE_SET_EXTRA(new_n, TB_NodeLocal, .size = configs[i].size, .align = alignment);

            // replace old pointer with new fancy
            subsume_node(f, configs[i].old_n, new_n);

            // mark all users, there may be some fun new opts now
            mark_node(f, new_n);
            mark_users(f, new_n);
        }

        // we marked the changes else where which is cheating the peephole
        // but still doing all the progress it needs to.
        mark_users(f, n);
    }

    tb_arena_restore(f->tmp_arena, sp);
    return config_count > 1 ? 1 + config_count : 1;
}
