/*
This file is part of Bohrium and copyright (c) 2012 the Bohrium
team <http://www.bh107.org>.

Bohrium is free software: you can redistribute it and/or modify
it under the terms of the GNU Lesser General Public License as
published by the Free Software Foundation, either version 3
of the License, or (at your option) any later version.

Bohrium is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the
GNU Lesser General Public License along with Bohrium.

If not, see <http://www.gnu.org/licenses/>.
*/

#include <cassert>

#include <bh_component.hpp>
#include <bh_extmethod.hpp>
#include <bh_idmap.hpp>

#include "kernel.hpp"
#include "block.hpp"
#include "instruction.hpp"
#include "type.hpp"
#include "store.hpp"

using namespace bohrium;
using namespace component;
using namespace std;

namespace {
class Impl : public ComponentImpl {
  private:
    // Compiled kernels store
    Store _store;
    // Known extension methods
    map<bh_opcode, extmethod::ExtmethodFace> extmethods;
    //Allocated base arrays
    set<bh_base*> _allocated_bases;
    // Update the allocate bases and returns a set of instructions that creates new arrays
    // This function should be called at each BhIR execution
    set<bh_instruction*> update_allocated_bases(bh_ir *bhir);
  public:
    Impl(int stack_level) : ComponentImpl(stack_level), _store(config) {}
    ~Impl() {}; // NB: a destructor implementation must exist
    void execute(bh_ir *bhir);
    void extmethod(const string &name, bh_opcode opcode) {
        // ExtmethodFace does not have a default or copy constructor thus
        // we have to use its move constructor.
        extmethods.insert(make_pair(opcode, extmethod::ExtmethodFace(config, name)));
    }
};
}

extern "C" ComponentImpl* create(int stack_level) {
    return new Impl(stack_level);
}
extern "C" void destroy(ComponentImpl* self) {
    delete self;
}

namespace {
void spaces(stringstream &out, int num) {
    for (int i = 0; i < num; ++i) {
        out << " ";
    }
}
}

set<bh_instruction*> Impl::update_allocated_bases(bh_ir *bhir) {
    set<bh_instruction*> ret;
    for(bh_instruction &instr: bhir->instr_list) {
        bh_view *operands = bh_inst_operands(&instr);

        //Save all new base arrays
        int nop = bh_noperands(instr.opcode);
        for (bh_intp o = 0; o < nop; ++o) {
            if (!bh_is_constant(&operands[o])) {
                if (_allocated_bases.insert(operands[o].base).second and o == 0){
                    // The base was in fact a new output array
                    ret.insert(&instr);
                }
            }
        }

        //And remove freed arrays
        if (instr.opcode == BH_FREE) {
            bh_base *base = operands[0].base;
            if (_allocated_bases.erase(base) != 1) {
                cerr << "[UNI-VE] freeing unknown base array: " << *base << endl;
                throw runtime_error("[UNI-VE] freeing unknown base array");
            }
        }
    }
    return ret;
}

void write_block(const IdMap<bh_base*> &base_ids, const Block &block, stringstream &out) {
    if (block.isInstr()) {
        if (block._instr != NULL) {
            spaces(out, 4 + block.rank*4);
            write_instr(base_ids, *block._instr, out);
        }
    } else {
        spaces(out, 4 + block.rank*4);
        // If this block is sweeped, we will "peel" the for-loop such that the
        // sweep instruction is replaced with BH_IDENTITY in the first iteration
        if (block._sweeps.size() > 0) {
            Block peeled_block(block);
            vector<bh_instruction> sweep_instr_list(block._sweeps.size());
            {
                size_t i = 0;
                for (const bh_instruction *instr: block._sweeps) {
                    Block *sweep_instr_block = peeled_block.findInstrBlock(instr);
                    assert(sweep_instr_block != NULL);
                    bh_instruction *sweep_instr = &sweep_instr_list[i++];
                    sweep_instr->opcode = BH_IDENTITY;
                    sweep_instr->operand[1] = instr->operand[1]; // The input is the same as in the sweep
                    sweep_instr->operand[0] = instr->operand[0];
                    // But the output needs an extra dimension when we are reducing to a non-scalar
                    if (bh_opcode_is_reduction(instr->opcode) and instr->operand[1].ndim > 1) {
                        sweep_instr->operand[0].insert_dim(instr->constant.get_int64(), 1, 0);
                    }
                    sweep_instr_block->_instr = sweep_instr;
                }
            }
            string itername;
            {stringstream t; t << "i" << block.rank; itername = t.str();}
            out << "{ // Peeled loop, 1. iteration" << endl;
            spaces(out, 8 + block.rank*4);
            out << "uint64_t " << itername << " = 0;" << endl;
            for (const Block &b: peeled_block._block_list) {
                write_block(base_ids, b, out);
            }
            spaces(out, 4 + block.rank*4);
            out << "}" << endl;
            spaces(out, 4 + block.rank*4);
        }

        string itername;
        {stringstream t; t << "i" << block.rank; itername = t.str();}
        out << "for(uint64_t " << itername;
        if (block._sweeps.size() > 0) // If the for-loop has been peeled, we should that at 1
            out << "=1; ";
        else
            out << "=0; ";
        out << itername << " < " << block.size << "; ++" << itername << ") {" << endl;
        for (const Block &b: block._block_list) {
            write_block(base_ids, b, out);
        }
        spaces(out, 4 + block.rank*4);
        out << "}" << endl;
    }
}

