#include "fb/codegen.hpp"

#include "fb/build_config.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <llvm-c/Analysis.h>
#include <llvm-c/Core.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>
#include <llvm-c/Transforms/PassBuilder.h>
#include <memory>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace fb {
namespace {

/// An id's position in the vector it indexes, like `mir_module_t::functions`.
template <typename Id>
std::size_t index(const Id id) {
    return std::to_underlying(id);
}

// Deleters that let std::unique_ptr own LLVM's objects.
struct context_deleter_t {
    void operator()(LLVMContextRef context) const {
        LLVMContextDispose(context);
    }
};

struct module_deleter_t {
    void operator()(LLVMModuleRef module) const {
        LLVMDisposeModule(module);
    }
};

struct builder_deleter_t {
    void operator()(LLVMBuilderRef builder) const {
        LLVMDisposeBuilder(builder);
    }
};

struct machine_deleter_t {
    void operator()(LLVMTargetMachineRef machine) const {
        LLVMDisposeTargetMachine(machine);
    }
};

/// Takes a message LLVM allocated, like an error, as a std::string.
std::string take_message(char* message) {
    std::string text = message != nullptr ? message : "";
    LLVMDisposeMessage(message);
    return text;
}

/// The canonical form of a triple: GCC's "x86_64-w64-mingw32" is "x86_64-w64-windows-gnu".
std::string normalize(const std::string& triple) {
    return take_message(LLVMNormalizeTargetTriple(triple.c_str()));
}

/// A function and its type, which LLVM needs for every call.
struct function_ref_t {
    LLVMTypeRef type = nullptr;
    LLVMValueRef value = nullptr;
};

/// Turns a MIR module into an LLVM module. Build it once, then print its IR or write an object
/// file.
class codegen_t {
public:
    codegen_t(const mir_module_t& module, const source_file_t& file,
              const codegen_options_t& options)
        : m_mir(module), m_file(file), m_options(options),
          m_triple(normalize(options.target_triple.empty()
                                 ? std::string(build_config::target_triple)
                                 : options.target_triple)),
          m_context(LLVMContextCreate()),
          m_module(LLVMModuleCreateWithNameInContext(file.path().c_str(), m_context.get())),
          m_builder(LLVMCreateBuilderInContext(m_context.get())) {}

    /// Builds the module, verifies it, and optimizes it in release builds.
    std::expected<void, std::string> build() {
        if (std::expected<void, std::string> machine = create_machine(); !machine) {
            return machine;
        }

        declare_types();
        declare_runtime();
        declare_globals();
        declare_functions();
        declare_classes();
        for (std::size_t i = 0; i < m_mir.functions.size(); ++i) {
            generate(i);
        }

        for (std::size_t i = 0; i < m_mir.classes.size(); ++i) {
            generate_destroy(i);
        }

        generate_main();

        char* error = nullptr;
        if (LLVMVerifyModule(m_module.get(), LLVMReturnStatusAction, &error) != 0) {
            return std::unexpected("invalid LLVM IR: " + take_message(error));
        }

        take_message(error);
        if (m_options.mode == build_mode_t::RELEASE) {
            return optimize();
        }

        return {};
    }

    [[nodiscard]] std::string ir() const {
        return take_message(LLVMPrintModuleToString(m_module.get()));
    }

    [[nodiscard]] std::expected<void, std::string>
    write_object(const std::filesystem::path& path) const {
        std::string name = path.string();
        char* error = nullptr;
        if (LLVMTargetMachineEmitToFile(m_machine.get(), m_module.get(), name.data(),
                                        LLVMObjectFile, &error) != 0) {
            return std::unexpected(take_message(error));
        }

        return {};
    }

private:
    const mir_module_t& m_mir;
    const source_file_t& m_file;
    const codegen_options_t& m_options;
    /// The options' target, or else the one this compiler was built for, normalized.
    std::string m_triple;
    std::unique_ptr<LLVMOpaqueContext, context_deleter_t> m_context;
    std::unique_ptr<LLVMOpaqueModule, module_deleter_t> m_module;
    std::unique_ptr<LLVMOpaqueBuilder, builder_deleter_t> m_builder;
    std::unique_ptr<LLVMOpaqueTargetMachine, machine_deleter_t> m_machine;

    LLVMTypeRef m_void = nullptr;
    LLVMTypeRef m_bool = nullptr;
    LLVMTypeRef m_i32 = nullptr;
    LLVMTypeRef m_i64 = nullptr;
    LLVMTypeRef m_ptr = nullptr;

    // The runtime's functions
    function_ref_t m_start;
    function_ref_t m_finish;
    function_ref_t m_retain;
    function_ref_t m_release;
    function_ref_t m_concat;
    function_ref_t m_join;
    function_ref_t m_equal;
    function_ref_t m_from_int;
    function_ref_t m_from_uint;
    function_ref_t m_from_float32;
    function_ref_t m_from_float64;
    function_ref_t m_from_bool;
    function_ref_t m_power_int;
    function_ref_t m_power_uint;
    function_ref_t m_print;
    function_ref_t m_panic;
    function_ref_t m_object_new;
    function_ref_t m_object_retain;
    function_ref_t m_object_release;

    /// Each MIR global's LLVM global, by global_id_t.
    std::vector<LLVMValueRef> m_globals;
    /// Each MIR function's declaration, by function_id_t.
    std::vector<function_ref_t> m_functions;
    std::unordered_map<std::string, LLVMValueRef> m_strings;   // static Strings, by text
    std::unordered_map<std::string, LLVMValueRef> m_c_strings; // C strings for panics
    /// Each Struct's LLVM type, made the first time it's needed.
    std::unordered_map<type_id_t, LLVMTypeRef> m_struct_types;
    /// Each Class's object type: the header, then the fields.
    std::unordered_map<type_id_t, LLVMTypeRef> m_object_types;
    /// Each Class's fb_class_t, by its type.
    std::unordered_map<type_id_t, LLVMValueRef> m_classes;
    /// Each Class's destroy, by its index in `mir_module_t::classes`.
    std::vector<function_ref_t> m_destroys;

    // The function being generated
    const mir_function_t* m_function = nullptr;
    LLVMValueRef m_llvm_function = nullptr;
    LLVMBasicBlockRef m_entry = nullptr; // holds the allocas, then jumps to the first block
    std::vector<LLVMValueRef> m_slots;   // where each local lives
    /// The LLVM block for each MIR block, by block_id_t.
    std::vector<LLVMBasicBlockRef> m_blocks;

    [[nodiscard]] LLVMContextRef context() const {
        return m_context.get();
    }

    [[nodiscard]] LLVMBuilderRef builder() const {
        return m_builder.get();
    }

