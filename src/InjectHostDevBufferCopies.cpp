#include "InjectHostDevBufferCopies.h"

#include "CodeGen_GPU_Dev.h"
#include "Debug.h"
#include "IRMutator.h"
#include "IROperator.h"
#include "IRPrinter.h"
#include "Substitute.h"

#include <map>

namespace Halide {
namespace Internal {

using std::string;
using std::map;
using std::vector;
using std::set;
using std::pair;

Stmt call_extern_and_assert(const string& name, const vector<Expr>& args) {
    Expr call = Call::make(Int(32), name, args, Call::Extern);
    string call_result_name = unique_name(name + "_result");
    Expr call_result_var = Variable::make(Int(32), call_result_name);
    return LetStmt::make(call_result_name, call,
                         AssertStmt::make(EQ::make(call_result_var, 0), call_result_var));
}

namespace {

Expr make_device_interface_call(DeviceAPI device_api) {
    std::string interface_name;
    switch (device_api) {
    case DeviceAPI::CUDA:
        interface_name = "halide_cuda_device_interface";
        break;
    case DeviceAPI::OpenCL:
        interface_name = "halide_opencl_device_interface";
        break;
    case DeviceAPI::Metal:
        interface_name = "halide_metal_device_interface";
        break;
    case DeviceAPI::GLSL:
        interface_name = "halide_opengl_device_interface";
        break;
    case DeviceAPI::OpenGLCompute:
        interface_name = "halide_openglcompute_device_interface";
        break;
    case DeviceAPI::Hexagon:
        interface_name = "halide_hexagon_device_interface";
        break;
    default:
        internal_error << "Bad DeviceAPI " << static_cast<int>(device_api) << "\n";
        break;
    }
    return Call::make(type_of<const char *>(), interface_name, {}, Call::Extern);
}


class FindBufferUsage : public IRVisitor {
    using IRVisitor::visit;

    void visit(const Variable *op) {
        if (is_buffer_var(op)) {
            // Passing the buffer variable out of Halide counts
            // as a read/write.
            devices_touched.insert(current_device_api);
            devices_reading.insert(current_device_api);
            devices_writing.insert(current_device_api);
        }
    }

    void visit(const Load *op) {
        IRVisitor::visit(op);
        if (op->name == buffer) {
            devices_touched.insert(current_device_api);
            devices_reading.insert(current_device_api);
        }
    }

    void visit(const Store *op) {
        IRVisitor::visit(op);
        if (op->name == buffer) {
            devices_touched.insert(current_device_api);
            devices_writing.insert(current_device_api);
        }
    }

    bool is_buffer_var(Expr e) {
        const Variable *var = e.as<Variable>();
        return var && (var->name == buffer + ".buffer");
    }

    void visit(const Call *op) {
        if (op->is_intrinsic(Call::image_load)) {
            internal_assert(op->args.size() >= 1);
            if (is_buffer_var(op->args[1])) {
                devices_reading.insert(current_device_api);
                devices_touched.insert(current_device_api);
            }
            for (size_t i = 0; i < op->args.size(); i++) {
                if (i == 1) continue;
                op->args[i].accept(this);
            }
        } else if (op->is_intrinsic(Call::image_store)) {
            internal_assert(op->args.size() >= 1);
            if (is_buffer_var(op->args[1])) {
                devices_reading.insert(current_device_api);
                devices_touched.insert(current_device_api);
            }
            for (size_t i = 0; i < op->args.size(); i++) {
                if (i == 1) continue;
                op->args[i].accept(this);
            }
        } else if (op->call_type == Call::Extern && op->func.defined()) {
            // This is a call to an extern stage
            Function f(op->func);

            internal_assert((f.extern_arguments().size() + f.outputs()) == op->args.size()) <<
                "Mismatch between args size and extern_arguments size in call to " << op->name << "\n";

            // Check each buffer arg
            for (size_t i = 0; i < op->args.size(); i++) {
                if (is_buffer_var(op->args[i])) {
                    DeviceAPI extern_device_api = f.extern_function_device_api();
                    devices_touched.insert(extern_device_api);
                    if (i < f.extern_arguments().size()) {
                        // An input
                        devices_reading.insert(extern_device_api);
                    } else {
                        // An output
                        devices_writing.insert(extern_device_api);
                    }
                } else {
                    op->args[i].accept(this);
                }
            }
        } else {
            IRVisitor::visit(op);
        }
    }

    void visit(const For *op) {
        internal_assert(op->device_api != DeviceAPI::Default_GPU)
            << "A GPU API should have been selected by this stage in lowering\n";
        DeviceAPI old = current_device_api;
        if (op->device_api != DeviceAPI::None) {
            current_device_api = op->device_api;
        }
        IRVisitor::visit(op);
        current_device_api = old;
    }

