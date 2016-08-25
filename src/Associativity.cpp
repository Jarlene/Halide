#include "Associativity.h"
#include "Substitute.h"
#include "Simplify.h"
#include "IROperator.h"
#include "IREquality.h"
#include "IRMutator.h"
#include "Solve.h"
#include "ExprUsesVar.h"
#include "CSE.h"
#include "Util.h"
#include "IRMatch.h"

#include <algorithm>
#include <sstream>

namespace Halide {
namespace Internal {

using std::map;
using std::pair;
using std::set;
using std::string;
using std::vector;

namespace {

class CountExprDepth : public IRVisitor {
public:
    int depth = 0;

    using IRVisitor::visit;

    template<typename T>
    void visit_const(const T *op) {
        depth = 1;
    }

    void visit(const IntImm *op)    { visit_const<IntImm>(op); }
    void visit(const UIntImm *op)   { visit_const<UIntImm>(op); }
    void visit(const FloatImm *op)  { visit_const<FloatImm>(op); }
    void visit(const Variable *op)  { visit_const<Variable>(op); }

    void visit(const Cast *op) {
        op->value.accept(this);
        depth += 1;
    }

    void visit(const Not *op) {
        op->a.accept(this);
        depth += 1;
    }

    template<typename T>
    void visit_binary_operator(const T *op) {
        op->a.accept(this);
        int depth_a = depth;
        op->b.accept(this);
        int depth_b = depth;
        depth = 1 + std::max(depth_a, depth_b);
    }

    void visit(const Add *op) {visit_binary_operator(op);}
    void visit(const Sub *op) {visit_binary_operator(op);}
    void visit(const Mul *op) {visit_binary_operator(op);}
    void visit(const Div *op) {visit_binary_operator(op);}
    void visit(const Mod *op) {visit_binary_operator(op);}
    void visit(const Min *op) {visit_binary_operator(op);}
    void visit(const Max *op) {visit_binary_operator(op);}
    void visit(const EQ *op) {visit_binary_operator(op);}
    void visit(const NE *op) {visit_binary_operator(op);}
    void visit(const LT *op) {visit_binary_operator(op);}
    void visit(const LE *op) {visit_binary_operator(op);}
    void visit(const GT *op) {visit_binary_operator(op);}
    void visit(const GE *op) {visit_binary_operator(op);}
    void visit(const And *op) {visit_binary_operator(op);}
    void visit(const Or *op) {visit_binary_operator(op);}

    void visit(const Select *op) {
        internal_error << "TODO(psuriana): implement this later\n";
    }
};

int count_expr_depth(Expr e) {
    CountExprDepth c;
    e.accept(&c);
    return c.depth;
}

// Replace self-reference to Func 'func' with arguments 'args' at index
// 'value_index' in the Expr/Stmt with Var 'substitute'
class ConvertSelfRef : public IRMutator {
    using IRMutator::visit;

    const string func;
    const vector<Expr> args;
    // If that function has multiple values, which value does this
    // call node refer to?
    int value_index;
    vector<string> op_x_names;
    map<int, Expr> *x_subs;
    bool is_conditional;