    /// Debug builds panic on overflow and out-of-range shifts; release builds wrap and mask.
    [[nodiscard]] bool checked() const {
        return m_options.mode == build_mode_t::DEBUG;
    }

    // Setup

    /// Sets up the target machine, and gives the module its triple and data layout.
    std::expected<void, std::string> create_machine() {
        // LLVM's targets are registered once per process.
        static std::once_flag initialized;
        std::call_once(initialized, [] {
            LLVMInitializeNativeTarget();
            LLVMInitializeNativeAsmPrinter();
        });

        LLVMTargetRef target = nullptr;
        char* error = nullptr;
        if (LLVMGetTargetFromTriple(m_triple.c_str(), &target, &error) != 0) {
            return std::unexpected(take_message(error));
        }

        const LLVMCodeGenOptLevel level = m_options.mode == build_mode_t::RELEASE
                                              ? LLVMCodeGenLevelDefault
                                              : LLVMCodeGenLevelNone;
        // A "generic" CPU, so programs run on any machine the triple covers, not just this one.
        m_machine.reset(LLVMCreateTargetMachine(target, m_triple.c_str(), "generic", "", level,
                                                LLVMRelocPIC, LLVMCodeModelDefault));
        LLVMSetTarget(m_module.get(), m_triple.c_str());
        LLVMTargetDataRef layout = LLVMCreateTargetDataLayout(m_machine.get());
        LLVMSetModuleDataLayout(m_module.get(), layout);
        LLVMDisposeTargetData(layout);
        return {};
    }

    std::expected<void, std::string> optimize() {
        LLVMPassBuilderOptionsRef options = LLVMCreatePassBuilderOptions();
        LLVMErrorRef error = LLVMRunPasses(m_module.get(), "default<O2>", m_machine.get(), options);
        LLVMDisposePassBuilderOptions(options);
        if (error != nullptr) {
            char* message = LLVMGetErrorMessage(error);
            const std::string text = message;
            LLVMDisposeErrorMessage(message);
            return std::unexpected(text);
        }

        return {};
    }

    void declare_types() {
        m_void = LLVMVoidTypeInContext(context());
        m_bool = LLVMInt1TypeInContext(context());
        m_i32 = LLVMInt32TypeInContext(context());
        m_i64 = LLVMInt64TypeInContext(context());
        m_ptr = LLVMPointerTypeInContext(context(), 0);
    }

    /// A String is a pointer to its header, a Struct is an LLVM struct of its fields, and no type
    /// (a function that returns nothing) is void. A String? or Class? is a pointer that's null
    /// for Null, and any other T? is a flag for whether it holds a value, then the T.
    [[nodiscard]] LLVMTypeRef llvm_type(const type_id_t type) {
        switch (type) {
        case type_id_t::BOOL:
            return m_bool;
        case type_id_t::INT8:
        case type_id_t::UINT8:
        case type_id_t::INT16:
        case type_id_t::UINT16:
        case type_id_t::INT32:
        case type_id_t::UINT32:
        case type_id_t::INT64:
        case type_id_t::UINT64:
            return LLVMIntTypeInContext(context(), bit_width(type));
        case type_id_t::FLOAT32:
            return LLVMFloatTypeInContext(context());
        case type_id_t::FLOAT64:
            return LLVMDoubleTypeInContext(context());
        case type_id_t::STRING:
            return m_ptr;
        default:
            if (m_mir.types->get(type).kind == type_kind_t::STRUCT) {
                return struct_type(type);
            }

            // A Class is a pointer to its object.
            if (is_class(type) || is_nullable_reference(type)) {
                return m_ptr;
            }

            if (is_nullable(type)) {
                std::array<LLVMTypeRef, 2> fields{ m_bool, llvm_type(inner_of(type)) };
                return LLVMStructTypeInContext(context(), fields.data(),
                                               static_cast<unsigned>(fields.size()), 0);
            }

            return m_void;
        }
    }

    [[nodiscard]] bool is_class(const type_id_t type) const {
        return m_mir.types->get(type).kind == type_kind_t::CLASS;
    }

    [[nodiscard]] bool is_nullable(const type_id_t type) const {
        return m_mir.types->get(type).kind == type_kind_t::NULLABLE;
    }

    /// The T of a T?.
    [[nodiscard]] type_id_t inner_of(const type_id_t type) const {
        return m_mir.types->get(type).inner;
    }

    /// A String? or Class?: the reference itself, which is null for Null.
    [[nodiscard]] bool is_nullable_reference(const type_id_t type) const {
        return is_nullable(type) &&
               (inner_of(type) == type_id_t::STRING || is_class(inner_of(type)));
    }

    [[nodiscard]] static bool is_null(const operand_t& operand) {
        const auto* value = std::get_if<const_value_t>(&operand);
        return value != nullptr && std::holds_alternative<std::nullptr_t>(value->data);
    }

    /// A Class's object in the IR, like fb_object_t's header followed by the fields. A field that
    /// refers to a Class is a pointer, so making the fields' types never comes back to it.
    LLVMTypeRef object_type(const type_id_t type) {
        if (const auto found = m_object_types.find(type); found != m_object_types.end()) {
            return found->second;
        }

        std::vector<LLVMTypeRef> fields{ m_i64, m_ptr };
        for (const field_t& field : m_mir.types->get(type).fields) {
            fields.push_back(llvm_type(field.type));
        }

        const std::string name = std::format("fb.{}", m_mir.types->name(type));
        LLVMTypeRef result = LLVMStructCreateNamed(context(), name.c_str());
        LLVMStructSetBody(result, fields.data(), static_cast<unsigned>(fields.size()), 0);
        m_object_types.emplace(type, result);
        return result;
    }

    /// "fb.Point" in the IR. A Struct never contains itself, so making its fields' types can't
    /// come back to it.
    LLVMTypeRef struct_type(const type_id_t type) {
        if (const auto found = m_struct_types.find(type); found != m_struct_types.end()) {
            return found->second;
        }

        std::vector<LLVMTypeRef> fields;
        for (const field_t& field : m_mir.types->get(type).fields) {
            fields.push_back(llvm_type(field.type));
        }

        const std::string name = std::format("fb.{}", m_mir.types->name(type));
        LLVMTypeRef result = LLVMStructCreateNamed(context(), name.c_str());
        LLVMStructSetBody(result, fields.data(), static_cast<unsigned>(fields.size()), 0);
        m_struct_types.emplace(type, result);
        return result;
    }

    function_ref_t declare(const char* name, LLVMTypeRef result,
                           std::vector<LLVMTypeRef> parameters) {
        LLVMTypeRef type = LLVMFunctionType(result, parameters.data(),
                                            static_cast<unsigned>(parameters.size()), 0);
        return { .type = type, .value = LLVMAddFunction(m_module.get(), name, type) };
    }