    string buffer;
    DeviceAPI current_device_api;
public:
    std::set<DeviceAPI> devices_reading, devices_writing, devices_touched;

    bool same_usage(const FindBufferUsage &other) const {
        return
            ((devices_reading == other.devices_reading) &&
             (devices_writing == other.devices_writing) &&
             (devices_touched == other.devices_touched));
    }

    FindBufferUsage(const std::string &buf, DeviceAPI d) : buffer(buf), current_device_api(d) {}
};

class InjectBufferCopiesForSingleBuffer : public IRMutator {
    using IRMutator::visit;

    // The buffer being managed
    string buffer;

    enum FlagState {
        Unknown,
        False,
        True
    };

    struct State {
        // What do we know about the dirty flags and the existence of a device allocation?
        FlagState device_dirty = Unknown, host_dirty = Unknown, device_allocation_exists = Unknown;

        // If it exists on a known device API, which device does it exist
        // on? Meaningless if device_allocation_exists is not True.
        DeviceAPI current_device = DeviceAPI::None;

        void union_with(const State &other) {
            if (device_dirty != other.device_dirty) {
                device_dirty = Unknown;
            }
            if (host_dirty != other.host_dirty) {
                host_dirty = Unknown;
            }
            if (device_allocation_exists != other.device_allocation_exists ||
                other.current_device != current_device) {
                device_allocation_exists = Unknown;
                current_device = DeviceAPI::None;
            }
        }
    } state;

    Expr buffer_var() {
        return Variable::make(type_of<struct halide_buffer_t *>(), buffer + ".buffer");
    }

    Stmt make_device_malloc(DeviceAPI target_device_api) {
        Expr device_interface = make_device_interface_call(target_device_api);
        Stmt device_malloc = call_extern_and_assert("halide_device_malloc",
                                                    {buffer_var(), device_interface});
        return device_malloc;
    }

    Stmt make_copy_to_host() {
        return call_extern_and_assert("halide_copy_to_host", {buffer_var()});
    }

    Stmt make_copy_to_device(DeviceAPI target_device_api) {
        Expr device_interface = make_device_interface_call(target_device_api);
        return call_extern_and_assert("halide_copy_to_device", {buffer_var(), device_interface});
    }

    Stmt make_host_dirty() {
        return Evaluate::make(Call::make(Int(32), Call::buffer_set_host_dirty,
                                         {buffer_var(), const_true()}, Call::Extern));
    }

    Stmt make_device_dirty() {
        return Evaluate::make(Call::make(Int(32), Call::buffer_set_device_dirty,
                                         {buffer_var(), const_true()}, Call::Extern));
    }

    Stmt do_copies(Stmt s) {
        // Sniff what happens to the buffer inside the stmt
        FindBufferUsage finder(buffer, DeviceAPI::Host);
        s.accept(&finder);

        // Insert any appropriate copies/allocations before, and set
        // dirty flags after. Do not recurse into the stmt.

        // First figure out what happened
        bool touched_on_host = finder.devices_touched.count(DeviceAPI::Host);
        bool touched_on_device = finder.devices_touched.size() > (touched_on_host ? 1 : 0);
        //bool read_on_host = finder.devices_reading.count(DeviceAPI::Host);
        //bool read_on_device = finder.devices_reading.size() > (touched_on_host ? 1 : 0);
        bool written_on_host = finder.devices_writing.count(DeviceAPI::Host);
        bool written_on_device = finder.devices_writing.size() > (touched_on_host ? 1 : 0);

        DeviceAPI touching_device = DeviceAPI::None;
        for (DeviceAPI d : finder.devices_touched) {
            if (d == DeviceAPI::Host) continue;
            internal_assert(touching_device == DeviceAPI::None)
                << "Buffer " << buffer << " was touched on multiple devices within a single leaf Stmt!\n";
            touching_device = d;
        }

        // Then figure out what to do
        bool needs_device_malloc = (written_on_device &&
                                    (state.device_allocation_exists != True));

        // TODO: If only written on device, and entirely clobbered on
        // device, a copy-to-device is not actually necessary.
        bool needs_copy_to_device = (touched_on_device &&
                                     ((state.host_dirty != False) ||
                                      state.current_device != touching_device));

        bool needs_copy_to_host = (touched_on_host &&
                                   (state.device_dirty != False));

        bool needs_host_dirty = (written_on_host &&
                                 (state.host_dirty != True));

        bool needs_device_dirty = (written_on_device &&
                                   (state.device_dirty != True));

        // Then do it, updating what we know about the buffer
        if (needs_copy_to_host) {
            s = Block::make(make_copy_to_host(), s);
            state.device_dirty = False;
        }

        if (needs_copy_to_device) {
            s = Block::make(make_copy_to_device(touching_device), s);
            state.host_dirty = False;
            state.current_device = touching_device;
        }

        if (needs_host_dirty) {
            s = Block::make(s, make_host_dirty());
            state.host_dirty = True;
        }

        if (needs_device_dirty) {
            s = Block::make(s, make_device_dirty());
            state.device_dirty = True;
        }

        if (needs_device_malloc) {
            s = Block::make(make_device_malloc(touching_device), s);
            state.device_allocation_exists = True;
            state.current_device = touching_device;
        }

        return s;
    }

