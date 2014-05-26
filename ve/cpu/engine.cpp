#include "engine.hpp"
#include "symbol_table.hpp"
#include "dag.hpp"
#include "timevault.hpp"

#include <algorithm>
#include <set>
#include <iomanip>

using namespace std;
using namespace bohrium::core;

namespace bohrium{
namespace engine {
namespace cpu {

const char Engine::TAG[] = "Engine";

Engine::Engine(
    const string compiler_cmd,
    const string template_directory,
    const string kernel_directory,
    const string object_directory,
    const size_t vcache_size,
    const bool preload,
    const bool jit_enabled,
    const bool jit_fusion,
    const bool jit_dumpsrc,
    const bool dump_rep)
: compiler_cmd(compiler_cmd),
    template_directory(template_directory),
    kernel_directory(kernel_directory),
    object_directory(object_directory),
    vcache_size(vcache_size),
    preload(preload),
    jit_enabled(jit_enabled),
    jit_fusion(jit_fusion),
    jit_dumpsrc(jit_dumpsrc),
    storage(object_directory, kernel_directory),
    specializer(template_directory),
    compiler(compiler_cmd),
    exec_count(0),
    dump_rep(dump_rep)
{
    bh_vcache_init(vcache_size);    // Victim cache
    if (preload) {
        storage.preload();
    }
}

Engine::~Engine()
{   
    if (vcache_size>0) {    // De-allocate the malloc-cache
        bh_vcache_clear();
        bh_vcache_delete();
    }
}

string Engine::text()
{
    stringstream ss;
    ss << "ENVIRONMENT {" << endl;
    ss << "  BH_CORE_VCACHE_SIZE="      << this->vcache_size  << endl;
    ss << "  BH_VE_CPU_PRELOAD="        << this->preload      << endl;    
    ss << "  BH_VE_CPU_JIT_ENABLED="    << this->jit_enabled  << endl;    
    ss << "  BH_VE_CPU_JIT_FUSION="     << this->jit_fusion   << endl;
    ss << "  BH_VE_CPU_JIT_DUMPSRC="    << this->jit_dumpsrc  << endl;
    ss << "}" << endl;
    
    ss << "Attributes {" << endl;
    ss << "  " << specializer.text();    
    ss << "  " << compiler.text();
    ss << "}" << endl;

    return ss.str();    
}

bh_error Engine::sij_mode(SymbolTable& symbol_table, vector<tac_t>& program, Block& block)
{
    DEBUG(TAG, "sij_mode(...)");
    bh_error res = BH_SUCCESS;

    tac_t& tac = block.tac(0);
    DEBUG(TAG, tac_text(tac));

    switch(tac.op) {
        case NOOP:
            break;

        case SYSTEM:
            switch(tac.oper) {
                case DISCARD:
                case SYNC:
                    break;

                case FREE:
                    res = bh_vcache_free_base(symbol_table[tac.out].base);
                    if (BH_SUCCESS != res) {
                        fprintf(stderr, "Unhandled error returned by bh_vcache_free_base(...) "
                                        "called from Engine::sij_mode)\n");
                        return res;
                    }
                    break;

                default:
                    fprintf(stderr, "Yeah that does not fly...\n");
                    return BH_ERROR;
            }
            break;

        case EXTENSION:
            {
                map<bh_opcode,bh_extmethod_impl>::iterator ext;
                ext = extensions.find(static_cast<bh_instruction*>(tac.ext)->opcode);
                if (ext != extensions.end()) {
                    bh_extmethod_impl extmethod = ext->second;
                    res = extmethod(static_cast<bh_instruction*>(tac.ext), NULL);
                    if (BH_SUCCESS != res) {
                        fprintf(stderr, "Unhandled error returned by extmethod(...) \n");
                        return res;
                    }
                }
            }
            break;

        // Array operations (MAP | ZIP | REDUCE | SCAN)
        case MAP:
        case ZIP:
        case GENERATE:
        case REDUCE:
        case SCAN:

            //
            // We start by creating a symbol for the block
            block.symbolize();
            //
            // JIT-compile the block if enabled
            if (jit_enabled && \
                (!storage.symbol_ready(block.symbol()))) {   
                                                            // Specialize sourcecode
                string sourcecode = specializer.specialize(symbol_table, block, 0, 0);
                if (jit_dumpsrc==1) {                       // Dump sourcecode to file                
                    core::write_file(
                        storage.src_abspath(block.symbol()),
                        sourcecode.c_str(), 
                        sourcecode.size()
                    );
                }
                TIMER_START
                // Send to compiler
                bool compile_res = compiler.compile(
                    storage.obj_abspath(block.symbol()), 
                    sourcecode.c_str(), 
                    sourcecode.size()
                );
                TIMER_STOP("Compiling (SIJ Kernels)")
                if (!compile_res) {
                    fprintf(stderr, "Engine::sij_mode(...) == Compilation failed.\n");
                    return BH_ERROR;
                }
                                                            // Inform storage
                storage.add_symbol(block.symbol(), storage.obj_filename(block.symbol()));
            }

            //
            // Load the compiled code
            //
            if ((!storage.symbol_ready(block.symbol())) && \
                (!storage.load(block.symbol()))) {                // Need but cannot load

                fprintf(stderr, "Engine::sij_mode(...) == Failed loading object.\n");
                return BH_ERROR;
            }

            //
            // Allocate memory for operands
            res = bh_vcache_malloc_base(symbol_table[tac.out].base);
            if (BH_SUCCESS != res) {
                fprintf(stderr, "Unhandled error returned by bh_vcache_malloc_base() "
                                "called from Engine::sij_mode\n");
                return res;
            }

            //
            // Execute block handling array operations.
            // 
            storage.funcs[block.symbol()](block.operands());

            break;
    }
    DEBUG(TAG, "sij_mode();");
    return BH_SUCCESS;
}

bh_error Engine::fuse_mode(SymbolTable& symbol_table, std::vector<tac_t>& program,
                    Dag& graph, size_t subgraph_idx, Block& block)
{
    DEBUG(TAG, "fuse_mode(...)");
    DEBUG(TAG, "fuse_mode(...) : instructions...");
    for(size_t tac_idx=0; tac_idx < block.ntacs(); ++tac_idx) {
        DEBUG(TAG, tac_text(block.tac(tac_idx)));
    }

    bh_error res = BH_SUCCESS;

    TIMER_START
    //
    // Determine ranges of operations which can be fused together
    vector<triplet_t> ranges;

    size_t range_begin  = 0,    // Current range
           range_end    = 0;

    LAYOUT fusion_layout = CONSTANT;
    LAYOUT next_layout   = CONSTANT;

    tac_t* first = &block.tac(0);
    for(size_t tac_idx=0; tac_idx<block.ntacs(); ++tac_idx) {
        range_end = tac_idx;
        tac_t& next = block.tac(tac_idx);

        //
        // Ignore these
        if ((next.op == SYSTEM) && (next.op == NOOP)) {
            continue;
        }

        //
        // Check for compatible operations
        if (!((next.op == MAP) || (next.op == ZIP))) {
            //
            // Got an instruction that we currently do not fuse,
            // store the current range and start a new.

            // Add the range up until this tac
            if (range_begin < range_end) {
                ranges.push_back((triplet_t){range_begin,   range_end-1,    fusion_layout});
                // Add a range for the single tac   
                ranges.push_back((triplet_t){range_end,     range_end,      fusion_layout});
            } else {
                ranges.push_back((triplet_t){range_begin,   range_begin,    fusion_layout});
            }
            range_begin = tac_idx+1;
            if (range_begin < block.ntacs()) {
                first = &block.tac(range_begin);
            }
            continue;
        }

        //
        // Check for compatible operands and note layout.
        bool compat_operands = true;
        switch(core::tac_noperands(next)) {
            case 3:
                // Second input
                next_layout = symbol_table[next.in2].layout;
                if (next_layout>fusion_layout) {
                    fusion_layout = next_layout;
                }
                compat_operands = compat_operands && (core::compatible(
                    symbol_table[first->out],
                    symbol_table[next.in2]
                ));
            case 2:
                // First input
                next_layout = symbol_table[next.in1].layout;
                if (next_layout>fusion_layout) {
                    fusion_layout = next_layout;
                }
                compat_operands = compat_operands && (core::compatible(
                    symbol_table[first->out],
                    symbol_table[next.in1]
                ));

                // Output operand
                next_layout = symbol_table[next.out].layout;
                if (next_layout>fusion_layout) {
                    fusion_layout = next_layout;
                }
                compat_operands = compat_operands && (core::compatible(
                    symbol_table[first->out],
                    symbol_table[next.out]
                ));
                break;

            default:
                fprintf(stderr, "ARGGG in checking operands!!!!\n");
        }
        if (!compat_operands) {
            //
            // Incompatible operands.
            // Store the current range and start a new.

            // Add the range up until this tac
            if (range_begin < range_end) {
                ranges.push_back((triplet_t){range_begin, range_end-1, fusion_layout});
            } else {
                ranges.push_back((triplet_t){range_begin, range_begin, fusion_layout});
            }

            range_begin = tac_idx;
            if (range_begin < block.ntacs()) {
                first = &block.tac(range_begin);
            }
            continue;
        }
    }
    //
    // Store the last range
    if (range_begin<block.ntacs()) {
        ranges.push_back((triplet_t){range_begin, block.ntacs()-1, fusion_layout});
    }
    TIMER_STOP("Determine fuse-ranges.")
   
    TIMER_START

    DEBUG(TAG, "fuse_mode(...) : scalar replacement...");
    //
    // Determine arrays suitable for scalar-replacement in the fuse-ranges.
    for(vector<triplet_t>::iterator it=ranges.begin();
        it!=ranges.end();
        it++) {
        vector<size_t> inputs, outputs;
        set<size_t> all_operands;
        
        range_begin = (*it).begin;
        range_end   = (*it).end;

        //
        // Ref-count within the range
        for(size_t tac_idx=range_begin; tac_idx<=range_end; ++tac_idx) {
            tac_t& tac = block.tac(tac_idx);
            switch(core::tac_noperands(tac)) {
                case 3:
                    if (tac.in2 != tac.in1) {
                        inputs.push_back(tac.in2);
                        all_operands.insert(tac.in2);
                    }
                case 2:
                    inputs.push_back(tac.in1);
                    all_operands.insert(tac.in1);
                case 1:
                    outputs.push_back(tac.out);
                    all_operands.insert(tac.out);
                    break;
                default:
                    cout << "ARGGG in scope-ref-count on: " << core::tac_text(tac) << endl;
            }
        }

        //
        // Turn the operand into a scalar
        for(set<size_t>::iterator opr_it=all_operands.begin();
            opr_it!=all_operands.end();
            opr_it++) {
            size_t operand = *opr_it;
            if ((count(inputs.begin(), inputs.end(), operand) == 1) && \
                (count(outputs.begin(), outputs.end(), operand) == 1) && \
                (symbol_table.temp().find(operand) != symbol_table.temp().end())) {
                //symbol_table.turn_scalar(operand);
                DEBUG(TAG, "Turning " << operand << " scalar.");
                //
                // TODO: Remove from inputs and/or outputs.
            }
        }
    }
    TIMER_STOP("Scalar replacement in fuse-ranges.")
    
    //
    // The operands might have been modified at this point, so we need to create a new symbol.
    if (!block.symbolize()) {
        fprintf(stderr, "Engine::execute(...) == Failed creating symbol.\n");
        return BH_ERROR;
    }

    DEBUG(TAG, "fuse_mode(...) : symbol(" << block.symbol() << ")");
    //
    // JIT-compile the block if enabled
    //
    DEBUG(TAG, "fuse_mode(...) : compilation...");
    if (jit_enabled && \
        ((graph.omask(subgraph_idx) & (ARRAY_OPS)) >0) && \
        (!storage.symbol_ready(block.symbol()))) {   
                                                    // Specialize sourcecode
        DEBUG(TAG, "fuse_mode(...) : specializing...");
        string sourcecode = specializer.specialize(symbol_table, block, ranges);
        if (jit_dumpsrc==1) {                       // Dump sourcecode to file                
            DEBUG(TAG, "fuse_mode(...) : writing source...");
            core::write_file(
                storage.src_abspath(block.symbol()),
                sourcecode.c_str(), 
                sourcecode.size()
            );
        }
        TIMER_START
        // Send to compiler
        DEBUG(TAG, "fuse_mode(...) : compiling...");
        bool compile_res = compiler.compile(
            storage.obj_abspath(block.symbol()),
            sourcecode.c_str(), 
            sourcecode.size()
        );
        TIMER_STOP("Compiling (Fused Kernels)")
        if (!compile_res) {
            fprintf(stderr, "Engine::execute(...) == Compilation failed.\n");

            return BH_ERROR;
        }
                                                    // Inform storage
        storage.add_symbol(block.symbol(), storage.obj_filename(block.symbol()));
    }

    //
    // Load the compiled code
    //
    DEBUG(TAG, "fuse_mode(...) : load compiled code...");
    if (((graph.omask(subgraph_idx) & (ARRAY_OPS)) >0) && \
        (!storage.symbol_ready(block.symbol())) && \
        (!storage.load(block.symbol()))) {// Need but cannot load

        fprintf(stderr, "Engine::execute(...) == Failed loading object.\n");
        return BH_ERROR;
    }

    //
    // Allocate memory for output
    //
    DEBUG(TAG, "fuse_mode(...) : allocate memory...");
    for(size_t i=0; i<block.ntacs(); ++i) {
        if ((block.tac(i).op & ARRAY_OPS)>0) {

            TIMER_START
            operand_t& operand = symbol_table[block.tac(i).out];
            if ((operand.layout == SCALAR) && \
                (operand.base->data == NULL)) {
                operand.base->nelem = 1;
            }
            bh_error res = bh_vcache_malloc_base(operand.base);
            if (BH_SUCCESS != res) {
                fprintf(stderr, "Unhandled error returned by bh_vcache_malloc() "
                                "called from bh_ve_cpu_execute()\n");
                return res;
            }
            TIMER_STOP("Allocating memory.")
        }
    }

    DEBUG(TAG, "Operands");
    for(size_t i=0; i<block.noperands(); i++) {
        DEBUG(TAG, operand_text(block.operand(i)));
    }

    //
    // Execute block handling array operations.
    // 
    TIMER_START
    DEBUG(TAG, "fuse_mode(...) : execute(" << block.symbol() << ")");
    storage.funcs[block.symbol()](block.operands());
    TIMER_STOP(block.symbol())

    //
    // De-Allocate operand memory
    DEBUG(TAG, "fuse_mode(...) : de-allocate...");
    for(size_t i=0; i<block.ntacs(); ++i) {
        tac_t& tac = block.tac(i);
        if (tac.oper == FREE) {
            TIMER_START
            res = bh_vcache_free_base(symbol_table[tac.out].base);
            if (BH_SUCCESS != res) {
                fprintf(stderr, "Unhandled error returned by bh_vcache_free(...) "
                                "called from bh_ve_cpu_execute)\n");
                return res;
            }
            TIMER_STOP("Deallocating memory.")
        }
    }

    DEBUG(TAG, "fuse_mode(...);");
    return BH_SUCCESS;
}

bh_error Engine::execute(bh_instruction* instrs, bh_intp ninstrs)
{
    DEBUG(TAG, "execute(...)");
    exec_count++;

    bh_error res = BH_SUCCESS;

    //
    // Instantiate the symbol-table and tac-program
    SymbolTable symbol_table(ninstrs*6+2);              // SymbolTable
    vector<tac_t> program(ninstrs);                     // Program

    // Map instructions to tac and symbol_table
    instrs_to_tacs(instrs, ninstrs, program, symbol_table);
    symbol_table.count_tmp();

    //
    // Construct graph with instructions as nodes.
    Dag graph(symbol_table, program);                   // Graph

    if (dump_rep) {                                     // Dump it to file
        stringstream filename;
        filename << "graph" << exec_count << ".dot";

        std::ofstream fout(filename.str());
        fout << graph.dot() << std::endl;
    }

    //
    //  Map subgraphs to blocks one at a time and execute them.
    Block block(symbol_table, program);                 // Block
    for(size_t subgraph_idx=0; subgraph_idx<graph.subgraphs().size(); ++subgraph_idx) {
        Graph& subgraph = *(graph.subgraphs()[subgraph_idx]);

        // FUSE_MODE
        if (jit_fusion && \
            ((graph.omask(subgraph_idx) & (NON_FUSABLE))==0) && \
            ((graph.omask(subgraph_idx) & (ARRAY_OPS)) > 0)) {
            block.clear();
            block.compose(subgraph);

            fuse_mode(symbol_table, program, graph, subgraph_idx, block);
        } else {
        // SIJ_MODE
            std::pair<vertex_iter, vertex_iter> vip = vertices(subgraph);
            for(vertex_iter vi = vip.first; vi != vip.second; ++vi) {
                // Compose the block
                block.clear();
                block.compose(  
                    subgraph.local_to_global(*vi), subgraph.local_to_global(*vi)
                );
                
                // Generate/Load code and execute it
                sij_mode(symbol_table, program, block);
            }
        }
    }
    DEBUG(TAG, "Execute(...);");
    
    return res;
}

bh_error Engine::register_extension(bh_component& instance, const char* name, bh_opcode opcode)
{
    bh_extmethod_impl extmethod;
    bh_error err = bh_component_extmethod(&instance, name, &extmethod);
    if (err != BH_SUCCESS) {
        return err;
    }

    if (extensions.find(opcode) != extensions.end()) {
        fprintf(stderr, "[CPU-VE] Warning, multiple registrations of the same"
               "extension method '%s' (opcode: %d)\n", name, (int)opcode);
    }
    extensions[opcode] = extmethod;

    return BH_SUCCESS;
}

}}}