    /// The runtime library's functions; see runtime/include/fb/runtime.hpp.
    void declare_runtime() {
        LLVMTypeRef f32 = LLVMFloatTypeInContext(context());
        LLVMTypeRef f64 = LLVMDoubleTypeInContext(context());
        m_start = declare("fb_start", m_void, {});
        m_finish = declare("fb_finish", m_void, { m_ptr, m_i64 });
        m_retain = declare("fb_string_retain", m_void, { m_ptr });
        m_release = declare("fb_string_release", m_void, { m_ptr });
        m_concat = declare("fb_string_concat", m_ptr, { m_ptr, m_ptr });
        m_join = declare("fb_string_join", m_ptr, { m_ptr, m_i64 });
        m_equal = declare("fb_string_equal", m_i32, { m_ptr, m_ptr });
        m_from_int = declare("fb_string_from_int", m_ptr, { m_i64 });
        m_from_uint = declare("fb_string_from_uint", m_ptr, { m_i64 });
        m_from_float32 = declare("fb_string_from_float32", m_ptr, { f32 });
        m_from_float64 = declare("fb_string_from_float64", m_ptr, { f64 });
        m_from_bool = declare("fb_string_from_bool", m_ptr, { m_i32 });
        m_power_int = declare("fb_power_int", m_i64, { m_i64, m_i64, m_i32, m_i32, m_ptr });
        m_power_uint = declare("fb_power_uint", m_i64, { m_i64, m_i64, m_i32, m_i32, m_ptr });
        m_print = declare("fb_print", m_void, { m_ptr });
        m_panic = declare("fb_panic", m_void, { m_ptr, m_ptr });
        m_object_new = declare("fb_object_new", m_ptr, { m_ptr });
        m_object_retain = declare("fb_object_retain", m_void, { m_ptr });
        m_object_release = declare("fb_object_release", m_void, { m_ptr });
    }

    /// Each Class's fb_class_t: its name, its objects' size, and its destroy, which
    /// generate_destroy() fills in. The runtime counts the Class's objects in it.
    void declare_classes() {
        std::array<LLVMTypeRef, 4> fields{ m_ptr, m_i64, m_ptr, m_i64 };
        LLVMTypeRef class_type = LLVMStructTypeInContext(context(), fields.data(),
                                                         static_cast<unsigned>(fields.size()), 0);
        for (const mir_class_t& info : m_mir.classes) {
            const std::string_view name = m_mir.types->name(info.type);
            const std::string destroy_name = std::format("fb.{}.destroy", name);
            const function_ref_t destroy = declare(destroy_name.c_str(), m_void, { m_ptr });
            LLVMSetLinkage(destroy.value, LLVMInternalLinkage);
            m_destroys.push_back(destroy);

            std::array<LLVMValueRef, 4> values{
                c_string(std::string(name)),
                LLVMSizeOf(object_type(info.type)),
                destroy.value,
                LLVMConstInt(m_i64, 0, 0),
            };
            const std::string global_name = std::format("fb.{}.class", name);
            LLVMValueRef global = LLVMAddGlobal(m_module.get(), class_type, global_name.c_str());
            LLVMSetInitializer(global,
                               LLVMConstStructInContext(context(), values.data(),
                                                        static_cast<unsigned>(values.size()), 0));
            LLVMSetLinkage(global, LLVMInternalLinkage);
            m_classes.emplace(info.type, global);
        }
    }

    /// Globals start out holding their `initial` value. Like functions, their names get an "fb."
    /// prefix.
    void declare_globals() {
        for (const global_t& global : m_mir.globals) {
            const std::string name = std::format("fb.{}", global.name);
            LLVMValueRef value =
                LLVMAddGlobal(m_module.get(), llvm_type(global.type), name.c_str());
            LLVMSetInitializer(value, global.initial ? constant(*global.initial)
                                                     : zero_constant(global.type));
            LLVMSetLinkage(value, LLVMInternalLinkage);
            m_globals.push_back(value);
        }
    }

    /// Names get an "fb." prefix, so they can't clash with C functions like the runtime's.
    void declare_functions() {
        for (const mir_function_t& function : m_mir.functions) {
            std::vector<LLVMTypeRef> parameters;
            for (std::size_t i = 0; i < function.parameter_count; ++i) {
                const local_t& parameter = function.locals[i];
                parameters.push_back(parameter.by_ref ? m_ptr : llvm_type(parameter.type));
            }

            const std::string name = "fb." + function.name;
            const function_ref_t declared =
                declare(name.c_str(), llvm_type(function.return_type), std::move(parameters));
            LLVMSetLinkage(declared.value, LLVMInternalLinkage);
            m_functions.push_back(declared);
        }
    }

    // Constants

    /// A String constant becomes a static literal. Null is its T?'s zero, and any other T?
    /// constant holds its value.
    LLVMValueRef constant(const const_value_t& value) {
        if (std::holds_alternative<std::nullptr_t>(value.data)) {
            return zero_constant(value.type);
        }

        if (is_nullable(value.type)) {
            LLVMValueRef inner = constant({ .type = inner_of(value.type), .data = value.data });
            if (is_nullable_reference(value.type)) {
                return inner;
            }

            std::array<LLVMValueRef, 2> fields{ LLVMConstInt(m_bool, 1, 0), inner };
            return LLVMConstStructInContext(context(), fields.data(),
                                            static_cast<unsigned>(fields.size()), 0);
        }

        LLVMTypeRef type = llvm_type(value.type);
        if (const auto* flag = std::get_if<bool>(&value.data)) {
            return LLVMConstInt(type, *flag ? 1U : 0U, 0);
        }

        if (const auto* number = std::get_if<std::int64_t>(&value.data)) {
            return LLVMConstInt(type, static_cast<unsigned long long>(*number), 1);
        }

        if (const auto* number = std::get_if<std::uint64_t>(&value.data)) {
            return LLVMConstInt(type, *number, 0);
        }

        if (const auto* number = std::get_if<double>(&value.data)) {
            return LLVMConstReal(type, *number);
        }

        return static_string(std::get<std::string>(value.data));
    }

    /// What a variable of `type` starts as. A String's zero is the static "", never null, and a
    /// Struct's has each field's zero. A T? that isn't a reference holds its T's zero while it's
    /// Null, so releasing what's inside is always safe.
    LLVMValueRef zero_constant(const type_id_t type) {
        if (type == type_id_t::STRING) {
            return static_string("");
        }

        if (is_nullable(type) && !is_nullable_reference(type)) {
            LLVMValueRef inner = zero_constant(inner_of(type));
            std::array<LLVMValueRef, 2> fields{ LLVMConstInt(m_bool, 0, 0), inner };
            return LLVMConstStructInContext(context(), fields.data(),
                                            static_cast<unsigned>(fields.size()), 0);
        }

        const type_t& info = m_mir.types->get(type);
        if (info.kind != type_kind_t::STRUCT) {
            return LLVMConstNull(llvm_type(type));
        }

        std::vector<LLVMValueRef> fields;
        fields.reserve(info.fields.size());
        for (const field_t& field : info.fields) {
            fields.push_back(zero_constant(field.type));
        }

        return LLVMConstNamedStruct(llvm_type(type), fields.data(),
                                    static_cast<unsigned>(fields.size()));
    }