vector<Block> fuser_singleton(vector<bh_instruction> &instr_list, const set<bh_instruction*> &news) {

    // Creates the block_list based on the instr_list
    vector<Block> block_list;
    for (auto instr=instr_list.begin(); instr != instr_list.end(); ++instr) {
        int nop = bh_noperands(instr->opcode);
        if (nop == 0)
            continue; // Ignore noop instructions such as BH_NONE or BH_TALLY

        vector<bh_instruction*> single_instr = {&instr[0]};
        assert(instr->dominating_shape().size() > (uint64_t)0);
        int64_t size_of_rank_dim = instr->dominating_shape()[0];
        block_list.push_back(create_nested_block(single_instr, 0, size_of_rank_dim, news));
    }
    return block_list;
}

// Check if 'a' and 'b' supports data-parallelism when merged
static bool data_parallel_compatible(const bh_instruction *a, const bh_instruction *b)
{
    if(bh_opcode_is_system(a->opcode) || bh_opcode_is_system(b->opcode))
        return true;

    const int a_nop = bh_noperands(a->opcode);
    for(int i=0; i<a_nop; ++i)
    {
        if(not bh_view_disjoint(&b->operand[0], &a->operand[i])
           && not bh_view_aligned(&b->operand[0], &a->operand[i]))
            return false;
    }
    const int b_nop = bh_noperands(b->opcode);
    for(int i=0; i<b_nop; ++i)
    {
        if(not bh_view_disjoint(&a->operand[0], &b->operand[i])
           && not bh_view_aligned(&a->operand[0], &b->operand[i]))
            return false;
    }
    return true;
}

// Check if 'b1' and 'b2' supports data-parallelism when merged
static bool data_parallel_compatible(const Block &b1, const Block &b2) {
    for (const bh_instruction *i1 : b1.getAllInstr()) {
        for (const bh_instruction *i2 : b2.getAllInstr()) {
            if (not data_parallel_compatible(i1, i2))
                return false;
        }
    }
    return true;
}

vector<Block> fuser_serial(vector<Block> &block_list, const set<bh_instruction*> &news) {
    vector<Block> ret;
    for (auto it = block_list.begin(); it != block_list.end(); ) {
        ret.push_back(*it);
        Block &cur = ret.back();
        ++it;
        if (cur.isInstr()) {
            continue; // We should never fuse instruction blocks
        }
        // Let's search for fusible blocks
        for (; it != block_list.end(); ++it) {
            if (it->isInstr())
                break;
            if (not data_parallel_compatible(cur, *it))
                break;
            if (cur._sweeps.size() > 0) //TODO: support merge of reduction
                break;
            assert(cur.rank == it->rank);

            // Check for perfect match, which is directly mergeable
            if (cur.size == it->size) {
                cur = merge(cur, *it);
                continue;
            }
            // Check fusibility of reshapable blocks
            if (it->_reshapable && it->size % cur.size == 0) {
                vector<bh_instruction *> cur_instr = cur.getAllInstr();
                vector<bh_instruction *> it_instr = it->getAllInstr();
                cur_instr.insert(cur_instr.end(), it_instr.begin(), it_instr.end());
                Block b = create_nested_block(cur_instr, it->rank, cur.size, news);
                assert(b.size == cur.size);
                cur = b;
                continue;
            }
            if (cur._reshapable && cur.size % it->size == 0) {
                vector<bh_instruction *> cur_instr = cur.getAllInstr();
                vector<bh_instruction *> it_instr = it->getAllInstr();
                cur_instr.insert(cur_instr.end(), it_instr.begin(), it_instr.end());
                Block b = create_nested_block(cur_instr, cur.rank, it->size, news);
                assert(b.size == it->size);
                cur = b;
                continue;
            }

            // We couldn't find any shape match
            break;
        }
        // Let's fuse at the next rank level
        cur._block_list = fuser_serial(cur._block_list, news);
    }
    return ret;
}

vector<Block> remove_empty_blocks(vector<Block> &block_list) {
    vector<Block> ret;
    for (Block &b: block_list) {
        if (b.getAllInstr().size() > 0 and not b.isSystemOnly())
            ret.push_back(b);
    }
    return ret;
}

