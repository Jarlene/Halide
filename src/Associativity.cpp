#include "Associativity.h"
#include "Substitute.h"
#include "Simplify.h"
#include "IREquality.h"
#include "IROperator.h"
#include "IRMutator.h"
#include "Solve.h"
#include "ExprUsesVar.h"
#include "CSE.h"
#include "Util.h"
#include "IRMatch.h"

#include <algorithm>
#include <iterator>
#include <sstream>

namespace Halide {
namespace Internal {

using std::map;
using std::pair;
using std::set;
using std::string;
using std::vector;

namespace {

template <typename T>
vector<T> get_subvector(const vector<T> &v, const set<int> &indices) {
    vector<T> sub;
    for (const auto &index : indices) {
        internal_assert(index < (int)v.size());
        sub.push_back(v[index]);
    }
    return sub;
}

class FindConflict : public IRGraphVisitor {
    using IRGraphVisitor::visit;

    string var;

    void visit(const Variable *v) {
        if (var == v->name) {
            if (expr.defined()) {
                internal_assert(equal(expr, Expr(v)));
            } else {
                expr = Expr(v);
            }
        }
    }
public:
    FindConflict(const string &v) : var(v) {}
    Expr expr;
};

Expr find_conflict(Expr e, const string &v) {
    FindConflict f(v);
    e.accept(&f);
    return f.expr;
}

// Replace self-references to 'func' with arguments 'args' at
// 'value_index' in the Expr/Stmt with some Var
class ConvertSelfRef : public IRMutator {
    using IRMutator::visit;

    const string &func;
    const vector<Expr> &args;
    // If that function has multiple values, which value does this
    // call node refer to?
    const int value_index;
    const vector<string> &op_x_names;
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
                debug(5) << "Self-reference of " << op->name
                         << " inside a conditional. Operation is not associative\n";
                is_solvable = false;
                return;
            }
            for (size_t i = 0; i < op->args.size(); i++) {
                if (!equal(op->args[i], args[i])) {
                    debug(5) << "Self-reference of " << op->name
                             << " with different args from the LHS. Operation is not associative\n";
                    is_solvable = false;
                    return;
                }
            }
            // Substitute the call
            internal_assert(op->value_index < (int)op_x_names.size());
            debug(5) << "   Substituting Call " << op->name << " at value index "
                     << op->value_index << " with " << op_x_names[op->value_index] << "\n";
            expr = Variable::make(op->type, op_x_names[op->value_index]);

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
                   const vector<string> &x_names) :
        func(f), args(args), value_index(idx), op_x_names(x_names), is_conditional(false) {}