    /// A String literal: a static block with refcount -1, which retain and release ignore.
    LLVMValueRef static_string(const std::string& text) {
        if (const auto found = m_strings.find(text); found != m_strings.end()) {
            return found->second;
        }

        std::array<LLVMValueRef, 3> fields{
            LLVMConstAllOnes(m_i64),
            LLVMConstInt(m_i64, text.size(), 0),
            LLVMConstStringInContext2(context(), text.data(), text.size(), 1),
        };
        LLVMValueRef value = LLVMConstStructInContext(context(), fields.data(),
                                                      static_cast<unsigned>(fields.size()), 0);
        LLVMValueRef global = LLVMAddGlobal(m_module.get(), LLVMTypeOf(value), "string");
        LLVMSetInitializer(global, value);
        LLVMSetGlobalConstant(global, 1);
        LLVMSetLinkage(global, LLVMPrivateLinkage);
        LLVMSetUnnamedAddress(global, LLVMGlobalUnnamedAddr);
        // The runtime reads it as an fb_string_t, so it needs the header's alignment.
        LLVMSetAlignment(global, 8);
        m_strings.emplace(text, global);
        return global;
    }

    /// A NUL-terminated copy of `text`, for the runtime's `const char*` parameters.
    LLVMValueRef c_string(const std::string& text) {
        if (const auto found = m_c_strings.find(text); found != m_c_strings.end()) {
            return found->second;
        }

        LLVMValueRef value = LLVMConstStringInContext2(context(), text.data(), text.size(), 0);
        LLVMValueRef global = LLVMAddGlobal(m_module.get(), LLVMTypeOf(value), "text");
        LLVMSetInitializer(global, value);
        LLVMSetGlobalConstant(global, 1);
        LLVMSetLinkage(global, LLVMPrivateLinkage);
        LLVMSetUnnamedAddress(global, LLVMGlobalUnnamedAddr);
        m_c_strings.emplace(text, global);
        return global;
    }

    /// An fb_location_t for a span, for panics to report.
    LLVMValueRef location(const span_t span) {
        const location_t start = m_file.location_of(span.begin, column_unit_t::CODEPOINT);
        std::array<LLVMValueRef, 3> fields{
            c_string(m_file.path()),
            LLVMConstInt(m_i32, start.line + 1, 0),
            LLVMConstInt(m_i32, start.column + 1, 0),
        };
        LLVMValueRef value = LLVMConstStructInContext(context(), fields.data(),
                                                      static_cast<unsigned>(fields.size()), 0);
        LLVMValueRef global = LLVMAddGlobal(m_module.get(), LLVMTypeOf(value), "location");
        LLVMSetInitializer(global, value);
        LLVMSetGlobalConstant(global, 1);
        LLVMSetLinkage(global, LLVMPrivateLinkage);
        return global;
    }

    // Functions

    /// Fills in the body of a declared function.
    void generate(const std::size_t function_index) {
        m_function = &m_mir.functions[function_index];
        m_llvm_function = m_functions[function_index].value;
        m_entry = LLVMAppendBasicBlockInContext(context(), m_llvm_function, "entry");
        LLVMPositionBuilderAtEnd(builder(), m_entry);

        // Every local lives in an alloca; LLVM turns them into registers when optimizing. A
        // ByRef parameter is already the address of the caller's variable.
        m_slots.clear();
        for (std::size_t i = 0; i < m_function->locals.size(); ++i) {
            const local_t& local = m_function->locals[i];
            const bool parameter = i < m_function->parameter_count;
            LLVMValueRef argument =
                parameter ? LLVMGetParam(m_llvm_function, static_cast<unsigned>(i)) : nullptr;
            if (local.by_ref) {
                m_slots.push_back(argument);
                continue;
            }

            const std::string name(local.name);
            LLVMValueRef slot = LLVMBuildAlloca(builder(), llvm_type(local.type), name.c_str());
            if (parameter) {
                LLVMBuildStore(builder(), argument, slot);
            }

            m_slots.push_back(slot);
        }

        m_blocks.clear();
        for (std::size_t i = 0; i < m_function->blocks.size(); ++i) {
            const std::string name = std::format("bb{}", i);
            m_blocks.push_back(
                LLVMAppendBasicBlockInContext(context(), m_llvm_function, name.c_str()));
        }

        for (std::size_t i = 0; i < m_function->blocks.size(); ++i) {
            LLVMPositionBuilderAtEnd(builder(), m_blocks[i]);
            const basic_block_t& block = m_function->blocks[i];
            for (const statement_t& statement : block.statements) {
                std::visit([this](const auto& node) { this->generate(node); }, statement);
            }

            std::visit([this](const auto& node) { this->generate(node); }, block.terminator);
        }

        // The entry block gets its jump last, since join adds allocas to it along the way.
        LLVMPositionBuilderAtEnd(builder(), m_entry);
        LLVMBuildBr(builder(), m_blocks[0]);
    }

    /// A Class's destroy, which the runtime calls when an object's last owner lets go: it runs
    /// the Class's Destroy(), if it has one, then releases the object's fields.
    void generate_destroy(const std::size_t class_index) {
        const mir_class_t& info = m_mir.classes[class_index];
        LLVMValueRef function = m_destroys[class_index].value;
        LLVMPositionBuilderAtEnd(builder(),
                                 LLVMAppendBasicBlockInContext(context(), function, "entry"));
        LLVMValueRef object = LLVMGetParam(function, 0);
        if (info.destroy) {
            call(m_functions[index(*info.destroy)], { object });
        }

        // The fields come after the header's two.
        const std::vector<field_t>& fields = m_mir.types->get(info.type).fields;
        for (std::uint32_t i = 0; i < fields.size(); ++i) {
            if (m_mir.types->has_references(fields[i].type)) {
                LLVMValueRef field =
                    LLVMBuildStructGEP2(builder(), object_type(info.type), object, i + 2, "");
                for_each_reference(field, fields[i].type, false);
            }
        }

        LLVMBuildRetVoid(builder());
    }