void Impl::execute(bh_ir *bhir) {

    // Get the set of new arrays in 'bhir'
    const set<bh_instruction*> news = update_allocated_bases(bhir);

    // Assign IDs to all base arrays
    IdMap<bh_base *> base_ids;
    // NB: by assigning the IDs in the order they appear in the 'instr_list',
    //     the kernels can better be reused
    for(const bh_instruction &instr: bhir->instr_list) {
        const int nop = bh_noperands(instr.opcode);
        for(int i=0; i<nop; ++i) {
            const bh_view &v = instr.operand[i];
            if (not bh_is_constant(&v)) {
                base_ids.insert(v.base);
            }
        }
    }
    // Do we have anything to do?
    if (base_ids.size() == 0)
        return;

    //Let's create a kernel
    Kernel kernel;
    {
        // Let's fuse the 'instr_list' into blocks
        kernel.block_list = fuser_singleton(bhir->instr_list, news);
        kernel.block_list = fuser_serial(kernel.block_list, news);
        kernel.block_list = remove_empty_blocks(kernel.block_list);

        // And fill kernel attributes
        for (bh_instruction &instr: bhir->instr_list) {
            if (instr.opcode == BH_RANDOM) {
                kernel.useRandom = true;
            } else if (instr.opcode == BH_FREE) {
                kernel.frees.insert(instr.operand[0].base);
            }
        }
    }

    // Do we even have any "real" operations to perform?
    if (kernel.block_list.size() == 0) {
        // Finally, let's cleanup
        for(bh_base *base: kernel.frees) {
            bh_data_free(base);
        }
        return;
    }

    // Debug print
    //cout << kernel.block_list;

    // Code generation
    stringstream ss;

    // Make sure all arrays are allocated
    for(bh_base *base: base_ids.getKeys()) {
        bh_data_malloc(base);
    }

    // Write the need includes
    ss << "#include <stdint.h>" << endl;
    ss << "#include <stdlib.h>" << endl;
    ss << "#include <stdbool.h>" << endl;
    ss << "#include <complex.h>" << endl;
    ss << "#include <tgmath.h>" << endl;
    ss << "#include <math.h>" << endl;
    ss << "#include <bh_memory.h>" << endl;
    ss << "#include <bh_type.h>" << endl;
    ss << endl;

    if (kernel.useRandom) { // Write the random function
        ss << "#include <Random123/philox.h>" << endl;
        ss << "uint64_t random123(uint64_t start, uint64_t key, uint64_t index) {" << endl;
        ss << "    union {philox2x32_ctr_t c; uint64_t ul;} ctr, res; " << endl;
        ss << "    ctr.ul = start + index; " << endl;
        ss << "    res.c = philox2x32(ctr.c, (philox2x32_key_t){{key}}); " << endl;
        ss << "    return res.ul; " << endl;
        ss << "} " << endl;
    }
    ss << endl;

    // Write the header of the execute function
    ss << "void execute(";
    for(size_t id=0; id < base_ids.size(); ++id) {
        const bh_base *b = base_ids.getKeys()[id];
        ss << write_type(b->type) << " a" << id << "[]";
        if (id+1 < base_ids.size()) {
            ss << ", ";
        }
    }
    ss << ") {" << endl;

    // Write the blocks that makes up the body of 'execute()'
    for(const Block &block: kernel.block_list) {
        write_block(base_ids, block, ss);
    }

    ss << "}" << endl << endl;

    // Write the launcher function, which will convert the data_list of void pointers
    // to typed arrays and call the execute function
    {
        ss << "void launcher(void* data_list[]) {" << endl;
        size_t i=0;
        for (bh_base *b: base_ids.getKeys()) {
            spaces(ss, 4);
            ss << write_type(b->type) << " *a" << base_ids[b];
            ss << " = data_list[" << i << "];" << endl;
            ++i;
        }
        spaces(ss, 4);
        ss << "execute(";
        for(size_t id=0; id < base_ids.size(); ++id) {
            ss << "a" << id;
            if (id+1 < base_ids.size()) {
                ss << ", ";
            }
        }
        ss << ");" << endl;
        ss << "}" << endl;
    }

  //  cout << ss.str();

    KernelFunction func = _store.getFunction(ss.str());
    assert(func != NULL);

    // Create a 'data_list' of data pointers
    vector<void*> data_list;
    data_list.reserve(base_ids.size());
    for(bh_base *base: base_ids.getKeys()) {
        assert(base->data != NULL);
        data_list.push_back(base->data);
    }

    // Call the launcher function with the 'data_list', which will execute the kernel
    func(&data_list[0]);
    // Finally, let's cleanup
    for(bh_base *base: kernel.frees) {
        bh_data_free(base);
    }
}

