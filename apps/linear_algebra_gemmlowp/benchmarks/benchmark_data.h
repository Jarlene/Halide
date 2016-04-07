#ifndef BENCHMARK_DATA_H
#define BENCHMARK_DATA_H

namespace benchmark {

const int a_offset = -75;
const int b_offset = -91;
const int c_offset = 74980;
const int c_mult_int = 123;
const int c_shift = 20;

const int benchmark_gemm_sizes[] = {
    10, 10, 10,
    20, 20, 20,
    30, 30, 30,
    40, 40, 40,
    50, 50, 50,
    60, 60, 60,
    64, 256, 147,
    100, 100, 1,
    100, 100, 100,
    1000, 1000, 1,
    1000, 1000, 10,
    1000, 1000, 100,
    1000, 1000, 1000
}

// These are the m, n, k sizes for a typical GoogLeNet.
const int googlenet_gemm_sizes[] = {
    12544, 64,  147, 3136, 64,   64,   3136, 192,  576,  784, 64,   192,
    784,   96,  192, 784,  128,  864,  784,  16,   192,  784, 32,   400,
    784,   32,  192, 784,  128,  256,  784,  128,  256,  784, 192,  1152,
    784,   32,  256, 784,  96,   800,  784,  64,   256,  196, 192,  480,
    196,   96,  480, 196,  204,  864,  196,  16,   480,  196, 48,   400,
    196,   64,  480, 196,  160,  508,  196,  112,  508,  196, 224,  1008,
    196,   24,  508, 196,  64,   600,  196,  64,   508,  196, 128,  512,
    196,   128, 512, 196,  256,  1152, 196,  24,   512,  196, 64,   600,
    196,   64,  512, 196,  112,  512,  196,  144,  512,  196, 288,  1296,
    196,   32,  512, 196,  64,   800,  196,  64,   512,  196, 256,  528,
    196,   160, 528, 196,  320,  1440, 196,  32,   528,  196, 128,  800,
    196,   128, 528, 49,   256,  832,  49,   160,  832,  49,  320,  1440,
    49,    48,  832, 49,   128,  1200, 49,   128,  832,  49,  384,  832,
    49,    192, 832, 49,   384,  1728, 49,   48,   832,  49,  128,  1200,
    49,    128, 832, 16,   128,  508,  1,    1024, 2048, 1,   1008, 1024,
    16,    128, 528, 1,    1024, 2048, 1,    1008, 1024, 1,   1008, 1024,
};

// These are the m, n, k sizes for a small model with large batches.
const int small_model_gemm_sizes[] = {
    29232, 16, 25, 7308, 6, 400, 203, 3002, 216,
};

} // namespace benchmark

#endif  // BENCHMARK_DATA_H