    /// main: starts the runtime, runs the top-level code, releases the Globals, and lets the
    /// runtime check that nothing leaked, with every Class's count.
    void generate_main() {
        const function_ref_t main = declare("main", m_i32, {});
        LLVMPositionBuilderAtEnd(builder(),
                                 LLVMAppendBasicBlockInContext(context(), main.value, "entry"));
        call(m_start, {});
        call(m_functions[index(m_mir.entry)], {});
        for (std::size_t i = 0; i < m_mir.globals.size(); ++i) {
            if (m_mir.types->has_references(m_mir.globals[i].type)) {
                for_each_reference(m_globals[i], m_mir.globals[i].type, false);
            }
        }

        std::vector<LLVMValueRef> classes;
        classes.reserve(m_mir.classes.size());
        for (const mir_class_t& info : m_mir.classes) {
            classes.push_back(m_classes.at(info.type));
        }

        LLVMValueRef list = LLVMConstNull(m_ptr);
        if (!classes.empty()) {
            LLVMValueRef array =
                LLVMConstArray2(m_ptr, classes.data(), static_cast<std::uint64_t>(classes.size()));
            list = LLVMAddGlobal(m_module.get(), LLVMTypeOf(array), "classes");
            LLVMSetInitializer(list, array);
            LLVMSetGlobalConstant(list, 1);
            LLVMSetLinkage(list, LLVMPrivateLinkage);
        }

        call(m_finish, { list, LLVMConstInt(m_i64, classes.size(), 0) });
        LLVMBuildRet(builder(), LLVMConstInt(m_i32, 0, 0));
    }

    LLVMValueRef call(const function_ref_t& function, std::vector<LLVMValueRef> arguments) {
        return LLVMBuildCall2(builder(), function.type, function.value, arguments.data(),
                              static_cast<unsigned>(arguments.size()), "");
    }

    /// Calls an LLVM intrinsic like "llvm.sadd.with.overflow", overloaded on `type`.
    LLVMValueRef call_intrinsic(const std::string_view name, LLVMTypeRef type,
                                std::vector<LLVMValueRef> arguments) {
        const unsigned id = LLVMLookupIntrinsicID(name.data(), name.size());
        const function_ref_t intrinsic{
            .type = LLVMIntrinsicGetType(context(), id, &type, 1),
            .value = LLVMGetIntrinsicDeclaration(m_module.get(), id, &type, 1),
        };
        return call(intrinsic, std::move(arguments));
    }

    /// Branches to a panic when `failed` is true, and carries on otherwise.
    void panic_if(LLVMValueRef failed, const std::string& message, const span_t span) {
        LLVMBasicBlockRef fail = LLVMAppendBasicBlockInContext(context(), m_llvm_function, "panic");
        LLVMBasicBlockRef ok = LLVMAppendBasicBlockInContext(context(), m_llvm_function, "ok");
        LLVMBuildCondBr(builder(), failed, fail, ok);
        LLVMPositionBuilderAtEnd(builder(), fail);
        panic(message, span);
        LLVMPositionBuilderAtEnd(builder(), ok);
    }

    void panic(const std::string& message, const span_t span) {
        call(m_panic, { c_string(message), location(span) });
        LLVMBuildUnreachable(builder());
    }

    // Places and operands

    [[nodiscard]] type_id_t type_of(const place_t& place) const {
        type_id_t type = type_id_t::INVALID;
        if (const auto* local = std::get_if<local_id_t>(&place.root)) {
            type = m_function->locals[index(*local)].type;
        } else {
            type = m_mir.globals[index(std::get<global_id_t>(place.root))].type;
        }

        for (const std::uint32_t field : place.fields) {
            type = m_mir.types->get(type).fields[field].type;
        }

        return type;
    }

    [[nodiscard]] type_id_t type_of(const operand_t& operand) const {
        if (const auto* value = std::get_if<const_value_t>(&operand)) {
            return value->type;
        }

        return type_of(std::get<place_t>(operand));
    }

    /// A local's alloca, a ByRef parameter's pointer, or a global, then a step to each field.
    [[nodiscard]] LLVMValueRef address_of(const place_t& place) {
        LLVMValueRef address = nullptr;
        type_id_t type = type_id_t::INVALID;
        if (const auto* local = std::get_if<local_id_t>(&place.root)) {
            address = m_slots[index(*local)];
            type = m_function->locals[index(*local)].type;
        } else {
            const auto global = index(std::get<global_id_t>(place.root));
            address = m_globals[global];
            type = m_mir.globals[global].type;
        }

        for (const std::uint32_t field : place.fields) {
            if (is_class(type)) {
                // A Class's variable holds a pointer; the fields are in the object, after the
                // header's two.
                LLVMValueRef object = LLVMBuildLoad2(builder(), m_ptr, address, "");
                address = LLVMBuildStructGEP2(builder(), object_type(type), object, field + 2, "");
            } else if (is_nullable(type)) {
                // The T in a T?: the reference itself, or what comes after the flag.
                if (!is_nullable_reference(type)) {
                    address = LLVMBuildStructGEP2(builder(), llvm_type(type), address, 1, "");
                }
            } else {
                address = LLVMBuildStructGEP2(builder(), llvm_type(type), address, field, "");
            }

            type = m_mir.types->get(type).fields[field].type;
        }

        return address;
    }

    /// Retains, or releases, every reference in the value of `type` at `address`: a String, an
    /// object, or each one among a Struct's fields or in a T?. The runtime ignores null, which a
    /// String? or Class? holds for Null, and any other T? holds its T's zero then.
    void for_each_reference(LLVMValueRef address, const type_id_t type, const bool retain) {
        if (is_nullable(type)) {
            if (!is_nullable_reference(type)) {
                address = LLVMBuildStructGEP2(builder(), llvm_type(type), address, 1, "");
            }

            for_each_reference(address, inner_of(type), retain);
            return;
        }

        if (type == type_id_t::STRING) {
            call(retain ? m_retain : m_release, { LLVMBuildLoad2(builder(), m_ptr, address, "") });
            return;
        }

        if (is_class(type)) {
            call(retain ? m_object_retain : m_object_release,
                 { LLVMBuildLoad2(builder(), m_ptr, address, "") });
            return;
        }

        const std::vector<field_t>& fields = m_mir.types->get(type).fields;
        for (std::uint32_t i = 0; i < fields.size(); ++i) {
            if (m_mir.types->has_references(fields[i].type)) {
                LLVMValueRef field =
                    LLVMBuildStructGEP2(builder(), llvm_type(type), address, i, "");
                for_each_reference(field, fields[i].type, retain);
            }
        }
    }

    LLVMValueRef value_of(const operand_t& operand) {
        if (const auto* value = std::get_if<const_value_t>(&operand)) {
            return constant(*value);
        }

        const auto& place = std::get<place_t>(operand);
        return LLVMBuildLoad2(builder(), llvm_type(type_of(place)), address_of(place), "");
    }

    // Statements

    void generate(const assign_t& node) {
        LLVMValueRef value =
            std::visit([this](const auto& rvalue) { return this->value(rvalue); }, node.value);
        LLVMBuildStore(builder(), value, address_of(node.target));
    }

