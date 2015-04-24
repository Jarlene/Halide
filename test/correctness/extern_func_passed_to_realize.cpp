#include "Halide.h"
#include <stdio.h>

using namespace Halide;

#ifdef _MSC_VER
#define DLLEXPORT __declspec(dllexport)
#else
#define DLLEXPORT
#endif

int call_counter = 0;
extern "C" DLLEXPORT float my_func(int x, float y) {
    call_counter++;
    return x*y;
}
HalideExtern_2(float, my_func, int, float);

int main(int argc, char **argv) {
    std::vector<ExternFuncArgument> args;
    args.push_back(user_context_value());

    Var x, y;
    Func monitor;
    monitor(x, y) = my_func(x, cast<float>(y));

    Func f;
    f.define_extern("extern_func", args, Float(32), 2);

    Image<float> imf = f.realize(32, 32, get_jit_target_from_environment(), { { "extern_func", monitor }});

    // Check the result was what we expected
    for (int i = 0; i < 32; i++) {
        for (int j = 0; j < 32; j++) {
            float correct = (float)(i*j);
            float delta = imf(i, j) - correct;
            if (delta < -0.001 || delta > 0.001) {
                printf("imf[%d, %d] = %f instead of %f\n", i, j, imf(i, j), correct);
                return -1;
            }
        }
    }

    if (call_counter != 32*32) {
        printf("In extern_func_passed_to_realize, my_func was called %d times instead of %d\n", call_counter, 32*32);
        return -1;
    }

    printf("Success!\n");
    return 0;
}
