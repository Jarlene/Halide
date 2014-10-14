// Generator requires C++11
#if __cplusplus > 199711L

#include "Generator.h"

namespace {

// Return true iff the name is valid for Generators or Params.
// (NOTE: gcc didn't add proper std::regex support until v4.9;
// we don't yet require this, hence the hand-rolled replacement.)

// Note that this includes '_'
bool is_alpha(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c == '_'); }

bool is_alnum(char c) { return is_alpha(c) || (c >= '0' && c <= '9'); }

bool is_valid_name(const std::string& n) {
    if (n.empty()) return false;
    if (!is_alpha(n[0])) return false;
    for (size_t i = 1; i < n.size(); ++i) {
        if (!is_alnum(n[i])) return false;
    }
    return true;
}

}  // namespace

namespace Halide {
namespace Internal {

int generate_filter_main(int argc, char **argv, std::ostream &cerr) {
    const char kUsage[] = "gengen [-g GENERATOR_NAME] [-f FUNCTION_NAME] [-o OUTPUT_DIR]  "
                          "target=target-string [generator_arg=value [...]]\n";

    std::map<std::string, std::string> flags_info = { { "-f", "" }, { "-g", "" }, { "-o", "" } };
    std::map<std::string, std::string> generator_args;

    for (int i = 1; i < argc; ++i) {
        if (argv[i][0] != '-') {
            std::vector<std::string> v = split_string(argv[i], "=");
            if (v.size() != 2 || v[0].empty() || v[1].empty()) {
                cerr << kUsage;
                return 1;
            }
            generator_args[v[0]] = v[1];
            continue;
        }
        auto it = flags_info.find(argv[i]);
        if (it != flags_info.end()) {
            if (i + 1 >= argc) {
                cerr << kUsage;
                return 1;
            }
            it->second = argv[i + 1];
            ++i;
            continue;
        }
        cerr << "Unknown flag: " << argv[i] << "\n";
        cerr << kUsage;
        return 1;
    }

    std::vector<std::string> generator_names = Internal::GeneratorRegistry::enumerate();
    if (generator_names.size() == 0) {
        cerr << "No generators have been registered\n";
        cerr << kUsage;
        return 1;
    }

    std::string generator_name = flags_info["-g"];
    if (generator_name.empty()) {
        // If -g isn't specified, but there's only one generator registered, just use that one.
        if (generator_names.size() != 1) {
            cerr << "-g must be specified if multiple generators are registered\n";
            cerr << kUsage;
            return 1;
        }
        generator_name = generator_names[0];
    }
    std::string function_name = flags_info["-f"];
    if (function_name.empty()) {
        // If -f isn't specified, but there's only one generator registered,
        // just assume function name = generator name.
        if (generator_names.size() != 1) {
            cerr << "-f must be specified if multiple generators are registered\n";
            cerr << kUsage;
            return 1;
        }
        function_name = generator_names[0];
    }
    std::string output_dir = flags_info["-o"];
    if (output_dir.empty()) {
        cerr << "-o must always be specified.\n";
        cerr << kUsage;
        return 1;
    }
    if (generator_args.find("target") == generator_args.end()) {
        cerr << "Target missing\n";
        cerr << kUsage;
        return 1;
    }

    std::unique_ptr<GeneratorBase> gen =
        Internal::GeneratorRegistry::create(generator_name, generator_args);
    if (gen == nullptr) {
        cerr << "Unknown generator: " << generator_name << "\n";
        cerr << kUsage;
        return 1;
    }
    gen->emit_filter(output_dir, function_name);
    return 0;
}

GeneratorParamBase::GeneratorParamBase(const std::string &name) : name(name) {
    ObjectInstanceRegistry::register_instance(this, 0, ObjectInstanceRegistry::GeneratorParam,
                                              this);
}

GeneratorParamBase::~GeneratorParamBase() { ObjectInstanceRegistry::unregister_instance(this); }

/* static */
GeneratorRegistry &GeneratorRegistry::get_registry() {
    static GeneratorRegistry *registry = new GeneratorRegistry;
    return *registry;
}

/* static */
void GeneratorRegistry::register_factory(const std::string &name,
                                         std::unique_ptr<GeneratorFactory> factory) {
    user_assert(is_valid_name(name)) << "Invalid Generator name: " << name;
    GeneratorRegistry &registry = get_registry();
    std::lock_guard<std::mutex> lock(registry.mutex);
    internal_assert(registry.factories.find(name) == registry.factories.end())
        << "Duplicate Generator name: " << name;
    registry.factories[name] = std::move(factory);
}

/* static */
void GeneratorRegistry::unregister_factory(const std::string &name) {
    GeneratorRegistry &registry = get_registry();
    std::lock_guard<std::mutex> lock(registry.mutex);
    internal_assert(registry.factories.find(name) != registry.factories.end())
        << "Generator not found: " << name;
    registry.factories.erase(name);
}

/* static */
std::unique_ptr<GeneratorBase> GeneratorRegistry::create(const std::string &name,
                                                         const GeneratorParamValues &params) {
    GeneratorRegistry &registry = get_registry();
    std::lock_guard<std::mutex> lock(registry.mutex);
    auto it = registry.factories.find(name);
    user_assert(it != registry.factories.end()) << "Generator not found: " << name;
    return it->second->create(params);
}

/* static */
std::vector<std::string> GeneratorRegistry::enumerate() {
    GeneratorRegistry &registry = get_registry();
    std::lock_guard<std::mutex> lock(registry.mutex);
    std::vector<std::string> result;
    for (auto it = registry.factories.begin(); it != registry.factories.end(); ++it) {
        result.push_back(it->first);
    }
    return result;
}

GeneratorBase::GeneratorBase(size_t size) : size(size), params_built(false) {
    ObjectInstanceRegistry::register_instance(this, size, ObjectInstanceRegistry::Generator, this);
}

GeneratorBase::~GeneratorBase() { ObjectInstanceRegistry::unregister_instance(this); }

void GeneratorBase::build_params() {
    if (!params_built) {
        std::vector<void *> vf = ObjectInstanceRegistry::instances_in_range(
            this, size, ObjectInstanceRegistry::FilterParam);
        for (size_t i = 0; i < vf.size(); ++i) {
            Parameter *param = static_cast<Parameter *>(vf[i]);
            internal_assert(param != nullptr);
            user_assert(is_valid_name(param->name())) << "Invalid Param name: " << param->name();
            user_assert(filter_params.find(param->name()) == filter_params.end())
                << "Duplicate Param name: " << param->name();
            filter_params[param->name()] = param;
            filter_arguments.push_back(Argument(param->name(), param->is_buffer(), param->type()));
        }

        std::vector<void *> vg = ObjectInstanceRegistry::instances_in_range(
            this, size, ObjectInstanceRegistry::GeneratorParam);
        for (size_t i = 0; i < vg.size(); ++i) {
            GeneratorParamBase *param = static_cast<GeneratorParamBase *>(vg[i]);
            internal_assert(param != nullptr);
            user_assert(is_valid_name(param->name)) << "Invalid GeneratorParam name: " << param->name;
            user_assert(generator_params.find(param->name) == generator_params.end())
                << "Duplicate GeneratorParam name: " << param->name;
            generator_params[param->name] = param;
        }
        params_built = true;
    }
}

void GeneratorBase::set_generator_param_values(const GeneratorParamValues &params) {
    build_params();
    for (auto key_value : params) {
        const std::string &key = key_value.first;
        const std::string &value = key_value.second;
        auto param = generator_params.find(key);
        user_assert(param != generator_params.end())
            << "Generator has no GeneratorParam named: " << key;
        param->second->set_from_string(value);
    }
}

void GeneratorBase::emit_filter(const std::string &output_dir, const std::string &function_name,
                                const std::string &file_base_name, const EmitOptions &options) {
    Func func = build();

    build_params();

    std::string base_path = file_base_name.empty() ?
        output_dir + "/" + function_name :
        output_dir + "/" + file_base_name;
    if (options.emit_o) {
        func.compile_to_object(base_path + ".o", filter_arguments, function_name, target);
    }
    if (options.emit_h) {
        func.compile_to_header(base_path + ".h", filter_arguments, function_name);
    }
    if (options.emit_cpp) {
        func.compile_to_c(base_path + ".cpp", filter_arguments, function_name, target);
    }
    if (options.emit_assembly) {
        func.compile_to_assembly(base_path + ".s", filter_arguments, function_name, target);
    }
    if (options.emit_bitcode) {
        func.compile_to_bitcode(base_path + ".bc", filter_arguments, function_name, target);
    }
    if (options.emit_stmt) {
        func.compile_to_lowered_stmt(base_path + ".stmt", Halide::Text, target);
    }
    if (options.emit_stmt_html) {
        func.compile_to_lowered_stmt(base_path + ".html", Halide::HTML, target);
    }
}

}  // namespace Internal
}  // namespace Halide

#endif  // __cplusplus > 199711L