    void generate(const evaluate_t& node) {
        std::visit([this](const auto& rvalue) { this->value(rvalue); }, node.value);
    }

    void generate(const retain_t& node) {
        if (const auto* place = std::get_if<place_t>(&node.value)) {
            for_each_reference(address_of(*place), type_of(*place), true);
        } else {
            call(m_retain, { value_of(node.value) });
        }
    }

    void generate(const release_t& node) {
        for_each_reference(address_of(node.target), type_of(node.target), false);
    }

    // Terminators

    void generate(const goto_t& node) {
        LLVMBuildBr(builder(), m_blocks[index(node.target)]);
    }

    void generate(const branch_t& node) {
        LLVMBuildCondBr(builder(), value_of(node.condition), m_blocks[index(node.if_true)],
                        m_blocks[index(node.if_false)]);
    }

    void generate(const return_t& node) {
        if (node.value) {
            LLVMBuildRet(builder(), value_of(*node.value));
        } else {
            LLVMBuildRetVoid(builder());
        }
    }

    void generate(const panic_t& node) {
        panic(std::string(node.message), node.span);
    }

    void generate(const unreachable_t& /*node*/) {
        LLVMBuildUnreachable(builder());
    }

    // Rvalues

    LLVMValueRef value(const use_t& node) {
        return value_of(node.value);
    }

    LLVMValueRef value(const unary_t& node) {
        const type_id_t type = type_of(node.operand);
        LLVMValueRef operand = value_of(node.operand);
        switch (node.op) {
        case unary_op_t::NOT:
        case unary_op_t::BIT_NOT:
            return LLVMBuildNot(builder(), operand, "");
        case unary_op_t::NEGATE:
            if (is_float(type)) {
                return LLVMBuildFNeg(builder(), operand, "");
            }

            // 0 - x, which only overflows for the most negative value.
            if (checked()) {
                return with_overflow("llvm.ssub.with.overflow", LLVMConstNull(LLVMTypeOf(operand)),
                                     operand, "-", node.span);
            }

            return LLVMBuildNeg(builder(), operand, "");
        }

        std::unreachable();
    }

    /// Integer + - * that panics on overflow.
    LLVMValueRef with_overflow(const std::string_view intrinsic, LLVMValueRef left,
                               LLVMValueRef right, const std::string_view op, const span_t span) {
        LLVMValueRef result = call_intrinsic(intrinsic, LLVMTypeOf(left), { left, right });
        panic_if(LLVMBuildExtractValue(builder(), result, 1, ""),
                 std::format("integer overflow in '{}'", op), span);
        return LLVMBuildExtractValue(builder(), result, 0, "");
    }

    LLVMValueRef value(const binary_t& node) {
        const type_id_t type = type_of(node.left);
        if (is_nullable(type)) {
            return compare_nullable(node, type);
        }

        LLVMValueRef left = value_of(node.left);
        LLVMValueRef right = value_of(node.right);
        if (is_float(type)) {
            return float_operation(node.op, type, left, right);
        }

        if (type == type_id_t::BOOL) {
            switch (node.op) {
            case binary_op_t::AND:
                return LLVMBuildAnd(builder(), left, right, "");
            case binary_op_t::OR:
                return LLVMBuildOr(builder(), left, right, "");
            case binary_op_t::EQUAL:
                return LLVMBuildICmp(builder(), LLVMIntEQ, left, right, "");
            default:
                return LLVMBuildICmp(builder(), LLVMIntNE, left, right, "");
            }
        }

        const bool is_signed_type = is_signed(type);
        // Lowering marks arithmetic it adds itself, like a For loop's counting, as wrapping.
        const bool check = checked() && !node.wrapping;
        switch (node.op) {
        case binary_op_t::ADD:
            if (check) {
                return with_overflow(is_signed_type ? "llvm.sadd.with.overflow"
                                                    : "llvm.uadd.with.overflow",
                                     left, right, "+", node.span);
            }

            return LLVMBuildAdd(builder(), left, right, "");
        case binary_op_t::SUBTRACT:
            if (check) {
                return with_overflow(is_signed_type ? "llvm.ssub.with.overflow"
                                                    : "llvm.usub.with.overflow",
                                     left, right, "-", node.span);
            }

            return LLVMBuildSub(builder(), left, right, "");
        case binary_op_t::MULTIPLY:
            if (check) {
                return with_overflow(is_signed_type ? "llvm.smul.with.overflow"
                                                    : "llvm.umul.with.overflow",
                                     left, right, "*", node.span);
            }

            return LLVMBuildMul(builder(), left, right, "");
        case binary_op_t::DIVIDE:
        case binary_op_t::REMAINDER:
            return divide(node.op, type, left, right, node.span);
        case binary_op_t::POWER:
            return power(type, left, right, node.span);
        case binary_op_t::SHIFT_LEFT:
        case binary_op_t::SHIFT_RIGHT:
            return shift(node.op, type, left, right, node.span);
        case binary_op_t::BIT_AND:
            return LLVMBuildAnd(builder(), left, right, "");
        case binary_op_t::BIT_OR:
            return LLVMBuildOr(builder(), left, right, "");
        case binary_op_t::BIT_XOR:
            return LLVMBuildXor(builder(), left, right, "");
        case binary_op_t::EQUAL:
            return LLVMBuildICmp(builder(), LLVMIntEQ, left, right, "");
        case binary_op_t::NOT_EQUAL:
            return LLVMBuildICmp(builder(), LLVMIntNE, left, right, "");
        case binary_op_t::LESS:
            return LLVMBuildICmp(builder(), is_signed_type ? LLVMIntSLT : LLVMIntULT, left, right,
                                 "");
        case binary_op_t::LESS_EQUAL:
            return LLVMBuildICmp(builder(), is_signed_type ? LLVMIntSLE : LLVMIntULE, left, right,
                                 "");
        case binary_op_t::GREATER:
            return LLVMBuildICmp(builder(), is_signed_type ? LLVMIntSGT : LLVMIntUGT, left, right,
                                 "");
        case binary_op_t::GREATER_EQUAL:
            return LLVMBuildICmp(builder(), is_signed_type ? LLVMIntSGE : LLVMIntUGE, left, right,
                                 "");
        default:
            std::unreachable();
        }
    }

    /// == or != on a T?. A String? or Class? compares references, like Classes do. Lowering only
    /// compares any other T? with Null, which asks whether it holds a value.
    LLVMValueRef compare_nullable(const binary_t& node, const type_id_t type) {
        const LLVMIntPredicate predicate = node.op == binary_op_t::EQUAL ? LLVMIntEQ : LLVMIntNE;
        if (is_nullable_reference(type)) {
            return LLVMBuildICmp(builder(), predicate, value_of(node.left), value_of(node.right),
                                 "");
        }

        const operand_t& other = is_null(node.left) ? node.right : node.left;
        LLVMValueRef holds = LLVMBuildExtractValue(builder(), value_of(other), 0, "");
        return LLVMBuildICmp(builder(), predicate, holds, LLVMConstInt(m_bool, 0, 0), "");
    }