    bool is_solvable = true;
    set<int> x_dependencies; // Contains dependencies on self-reference at different tuple indices
    Expr x_part; // Undefined if there is no self-reference at value_index
};

bool associative_op_pattern_match(Expr e,
                                  const Expr &op,
                                  const vector<string> &x_names,
                                  const vector<string> &y_names,
                                  const Scope<int> &x_scope,
                                  map<string, Expr> &match) {

    map<string, Expr> result;
    if (expr_match(op, e, result)) {
        debug(5) << "Found associative ops for " << e << " -> " << op
                 << ", y_part: " << result["y0"] << "\n";

        for (size_t i = 0; i < x_names.size(); ++i) {
            const auto &iter = result.find("x" + std::to_string(i));
            if (iter != result.end()) {
                const Variable *xvar = iter->second.as<Variable>();
                if ((xvar == nullptr) || (xvar->name != x_names[i])) {
                    debug(5) << "...Skipping match since the x_part is different than expected. "
                             << "Expect: " << x_names[i] << "; get: " << iter->second << "\n";
                    return false;
                }
            }
        }
        for (size_t i = 0; i < y_names.size(); ++i) {
            const auto &iter = result.find("y" + std::to_string(i));
            if (iter != result.end()) {
                // Make sure that y_part should not depend on x vars
                if (expr_uses_vars(iter->second, x_scope)) {
                    debug(5) << "...Skipping match since the y_part depends on x vars\n";
                    return false;
                }
            }
        }
        // Make sure that the new matches are in agreement with any previous matches
        for (const auto &iter : result) {
            const auto &match_iter = match.find(iter.first);
            if (match_iter == match.end()) {
                debug(5) << "Adding result: " << iter.first << " -> " << iter.second << "\n";
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

bool find_match(const vector<AssociativePattern> &table, const vector<string> &op_x_names,
                const vector<string> &op_y_names, const vector<Expr> &x_parts,
                const vector<Expr> &exprs, AssociativeOp &assoc_op) {
    internal_assert(op_x_names.size() == op_y_names.size());
    internal_assert(op_x_names.size() == x_parts.size());
    internal_assert(op_x_names.size() == exprs.size());
    internal_assert(op_x_names.size() == assoc_op.size());

    Scope<int> x_scope;
    for (const auto &x : op_x_names) {
        x_scope.push(x, 0);
    }

    for (const AssociativePattern &pattern : table) {
        internal_assert(pattern.size() == op_x_names.size());
        map<string, Expr> pattern_match;
        bool matched = true;
        // If any of element in 'pattern' does not match, try the next thing in
        // the table.
        for (size_t i = 0; i < pattern.size(); ++i) {
            if (!associative_op_pattern_match(exprs[i], pattern.ops[i], op_x_names,
                                              op_y_names, x_scope, pattern_match)) {
                matched = false;
                break;
            }
        }
        if (!matched) {
            continue;
        }

        vector<pair<Expr, Expr>> replacement;
        for (size_t index = 0; index < op_y_names.size(); ++index) {
            const auto &iter = pattern_match.find("y" + std::to_string(index));
            if (iter == pattern_match.end()) {
                // Didn't find y{index} during pattern matching. Try next pattern.
                matched = false;
                break;
            }
            Expr y_part = iter->second;

            debug(5) << "Pattern at index " << index << ": " << op_x_names[index]
                     << " -> " << x_parts[index] << ", " << op_y_names[index]
                     << " -> " << y_part << "\n";
            assoc_op.xs[index] = {op_x_names[index], x_parts[index]};
            assoc_op.ys[index] = {op_y_names[index], y_part};
            replacement.push_back({y_part, Variable::make(y_part.type(), op_y_names[index])});
        }
        if (!matched) {
            continue;
        }
        for (size_t index = 0; index < exprs.size(); ++index) {
            Expr e = exprs[index];
            // Order of substitution matters, e.g. in the argmin case, _y_0 -> g(rx)[0]
            // and _y_1 -> rx. If we substitute the 2nd element rx first, substitution
            // of g(rx)[0] will fail.
            for (const auto &iter : replacement) {
                e = substitute(iter.first, iter.second, e);
            }
            assoc_op.pattern.ops[index] = e;
            assoc_op.pattern.identities[index] = pattern.identities[index];
        }
        return true;
    }

    return false;
}

template<typename T>
std::pair<bool, bool> visit_associative_binary_op(
        int index, const string &op_x, const string &op_y,
        Expr x_part, Expr lhs, Expr rhs, AssociativeOp &assoc_op) {

    const Variable *var_a = lhs.as<Variable>();
    if (!var_a || (var_a->name != op_x)) {
        debug(5) << "Can't prove associativity of " << T::make(lhs, rhs) << "\n";
        return {false, false};
    } else if (expr_uses_var(rhs, op_x)) {
        debug(5) << "Can't prove associativity of " << T::make(lhs, rhs) << "\n";
        return {false, false};
    } else {
        // op(x, y)
        assoc_op.xs[index] = {op_x, x_part};
        assoc_op.ys[index] = {op_y, rhs};
    }
    if (std::is_same<T, Sub>::value) {
        // Sub is associative but not commutative
        return {true, false};
    }
    return {true, true};
}

// Return a pair of booleans indicating if an operator is associative and commutative
// respectively. 'assoc_op' contains the the equivalent associative binary/unary operator
// for that operator. If the operator is non-associative, 'assoc_op' is not valid.
std::pair<bool, bool> extract_associative_op_single_element(
        int index, const vector<string> &op_x_names,
        const vector<string> &op_y_names, Expr x_part,
        Expr e, AssociativeOp &assoc_op) {

    Type t = e.type();
    const string &op_x = op_x_names[index];
    const string &op_y = op_y_names[index];
    Expr x = Variable::make(t, op_x);
    Expr y = Variable::make(t, op_y);

    if (!x_part.defined()) { // op(y)
        // Update with no self-recurrence is associative and the identity can be
        // anything since it's going to be replaced anyway, but it's not
        // commutative
        assoc_op.pattern.ops[index] = y;
        assoc_op.pattern.identities[index] = make_const(t, 0);
        assoc_op.xs[index] = {"", Expr()};
        assoc_op.ys[index] = {op_y, e};
        return {true, false};
    }

    bool is_associative = false, is_commutative = false;
    if (const Add *a = e.as<Add>()) {
        assoc_op.pattern.ops[index] = x + y;
        assoc_op.pattern.identities[index] = make_const(t, 0);
        std::tie(is_associative, is_commutative) =
            visit_associative_binary_op<Add>(index, op_x, op_y, x_part, a->a, a->b, assoc_op);
    } else if (const Sub *s = e.as<Sub>()) {
        assoc_op.pattern.ops[index] = x + y;
        assoc_op.pattern.identities[index] = make_const(t, 0);
        std::tie(is_associative, is_commutative) =
            visit_associative_binary_op<Sub>(index, op_x, op_y, x_part, s->a, s->b, assoc_op);
    } else if (const Mul *m = e.as<Mul>()) {
        assoc_op.pattern.ops[index] = x * y;
        assoc_op.pattern.identities[index] = make_const(t, 1);
        std::tie(is_associative, is_commutative) =
            visit_associative_binary_op<Mul>(index, op_x, op_y, x_part, m->a, m->b, assoc_op);
    } else if (const Min *m = e.as<Min>()) {
        assoc_op.pattern.ops[index] = Min::make(x, y);
        assoc_op.pattern.identities[index] = t.max();
        std::tie(is_associative, is_commutative) =
            visit_associative_binary_op<Min>(index, op_x, op_y, x_part, m->a, m->b, assoc_op);
    } else if (const Max *m = e.as<Max>()) {
        assoc_op.pattern.ops[index] = Max::make(x, y);
        assoc_op.pattern.identities[index] = t.min();
        std::tie(is_associative, is_commutative) =
            visit_associative_binary_op<Max>(index, op_x, op_y, x_part, m->a, m->b, assoc_op);
    } else if (const And *a = e.as<And>()) {
        assoc_op.pattern.ops[index] = And::make(x, y);
        assoc_op.pattern.identities[index] = make_const(t, 1);
        std::tie(is_associative, is_commutative) =
            visit_associative_binary_op<And>(index, op_x, op_y, x_part, a->a, a->b, assoc_op);
    } else if (const Or *o = e.as<Or>()) {
        assoc_op.pattern.ops[index] = Or::make(x, y);
        assoc_op.pattern.identities[index] = make_const(t, 0);
        std::tie(is_associative, is_commutative) =
            visit_associative_binary_op<Or>(index, op_x, op_y, x_part, o->a, o->b, assoc_op);
    } else if (e.as<Let>()) {
        internal_error << "Let should have been substituted before calling this function\n";
    }

    if (!is_associative && t.is_int() && (t.bits() == 32)) {
        // It's non-trivial binary ops. Try looking at the associative ops table for int32
        debug(5) << "Look-up associativity table for: " << e << "\n";
        AssociativeOp tmp(1);
        is_associative = find_match(get_ops_table({e}), {op_x}, {op_y}, {x_part}, {e}, tmp);
        if (is_associative) {
            // Copy the result over
            assoc_op.pattern.ops[index] = tmp.pattern.ops[0];
            assoc_op.pattern.identities[index] = tmp.pattern.identities[0];
            assoc_op.xs[index] = tmp.xs[0];
            assoc_op.ys[index] = tmp.ys[0];
        }
    }
    debug(5) << e << " -> is associative? " << is_associative << ", is commutative? " << is_commutative << "\n";
    return {is_associative, is_commutative};
}

void add_transitive_dependencies(vector<set<int>> &dependencies) {
    // TODO(psuriana): there might be a better way to find all the transitive dependencies
    bool change = true;
    while (change) {
        change = false;
        for (size_t i = 0; i < dependencies.size(); ++i) {
            for (size_t j = 0; j < dependencies.size(); ++j) {
                if (i == j) {
                    continue;
                }
                if (dependencies[i].find(j) != dependencies[i].end()) {
                    for (const auto &idx : dependencies[j]) {
                        if (dependencies[i].find(idx) == dependencies[i].end()) {
                            dependencies[i].insert(idx);
                            change = false;
                        }
                    }
                }
            }
        }
    }
}

// Given dependencies of each tuple element, compute the set of subgraphs:
// all vertices that are reachable from a given vertex. If a subgraph is fully
// contained in another subgraph, remove it from the final output.
vector<set<int>> compute_subgraphs(vector<set<int>> dependencies) {
    vector<set<int>> subgraphs(dependencies.size());
    for (size_t i = 0; i < dependencies.size(); ++i) {
        // Check if the current subgraph is a subset of another
        const auto &current = dependencies[i];
        if (current.empty()) {
            continue;
        }
        bool should_remove = false;
        for (size_t j = 0; j < dependencies.size(); ++j) {
            const auto &other = dependencies[j];
            if ((i == j) || (current.size() > other.size()) || (j < i && subgraphs[i].empty())) {
                continue;
            }
            vector<int> diff;
            // Compute the vertices in the current set that are not contained in the other
            std::set_difference(current.begin(), current.end(), other.begin(), other.end(),
                                std::inserter(diff, diff.begin()));
            if (diff.empty()) {
                // 'current' is fully contained in 'other'
                should_remove = true;
                break;
            }
        }
        if (!should_remove) {
            subgraphs[i] = current;
        }
    }
    return subgraphs;
}

} // anonymous namespace


AssociativeOp prove_associativity(const string &f, vector<Expr> args, vector<Expr> exprs) {
    AssociativeOp assoc_op(exprs.size());

    for (Expr &arg : args) {
        arg = common_subexpression_elimination(arg);
        arg = simplify(arg);
        arg = substitute_in_all_lets(arg);
    }

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
    for (int idx = exprs.size()-1; idx >= 0; --idx) {
        string op_x = op_x_names[idx];
        string op_y = op_y_names[idx];

        exprs[idx] = simplify(exprs[idx]);
        exprs[idx] = common_subexpression_elimination(exprs[idx]);
        // Calling Simplify or the original expr itself might have let exprs,
        // so we should substitutes in all lets first
        exprs[idx] = substitute_in_all_lets(exprs[idx]);

        // Replace any self-reference to Func 'f' with a Var
        ConvertSelfRef csr(f, args, idx, op_x_names);
        exprs[idx] = csr.mutate(exprs[idx]);
        if (!csr.is_solvable) {
            return AssociativeOp();
        }
        if (!csr.x_dependencies.empty()) {
            all_independent = false;
        }
        x_parts[idx] = csr.x_part;
        dependencies[idx] = csr.x_dependencies;
        if (csr.x_part.defined()) {
            // Dependency on itself
            dependencies[idx].insert(idx);
        }

        exprs[idx] = common_subexpression_elimination(exprs[idx]);
        exprs[idx] = simplify(exprs[idx]);
        exprs[idx] = solve_expression(exprs[idx], op_x).result; // Move 'x' to the left as possible
        exprs[idx] = substitute_in_all_lets(exprs[idx]);
    }

    // Find all transitive dependencies and add them to the graph
    add_transitive_dependencies(dependencies);

    if (all_independent || (exprs.size() == 1)) {
        debug(5) << "All tuple elements are independent. Try proving associativity of "
                 << "each element separately.\n";
        // Since there is no x-cross-dependencies, check associativity of each tuple
        // element separately.
        for (size_t idx = 0; idx < exprs.size(); ++idx) {
            // Try to infer the 'y' part of the operator. If we couldn't find
            // a single 'y' that satisfy the operator, give up
            bool is_associative, is_commutative;
            std::tie(is_associative, is_commutative) = extract_associative_op_single_element(
                idx, op_x_names, op_y_names, x_parts[idx], exprs[idx], assoc_op);
            if (!is_associative) {
                return AssociativeOp();
            }
        }
    } else {
        debug(5) << "There is cross-dependencies. Need to prove associativity in bulk.\n";

        // Decompose the tuple into subgraphs and solve for each separately
        vector<set<int>> subgraphs = compute_subgraphs(dependencies);
        internal_assert(subgraphs.size() == exprs.size());
        for (size_t i = 0; i < subgraphs.size(); ++i) {
            if (subgraphs[i].empty()) {
                debug(5) << "Empty subgraph\n";
                continue;
            }
            if (subgraphs[i].size() > 2) {
                // TODO(psuriana): currently only support max of 2 tuple elements
                debug(5) << "Subgraph size is bigger than 2\n";
                return AssociativeOp();
            }

            vector<Expr> sub_exprs = get_subvector(exprs, subgraphs[i]);
            vector<string> sub_op_x_names = get_subvector(op_x_names, subgraphs[i]);
            vector<string> sub_op_y_names = get_subvector(op_y_names, subgraphs[i]);
            vector<Expr> sub_x_parts = get_subvector(x_parts, subgraphs[i]);
            AssociativeOp sub_assoc_op(sub_exprs.size());

            // TODO(psuriana): In general, if we fail to find a match for the
            // set of initial subgraphs, we need to consider other possible
            // grouping of those initial subgraphs. Since only the 'x' is
            // apparent from the Halide update definition, the compute_subgraphs
            // method over-partitions the graph (e.g. 2x2 matrix multiplication
            // written as a four-dimensional reduction).

            if (!find_match(get_ops_table(sub_exprs), sub_op_x_names, sub_op_y_names,
                            sub_x_parts, sub_exprs, sub_assoc_op)) {
                debug(5) << "Cannot find matching associative ops\n";
                return AssociativeOp();
            }

            debug(5) << "...Proving associativity of subgraph " << i << "\n";
            const set<int> &indices = subgraphs[i];
            for (auto iter = indices.begin(); iter != indices.end(); ++iter) {
                int index = *iter;
                int j = std::distance(indices.begin(), iter);

                // If the ops/x/y have been extracted previously, we have to make sure
                // they are consistent with the new extracted values.
                if (assoc_op.pattern.ops[index].defined()) {
                    if (!equal(assoc_op.pattern.ops[index], sub_assoc_op.pattern.ops[j]) ||
                        !equal(assoc_op.pattern.identities[index], sub_assoc_op.pattern.identities[j])) {
                        debug(5) << "Conflicting associative ops/identities from different subgraphs\n";
                        return AssociativeOp();
                    }
                }
                if (assoc_op.xs[index].expr.defined()) {
                    if (assoc_op.xs[index] != sub_assoc_op.xs[j]) {
                        debug(5) << "Conflicting associative x-replacements from different subgraphs\n";
                        return AssociativeOp();
                    }
                }
                if (assoc_op.ys[index].expr.defined()) {
                    if (assoc_op.ys[index] != sub_assoc_op.ys[j]) {
                        debug(5) << "Conflicting associative y-replacements from different subgraphs\n";
                        return AssociativeOp();
                    }
                }

                assoc_op.pattern.ops[index] = sub_assoc_op.pattern.ops[j];
                assoc_op.pattern.identities[index] = sub_assoc_op.pattern.identities[j];
                assoc_op.xs[index] = sub_assoc_op.xs[j];
                assoc_op.ys[index] = sub_assoc_op.ys[j];
            }
        }
    }

    assoc_op.is_associative = true;
    return assoc_op;
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
                         const AssociativeOp &assoc_op) {
    auto result = prove_associativity(f, args, exprs);
    internal_assert(result.associative() == assoc_op.associative())
        << "Checking associativity: " << print_args(f, args, exprs) << "\n"
        << "  Expect is associative: " << assoc_op.associative() << "\n"
        << "  instead of " << result.associative() << "\n";
    if (assoc_op.associative()) {
        map<string, Expr> replacement;
        for (size_t i = 0; i < assoc_op.size(); ++i) {
            internal_assert(equal(result.pattern.identities[i], assoc_op.pattern.identities[i]))
                << "Checking associativity: " << print_args(f, args, exprs) << "\n"
                << "  Index: " << i << "\n"
                << "  Expect identity: " << assoc_op.pattern.identities[i] << "\n"
                << "  instead of " << result.pattern.identities[i] << "\n";
            internal_assert(equal(result.xs[i].expr, assoc_op.xs[i].expr))
                << "Checking associativity: " << print_args(f, args, exprs) << "\n"
                << "  Index: " << i << "\n"
                << "  Expect x: " << assoc_op.xs[i].expr << "\n"
                << "  instead of " << result.xs[i].expr << "\n";
            internal_assert(equal(result.ys[i].expr, assoc_op.ys[i].expr))
                << "Checking associativity: " << print_args(f, args, exprs) << "\n"
                << "  Index: " << i << "\n"
                << "  Expect y: " << assoc_op.xs[i].expr << "\n"
                << "  instead of " << result.ys[i].expr << "\n";

            if (result.xs[i].expr.defined()) {
                replacement.emplace(assoc_op.xs[i].var, Variable::make(result.xs[i].expr.type(), result.xs[i].var));
            }
            if (result.ys[i].expr.defined()) {
                replacement.emplace(assoc_op.ys[i].var, Variable::make(result.ys[i].expr.type(), result.ys[i].var));
            }
        }

        for (size_t i = 0; i < assoc_op.size(); ++i) {
            Expr expected_op = substitute(replacement, assoc_op.pattern.ops[i]);

            internal_assert(equal(result.pattern.ops[i], expected_op))
                << "Checking associativity: " << print_args(f, args, exprs) << "\n"
                << "  Index: " << i << "\n"
                << "  Expect bin op: " << expected_op << "\n"
                << "  instead of " << result.pattern.ops[i] << "\n";

            debug(5) << "\nExpected op: " << expected_op << "\n";
            debug(5) << "Operator: " << result.pattern.ops[i] << "\n";
            debug(5) << "   identity: " << result.pattern.identities[i] << "\n";
            debug(5) << "   x: " << result.xs[i].var << " -> " << result.xs[i].expr << "\n";
            debug(5) << "   y: " << result.ys[i].var << " -> " << result.ys[i].expr << "\n";
        }
    }
}

} // anonymous namespace

void associativity_test() {
    typedef AssociativeOp::Replacement Replacement;

    Type t = Int(32);
    Expr x = Variable::make(t, "x");
    Expr y = Variable::make(t, "y");
    Expr z = Variable::make(t, "z");
    Expr rx = Variable::make(t, "rx");

    vector<Expr> xs(3), ys(3), zs(3);
    for (size_t i = 0; i < 3; ++i) {
        xs[i] = Variable::make(t, "x" + std::to_string(i));
        ys[i] = Variable::make(t, "y" + std::to_string(i));
        zs[i] = Variable::make(t, "z" + std::to_string(i));
    }

    Expr f_call_0 = Call::make(t, "f", {x}, Call::CallType::Halide, nullptr, 0);
    Expr f_call_1 = Call::make(t, "f", {x}, Call::CallType::Halide, nullptr, 1);
    Expr f_call_2 = Call::make(t, "f", {x}, Call::CallType::Halide, nullptr, 2);
    Expr g_call_0 = Call::make(t, "g", {rx}, Call::CallType::Halide, nullptr, 0);
    Expr g_call_1 = Call::make(t, "g", {rx}, Call::CallType::Halide, nullptr, 1);

    // f(x) = f(x) - g(rx) -> Is associative given that the merging operator is +
    check_associativity("f", {x}, {f_call_0 - g_call_0},
                        AssociativeOp(
                          AssociativePattern(x + y, 0, true),
                          {Replacement("x", f_call_0)},
                          {Replacement("y", g_call_0)},
                          true)
                        );

    // f(x) = min(f(x), int16(z))
    check_associativity("f", {x}, {min(f_call_0, y + Cast::make(Int(16), z))},
                        AssociativeOp(
                          AssociativePattern(min(x, y), t.max(), true),
                          {Replacement("x", f_call_0)},
                          {Replacement("y", y + Cast::make(Int(16), z))},
                          true)
                        );

    // f(x) = f(x) + g(rx) + y + z
    check_associativity("f", {x}, {y + z + f_call_0},
                        AssociativeOp(
                          AssociativePattern(x + y, make_const(t, 0), true),
                          {Replacement("x", f_call_0)},
                          {Replacement("y", y + z)},
                          true)
                        );

    // f(x) = max(y, f(x))
    check_associativity("f", {x}, {max(y, f_call_0)},
                        AssociativeOp(
                          AssociativePattern(max(x, y), t.min(), true),
                          {Replacement("x", f_call_0)},
                          {Replacement("y", y)},
                          true)
                        );

    // f(x) = Tuple(2, 3, f(x)[2] + z)
    check_associativity("f", {x}, {2, 3, f_call_2 + z},
                        AssociativeOp(
                          AssociativePattern({ys[0], ys[1], xs[2] + ys[2]}, {make_const(t, 0), make_const(t, 0), make_const(t, 0)}, true),
                          {Replacement("", Expr()), Replacement("", Expr()), Replacement("x2", f_call_2)},
                          {Replacement("y0", 2), Replacement("y1", 3), Replacement("y2", z)},
                          true)
                        );

    // f(x) = Tuple(min(f(x)[0], g(rx)), f(x)[1]*g(x)*2, f(x)[2] + z)
    check_associativity("f", {x}, {min(f_call_0, g_call_0), f_call_1*g_call_0*2, f_call_2 + z},
                        AssociativeOp(
                          AssociativePattern(
                              {min(xs[0], ys[0]), xs[1] * ys[1], xs[2] + ys[2]},
                              {t.max(), make_const(t, 1), make_const(t, 0)},
                              true),
                          {Replacement("x0", f_call_0), Replacement("x1", f_call_1), Replacement("x2", f_call_2)},
                          {Replacement("y0", g_call_0), Replacement("y1", g_call_0*2), Replacement("y2", z)},
                          true)
                        );

    // f(x) = max(f(x) + g(rx), g(rx)) -> not associative
    check_associativity("f", {x}, {max(f_call_0 + g_call_0, g_call_0)}, AssociativeOp());

    // f(x) = max(f(x) + g(rx), f(x) - 3) -> f(x) + max(g(rx) - 3)
    check_associativity("f", {x}, {max(f_call_0 + g_call_0, f_call_0 - 3)},
                        AssociativeOp(
                          AssociativePattern(x + y, 0, true),
                          {Replacement("x", f_call_0)},
                          {Replacement("y", max(g_call_0, -3))},
                          true)
                        );

    // f(x) = min(4, g(rx)) -> trivially associative
    check_associativity("f", {x}, {min(4, g_call_0)},
                        AssociativeOp(
                          AssociativePattern(y, make_const(t, 0), true),
                          {Replacement("", Expr())},
                          {Replacement("y", min(g_call_0, 4))},
                          true)
                        );

    // f(x) = f(x) -> associative but doesn't really make any sense, so we'll treat it as non-associative
    check_associativity("f", {x}, {f_call_0}, AssociativeOp());

    // f(x) = max(max(min(f(x), g(rx) + 2), f(x)), g(rx) + 2) -> can be simplified into max(f(x), g(rx) + 2)
    check_associativity("f", {x}, {max(max(min(f_call_0, g_call_0 + 2), f_call_0), g_call_0 + 2)},
                        AssociativeOp(
                          AssociativePattern(max(x, y), t.min(), true),
                          {Replacement("x", f_call_0)},
                          {Replacement("y", g_call_0 + 2)},
                          true)
                        );

    // Complex multiplication: f(x) = Tuple(f(x)[0]*g(r.x)[0] - f(x)[1]*g(r.x)[1], f(x)[0]*g(r.x)[1] + f(x)[1]*g(r.x)[0])
    check_associativity("f", {x}, {f_call_0*g_call_0 - f_call_1*g_call_1, f_call_0*g_call_1 + f_call_1*g_call_0},
                        AssociativeOp(
                          AssociativePattern(
                            {xs[0]*ys[0] - xs[1]*ys[1], xs[1]*ys[0] + xs[0]*ys[1]},
                            {make_const(t, 1), make_const(t, 0)},
                            true),
                          {Replacement("x0", f_call_0), Replacement("x1", f_call_1)},
                          {Replacement("y0", g_call_0), Replacement("y1", g_call_1)},
                          true)
                        );

    // 1D argmin: f(x) = Tuple(min(f(x)[0], g(r.x)[0]), select(f(x)[0] < g(r.x)[0], f(x)[1], r.x)
    check_associativity("f", {x}, {min(f_call_0, g_call_0), select(f_call_0 < g_call_0, f_call_1, rx)},
                        AssociativeOp(
                          AssociativePattern(
                            {min(xs[0], ys[0]), select(xs[0] < ys[0], xs[1], ys[1])},
                            {t.max(), make_const(t, 0)},
                            true),
                          {Replacement("x0", f_call_0), Replacement("x1", f_call_1)},
                          {Replacement("y0", g_call_0), Replacement("y1", rx)},
                          true)
                        );

    // f(x) = max(x0, f(x)) -> x0 may conflict with the wildcard associative op pattern
    check_associativity("f", {x}, {max(xs[0], f_call_0)},
                        AssociativeOp(
                          AssociativePattern(max(x, y), t.min(), true),
                          {Replacement("x", f_call_0)},
                          {Replacement("y", xs[0])},
                          true)
                        );

    {
        Expr ry = Variable::make(t, "ry");
        Expr f_xy_call_0 = Call::make(t, "f", {x, y}, Call::CallType::Halide, nullptr, 0);
        Expr f_xy_call_1 = Call::make(t, "f", {x, y}, Call::CallType::Halide, nullptr, 1);
        Expr f_xy_call_2 = Call::make(t, "f", {x, y}, Call::CallType::Halide, nullptr, 2);
        Expr g_xy_call_0 = Call::make(t, "g", {rx, ry}, Call::CallType::Halide, nullptr, 0);

        // 2D argmin:
        // f(x, y) = Tuple(min(f(x, y)[0], g(r.x, r.y)[0]),
        //                 select(f(x, y)[0] < g(r.x, r.y)[0], f(x)[1], r.x),
        //                 select(f(x, y)[0] < g(r.x, r.y)[0], f(x)[2], r.y))
        check_associativity("f", {x, y},
                            {min(f_xy_call_0, g_xy_call_0),
                             select(f_xy_call_0 < g_xy_call_0, f_xy_call_1, rx),
                             select(f_xy_call_0 < g_xy_call_0, f_xy_call_2, ry)},
                            AssociativeOp(
                              AssociativePattern(
                                {min(xs[0], ys[0]), select(xs[0] < ys[0], xs[1], ys[1]), select(xs[0] < ys[0], xs[2], ys[2])},
                                {t.max(), make_const(t, 0), make_const(t, 0)},
                                true),
                              {Replacement("x0", f_xy_call_0), Replacement("x1", f_xy_call_1), Replacement("x2", f_xy_call_2)},
                              {Replacement("y0", g_xy_call_0), Replacement("y1", rx), Replacement("y2", ry)},
                              true)
                            );
    }

    std::cout << "Associativity test passed" << std::endl;
}


}
}
