#include "Halide.h"

using namespace Halide;

namespace {

class UserContext : public Generator<UserContext> {
public:
    ImageParam input{ Int(32), 2, "input" };
    Param<void *> user_context{ "__user_context" };

    Func build() override {
        Var x, y;

        Func g;
        g(x, y) = input(x, y) * 2;
        g.compute_root();

        Func f;
        f(x, y) = g(x, y);

        f.parallel(y);
        f.trace_stores();
        return f;
    }
};

RegisterGenerator<UserContext> register_my_gen("user_context");

}  // namespace