    void visit(const Call *op) {
        if (!is_solvable) {
            return;
        }
        IRMutator::visit(op);
        op = expr.as<Call>();
        internal_assert(op);

        if ((op->call_type == Call::Halide) && (func == op->name)) {
            internal_assert(!op->func.defined())
                << "Func should not have been defined for a self-reference\n";
            internal_assert(args.size() == op->args.size())
                << "Self-reference should have the same number of args as the original\n";
            if (is_conditional && (op->value_index == value_index)) {
                debug(4) << "Self-reference of " << op->name
                         << " inside a conditional. Operation is not associative\n";
                is_solvable = false;
                return;
            }
            for (size_t i = 0; i < op->args.size(); i++) {
                if (!equal(op->args[i], args[i])) {
                    debug(4) << "Self-reference of " << op->name
                             << " with different args from the LHS. Operation is not associative\n";
                    is_solvable = false;
                    return;
                }
            }
            // Substitute the call
            const auto &iter = x_subs->find(op->value_index);
            if (iter != x_subs->end()) {
                const Variable *v = iter->second.as<Variable>();
                internal_assert(v && (v->type == op->type));
                debug(4) << "   Substituting Call " << op->name << " at value index "
                         << op->value_index << " with " << v->name << "\n";
                expr = iter->second;
            } else {
                internal_assert(op->value_index < (int)op_x_names.size());
                debug(4) << "   Substituting Call " << op->name << " at value index "
                         << op->value_index << " with " << op_x_names[op->value_index] << "\n";
                expr = Variable::make(op->type, op_x_names[op->value_index]);
                x_subs->emplace(op->value_index, expr);
            }
            if (op->value_index == value_index) {
                x_part = op;
            } else {
                x_dependencies.insert(op->value_index);
            }
        }
    }

    void visit(const Select *op) {
        is_conditional = true;
        Expr cond = mutate(op->condition);
        is_conditional = false;

        Expr t = mutate(op->true_value);
        Expr f = mutate(op->false_value);
        if (cond.same_as(op->condition) &&
            t.same_as(op->true_value) &&
            f.same_as(op->false_value)) {
            expr = op;
        } else {
            expr = Select::make(cond, t, f);
        }
    }

public:
    ConvertSelfRef(const string &f, const vector<Expr> &args, int idx,
                   const vector<string> &x_names, map<int, Expr> *subs) :
        func(f), args(args), value_index(idx), op_x_names(x_names), x_subs(subs), is_conditional(false) {}