    // We want to break things down into a serial sequence of leaf
    // stmts, and possibly do copies and update state around each
    // leaf.

    void visit(const For *op) {
        // All copies happen at the same loop level as the allocation
        stmt = do_copies(op);
    }

    void visit(const Evaluate *op) {
        stmt = do_copies(op);
    }

    void visit(const LetStmt *op) {
        // Could be a letstmt that calls an extern stage, wrapping an AssertStmt that checks the result
        Stmt body = mutate(op->body);
        if (body.same_as(op->body)) {
            stmt = do_copies(op);
        } else {
            stmt = LetStmt::make(op->name, op->value, body);
        }
    }

    void visit(const AssertStmt *op) {
        stmt = do_copies(op);
    }

    void visit(const Block *op) {
        // If both sides of the block have the same usage pattern,
        // treat it as a single leaf.
        FindBufferUsage finder_first(buffer, DeviceAPI::Host);
        FindBufferUsage finder_rest(buffer, DeviceAPI::Host);
        op->first.accept(&finder_first);
        op->rest.accept(&finder_rest);
        if (finder_first.same_usage(finder_rest)) {
            stmt = do_copies(op);
        } else {
            IRMutator::visit(op);
        }
    }

    void visit(const Fork *op) {
        // If both sides of the fork have the same usage pattern,
        // treat it as a single leaf.
        FindBufferUsage finder_first(buffer, DeviceAPI::Host);
        FindBufferUsage finder_rest(buffer, DeviceAPI::Host);
        op->first.accept(&finder_first);
        op->rest.accept(&finder_rest);
        if (finder_first.same_usage(finder_rest)) {
            stmt = do_copies(op);
        } else {
            // The sides could run in any order, so just be maximally
            // conservative for now and forget everything we know at
            // every point.
            state = State{};
            Stmt first = mutate(op->first);
            state = State{};
            Stmt rest = mutate(op->rest);
            state = State{};
            stmt = Fork::make(first, rest);
        }
    }

    void visit(const Store *op) {
        stmt = do_copies(op);
    }

    void visit(const IfThenElse *op) {
        State old = state;
        Stmt then_case = mutate(op->then_case);
        State then_state = state;
        state = old;
        Stmt else_case = mutate(op->else_case);
        state.union_with(then_state);
        stmt = IfThenElse::make(op->condition, then_case, else_case);
    }

public:
    InjectBufferCopiesForSingleBuffer(const std::string &b, bool is_external) : buffer(b) {
        if (is_external) {
            // The state of the buffer is totally unknown, which is
            // the default constructor for this->state
        } else {
            // This is a fresh allocation
            state.device_allocation_exists = False;
            state.device_dirty = False;
            state.host_dirty = False;
            state.current_device = DeviceAPI::None;
        }
    }
};

class FindInputsAndOutputs : public IRVisitor {
    using IRVisitor::visit;

    void include(const Parameter &p) {
        if (p.defined()) {
            result.insert(p.name());
        }
    }

    void include(const Buffer<> &b) {
        if (b.defined()) {
            result.insert(b.name());
        }
    }

    void visit(const Variable *op) {
        include(op->param);
    }

    void visit(const Load *op) {
        include(op->param);
        include(op->image);
        IRVisitor::visit(op);
    }

    void visit(const Store *op) {
        include(op->param);
        IRVisitor::visit(op);
    }

public:
    set<string> result;
};

class InjectBufferCopies : public IRMutator {
    using IRMutator::visit;

    // Inject the registration of a device destructor just after the
    // .buffer symbol is defined (which is safely before the first
    // device_malloc).
    class InjectDeviceDestructor : public IRMutator {
        using IRMutator::visit;

        void visit(const LetStmt *op) {
            if (op->name == buffer) {
                Expr buf = Variable::make(type_of<struct halide_buffer_t *>(), buffer);
                Stmt destructor =
                    Evaluate::make(Call::make(Int(32), Call::register_destructor,
                                              {Expr("halide_device_free_as_destructor"), buf}, Call::Intrinsic));
                Stmt body = Block::make(destructor, op->body);
                stmt = LetStmt::make(op->name, op->value, body);
            } else {
                IRMutator::visit(op);
            }
        }