    /// Float comparisons are false when either side is NaN, except !=.
    LLVMValueRef float_operation(const binary_op_t op, const type_id_t type, LLVMValueRef left,
                                 LLVMValueRef right) {
        switch (op) {
        case binary_op_t::ADD:
            return LLVMBuildFAdd(builder(), left, right, "");
        case binary_op_t::SUBTRACT:
            return LLVMBuildFSub(builder(), left, right, "");
        case binary_op_t::MULTIPLY:
            return LLVMBuildFMul(builder(), left, right, "");
        case binary_op_t::DIVIDE:
            return LLVMBuildFDiv(builder(), left, right, "");
        case binary_op_t::REMAINDER:
            return LLVMBuildFRem(builder(), left, right, "");
        case binary_op_t::POWER:
            return call_intrinsic("llvm.pow", llvm_type(type), { left, right });
        case binary_op_t::EQUAL:
            return LLVMBuildFCmp(builder(), LLVMRealOEQ, left, right, "");
        case binary_op_t::NOT_EQUAL:
            return LLVMBuildFCmp(builder(), LLVMRealUNE, left, right, "");
        case binary_op_t::LESS:
            return LLVMBuildFCmp(builder(), LLVMRealOLT, left, right, "");
        case binary_op_t::LESS_EQUAL:
            return LLVMBuildFCmp(builder(), LLVMRealOLE, left, right, "");
        case binary_op_t::GREATER:
            return LLVMBuildFCmp(builder(), LLVMRealOGT, left, right, "");
        case binary_op_t::GREATER_EQUAL:
            return LLVMBuildFCmp(builder(), LLVMRealOGE, left, right, "");
        default:
            std::unreachable();
        }
    }

    /// Division by zero always panics. MIN / -1 overflows: debug builds panic, and release builds
    /// divide by 1 instead, which gives the wrapped result for both / and %.
    LLVMValueRef divide(const binary_op_t op, const type_id_t type, LLVMValueRef left,
                        LLVMValueRef right, const span_t span) {
        LLVMTypeRef llvm = LLVMTypeOf(left);
        panic_if(LLVMBuildICmp(builder(), LLVMIntEQ, right, LLVMConstNull(llvm), ""),
                 "division by zero", span);
        if (!is_signed(type)) {
            return op == binary_op_t::DIVIDE ? LLVMBuildUDiv(builder(), left, right, "")
                                             : LLVMBuildURem(builder(), left, right, "");
        }

        const unsigned long long min = 1ULL << (bit_width(type) - 1);
        LLVMValueRef is_min =
            LLVMBuildICmp(builder(), LLVMIntEQ, left, LLVMConstInt(llvm, min, 0), "");
        LLVMValueRef is_minus_one =
            LLVMBuildICmp(builder(), LLVMIntEQ, right, LLVMConstAllOnes(llvm), "");
        LLVMValueRef overflows = LLVMBuildAnd(builder(), is_min, is_minus_one, "");
        if (checked()) {
            panic_if(overflows, std::format("integer overflow in '{}'", spelling(op)), span);
        } else {
            right = LLVMBuildSelect(builder(), overflows, LLVMConstInt(llvm, 1, 0), right, "");
        }

        return op == binary_op_t::DIVIDE ? LLVMBuildSDiv(builder(), left, right, "")
                                         : LLVMBuildSRem(builder(), left, right, "");
    }

    /// Integer powers go through the runtime, in 64 bits.
    LLVMValueRef power(const type_id_t type, LLVMValueRef left, LLVMValueRef right,
                       const span_t span) {
        const bool is_signed_type = is_signed(type);
        LLVMValueRef base = LLVMBuildIntCast2(builder(), left, m_i64, is_signed_type ? 1 : 0, "");
        LLVMValueRef exponent =
            LLVMBuildIntCast2(builder(), right, m_i64, is_signed_type ? 1 : 0, "");
        LLVMValueRef result = call(is_signed_type ? m_power_int : m_power_uint,
                                   {
                                       base,
                                       exponent,
                                       LLVMConstInt(m_i32, bit_width(type), 0),
                                       LLVMConstInt(m_i32, checked() ? 1U : 0U, 0),
                                       location(span),
                                   });
        return LLVMBuildIntCast2(builder(), result, LLVMTypeOf(left), 0, "");
    }

    /// The amount can be any integer type. Out of range, debug builds panic and release builds
    /// mask it.
    LLVMValueRef shift(const binary_op_t op, const type_id_t type, LLVMValueRef left,
                       LLVMValueRef amount, const span_t span) {
        const unsigned bits = bit_width(type);
        LLVMTypeRef amount_type = LLVMTypeOf(amount);
        if (checked()) {
            // Unsigned, so a negative amount is out of range too.
            LLVMValueRef out_of_range = LLVMBuildICmp(builder(), LLVMIntUGE, amount,
                                                      LLVMConstInt(amount_type, bits, 0), "");
            panic_if(out_of_range, std::format("the shift amount must be from 0 to {}", bits - 1),
                     span);
        } else {
            amount = LLVMBuildAnd(builder(), amount, LLVMConstInt(amount_type, bits - 1, 0), "");
        }

        amount = LLVMBuildIntCast2(builder(), amount, LLVMTypeOf(left), 0, "");
        if (op == binary_op_t::SHIFT_LEFT) {
            return LLVMBuildShl(builder(), left, amount, "");
        }

        return is_signed(type) ? LLVMBuildAShr(builder(), left, amount, "")
                               : LLVMBuildLShr(builder(), left, amount, "");
    }

    LLVMValueRef value(const cast_t& node) {
        const type_id_t from = type_of(node.value);
        LLVMValueRef value = value_of(node.value);
        if (is_nullable(node.type)) {
            return to_nullable(value, from, node.type, node.span);
        }

        return convert(value, from, node.type, node.span);
    }

    /// A T, or a T?, as a U? whose U the T converts to. A String? or Class? is the reference
    /// either way; any other gets the converted value and the flag.
    LLVMValueRef to_nullable(LLVMValueRef value, const type_id_t from, const type_id_t to,
                             const span_t span) {
        if (from == to || is_nullable_reference(to)) {
            return value;
        }

        LLVMValueRef holds = LLVMConstInt(m_bool, 1, 0);
        type_id_t from_inner = from;
        if (is_nullable(from)) {
            holds = LLVMBuildExtractValue(builder(), value, 0, "");
            value = LLVMBuildExtractValue(builder(), value, 1, "");
            from_inner = inner_of(from);
        }

        value = convert(value, from_inner, inner_of(to), span);
        LLVMValueRef result = LLVMGetUndef(llvm_type(to));
        result = LLVMBuildInsertValue(builder(), result, holds, 0, "");
        return LLVMBuildInsertValue(builder(), result, value, 1, "");
    }