    bool is_solvable = true;
    set<int> x_dependencies; // Contains dependencies on self-reference at different tuple indices
    Expr x_part;
};

template<typename T>
bool visit_associative_binary_op(int index, const string &op_x, const string &op_y,
                                 Expr x_part, Expr lhs, Expr rhs, AssociativeOps &assoc_ops) {
    const Variable *var_a = lhs.as<Variable>();
    if (!var_a || (var_a->name != op_x)) {
        debug(4) << "Can't prove associativity of " << T::make(lhs, rhs) << "\n";
        return false;
    } else if (expr_uses_var(rhs, op_x)) {
        debug(4) << "Can't prove associativity of " << T::make(lhs, rhs) << "\n";
        return false;
    } else {
        // op(x, y)
        assoc_ops.x[index] = {op_x, x_part};
        assoc_ops.y[index] = {op_y, rhs};
    }
    return true;
}

bool compare_expr_depths(const AssociativePair& lhs, const AssociativePair& rhs) {
    int lhs_depth = count_expr_depth(lhs.op);
    int rhs_depth = count_expr_depth(rhs.op);
    return lhs_depth < rhs_depth;
}

bool associative_op_pattern_match(Expr e,
                                  const AssociativePair &pattern,
                                  const vector<string> &x_names,
                                  const vector<string> &y_names,
                                  const Scope<int> &x_scope,
                                  map<string, Expr> &match) {

    map<string, Expr> result;
    if (expr_match(pattern.op, e, result)) {
        debug(0) << "Find associative ops for " << e << " -> " << pattern.op
                 << " with identity: " << pattern.identity
                 << ", y_part: " << result["y0"] << "\n";

        for (const auto &x_name : x_names) {
            debug(0) << "x_name: " << x_name << "\n";
            const auto &iter = result.find(x_name);
            if (iter != result.end()) {
                const Variable *xvar = iter->second.as<Variable>();
                if ((xvar == nullptr) || (xvar->name != x_name)) {
                    debug(0) << "...Skipping match since the x_part is different than expected "
                             << iter->second << "\n";
                    return false;
                }
                debug(0) << "...x: " << iter->first << " -> " << iter->second << "\n";
            }
        }
        for (const auto &y_name : y_names) {
            debug(0) << "y_name: " << y_name << "\n";
            const auto &iter = result.find(y_name);
            if (iter != result.end()) {
                // Make sure that y_part should not depend on x vars
                if (expr_uses_vars(iter->second, x_scope)) {
                    debug(0) << "...Skipping match since the y_part depends on x vars\n";
                    return false;
                }
                debug(0) << "...y: " << iter->first << " -> " << iter->second << "\n";
            }
        }

        // Make sure that the new matches are in agreement with any previous matches
        for (const auto &iter : result) {
            const auto &match_iter = match.find(iter.first);
            if (match_iter == match.end()) {
                debug(0) << "Adding result: " << iter.first << " -> " << iter.second << "\n";
                match.emplace(iter.first, iter.second);
            } else {
                if (!equal(iter.first, match_iter->first) || !equal(iter.second, match_iter->second)) {
                    return false;
                }
            }
        }

        return true;
    }
    return false;
}

bool find_match(const vector<vector<AssociativePair>> &table, const vector<string> &op_x_names,
                const vector<string> &op_y_names, const vector<Expr> &x_parts,
                const vector<Expr> &exprs, AssociativeOps &assoc_ops) {
    internal_assert(op_x_names.size() == op_y_names.size());
    internal_assert(op_x_names.size() == x_parts.size());
    internal_assert(op_x_names.size() == exprs.size());

    //TODO(psuriana): Find a tighter bound
    //auto lower = std::lower_bound(table.begin(), table.end(), AssociativePair(e), compare_expr_depths);
    //auto upper = std::upper_bound(table.begin(), table.end(), AssociativePair(e), compare_expr_depths);

    auto lower = table.begin();
    auto upper = table.end();

    // We need to convert the variable names into x{tuple_idx}
    // to match it with the associative ops table.
    vector<string> pattern_x_names(op_x_names.size());
    vector<string> pattern_y_names(op_y_names.size());
    Scope<int> pattern_x_scope;
    vector<Expr> sub_exprs(exprs);
    for (size_t i = 0; i < op_x_names.size(); ++i) {
        pattern_x_names[i] = "x" + std::to_string(i);
        pattern_x_scope.push(pattern_x_names[i], 0);
        pattern_y_names[i] = "y" + std::to_string(i);
        for (size_t j = 0; j < sub_exprs.size(); ++j) {
            sub_exprs[j] = substitute(op_x_names[i], Variable::make(x_parts[i].type(),
                                      pattern_x_names[i]), sub_exprs[j]);
        }
        debug(0) << "**** expr: " << exprs[i] << " -> " << sub_exprs[i] << "\n";
    }

    for (; lower < upper; lower++) {
        vector<AssociativePair> patterns = *lower;
        internal_assert(patterns.size() == op_x_names.size());
        map<string, Expr> pattern_match;
        bool matched = true;
        for (size_t i = 0; i < patterns.size(); ++i) {
            if (!associative_op_pattern_match(sub_exprs[i], patterns[i], pattern_x_names,
                                              pattern_y_names, pattern_x_scope, pattern_match)) {
                matched = false;
                break;
            }
        }

        if (!matched) {
            continue;
        }

        for (const auto &y_name : pattern_y_names) {
            const auto &iter = pattern_match.find(y_name);
            if (iter == pattern_match.end()) {
                // Didn't find y{index} during pattern matching. Try next pattern.
                continue;
            }
            Expr y_part = iter->second;
            int index = atoi(y_name.substr(1, y_name.size()-1).c_str());
            internal_assert(y_name == "y" + std::to_string(index));

            Expr var = Variable::make(y_part.type(), op_y_names[index]);
            Expr e = substitute(y_part, var, exprs[index]);
            assoc_ops.ops[index] = {e, patterns[index].identity};
            assoc_ops.x[index] = {op_x_names[index], x_parts[index]};
            assoc_ops.y[index] = {op_y_names[index], y_part};
        }
        return true;
    }

    return false;
}

bool find_match_single(const vector<AssociativePair> &table, int index, const string &op_x,
                       const string &op_y, Expr x_part, Expr e, AssociativeOps &assoc_ops) {

    //TODO(psuriana): Find a tighter bound
    //auto lower = std::lower_bound(table.begin(), table.end(), AssociativePair(e), compare_expr_depths);
    //auto upper = std::upper_bound(table.begin(), table.end(), AssociativePair(e), compare_expr_depths);

    auto lower = table.begin();
    auto upper = table.end();

    // We need to convert the variable names into x{tuple_idx}
    // to match it with the associative ops table.
    Expr original = e;
    e = substitute(op_x, Variable::make(x_part.type(), "x0"), e);

    for (; lower < upper; lower++) {
        AssociativePair pattern = *lower;
        map<string, Expr> result;
        if (expr_match(pattern.op, e, result)) {
            debug(0) << "Find associative ops for " << e << " -> " << pattern.op
                     << " with identity: " << pattern.identity
                     << ", y_part: " << result["y0"] << "\n";

            const Variable *xvar = result["x0"].as<Variable>();
            if ((xvar == nullptr) || (xvar->name != "x0")) {
                debug(0) << "...Skipping match since the x_part is different than expected "
                         << result["x0"] << "\n";
                continue;
            }

            Expr y_part = result["y0"];
            // Make sure that y_part should not depend on x
            if (expr_uses_var(y_part, "x0")) {
                debug(0) << "...Skipping match since the y_part depends on x\n";
                continue;
            }

            Expr var = Variable::make(y_part.type(), op_y);
            e = substitute(y_part, var, original);
            assoc_ops.ops[index] = {e, pattern.identity};
            assoc_ops.x[index] = {op_x, x_part};
            assoc_ops.y[index] = {op_y, y_part};
            return true;
        }
    }

    return false;
}

bool lookup_single_i32_associative_ops_table(int index, const string &op_x, const string &op_y,
                                             Expr x_part, Expr e, AssociativeOps &assoc_ops) {
    Type t = Int(32);
    internal_assert(e.type() == t);

    if (e.as<Add>()) {
        return find_match_single(get_single_i32_ops_table_add(), index, op_x, op_y, x_part, e, assoc_ops);
    } else if (e.as<Sub>()) {
        return find_match_single(get_single_i32_ops_table_sub(), index, op_x, op_y, x_part, e, assoc_ops);
    } else if (e.as<Mul>()) {
        return find_match_single(get_single_i32_ops_table_mul(), index, op_x, op_y, x_part, e, assoc_ops);
    } else if (e.as<Min>()) {
        return find_match_single(get_single_i32_ops_table_min(), index, op_x, op_y, x_part, e, assoc_ops);
    } else if (e.as<Max>()) {
        return find_match_single(get_single_i32_ops_table_max(), index, op_x, op_y, x_part, e, assoc_ops);
    }
    return false;
}

bool extract_associative_op_single_element(int index, const vector<string> &op_x_names,
                                           const vector<string> &op_y_names, Expr x_part,
                                           Expr e, AssociativeOps &assoc_ops) {
    Type t = e.type();
    const string &op_x = op_x_names[index];
    const string &op_y = op_y_names[index];
    Expr x = Variable::make(t, op_x);
    Expr y = Variable::make(t, op_y);

    debug(0) << "\n\nProving associativity of:  " << e << "\n";

    if (!x_part.defined()) { // op(y)
        // Update with no self-recurrence is associative and the identity can be
        // anything since it's going to be replaced anyway
        assoc_ops.ops[index] = {y, make_const(t, 0)};
        assoc_ops.x[index] = {"", Expr()};
        assoc_ops.y[index] = {op_y, e};
        return true;
    }

    bool success = false;
    if (const Add *a = e.as<Add>()) {
        assoc_ops.ops[index] = {x + y, make_const(t, 0)};
        success = visit_associative_binary_op<Add>(index, op_x, op_y, x_part, a->a, a->b, assoc_ops);
    } else if (const Sub *s = e.as<Sub>()) {
        assoc_ops.ops[index] = {x + y, make_const(t, 0)};
        success = visit_associative_binary_op<Sub>(index, op_x, op_y, x_part, s->a, s->b, assoc_ops);
    } else if (const Mul *m = e.as<Mul>()) {
        assoc_ops.ops[index] = {x * y, make_const(t, 1)};
        success = visit_associative_binary_op<Mul>(index, op_x, op_y, x_part, m->a, m->b, assoc_ops);
    } else if (const Min *m = e.as<Min>()) {
        assoc_ops.ops[index] = {Min::make(x, y), t.max()};
        success = visit_associative_binary_op<Min>(index, op_x, op_y, x_part, m->a, m->b, assoc_ops);
    } else if (const Max *m = e.as<Max>()) {
        assoc_ops.ops[index] = {Max::make(x, y), t.min()};
        success = visit_associative_binary_op<Max>(index, op_x, op_y, x_part, m->a, m->b, assoc_ops);
    } else if (const And *a = e.as<And>()) {
        assoc_ops.ops[index] = {And::make(x, y), make_const(t, 1)};
        success = visit_associative_binary_op<And>(index, op_x, op_y, x_part, a->a, a->b, assoc_ops);
    } else if (const Or *o = e.as<Or>()) {
        assoc_ops.ops[index] = {Or::make(x, y), make_const(t, 0)};
        success = visit_associative_binary_op<Or>(index, op_x, op_y, x_part, o->a, o->b, assoc_ops);
    } else if (e.as<Let>()) {
        internal_error << "Let should have been substituted before calling this function\n";
    }

    if (!success && t.is_int() && (t.bits() == 32)) {
        // It's non-trivial binary ops. Try looking at the associative ops table for int32
        debug(0) << "Look-up associativity table for: " << e << "\n";
        success = lookup_single_i32_associative_ops_table(index, op_x, op_y, x_part, e, assoc_ops);
    }
    debug(0) << e << " is associative? " << success << "\n";
    return success;
}

} // anonymous namespace


// TODO(psuriana): This does not handle cross-dependencies among tuple elements.
// It also is not able to handle associative select() (e.g. argmin/argmax)
AssociativityProverResult prove_associativity(const string &f, vector<Expr> args,
                                              vector<Expr> exprs) {
    AssociativeOps assoc_ops(exprs.size());
    map<int, Expr> x_subs;

    for (Expr &arg : args) {
        arg = common_subexpression_elimination(arg);
        arg = simplify(arg);
        arg = substitute_in_all_lets(arg);
    }

    //TODO(psuriana): need to cleanup all occurences of x0, y0, etc, to avoid
    // clashing with the wildcard matching later on

    vector<string> op_x_names(exprs.size()), op_y_names(exprs.size());
    for (size_t idx = 0; idx < exprs.size(); ++idx) {
        op_x_names[idx] = unique_name("_x_" + std::to_string(idx));
        op_y_names[idx] = unique_name("_y_" + std::to_string(idx));
    }

    vector<set<int>> dependencies(exprs.size());
    vector<Expr> x_parts(exprs.size());
    bool all_independent = true;

    // For a Tuple of exprs to be associative, each element of the Tuple
    // has to be associative.
    for (size_t idx = 0; idx < exprs.size(); ++idx) {
        string op_x = op_x_names[idx];
        string op_y = op_y_names[idx];

        exprs[idx] = simplify(exprs[idx]);

        // Replace any self-reference to Func 'f' with a Var
        ConvertSelfRef csr(f, args, idx, op_x_names, &x_subs);
        exprs[idx] = csr.mutate(exprs[idx]);
        if (!csr.is_solvable) {
            return AssociativityProverResult();
        }
        dependencies[idx] = csr.x_dependencies;
        if (!csr.x_dependencies.empty()) {
            all_independent = false;
        }
        x_parts[idx] = csr.x_part;

        exprs[idx] = common_subexpression_elimination(exprs[idx]);
        exprs[idx] = simplify(exprs[idx]);
        exprs[idx] = solve_expression(exprs[idx], op_x).result; // Move 'x' to the left as possible
        exprs[idx] = substitute_in_all_lets(exprs[idx]);
    }

    if (all_independent || (exprs.size() == 1)) {
        debug(0) << "All tuple elements are independent. Try proving associativity of "
                 << "each element separately.\n";
        // Since there is no x-cross-dependencies, check associativity of each tuple
        // element separately.
        for (size_t idx = 0; idx < exprs.size(); ++idx) {
            // Try to infer the 'y' part of the operator. If we couldn't find
            // a single 'y' that satisfy the operator, give up
            bool is_associative = extract_associative_op_single_element(
                idx, op_x_names, op_y_names, x_parts[idx], exprs[idx], assoc_ops);
            if (!is_associative) {
                return AssociativityProverResult();
            }
        }
    } else {
        debug(0) << "There is cross-dependencies. Need to prove associativity in bulk.\n";
        //TODD(psuriana): currently only works for 32-bit integers
        if (!find_match(get_i32_ops_table(exprs), op_x_names, op_y_names, x_parts, exprs, assoc_ops)) {
            debug(0) << "Cannot find matching associative ops\n";
            return AssociativityProverResult();
        }

    }

    return AssociativityProverResult(true, assoc_ops);
}

namespace {

std::string print_args(const string &f, const vector<Expr> &args, const vector<Expr> &exprs) {
    std::ostringstream stream;
    stream << f << "(";
    for (size_t i = 0; i < args.size(); ++i) {
        stream << args[i];
        if (i != args.size() - 1) {
            stream << ", ";
        }
    }
    stream << ") = ";

    if (exprs.size() == 1) {
        stream << exprs[0];
    } else if (exprs.size() > 1) {
        stream << "Tuple(";
        for (size_t i = 0; i < exprs.size(); ++i) {
            stream << exprs[i];
            if (i != exprs.size() - 1) {
                stream << ", ";
            }
        }
        stream << ")";
    }
    return stream.str();
}

void check_associativity(const string &f, vector<Expr> args, vector<Expr> exprs,
                         bool is_associative, const AssociativeOps &assoc_ops) {
    auto result = prove_associativity(f, args, exprs);
    internal_assert(result.is_associative == is_associative)
        << "Checking associativity: " << print_args(f, args, exprs) << "\n"
        << "  Expect is_associative: " << is_associative << "\n"
        << "  instead of " << result.is_associative << "\n";
    if (is_associative) {
        for (size_t i = 0; i < assoc_ops.size(); ++i) {
            internal_assert(equal(result.ops()[i].identity, assoc_ops.ops[i].identity))
                << "Checking associativity: " << print_args(f, args, exprs) << "\n"
                << "  Index: " << i << "\n"
                << "  Expect identity: " << assoc_ops.ops[i].identity << "\n"
                << "  instead of " << result.ops()[i].identity << "\n";
            internal_assert(equal(result.x()[i].expr, assoc_ops.x[i].expr))
                << "Checking associativity: " << print_args(f, args, exprs) << "\n"
                << "  Index: " << i << "\n"
                << "  Expect x: " << assoc_ops.x[i].expr << "\n"
                << "  instead of " << result.x()[i].expr << "\n";
            internal_assert(equal(result.y()[i].expr, assoc_ops.y[i].expr))
                << "Checking associativity: " << print_args(f, args, exprs) << "\n"
                << "  Index: " << i << "\n"
                << "  Expect y: " << assoc_ops.x[i].expr << "\n"
                << "  instead of " << result.y()[i].expr << "\n";
            Expr expected_op = assoc_ops.ops[i].op;
            if (result.y()[i].expr.defined()) {
                expected_op = substitute(
                    assoc_ops.y[i].var, Variable::make(result.y()[i].expr.type(), result.y()[i].var), expected_op);
            }
            if (result.x()[i].expr.defined()) {
                expected_op = substitute(
                    assoc_ops.x[i].var, Variable::make(result.x()[i].expr.type(), result.x()[i].var), expected_op);
            }
            internal_assert(equal(result.ops()[i].op, expected_op))
                << "Checking associativity: " << print_args(f, args, exprs) << "\n"
                << "  Index: " << i << "\n"
                << "  Expect bin op: " << expected_op << "\n"
                << "  instead of " << result.ops()[i].op << "\n";

            debug(4) << "\nExpected op: " << expected_op << "\n";
            debug(4) << "Operator: " << result.ops()[i].op << "\n";
            debug(4) << "   identity: " << result.ops()[i].identity << "\n";
            debug(4) << "   x: " << result.x()[i].var << " -> " << result.x()[i].expr << "\n";
            debug(4) << "   y: " << result.y()[i].var << " -> " << result.y()[i].expr << "\n";
        }
    }
}

} // anonymous namespace

void associativity_test() {
    typedef AssociativeOps::Replacement Replacement;

    Expr x = Variable::make(Int(32), "x");
    Expr y = Variable::make(Int(32), "y");
    Expr z = Variable::make(Int(32), "z");
    Expr rx = Variable::make(Int(32), "rx");

    Expr f_call_0 = Call::make(Int(32), "f", {x}, Call::CallType::Halide, nullptr, 0);
    Expr f_call_1 = Call::make(Int(32), "f", {x}, Call::CallType::Halide, nullptr, 1);
    Expr f_call_2 = Call::make(Int(32), "f", {x}, Call::CallType::Halide, nullptr, 2);
    Expr g_call_0 = Call::make(Int(32), "g", {rx}, Call::CallType::Halide, nullptr, 0);
    Expr g_call_1 = Call::make(Int(32), "g", {rx}, Call::CallType::Halide, nullptr, 1);

    // f(x) = f(x) - g(rx) -> Is associative given that the merging operator is +
    /*check_associativity("f", {x}, {f_call_0 - g_call_0}, true,
                        {{AssociativePair(x + y, 0)},
                         {Replacement("x", f_call_0)},
                         {Replacement("y", g_call_0)}
                        });*/

    // f(x) = min(f(x), int16(z))
    /*check_associativity("f", {x}, {min(f_call_0, y + Cast::make(Int(16), z))}, true,
                        {{AssociativePair(min(x, y), Int(32).max())},
                         {Replacement("x", f_call_0)},
                         {Replacement("y", y + Cast::make(Int(16), z))}
                        });

    // f(x) = f(x) + g(rx) + y + z
    check_associativity("f", {x}, {y + z + f_call_0}, true,
                        {{AssociativePair(x + y, make_const(Int(32), 0))},
                         {Replacement("x", f_call_0)},
                         {Replacement("y", y + z)}
                        });

    // f(x) = max(y, f(x))
    check_associativity("f", {x}, {max(y, f_call_0)}, true,
                        {{AssociativePair(max(x, y), Int(32).min())},
                         {Replacement("x", f_call_0)},
                         {Replacement("y", y)}
                        });

    // f(x) = Tuple(2, 3, f(x)[2] + z)
    check_associativity("f", {x}, {2, 3, f_call_2 + z}, true,
                        {{AssociativePair(y, make_const(Int(32), 0)),
                            AssociativePair(y, make_const(Int(32), 0)),
                            AssociativePair(x + y, make_const(Int(32), 0))},
                         {Replacement("", Expr()), Replacement("", Expr()), Replacement("x", f_call_2)},
                         {Replacement("y", 2), Replacement("y", 3), Replacement("y", z)},
                        });

    // f(x) = Tuple(min(f(x)[0], g(rx)), f(x)[1]*g(x)*2, f(x)[2] + z)
    check_associativity("f", {x}, {min(f_call_0, g_call_0), f_call_1*g_call_0*2, f_call_2 + z}, true,
                        {{AssociativePair(min(x, y), Int(32).max()),
                            AssociativePair(x * y, make_const(Int(32), 1)),
                            AssociativePair(x + y, make_const(Int(32), 0))},
                         {Replacement("x", f_call_0), Replacement("x", f_call_1), Replacement("x", f_call_2)},
                         {Replacement("y", g_call_0), Replacement("y", g_call_0*2), Replacement("y", z)},
                        });

    // f(x) = max(f(x) + g(rx), g(rx)) -> not associative
    check_associativity("f", {x}, {max(f_call_0 + g_call_0, g_call_0)}, false, AssociativeOps());

    // f(x) = max(f(x) + g(rx), f(x) - 3) -> f(x) + max(g(rx) - 3)
    check_associativity("f", {x}, {max(f_call_0 + g_call_0, f_call_0 - 3)}, true,
                        {{AssociativePair(x + y, 0)},
                         {Replacement("x", f_call_0)},
                         {Replacement("y", max(g_call_0, -3))}
                        });

    // f(x) = min(4, g(rx)) -> trivially associative
    check_associativity("f", {x}, {min(4, g_call_0)}, true,
                        {{AssociativePair(y, make_const(Int(32), 0))},
                         {Replacement("", Expr())},
                         {Replacement("y", min(g_call_0, 4))}
                        });

    // f(x) = f(x) -> associative but doesn't really make any sense, so we'll treat it as non-associative
    check_associativity("f", {x}, {f_call_0}, false, AssociativeOps());

    // f(x) = max(max(min(f(x), g(rx) + 2), f(x)), g(rx) + 2)
    check_associativity("f", {x}, {max(max(min(f_call_0, g_call_0 + 2), f_call_0), g_call_0 + 2)}, true,
                        {{AssociativePair(max(max(min(x, y), x), y), Int(32).min())},
                         {Replacement("x", f_call_0)},
                         {Replacement("y", g_call_0 + 2)}
                        });

    // f(x) = ((min(max((f(x)*g(rx)), g(rx)), (f(x)*g(rx))) + g(rx)) + f(x))
    check_associativity("f", {x}, {((min(max((g_call_0*f_call_0), g_call_0), (f_call_0*g_call_0)) + g_call_0) + f_call_0)},
                        true,
                        {{AssociativePair(((min(max((x*y), y), (x*y)) + y) + x), make_const(Int(32), 0))},
                         {Replacement("x", f_call_0)},
                         {Replacement("y", g_call_0)}
                        });*/

    // f(x) = Tuple(f(x)[0]*g(r.x)[0] - f(x)[1]*g(r.x)[1], f(x)[0]*g(r.x)[1] + f(x)[1]*g(r.x)[0])
    check_associativity("f", {x}, {f_call_0*g_call_0 - f_call_1*g_call_1, f_call_0*g_call_1 + f_call_1*g_call_0}, true,
                        {{AssociativePair(y, make_const(Int(32), 1)),
                            AssociativePair(y, make_const(Int(32), 0))},
                         {Replacement("x", f_call_0), Replacement("x", f_call_1)},
                         {Replacement("y", g_call_0), Replacement("y", g_call_1)},
                        });

    /*Expr x0 = Variable::make(Int(32), "x0");
    Expr y0 = Variable::make(Int(32), "y0");

    Expr expr = max(max(min((x0*y0), x0), y0), x0); // Leaves 5
    int depth = count_expr_depth(expr);
    std::cout << "Depth of " << expr << ": " << depth << "\n";

    expr = min(max(((x0 + y0)*x0), max((y0*y0), y0)), y0); // Leaves 6
    depth = count_expr_depth(expr);
    std::cout << "Depth of " << expr << ": " << depth << "\n";

    expr = max(min(((y0 - x0)*(y0 - x0)), min((y0*y0), y0)), y0); // Leaves 8
    depth = count_expr_depth(expr);
    std::cout << "Depth of " << expr << ": " << depth << "\n";*/

    std::cout << "Associativity test passed" << std::endl;
}


}
}