        string buffer;
    public:
        InjectDeviceDestructor(string b) : buffer(b) {}
    };

    // Find the let stmt that defines the .buffer and insert inside of
    // it a combined host/dev allocation, a destructor registration,
    // and an Allocate node that takes its host field from the
    // .buffer.
    class InjectCombinedAllocation : public IRMutator {

        using IRMutator::visit;

        void visit(const LetStmt *op) {
            if (op->name == buffer + ".buffer") {
                Expr buf = Variable::make(type_of<struct halide_buffer_t *>(), buffer + ".buffer");
                Stmt body = op->body;

                // The allocate node is innermost
                Expr host = Call::make(Handle(), Call::buffer_get_host, {buf}, Call::Extern);
                body = Allocate::make(buffer, type, extents, condition, body,
                                      host, "halide_device_host_nop_free");

                // Then the destructor
                Stmt destructor =
                    Evaluate::make(Call::make(Int(32), Call::register_destructor,
                                              {Expr("halide_device_and_host_free_as_destructor"), buf},
                                              Call::Intrinsic));
                body = Block::make(destructor, body);

                // Then the device_and_host malloc
                Expr device_interface = make_device_interface_call(device_api);
                Stmt device_malloc = call_extern_and_assert("halide_device_and_host_malloc",
                                                            {buf, device_interface});
                if (!is_one(condition)) {
                    device_malloc = IfThenElse::make(condition, device_malloc);
                }
                body = Block::make(device_malloc, body);

                // In the value, we want to use null for the initial value of the host field.
                Expr value = substitute(buffer, reinterpret(Handle(), make_zero(UInt(64))), op->value);

                // Rewrap the letstmt
                stmt = LetStmt::make(op->name, value, body);
            } else {
                IRMutator::visit(op);
            }
        }

        string buffer;
        Type type;
        vector<Expr> extents;
        Expr condition;
        DeviceAPI device_api;
    public:
        InjectCombinedAllocation(string b, Type t, vector<Expr> e, Expr c, DeviceAPI d) :
            buffer(b), type(t), extents(e), condition(c), device_api(d) {}
    };

    void visit(const Allocate *op) {
        FindBufferUsage finder(op->name, DeviceAPI::Host);
        op->body.accept(&finder);

        bool touched_on_host = finder.devices_touched.count(DeviceAPI::Host);
        bool touched_on_device = finder.devices_touched.size() > (touched_on_host ? 1 : 0);

        if (!touched_on_device) {
            // Boring.
            IRMutator::visit(op);
            return;
        }

        Stmt body = mutate(op->body);

        body = InjectBufferCopiesForSingleBuffer(op->name, false).mutate(body);

        if (finder.devices_touched.size() == 1) {
            // Only touched on device.

            // Add a device destructor
            body = InjectDeviceDestructor(op->name + ".buffer").mutate(body);

            // Remove the host allocation
            stmt = Allocate::make(op->name, op->type, op->extents, const_false(), body, op->new_expr, op->free_function);
        } else if (touched_on_host && finder.devices_touched.size() == 2) {
            // Touched on a single device and the host. Use a combined allocation.
            DeviceAPI touching_device = DeviceAPI::None;
            for (DeviceAPI d : finder.devices_touched) {
                if (d != DeviceAPI::Host) {
                    touching_device = d;
                }
            }

            stmt = InjectCombinedAllocation(op->name, op->type, op->extents,
                                            op->condition, touching_device).mutate(body);
        } else {
            // Touched on multiple devices. Do separate device and host allocations.

            // Add a device destructor
            body = InjectDeviceDestructor(op->name + ".buffer").mutate(body);

            stmt = Allocate::make(op->name, op->type, op->extents, op->condition, body, op->new_expr, op->free_function);
        }
    }

    void visit(const For *op) {
        if (op->device_api != DeviceAPI::Host &&
            op->device_api != DeviceAPI::None) {
            // Don't enter device loops
            stmt = op;
        } else {
            IRMutator::visit(op);
        }
    }
};

}  // namespace

Stmt inject_host_dev_buffer_copies(Stmt s, const Target &t) {
    s = InjectBufferCopies().mutate(s);

    // Now handle inputs and outputs
    FindInputsAndOutputs finder;
    s.accept(&finder);
    for (const string &buf : finder.result) {
        s = InjectBufferCopiesForSingleBuffer(buf, true).mutate(s);
    }

    return s;
}

}
}