    /// Between numbers, or from a Bool to an integer.
    LLVMValueRef convert(LLVMValueRef value, const type_id_t from, const type_id_t to,
                         const span_t span) {
        LLVMTypeRef target = llvm_type(to);
        if (from == to) {
            return value;
        }

        if (from == type_id_t::BOOL) {
            return LLVMBuildZExt(builder(), value, target, "");
        }

        if (is_integer(from) && is_integer(to)) {
            return LLVMBuildIntCast2(builder(), value, target, is_signed(from) ? 1 : 0, "");
        }

        if (is_integer(from)) {
            return is_signed(from) ? LLVMBuildSIToFP(builder(), value, target, "")
                                   : LLVMBuildUIToFP(builder(), value, target, "");
        }

        if (is_float(to)) {
            return LLVMBuildFPCast(builder(), value, target, "");
        }

        // Float to integer drops the fraction, and the rest has to fit. NaN never does.
        LLVMTypeRef source = LLVMTypeOf(value);
        LLVMValueRef whole = call_intrinsic("llvm.trunc", source, { value });
        const bool to_signed = is_signed(to);
        const double limit = std::ldexp(1.0, static_cast<int>(bit_width(to)) - (to_signed ? 1 : 0));
        LLVMValueRef above_low =
            to_signed
                ? LLVMBuildFCmp(builder(), LLVMRealOGE, whole, LLVMConstReal(source, -limit), "")
                : LLVMBuildFCmp(builder(), LLVMRealOGT, whole, LLVMConstReal(source, -1.0), "");
        LLVMValueRef below_high =
            LLVMBuildFCmp(builder(), LLVMRealOLT, whole, LLVMConstReal(source, limit), "");
        LLVMValueRef fits = LLVMBuildAnd(builder(), above_low, below_high, "");
        panic_if(LLVMBuildNot(builder(), fits, ""),
                 std::format("the value doesn't fit in {}", m_mir.types->name(to)), span);
        return to_signed ? LLVMBuildFPToSI(builder(), whole, target, "")
                         : LLVMBuildFPToUI(builder(), whole, target, "");
    }

    LLVMValueRef value(const call_t& node) {
        std::vector<LLVMValueRef> arguments;
        arguments.reserve(node.arguments.size());
        for (const call_argument_t& argument : node.arguments) {
            arguments.push_back(argument.by_ref ? address_of(std::get<place_t>(argument.value))
                                                : value_of(argument.value));
        }

        return call(m_functions[index(node.function)], std::move(arguments));
    }

    LLVMValueRef value(const aggregate_t& node) {
        LLVMValueRef result = LLVMGetUndef(llvm_type(node.type));
        for (std::size_t i = 0; i < node.fields.size(); ++i) {
            result = LLVMBuildInsertValue(builder(), result, value_of(node.fields[i]),
                                          static_cast<unsigned>(i), "");
        }

        return result;
    }

    LLVMValueRef value(const zero_t& node) {
        return zero_constant(node.type);
    }

    LLVMValueRef value(const new_t& node) {
        return call(m_object_new, { m_classes.at(node.type) });
    }

    LLVMValueRef value(const runtime_call_t& node) {
        switch (node.function) {
        case runtime_function_t::CONCAT:
            return call(m_concat, { value_of(node.arguments[0]), value_of(node.arguments[1]) });
        case runtime_function_t::JOIN:
            return join(node.arguments);
        case runtime_function_t::EQUAL: {
            // The runtime returns an Int32, and MIR wants a Bool.
            LLVMValueRef equal =
                call(m_equal, { value_of(node.arguments[0]), value_of(node.arguments[1]) });
            return LLVMBuildICmp(builder(), LLVMIntNE, equal, LLVMConstNull(m_i32), "");
        }
        case runtime_function_t::TO_TEXT:
            return to_text(node.arguments[0]);
        case runtime_function_t::PRINT:
            return call(m_print, { value_of(node.arguments[0]) });
        }

        std::unreachable();
    }

    /// The parts go in an array in the entry block, so a loop doesn't grow the stack.
    LLVMValueRef join(const std::vector<operand_t>& parts) {
        LLVMTypeRef array_type = LLVMArrayType2(m_ptr, parts.size());
        LLVMBasicBlockRef current = LLVMGetInsertBlock(builder());
        LLVMPositionBuilderAtEnd(builder(), m_entry); // it has no terminator until the end
        LLVMValueRef array = LLVMBuildAlloca(builder(), array_type, "parts");
        LLVMPositionBuilderAtEnd(builder(), current);
        for (std::size_t i = 0; i < parts.size(); ++i) {
            LLVMValueRef offset = LLVMConstInt(m_i64, i, 0);
            LLVMValueRef element = LLVMBuildGEP2(builder(), m_ptr, array, &offset, 1, "");
            LLVMBuildStore(builder(), value_of(parts[i]), element);
        }

        return call(m_join, { array, LLVMConstInt(m_i64, parts.size(), 0) });
    }

    LLVMValueRef to_text(const operand_t& operand) {
        const type_id_t type = type_of(operand);
        LLVMValueRef value = value_of(operand);
        switch (type) {
        case type_id_t::BOOL:
            return call(m_from_bool, { LLVMBuildZExt(builder(), value, m_i32, "") });
        case type_id_t::FLOAT32:
            return call(m_from_float32, { value });
        case type_id_t::FLOAT64:
            return call(m_from_float64, { value });
        default:
            if (is_signed(type)) {
                return call(m_from_int, { LLVMBuildSExt(builder(), value, m_i64, "") });
            }

            return call(m_from_uint, { LLVMBuildZExt(builder(), value, m_i64, "") });
        }
    }
};

} // namespace

std::expected<std::string, std::string>
emit_ir(const mir_module_t& module, const source_file_t& file, const codegen_options_t& options) {
    codegen_t codegen(module, file, options);
    if (std::expected<void, std::string> built = codegen.build(); !built) {
        return std::unexpected(built.error());
    }

    return codegen.ir();
}

std::expected<void, std::string> emit_object(const mir_module_t& module, const source_file_t& file,
                                             const codegen_options_t& options,
                                             const std::filesystem::path& path) {
    codegen_t codegen(module, file, options);
    if (std::expected<void, std::string> built = codegen.build(); !built) {
        return built;
    }

    return codegen.write_object(path);
}

} // namespace fb
