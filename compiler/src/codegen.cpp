#include "fb/codegen.hpp"

#include "fb/build_config.hpp"
#include "fb/c_abi.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <llvm-c/Analysis.h>
#include <llvm-c/Core.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>
#include <llvm-c/Transforms/PassBuilder.h>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
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

/// How an Extern Function's call passes a Struct, or gets one back, by the target's C ABI.
struct c_struct_abi_t {
    c_passing_t passing;
    /// Where the Struct's pieces start in it, one per piece, for DIRECT.
    std::vector<std::uint64_t> offsets;
};

/// An Extern Function's C ABI, where it takes or gives a Struct by value. Each parameter's
/// Struct, by its index, and what the declaration's parameters start with: the result's memory
/// when it's MEMORY.
struct c_signature_t {
    std::vector<std::optional<c_struct_abi_t>> parameters;
    std::optional<c_struct_abi_t> result;
};

/// Turns a MIR module into an LLVM module. Build it once, then print its IR or write an object
/// file.
class codegen_t {
public:
    codegen_t(const mir_module_t& module, const source_manager_t& sources,
              const source_file_t& main, const codegen_options_t& options)
        : m_mir(module), m_sources(sources), m_options(options),
          m_triple(normalize(options.target_triple.empty()
                                 ? std::string(build_config::target_triple)
                                 : options.target_triple)),
          m_context(LLVMContextCreate()),
          m_module(LLVMModuleCreateWithNameInContext(main.path().c_str(), m_context.get())),
          m_builder(LLVMCreateBuilderInContext(m_context.get())) {}

    /// Builds the module, verifies it, and optimizes it in release builds.
    std::expected<void, std::string> build() {
        // Lowering stopped where code needs a larger instance each time it runs.
        if (const std::optional<mir_error_t>& error = m_mir.error) {
            const source_file_t& file = m_sources.get(error->span.file);
            const location_t at = file.location_of(error->span.begin, column_unit_t::CODEPOINT);
            return std::unexpected(std::format("{}:{}:{}: {}", file.path(), at.line + 1,
                                               at.column + 1, error->message));
        }

        if (std::expected<void, std::string> machine = create_machine(); !machine) {
            return machine;
        }

        declare_types();
        declare_runtime();
        declare_globals();
        declare_functions();
        declare_thread_local_functions();
        declare_classes();
        for (std::size_t i = 0; i < m_mir.functions.size(); ++i) {
            if (!m_mir.functions[i].external) {
                generate(i);
            }
        }

        for (std::size_t i = 0; i < m_mir.classes.size(); ++i) {
            generate_class_functions(i);
        }

        generate_array_functions();
        generate_channel_functions();
        generate_scheduler_functions();
        generate_thread_local_functions();
        generate_main();
        if (!m_unsupported.empty()) {
            return std::unexpected(m_unsupported);
        }

        char* error = nullptr;
        if (LLVMVerifyModule(m_module.get(), LLVMReturnStatusAction, &error) != 0) {
            return std::unexpected("invalid LLVM IR: " + take_message(error));
        }

        take_message(error);
        // A debug build only inlines what @Inline asks for.
        // Async Functions' coroutines are split into their pieces in every build. The call graph
        // that coro-split walks starts from what's visible outside the module, so code nothing
        // calls, like an Async Function the program never calls, goes first, or it would reach
        // the backend unsplit.
        return optimize(m_options.mode == build_mode_t::RELEASE
                            ? "default<O2>"
                            : "always-inline,globaldce,coro-early,cgscc(coro-split),coro-cleanup");
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
    const source_manager_t& m_sources;
    const codegen_options_t& m_options;
    /// The options' target, or else the one this compiler was built for, normalized.
    std::string m_triple;
    /// Why the module can't be made for its target, found while making it.
    std::string m_unsupported;
    /// The C ABI of each Extern Function that takes or gives a Struct by value, by its index.
    std::unordered_map<std::size_t, c_signature_t> m_c_signatures;
    /// The function each Callback calls through, by the function it calls and whether that's a
    /// lambda's code.
    std::map<std::pair<std::uint32_t, bool>, LLVMValueRef> m_thunks;
    /// For each @PanicsAtCaller function an Interface's table holds, the function that calls it
    /// without a place.
    std::unordered_map<function_id_t, LLVMValueRef> m_placeless;
    std::unique_ptr<LLVMOpaqueContext, context_deleter_t> m_context;
    std::unique_ptr<LLVMOpaqueModule, module_deleter_t> m_module;
    std::unique_ptr<LLVMOpaqueBuilder, builder_deleter_t> m_builder;
    std::unique_ptr<LLVMOpaqueTargetMachine, machine_deleter_t> m_machine;

    LLVMTypeRef m_void = nullptr;
    LLVMTypeRef m_bool = nullptr;
    LLVMTypeRef m_i32 = nullptr;
    LLVMTypeRef m_i64 = nullptr;
    LLVMTypeRef m_ptr = nullptr;
    /// fb_object_t, an object's header.
    LLVMTypeRef m_object_header = nullptr;
    /// fb_class_t, followed by a Method table of unknown length.
    LLVMTypeRef m_class_header = nullptr;
    /// fb_array_t: the header, the count, the capacity, and the buffer.
    LLVMTypeRef m_array_type = nullptr;

    // The runtime's functions
    function_ref_t m_start;
    function_ref_t m_pool_finish;
    function_ref_t m_finish;
    function_ref_t m_retain;
    function_ref_t m_release;
    function_ref_t m_concat;
    function_ref_t m_join;
    function_ref_t m_equal;
    function_ref_t m_compare;
    function_ref_t m_hash_string;
    function_ref_t m_hash_integer;
    function_ref_t m_hash_float;
    function_ref_t m_hash_object;
    function_ref_t m_hash_combine;
    function_ref_t m_string_chars;
    function_ref_t m_string_bytes;
    function_ref_t m_from_int;
    function_ref_t m_from_uint;
    function_ref_t m_from_float32;
    function_ref_t m_from_float64;
    function_ref_t m_from_bool;
    function_ref_t m_format_int;
    function_ref_t m_format_uint;
    function_ref_t m_format_float;
    function_ref_t m_format_text;
    function_ref_t m_string_debug;
    function_ref_t m_char_debug;
    function_ref_t m_debug_object;
    function_ref_t m_debug_indent;
    function_ref_t m_from_char;
    function_ref_t m_power_int;
    function_ref_t m_power_uint;
    function_ref_t m_cstring_new;
    function_ref_t m_cstring_free;
    function_ref_t m_string_from_cstring;
    function_ref_t m_track_objects;
    function_ref_t m_retain_weak;
    function_ref_t m_release_weak;
    function_ref_t m_load_weak;
    function_ref_t m_discard;
    function_ref_t m_panic;
    function_ref_t m_panic_string;
    function_ref_t m_error_uncaught;
    function_ref_t m_error_thrown;
    function_ref_t m_object_new;
    function_ref_t m_object_retain;
    function_ref_t m_object_release;
    function_ref_t m_object_is;
    function_ref_t m_object_expect;
    function_ref_t m_interface_methods;
    function_ref_t m_object_implements;
    function_ref_t m_object_expect_implements;
    function_ref_t m_mutex_lock;
    function_ref_t m_mutex_unlock;
    function_ref_t m_task_start;
    function_ref_t m_task_start_thread;
    function_ref_t m_task_wait;
    function_ref_t m_task_done;
    function_ref_t m_task_release;
    function_ref_t m_parallel_for;
    function_ref_t m_channel_new;
    function_ref_t m_channel_send;
    function_ref_t m_channel_try_send;
    function_ref_t m_channel_receive;
    function_ref_t m_channel_try_receive;
    function_ref_t m_channel_close;
    function_ref_t m_channel_take_left;
    function_ref_t m_channel_element;
    function_ref_t m_channel_free;
    function_ref_t m_task_begin;
    function_ref_t m_task_finish;
    function_ref_t m_await;
    function_ref_t m_wait_frame;
    function_ref_t m_wait_time;
    function_ref_t m_scheduler_new;
    function_ref_t m_scheduler_run;
    function_ref_t m_scheduler_tick;
    function_ref_t m_scheduler_idle;
    function_ref_t m_scheduler_free;
    function_ref_t m_coroutine_alloc;
    function_ref_t m_coroutine_free;
    function_ref_t m_scheduler_finish;
    function_ref_t m_thread_local_added;
    function_ref_t m_thread_local_making;
    function_ref_t m_thread_local_made;
    function_ref_t m_thread_locals_release;
    /// In an Async Function's code: its coroutine's id and handle, and its last two blocks.
    LLVMValueRef m_coroutine_id = nullptr;
    LLVMValueRef m_coroutine = nullptr;
    LLVMBasicBlockRef m_coroutine_cleanup = nullptr;
    LLVMBasicBlockRef m_coroutine_end = nullptr;
    /// Each Interface's fb_interface_t, by its type, made when it's first needed.
    std::unordered_map<type_id_t, LLVMValueRef> m_interfaces;
    function_ref_t m_array_new;
    function_ref_t m_array_push;
    function_ref_t m_array_insert;
    function_ref_t m_array_remove;
    function_ref_t m_array_clear;
    function_ref_t m_array_clone;
    function_ref_t m_array_reserve;
    function_ref_t m_array_trim;
    function_ref_t m_array_free;
    function_ref_t m_index_out_of_range;
    function_ref_t m_unsigned_index_out_of_range;

    /// Each MIR global's LLVM global, by global_id_t.
    std::vector<LLVMValueRef> m_globals;
    /// For each Global main sets, in debug builds, and in release builds for one whose zero
    /// holds a null object: an Int32 that's 0 until its initializer has run, `ready` then, and
    /// `released` once the program has let go of it at its end. Null for the rest.
    std::vector<LLVMValueRef> m_ready;
    static constexpr unsigned ready = 1;
    static constexpr unsigned released = 2;
    /// For each ThreadLocal Global, its thread's state, an Int32 of its own on each thread: 0
    /// until the thread first uses it, `making` while its initializer runs, `ready` once it has,
    /// and `released` once the thread has let go of it. Null for the rest.
    std::vector<LLVMValueRef> m_thread_states;
    static constexpr unsigned making = 3;
    /// For each ThreadLocal Global, what makes the thread's own, and, for one that holds
    /// references, what releases it.
    std::vector<std::optional<function_ref_t>> m_thread_makers;
    std::vector<std::optional<function_ref_t>> m_thread_releasers;
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
    /// What the runtime calls on a Class's objects; see fb_class_t. Its destroy is nullopt when
    /// neither it nor a Class it extends has a Destroy().
    struct class_functions_t {
        std::optional<function_ref_t> destroy;
        function_ref_t release;
        function_ref_t visit;
    };

    /// Each Class's functions, by its index in `mir_module_t::classes`.
    std::vector<class_functions_t> m_class_functions;
    /// Each Class's index in `mir_module_t::classes`, by its type.
    std::unordered_map<type_id_t, std::size_t> m_class_index;
    /// A T[] whose fb_class_t has been made, with its functions, which generate_array_functions()
    /// fills in. A T[] has no Destroy().
    struct array_class_t {
        type_id_t type;
        function_ref_t release;
        function_ref_t visit;
    };

    std::vector<array_class_t> m_array_classes;
    /// Each Channel type's Class, made when the program makes one: its release releases the
    /// elements left and frees the ring.
    std::vector<array_class_t> m_channel_classes;
    /// fb_channel_t.
    LLVMTypeRef m_channel_type = nullptr;
    /// The Scheduler's Class, once the program makes one: its release frees what the runtime
    /// keeps for it.
    std::optional<array_class_t> m_scheduler_class;
    /// What a Class's visit calls with each object it refers to: (context, object, field name).
    LLVMTypeRef m_visitor = nullptr;

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
        // LLVM's targets are registered once per process: both of FutureBlitz's, so either's code
        // can be made on either.
        static std::once_flag initialized;
        std::call_once(initialized, [] {
            LLVMInitializeX86TargetInfo();
            LLVMInitializeX86Target();
            LLVMInitializeX86TargetMC();
            LLVMInitializeX86AsmPrinter();
            LLVMInitializeAArch64TargetInfo();
            LLVMInitializeAArch64Target();
            LLVMInitializeAArch64TargetMC();
            LLVMInitializeAArch64AsmPrinter();
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

    std::expected<void, std::string> optimize(const char* passes) {
        LLVMPassBuilderOptionsRef options = LLVMCreatePassBuilderOptions();
        // A release build turns loops over arrays and straight-line code on several values, like
        // a Vec4's Operators, into SIMD instructions.
        if (m_options.mode == build_mode_t::RELEASE) {
            LLVMPassBuilderOptionsSetLoopVectorization(options, 1);
            LLVMPassBuilderOptionsSetSLPVectorization(options, 1);
        }

        LLVMErrorRef error = LLVMRunPasses(m_module.get(), passes, m_machine.get(), options);
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
        std::array<LLVMTypeRef, 2> header{ m_i64, m_ptr };
        m_object_header = LLVMStructTypeInContext(context(), header.data(),
                                                  static_cast<unsigned>(header.size()), 0);
        LLVMTypeRef methods = LLVMArrayType2(m_ptr, 0);
        m_class_header = class_type(methods);
        std::array<LLVMTypeRef, 3> visitor{ m_ptr, m_ptr, m_ptr };
        m_visitor =
            LLVMFunctionType(m_void, visitor.data(), static_cast<unsigned>(visitor.size()), 0);
        std::array<LLVMTypeRef, 5> array{ m_i64, m_ptr, m_i64, m_i64, m_ptr };
        m_array_type = LLVMStructTypeInContext(context(), array.data(),
                                               static_cast<unsigned>(array.size()), 0);
        // The header, the element size, capacity, head, count, and closed, the ring, and the lock.
        std::array channel{ m_i64, m_ptr, m_i64, m_i64, m_i64, m_i64, m_i64, m_ptr, m_ptr };
        m_channel_type = LLVMStructTypeInContext(context(), channel.data(),
                                                 static_cast<unsigned>(channel.size()), 0);
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
        case type_id_t::CHAR:
            return m_i32;
        case type_id_t::FLOAT32:
            return LLVMFloatTypeInContext(context());
        case type_id_t::FLOAT64:
            return LLVMDoubleTypeInContext(context());
        case type_id_t::STRING:
        case type_id_t::CSTRING:
            return m_ptr;
        default:
            if (m_mir.types->get(type).kind == type_kind_t::STRUCT ||
                m_mir.types->get(type).kind == type_kind_t::TUPLE) {
                return struct_type(type);
            }

            // A Class, T[], Weak<T>, or function value is a pointer to its object.
            if (is_object(type) || is_array(type) || is_nullable_reference(type) || is_weak(type)) {
                return m_ptr;
            }

            // An Enum is its integer type.
            if (is_enum(type)) {
                return llvm_type(inner_of(type));
            }

            // A fixed array's elements are in one run, whatever its dimensions.
            if (is_fixed_array(type)) {
                return LLVMArrayType2(llvm_type(inner_of(type)), m_mir.types->element_count(type));
            }

            if (is_nullable(type)) {
                std::array<LLVMTypeRef, 2> fields{ m_bool, llvm_type(inner_of(type)) };
                return LLVMStructTypeInContext(context(), fields.data(),
                                               static_cast<unsigned>(fields.size()), 0);
            }

            // An Atomic is its integer. An Atomic<Bool> is a byte, since atomic operations
            // need one at least.
            if (m_mir.types->get(type).kind == type_kind_t::ATOMIC) {
                return inner_of(type) == type_id_t::BOOL ? LLVMInt8TypeInContext(context())
                                                         : llvm_type(inner_of(type));
            }

            // A Mutex is its lock, then its value.
            if (m_mir.types->get(type).kind == type_kind_t::MUTEX) {
                return struct_type(type);
            }

            if (is_pointer(type) || is_callback(type)) {
                return m_ptr;
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

    /// How many T?s a type is inside: 2 for an Int32??.
    [[nodiscard]] std::size_t nullable_depth(type_id_t type) const {
        std::size_t depth = 0;
        while (is_nullable(type)) {
            type = inner_of(type);
            ++depth;
        }

        return depth;
    }

    /// A String? or Class?: the reference itself, which is null for Null.
    [[nodiscard]] bool is_nullable_reference(const type_id_t type) const {
        return is_nullable(type) && (inner_of(type) == type_id_t::STRING ||
                                     is_object(inner_of(type)) || is_array(inner_of(type)));
    }

    /// A Class, an Interface, or a function value, which refers to a closure: each one is a
    /// reference to an object.
    [[nodiscard]] bool is_object(const type_id_t type) const {
        const type_kind_t kind = m_mir.types->get(type).kind;
        return is_class(type) || is_interface(type) || is_task(type) ||
               kind == type_kind_t::FUNCTION || kind == type_kind_t::CHANNEL ||
               kind == type_kind_t::SCHEDULER;
    }

    /// A Task or Thread, which refers to an object of a Class the compiler makes for it.
    [[nodiscard]] bool is_task(const type_id_t type) const {
        const type_kind_t kind = m_mir.types->get(type).kind;
        return kind == type_kind_t::TASK || kind == type_kind_t::THREAD;
    }

    [[nodiscard]] bool is_interface(const type_id_t type) const {
        return m_mir.types->get(type).kind == type_kind_t::INTERFACE;
    }

    [[nodiscard]] bool is_array(const type_id_t type) const {
        return m_mir.types->get(type).kind == type_kind_t::ARRAY;
    }

    [[nodiscard]] bool is_weak(const type_id_t type) const {
        return m_mir.types->get(type).kind == type_kind_t::WEAK;
    }

    [[nodiscard]] bool is_pointer(const type_id_t type) const {
        return m_mir.types->get(type).kind == type_kind_t::PTR;
    }

    [[nodiscard]] bool is_callback(const type_id_t type) const {
        return m_mir.types->get(type).kind == type_kind_t::CALLBACK;
    }

    [[nodiscard]] bool is_fixed_array(const type_id_t type) const {
        return m_mir.types->get(type).kind == type_kind_t::FIXED_ARRAY;
    }

    [[nodiscard]] bool is_struct(const type_id_t type) const {
        return m_mir.types->get(type).kind == type_kind_t::STRUCT;
    }

    [[nodiscard]] bool is_enum(const type_id_t type) const {
        return m_mir.types->get(type).kind == type_kind_t::ENUM;
    }

    /// Whether a type's zero is all zero bytes: everything's but a String's, an Enum's whose first
    /// value isn't 0, and what holds one.
    [[nodiscard]] bool zero_is_null(const type_id_t type) const {
        if (type == type_id_t::STRING) {
            return false;
        }

        // A Class's, Interface's, T[]'s, Weak<T>'s, or function value's zero is a null
        // reference, and a Ptr's is a null address.
        const type_t& info = m_mir.types->get(type);
        if (info.kind == type_kind_t::PTR || info.kind == type_kind_t::CALLBACK) {
            return true;
        }

        if (info.kind == type_kind_t::CLASS || info.kind == type_kind_t::INTERFACE ||
            info.kind == type_kind_t::ARRAY || info.kind == type_kind_t::WEAK ||
            info.kind == type_kind_t::FUNCTION || info.kind == type_kind_t::CHANNEL ||
            info.kind == type_kind_t::SCHEDULER || is_task(type)) {
            return true;
        }

        if (info.kind == type_kind_t::NULLABLE) {
            return is_nullable_reference(type) || zero_is_null(info.inner);
        }

        if (info.kind == type_kind_t::FIXED_ARRAY) {
            return zero_is_null(info.inner);
        }

        if (info.kind == type_kind_t::ENUM) {
            return m_mir.types->enum_zero(type) == 0;
        }

        return std::ranges::all_of(
            info.fields, [this](const field_t& field) { return zero_is_null(field.type); });
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

        const std::string name = std::format("fb.{}", m_mir.types->qualified_name(type));
        LLVMTypeRef result = LLVMStructCreateNamed(context(), name.c_str());
        LLVMStructSetBody(result, fields.data(), static_cast<unsigned>(fields.size()), 0);
        m_object_types.emplace(type, result);
        return result;
    }

    /// "fb.Point" in the IR, or "fb.(Int32, String)" for a tuple. A Struct never contains itself,
    /// so making its fields' types can't come back to it.
    LLVMTypeRef struct_type(const type_id_t type) {
        if (const auto found = m_struct_types.find(type); found != m_struct_types.end()) {
            return found->second;
        }

        std::vector<LLVMTypeRef> fields;
        for (const field_t& field : m_mir.types->get(type).fields) {
            fields.push_back(llvm_type(field.type));
        }

        const std::string name = std::format("fb.{}", m_mir.types->qualified_name(type));
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
        m_track_objects = declare("fb_track_objects", m_void, {});
        m_pool_finish = declare("fb_pool_finish", m_void, {});
        m_finish = declare("fb_finish", m_void, { m_ptr, m_i64 });
        m_retain = declare("fb_string_retain", m_void, { m_ptr });
        m_release = declare("fb_string_release", m_void, { m_ptr });
        m_concat = declare("fb_string_concat", m_ptr, { m_ptr, m_ptr });
        m_join = declare("fb_string_join", m_ptr, { m_ptr, m_i64 });
        m_equal = declare("fb_string_equal", m_i32, { m_ptr, m_ptr });
        m_compare = declare("fb_string_compare", m_i32, { m_ptr, m_ptr });
        m_hash_string = declare("fb_string_hash", m_i64, { m_ptr });
        m_hash_integer = declare("fb_hash_integer", m_i64, { m_i64 });
        m_hash_float = declare("fb_hash_float", m_i64, { f64 });
        m_hash_object = declare("fb_hash_object", m_i64, { m_ptr });
        m_hash_combine = declare("fb_hash_combine", m_i64, { m_i64, m_i64 });
        m_string_chars = declare("fb_string_chars", m_void, { m_ptr, m_ptr });
        m_string_bytes = declare("fb_string_bytes", m_void, { m_ptr, m_ptr });
        m_from_int = declare("fb_string_from_int", m_ptr, { m_i64 });
        m_from_uint = declare("fb_string_from_uint", m_ptr, { m_i64 });
        m_from_float32 = declare("fb_string_from_float32", m_ptr, { f32 });
        m_from_float64 = declare("fb_string_from_float64", m_ptr, { f64 });
        m_from_bool = declare("fb_string_from_bool", m_ptr, { m_i32 });
        m_format_int = declare("fb_format_int", m_ptr, { m_i64, m_ptr });
        m_format_uint = declare("fb_format_uint", m_ptr, { m_i64, m_ptr });
        m_format_float = declare("fb_format_float", m_ptr, { f64, m_i32, m_ptr });
        m_format_text = declare("fb_format_text", m_ptr, { m_ptr, m_ptr, m_i32 });
        m_string_debug = declare("fb_string_debug", m_ptr, { m_ptr });
        m_char_debug = declare("fb_char_debug", m_ptr, { m_i32 });
        m_debug_object = declare("fb_debug_object", m_ptr, { m_ptr, m_i32 });
        m_debug_indent = declare("fb_debug_indent", m_ptr, { m_i32 });
        m_from_char = declare("fb_string_from_char", m_ptr, { m_i32 });
        m_power_int = declare("fb_power_int", m_i64, { m_i64, m_i64, m_i32, m_i32, m_ptr });
        m_power_uint = declare("fb_power_uint", m_i64, { m_i64, m_i64, m_i32, m_i32, m_ptr });
        m_cstring_new = declare("fb_cstring_new", m_ptr, { m_ptr });
        m_cstring_free = declare("fb_cstring_free", m_void, { m_ptr });
        m_string_from_cstring = declare("fb_string_from_cstring", m_ptr, { m_ptr });
        m_panic = declare("fb_panic", m_void, { m_ptr, m_ptr });
        m_panic_string = declare("fb_panic_string", m_void, { m_ptr, m_ptr });
        m_error_uncaught = declare("fb_error_uncaught", m_void, { m_ptr, m_ptr });
        m_error_thrown = declare("fb_error_thrown", m_void, { m_ptr, m_ptr });
        m_object_new = declare("fb_object_new", m_ptr, { m_ptr });
        m_object_retain = declare("fb_object_retain", m_void, { m_ptr });
        m_object_release = declare("fb_object_release", m_void, { m_ptr });
        m_retain_weak = declare("fb_object_retain_weak", m_void, { m_ptr });
        m_release_weak = declare("fb_object_release_weak", m_void, { m_ptr });
        m_load_weak = declare("fb_object_load_weak", m_ptr, { m_ptr });
        m_discard = declare("fb_object_discard", m_void, { m_ptr });
        m_object_is = declare("fb_object_is", m_i32, { m_ptr, m_ptr });
        m_object_expect = declare("fb_object_expect", m_void, { m_ptr, m_ptr, m_ptr });
        m_interface_methods = declare("fb_interface_methods", m_ptr, { m_ptr, m_ptr });
        m_object_implements = declare("fb_object_implements", m_i32, { m_ptr, m_ptr });
        m_object_expect_implements =
            declare("fb_object_expect_implements", m_void, { m_ptr, m_ptr, m_ptr });
        m_mutex_lock = declare("fb_mutex_lock", m_void, { m_ptr, m_ptr });
        m_mutex_unlock = declare("fb_mutex_unlock", m_void, { m_ptr });
        m_task_start = declare("fb_task_start", m_void, { m_ptr });
        m_task_start_thread = declare("fb_task_start_thread", m_void, { m_ptr });
        m_task_wait = declare("fb_task_wait", m_void, { m_ptr });
        m_task_done = declare("fb_task_done", m_i32, { m_ptr });
        m_task_release = declare("fb_task_release", m_void, { m_ptr });
        m_parallel_for = declare("fb_parallel_closure", m_void, { m_i64, m_i64, m_ptr });
        m_channel_new = declare("fb_channel_new", m_ptr, { m_ptr, m_i64, m_i64, m_ptr });
        m_channel_send = declare("fb_channel_send", m_void, { m_ptr, m_ptr, m_ptr });
        m_channel_try_send = declare("fb_channel_try_send", m_i32, { m_ptr, m_ptr });
        m_channel_receive = declare("fb_channel_receive", m_i32, { m_ptr, m_ptr });
        m_channel_try_receive = declare("fb_channel_try_receive", m_i32, { m_ptr, m_ptr });
        m_channel_close = declare("fb_channel_close", m_void, { m_ptr });
        m_channel_take_left = declare("fb_channel_take_left", m_ptr, { m_ptr });
        m_channel_element = declare("fb_channel_element", m_ptr, { m_ptr, m_i64 });
        m_channel_free = declare("fb_channel_free", m_void, { m_ptr });
        m_task_begin = declare("fb_task_begin", m_void, { m_ptr });
        m_task_finish = declare("fb_task_finish", m_void, { m_ptr });
        m_await = declare("fb_await", m_void, { m_ptr, m_ptr, m_ptr });
        m_wait_frame = declare("fb_wait_frame", m_void, { m_ptr });
        m_wait_time = declare("fb_wait_time", m_void, { m_ptr, m_i64, m_i32 });
        m_scheduler_new = declare("fb_scheduler_new", m_ptr, { m_ptr });
        m_scheduler_run = declare("fb_scheduler_run", m_void, { m_ptr, m_ptr });
        m_scheduler_tick = declare("fb_scheduler_tick", m_void, { m_ptr, f64 });
        m_scheduler_idle = declare("fb_scheduler_idle", m_i32, { m_ptr });
        m_scheduler_free = declare("fb_scheduler_free", m_void, { m_ptr });
        m_coroutine_alloc = declare("fb_coroutine_alloc", m_ptr, { m_i64 });
        m_coroutine_free = declare("fb_coroutine_free", m_void, { m_ptr });
        m_scheduler_finish = declare("fb_scheduler_finish", m_void, {});
        m_thread_local_added = declare("fb_thread_local_added", m_void, { m_ptr });
        m_thread_local_making = declare("fb_thread_local_making", m_void, {});
        m_thread_local_made = declare("fb_thread_local_made", m_void, {});
        m_thread_locals_release = declare("fb_thread_locals_release", m_void, {});
        m_array_new = declare("fb_array_new", m_ptr, { m_ptr, m_i64, m_i64 });
        m_array_push = declare("fb_array_push", m_ptr, { m_ptr, m_i64, m_ptr });
        m_array_insert = declare("fb_array_insert", m_ptr, { m_ptr, m_i64, m_i64, m_ptr });
        m_array_remove = declare("fb_array_remove", m_void, { m_ptr, m_i64, m_i64 });
        m_array_clear = declare("fb_array_clear", m_void, { m_ptr });
        m_array_clone = declare("fb_array_clone", m_ptr, { m_ptr, m_i64 });
        m_array_reserve = declare("fb_array_reserve", m_void, { m_ptr, m_i64, m_i64, m_ptr });
        m_array_trim = declare("fb_array_trim", m_void, { m_ptr, m_i64 });
        m_array_free = declare("fb_array_free", m_void, { m_ptr });
        m_index_out_of_range = declare("fb_index_out_of_range", m_void, { m_i64, m_i64, m_ptr });
        m_unsigned_index_out_of_range =
            declare("fb_unsigned_index_out_of_range", m_void, { m_i64, m_i64, m_ptr });
    }

    /// An fb_class_t, with a Method table of type `table`: its name and full name, its objects'
    /// size, its destroy, release, and visit, its count of objects, its Interfaces and their
    /// count, and the Class it extends.
    LLVMTypeRef class_type(LLVMTypeRef table) {
        std::array<LLVMTypeRef, 13> fields{
            m_ptr, m_ptr, m_i64,        // name, full_name, size
            m_ptr, m_ptr, m_ptr, m_ptr, // destroy, release, visit, debug
            m_i64, m_ptr, m_i64,        // live, interfaces, interface_count
            m_i64, m_ptr, table,        // shared, base, methods
        };
        return LLVMStructTypeInContext(context(), fields.data(),
                                       static_cast<unsigned>(fields.size()), 0);
    }

    /// Where the Method table is in an fb_class_t.
    static constexpr unsigned methods_field = 12;

    /// How the runtime's messages show a type when another they show has the same name: with
    /// each module first, the main module's too.
    [[nodiscard]] std::string full_name(const type_id_t type) const {
        return m_mir.types->qualified_name(type, m_mir.main_module);
    }

    /// An Interface's fb_interface_t, made the first time it's needed.
    LLVMValueRef interface_info(const type_id_t interface) {
        if (const auto found = m_interfaces.find(interface); found != m_interfaces.end()) {
            return found->second;
        }

        std::array<LLVMValueRef, 2> names{
            c_string(std::string(m_mir.types->name(interface))),
            c_string(full_name(interface)),
        };
        LLVMValueRef value = LLVMConstStructInContext(context(), names.data(),
                                                      static_cast<unsigned>(names.size()), 0);
        LLVMValueRef global = LLVMAddGlobal(
            m_module.get(), LLVMTypeOf(value),
            std::format("fb.{}.interface", m_mir.types->qualified_name(interface)).c_str());
        LLVMSetInitializer(global, value);
        LLVMSetGlobalConstant(global, 1);
        LLVMSetLinkage(global, LLVMInternalLinkage);
        return m_interfaces[interface] = global;
    }

    /// A function as an Interface's table holds it: itself, or for a @PanicsAtCaller one, which
    /// a call through the Interface gives no place, a function that calls it with none.
    LLVMValueRef without_place(const function_id_t id) {
        const mir_function_t& function = m_mir.functions[index(id)];
        const function_ref_t& declared = m_functions[index(id)];
        if (!function.panics_at_caller) {
            return declared.value;
        }

        if (const auto found = m_placeless.find(id); found != m_placeless.end()) {
            return found->second;
        }

        std::vector<LLVMTypeRef> parameters(LLVMCountParamTypes(declared.type));
        LLVMGetParamTypes(declared.type, parameters.data());
        parameters.pop_back();
        LLVMTypeRef type = LLVMFunctionType(LLVMGetReturnType(declared.type), parameters.data(),
                                            static_cast<unsigned>(parameters.size()), 0);
        const std::string name = std::format("fb.{} without its place", function.name);
        LLVMValueRef thunk = LLVMAddFunction(m_module.get(), name.c_str(), type);
        LLVMSetLinkage(thunk, LLVMInternalLinkage);
        LLVMBasicBlockRef current = LLVMGetInsertBlock(builder());
        LLVMValueRef outer = std::exchange(m_llvm_function, thunk);
        LLVMPositionBuilderAtEnd(builder(),
                                 LLVMAppendBasicBlockInContext(context(), thunk, "entry"));
        std::vector<LLVMValueRef> arguments;
        arguments.reserve(parameters.size() + 1);
        for (unsigned i = 0; i < parameters.size(); ++i) {
            arguments.push_back(LLVMGetParam(thunk, i));
        }

        arguments.push_back(LLVMConstNull(m_ptr));
        LLVMValueRef result = call(declared, std::move(arguments));
        if (LLVMGetTypeKind(LLVMGetReturnType(type)) == LLVMVoidTypeKind) {
            LLVMBuildRetVoid(builder());
        } else {
            LLVMBuildRet(builder(), result);
        }

        m_llvm_function = outer;
        if (current != nullptr) {
            LLVMPositionBuilderAtEnd(builder(), current);
        }

        m_placeless.emplace(id, thunk);
        return thunk;
    }

    /// A Class's fb_implementation_t for each Interface its objects implement, each with a table
    /// of its own, and how many there are.
    std::pair<LLVMValueRef, LLVMValueRef> implementations(const mir_class_t& info) {
        if (info.interfaces.empty()) {
            return { LLVMConstNull(m_ptr), LLVMConstInt(m_i64, 0, 0) };
        }

        const std::string name = m_mir.types->qualified_name(info.type);
        std::vector<LLVMValueRef> entries;
        for (const mir_implementation_t& implementation : info.interfaces) {
            std::vector<LLVMValueRef> methods;
            methods.reserve(implementation.methods.size());
            for (const function_id_t method : implementation.methods) {
                methods.push_back(without_place(method));
            }

            LLVMValueRef table_value = LLVMConstArray2(m_ptr, methods.data(), methods.size());
            LLVMValueRef table =
                LLVMAddGlobal(m_module.get(), LLVMTypeOf(table_value),
                              std::format("fb.{}.{}.methods", name,
                                          m_mir.types->qualified_name(implementation.interface))
                                  .c_str());
            LLVMSetInitializer(table, table_value);
            LLVMSetGlobalConstant(table, 1);
            LLVMSetLinkage(table, LLVMInternalLinkage);
            std::array<LLVMValueRef, 2> entry{ interface_info(implementation.interface), table };
            entries.push_back(LLVMConstStructInContext(context(), entry.data(),
                                                       static_cast<unsigned>(entry.size()), 0));
        }

        std::array<LLVMTypeRef, 2> entry_fields{ m_ptr, m_ptr };
        LLVMTypeRef entry_type = LLVMStructTypeInContext(
            context(), entry_fields.data(), static_cast<unsigned>(entry_fields.size()), 0);
        LLVMValueRef list_value = LLVMConstArray2(entry_type, entries.data(), entries.size());
        LLVMValueRef list = LLVMAddGlobal(m_module.get(), LLVMTypeOf(list_value),
                                          std::format("fb.{}.interfaces", name).c_str());
        LLVMSetInitializer(list, list_value);
        LLVMSetGlobalConstant(list, 1);
        LLVMSetLinkage(list, LLVMInternalLinkage);
        return { list, LLVMConstInt(m_i64, entries.size(), 0) };
    }

    /// A function only this module calls.
    function_ref_t declare_internal(const std::string& name, LLVMTypeRef result,
                                    std::vector<LLVMTypeRef> parameters) {
        const function_ref_t declared = declare(name.c_str(), result, std::move(parameters));
        LLVMSetLinkage(declared.value, LLVMInternalLinkage);
        return declared;
    }

    /// Whether a Class, or a Class it extends, has a Destroy().
    [[nodiscard]] bool has_destroy(const type_id_t type) const {
        for (type_id_t current = type; current != type_id_t::INVALID;
             current = m_mir.types->get(current).base) {
            if (m_mir.classes[m_class_index.at(current)].destroy) {
                return true;
            }
        }

        return false;
    }

    /// Each Class's fb_class_t. Its functions are declared here, and generate_class_functions()
    /// fills them in. The runtime counts the Class's objects in it, and its Method table follows.
    /// Every Class's global is made before any is filled in, since they refer to each other.
    void declare_classes() {
        for (std::size_t i = 0; i < m_mir.classes.size(); ++i) {
            const mir_class_t& info = m_mir.classes[i];
            m_class_index.emplace(info.type, i);
            LLVMTypeRef type = class_type(LLVMArrayType2(m_ptr, info.methods.size()));
            const std::string name =
                std::format("fb.{}.class", m_mir.types->qualified_name(info.type));
            LLVMValueRef global = LLVMAddGlobal(m_module.get(), type, name.c_str());
            LLVMSetLinkage(global, LLVMInternalLinkage);
            m_classes.emplace(info.type, global);
        }

        for (const mir_class_t& info : m_mir.classes) {
            const std::string name = m_mir.types->qualified_name(info.type);
            class_functions_t functions{
                .release = declare_internal(std::format("fb.{}.release", name), m_void, { m_ptr }),
                .visit = declare_internal(std::format("fb.{}.visit", name), m_void,
                                          { m_ptr, m_ptr, m_ptr }),
            };
            if (has_destroy(info.type)) {
                functions.destroy =
                    declare_internal(std::format("fb.{}.destroy", name), m_void, { m_ptr });
            }

            std::vector<LLVMValueRef> methods;
            methods.reserve(info.methods.size());
            for (const function_id_t method : info.methods) {
                methods.push_back(m_functions[index(method)].value);
            }

            const type_id_t base = m_mir.types->get(info.type).base;
            LLVMValueRef extended =
                base != type_id_t::INVALID ? m_classes.at(base) : LLVMConstNull(m_ptr);
            LLVMSetInitializer(
                m_classes.at(info.type),
                class_value(info.type, LLVMSizeOf(object_type(info.type)), functions.destroy,
                            functions.release, functions.visit, info.debug, implementations(info),
                            extended, std::move(methods),
                            m_mir.types->get(info.type).shared || is_task(info.type)));
            m_class_functions.push_back(functions);
        }
    }

    /// The fb_class_t's value of `type`, with no objects counted yet. `destroy` is nullopt for a
    /// Class without a Destroy() in its line, and `base` is null for one that extends nothing.
    /// `interfaces` is its list of fb_implementation_t and their count. A Shared Class's objects
    /// count their owners atomically.
    LLVMValueRef class_value(const type_id_t type, LLVMValueRef size,
                             const std::optional<function_ref_t>& destroy,
                             const function_ref_t& release, const function_ref_t& visit,
                             const std::optional<function_id_t> debug,
                             const std::pair<LLVMValueRef, LLVMValueRef>& interfaces,
                             LLVMValueRef base, std::vector<LLVMValueRef> methods,
                             const bool shared = false) {
        std::array<LLVMValueRef, 13> values{
            c_string(std::string(m_mir.types->name(type))),
            c_string(full_name(type)),
            size,
            destroy ? destroy->value : LLVMConstNull(m_ptr),
            release.value,
            visit.value,
            debug ? m_functions[index(*debug)].value : LLVMConstNull(m_ptr),
            LLVMConstInt(m_i64, 0, 0),
            interfaces.first,
            interfaces.second,
            LLVMConstInt(m_i64, shared ? 1 : 0, 0),
            base,
            LLVMConstArray2(m_ptr, methods.data(), methods.size()),
        };
        return LLVMConstStructInContext(context(), values.data(),
                                        static_cast<unsigned>(values.size()), 0);
    }

    /// Whether the zero of `type` holds a null where the type promises an object: a Class's,
    /// Interface's, T[]'s, function value's, Task's, Channel's, or Scheduler's, alone or in a
    /// Struct, tuple, fixed array, or Mutex. A T?'s zero is Null, and a Weak<T>'s is gone.
    [[nodiscard]] bool holds_null_object(const type_id_t type) const {
        if (is_object(type) || is_array(type)) {
            return true;
        }

        const type_t& info = m_mir.types->get(type);
        if (info.kind == type_kind_t::FIXED_ARRAY) {
            return holds_null_object(info.inner);
        }

        if (info.kind != type_kind_t::STRUCT && info.kind != type_kind_t::TUPLE &&
            info.kind != type_kind_t::MUTEX) {
            return false;
        }

        return std::ranges::any_of(
            info.fields, [&](const field_t& field) { return holds_null_object(field.type); });
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
            m_thread_states.push_back(global.is_thread_local ? declare_thread_local(global, value)
                                                             : nullptr);

            // Release builds check only a Global whose zero holds a null object, whose use would
            // crash; any other zero is a value of its type, which they read, as they let
            // overflow wrap.
            LLVMValueRef flag = nullptr;
            if ((checked() || holds_null_object(global.type)) && global.set_in_main) {
                const std::string flag_name = std::format("fb.ready.{}", global.name);
                flag = LLVMAddGlobal(m_module.get(), m_i32, flag_name.c_str());
                LLVMSetInitializer(flag, LLVMConstInt(m_i32, 0, 0));
                LLVMSetLinkage(flag, LLVMInternalLinkage);
            }

            m_ready.push_back(flag);
        }
    }

    /// A ThreadLocal Global's LLVM global, which each thread has its own of, and its state, which
    /// it gives.
    LLVMValueRef declare_thread_local(const global_t& global, LLVMValueRef value) {
        LLVMSetThreadLocal(value, 1);
        const std::string state_name = std::format("fb.state.{}", global.name);
        LLVMValueRef state = LLVMAddGlobal(m_module.get(), m_i32, state_name.c_str());
        LLVMSetInitializer(state, LLVMConstInt(m_i32, 0, 0));
        LLVMSetLinkage(state, LLVMInternalLinkage);
        LLVMSetThreadLocal(state, 1);
        return state;
    }

    /// Each ThreadLocal Global's functions, declared once the MIR functions are, which the
    /// one that makes the thread's own calls for its initializer.
    void declare_thread_local_functions() {
        for (const global_t& global : m_mir.globals) {
            std::optional<function_ref_t> maker;
            std::optional<function_ref_t> releaser;
            if (global.is_thread_local) {
                const std::string make = std::format("fb.make.{}", global.name);
                maker = declare(make.c_str(), m_void, {});
                LLVMSetLinkage(maker->value, LLVMInternalLinkage);
                if (m_mir.types->has_references(global.type)) {
                    const std::string release = std::format("fb.release.{}", global.name);
                    releaser = declare(release.c_str(), m_void, {});
                    LLVMSetLinkage(releaser->value, LLVMInternalLinkage);
                }
            }

            m_thread_makers.push_back(maker);
            m_thread_releasers.push_back(releaser);
        }
    }

    /// What makes a thread's own ThreadLocal Global: its initializer runs, while its state says
    /// so, and one that holds references is handed to the runtime, which runs what releases it
    /// as the thread ends, the last made first. What releases it says so first, so a Destroy
    /// that then uses it panics.
    void generate_thread_local_functions() {
        for (std::size_t i = 0; i < m_mir.globals.size(); ++i) {
            // Only a ThreadLocal Global has a maker.
            const std::optional<function_ref_t>& maker = m_thread_makers[i];
            if (!maker) {
                continue;
            }

            const global_t& global = m_mir.globals[i];
            LLVMValueRef state = m_thread_states[i];
            LLVMPositionBuilderAtEnd(
                builder(), LLVMAppendBasicBlockInContext(context(), maker->value, "entry"));
            LLVMBuildStore(builder(), LLVMConstInt(m_i32, making, 0), state);
            // The runtime keeps a worker's waits from running its older jobs meanwhile.
            if (global.initializer) {
                call(m_thread_local_making, {});
                call(m_functions[index(*global.initializer)], {});
                call(m_thread_local_made, {});
            }

            LLVMBuildStore(builder(), LLVMConstInt(m_i32, ready, 0), state);
            if (const std::optional<function_ref_t>& releaser = m_thread_releasers[i]) {
                call(m_thread_local_added, { releaser->value });
            }

            LLVMBuildRetVoid(builder());
            if (const std::optional<function_ref_t>& releaser = m_thread_releasers[i]) {
                LLVMPositionBuilderAtEnd(
                    builder(), LLVMAppendBasicBlockInContext(context(), releaser->value, "entry"));
                LLVMBuildStore(builder(), LLVMConstInt(m_i32, released, 0), state);
                let_go(m_globals[i], global.type);
                LLVMBuildRetVoid(builder());
            }
        }
    }

    /// Names get an "fb." prefix, so they can't clash with C functions like the runtime's.
    void declare_functions() {
        for (const mir_function_t& function : m_mir.functions) {
            if (function.external) {
                m_functions.push_back(declare_extern(function, *function.external));
                continue;
            }

            std::vector<LLVMTypeRef> parameters;
            for (std::size_t i = 0; i < function.parameter_count; ++i) {
                const local_t& parameter = function.locals[i];
                parameters.push_back(parameter.by_ref ? m_ptr : llvm_type(parameter.type));
            }

            // A @PanicsAtCaller function takes its call's fb_location_t last.
            if (function.panics_at_caller) {
                parameters.push_back(m_ptr);
            }

            // An Async Function's code is a coroutine, which gives its handle.
            const std::string name = "fb." + function.name;
            const function_ref_t declared =
                declare(name.c_str(), function.is_async ? m_ptr : llvm_type(function.return_type),
                        std::move(parameters));
            LLVMSetLinkage(declared.value, LLVMInternalLinkage);
            if (function.is_async) {
                constexpr std::string_view coroutine = "presplitcoroutine";
                const unsigned kind =
                    LLVMGetEnumAttributeKindForName(coroutine.data(), coroutine.size());
                LLVMAddAttributeAtIndex(declared.value,
                                        static_cast<LLVMAttributeIndex>(LLVMAttributeFunctionIndex),
                                        LLVMCreateEnumAttribute(context(), kind, 0));
            }
            if (function.is_inline) {
                constexpr std::string_view always = "alwaysinline";
                const unsigned kind = LLVMGetEnumAttributeKindForName(always.data(), always.size());
                LLVMAddAttributeAtIndex(declared.value,
                                        static_cast<LLVMAttributeIndex>(LLVMAttributeFunctionIndex),
                                        LLVMCreateEnumAttribute(context(), kind, 0));
            }

            m_functions.push_back(declared);
        }
    }

    /// An Extern Function's declaration under its symbol, which the runtime or C code defines,
    /// or the one the runtime's functions have already. Both conventions extend small integers
    /// and Bools as clang does, since the runtime is C code built for the target too.
    function_ref_t declare_extern(const mir_function_t& function, const mir_extern_t& external) {
        const bool structs =
            external.c_abi &&
            (is_struct(function.return_type) ||
             std::ranges::any_of(std::span(function.locals).first(function.parameter_count),
                                 [this](const local_t& parameter) {
                                     return !parameter.by_ref && is_struct(parameter.type);
                                 }));
        if (structs) {
            return declare_c_structs(function, external);
        }

        std::vector<LLVMTypeRef> parameters;
        for (std::size_t i = 0; i < function.parameter_count; ++i) {
            const local_t& parameter = function.locals[i];
            parameters.push_back(parameter.by_ref ? m_ptr : llvm_type(parameter.type));
        }

        LLVMTypeRef type = LLVMFunctionType(llvm_type(function.return_type), parameters.data(),
                                            static_cast<unsigned>(parameters.size()), 0);
        if (LLVMValueRef found = LLVMGetNamedFunction(m_module.get(), external.symbol.c_str())) {
            return { .type = type, .value = found };
        }

        LLVMValueRef value = LLVMAddFunction(m_module.get(), external.symbol.c_str(), type);
        extend_for_c(value, 0, function.return_type);
        for (std::size_t i = 0; i < function.parameter_count; ++i) {
            if (!function.locals[i].by_ref) {
                extend_for_c(value, static_cast<unsigned>(i + 1), function.locals[i].type);
            }
        }

        return { .type = type, .value = value };
    }

    /// How a Struct goes by the target's C ABI, as an argument taking `registers`, or as the
    /// result.
    c_struct_abi_t struct_abi(const type_id_t type, const bool result, c_registers_t& registers) {
        if (c_abi_of(m_triple) == c_abi_t::OTHER) {
            m_unsupported = std::format(
                "a Struct goes to and from C by value only on x86-64 and AArch64 yet, not on {}",
                m_triple);
        }

        c_struct_abi_t abi{
            .passing =
                classify(c_layout(*m_mir.types, type), c_abi_of(m_triple), result, registers),
        };
        for (std::size_t i = 0; i < abi.passing.pieces.size(); ++i) {
            abi.offsets.push_back(8 * i);
        }

        return abi;
    }

    /// The LLVM type of one piece a Struct goes in.
    [[nodiscard]] LLVMTypeRef piece_type(const c_piece_t& piece) const {
        switch (piece.part) {
        case c_part_t::INTEGER:
            return LLVMIntTypeInContext(context(), static_cast<unsigned>(piece.size * 8));
        case c_part_t::FLOAT:
            return LLVMFloatTypeInContext(context());
        case c_part_t::DOUBLE:
            return LLVMDoubleTypeInContext(context());
        case c_part_t::TWO_FLOATS:
            return LLVMVectorType(LLVMFloatTypeInContext(context()), 2);
        }

        std::unreachable();
    }

    /// The LLVM types a Struct's pieces go as: each its own, or one array of them all, which
    /// AArch64 keeps in consecutive registers.
    [[nodiscard]] std::vector<LLVMTypeRef> pieces_types(const c_passing_t& passing) const {
        if (passing.together) {
            return { LLVMArrayType2(piece_type(passing.pieces.front()), passing.pieces.size()) };
        }

        std::vector<LLVMTypeRef> types;
        types.reserve(passing.pieces.size());
        for (const c_piece_t& piece : passing.pieces) {
            types.push_back(piece_type(piece));
        }

        return types;
    }

    /// An attribute like "sret" or "byval" that holds a type, or "align" with a number.
    void add_type_attribute(LLVMValueRef function, const LLVMAttributeIndex index,
                            const std::string_view name, LLVMTypeRef type) {
        const unsigned kind = LLVMGetEnumAttributeKindForName(name.data(), name.size());
        LLVMAddAttributeAtIndex(function, index, LLVMCreateTypeAttribute(context(), kind, type));
    }

    void add_align(LLVMValueRef function, const LLVMAttributeIndex index,
                   const std::uint64_t align) {
        constexpr std::string_view name = "align";
        const unsigned kind = LLVMGetEnumAttributeKindForName(name.data(), name.size());
        LLVMAddAttributeAtIndex(function, index, LLVMCreateEnumAttribute(context(), kind, align));
    }

    /// An Extern Function that takes or gives a Struct by value, declared as the C ABI has it,
    /// as clang does: a Struct in registers as its pieces, through a pointer to a copy, or in
    /// memory, byval for an argument and sret for the result. Scalars take registers before and
    /// after.
    function_ref_t declare_c_structs(const mir_function_t& function, const mir_extern_t& external) {
        c_registers_t registers;
        c_signature_t signature;
        std::vector<LLVMTypeRef> parameters;
        LLVMTypeRef result = llvm_type(function.return_type);
        if (is_struct(function.return_type)) {
            signature.result = struct_abi(function.return_type, true, registers);
            if (signature.result->passing.pass == c_pass_t::MEMORY) {
                parameters.push_back(m_ptr);
                registers.integers = registers.integers > 0 ? registers.integers - 1 : 0;
                result = m_void;
            } else {
                std::vector<LLVMTypeRef> pieces = pieces_types(signature.result->passing);
                result = pieces.size() == 1
                             ? pieces.front()
                             : LLVMStructTypeInContext(context(), pieces.data(),
                                                       static_cast<unsigned>(pieces.size()), 0);
            }
        }

        for (std::size_t i = 0; i < function.parameter_count; ++i) {
            const local_t& parameter = function.locals[i];
            if (parameter.by_ref || !is_struct(parameter.type)) {
                // A scalar takes a register of its kind, while one is left.
                unsigned& left = !parameter.by_ref && is_float(parameter.type) ? registers.sse
                                                                               : registers.integers;
                left = left > 0 ? left - 1 : 0;
                parameters.push_back(parameter.by_ref ? m_ptr : llvm_type(parameter.type));
                signature.parameters.emplace_back();
                continue;
            }

            const c_struct_abi_t abi = struct_abi(parameter.type, false, registers);
            if (abi.passing.pass == c_pass_t::DIRECT) {
                std::ranges::copy(pieces_types(abi.passing), std::back_inserter(parameters));
            } else {
                parameters.push_back(m_ptr);
            }

            signature.parameters.emplace_back(abi);
        }

        LLVMTypeRef type = LLVMFunctionType(result, parameters.data(),
                                            static_cast<unsigned>(parameters.size()), 0);
        m_c_signatures[m_functions.size()] = signature;
        if (LLVMValueRef found = LLVMGetNamedFunction(m_module.get(), external.symbol.c_str())) {
            return { .type = type, .value = found };
        }

        LLVMValueRef value = LLVMAddFunction(m_module.get(), external.symbol.c_str(), type);
        unsigned index = 1;
        if (signature.result && signature.result->passing.pass == c_pass_t::MEMORY) {
            add_type_attribute(value, index, "sret", llvm_type(function.return_type));
            add_align(value, index, c_layout(*m_mir.types, function.return_type).align);
            ++index;
        } else if (!signature.result) {
            extend_for_c(value, 0, function.return_type);
        }

        for (std::size_t i = 0; i < function.parameter_count; ++i) {
            const std::optional<c_struct_abi_t>& abi = signature.parameters[i];
            if (!abi) {
                if (!function.locals[i].by_ref) {
                    extend_for_c(value, index, function.locals[i].type);
                }

                ++index;
            } else if (abi->passing.pass == c_pass_t::DIRECT) {
                index += static_cast<unsigned>(pieces_types(abi->passing).size());
            } else {
                // A SysV argument in memory takes eightbytes of the stack, as clang has it.
                if (abi->passing.pass == c_pass_t::MEMORY) {
                    add_type_attribute(value, index, "byval", llvm_type(function.locals[i].type));
                    add_align(value, index,
                              std::max<std::uint64_t>(
                                  c_layout(*m_mir.types, function.locals[i].type).align, 8));
                }

                ++index;
            }
        }

        return { .type = type, .value = value };
    }

    /// Memory for a Struct of `type` in the entry block, aligned for the C ABI's 8-byte pieces, and
    /// at least 16 bytes, since AArch64 reads a Struct of 12 bytes as two i64s, and one of 4 as an
    /// i64.
    LLVMValueRef entry_alloca(const type_id_t type) {
        const std::uint64_t size = std::max<std::uint64_t>(c_layout(*m_mir.types, type).size, 16);
        LLVMBasicBlockRef current = LLVMGetInsertBlock(builder());
        LLVMPositionBuilderAtEnd(builder(), m_entry); // it has no terminator until the end
        LLVMValueRef slot = LLVMBuildAlloca(
            builder(), LLVMArrayType2(LLVMInt8TypeInContext(context()), (size + 7) / 8 * 8),
            "c_struct");
        LLVMSetAlignment(slot, 16);
        LLVMPositionBuilderAtEnd(builder(), current);
        return slot;
    }

    /// A call of an Extern Function that takes or gives a Struct by value, by `signature`.
    LLVMValueRef call_c_structs(const function_ref_t& function, const c_signature_t& signature,
                                const call_t& node) {
        std::vector<LLVMValueRef> arguments;
        LLVMValueRef result_memory = nullptr;
        const type_id_t result_type = m_mir.functions[index(node.function)].return_type;
        if (signature.result && signature.result->passing.pass == c_pass_t::MEMORY) {
            result_memory = entry_alloca(result_type);
            arguments.push_back(result_memory);
        }

        std::vector<std::pair<unsigned, type_id_t>> byval;
        for (std::size_t i = 0; i < node.arguments.size(); ++i) {
            const call_argument_t& argument = node.arguments[i];
            if (argument.by_ref) {
                arguments.push_back(address_of(std::get<place_t>(argument.value)));
                continue;
            }

            const std::optional<c_struct_abi_t>& abi = signature.parameters[i];
            if (!abi) {
                arguments.push_back(value_of(argument.value));
                continue;
            }

            const type_id_t type = type_of(argument.value);
            LLVMValueRef copy = entry_alloca(type);
            LLVMBuildStore(builder(), value_of(argument.value), copy);
            if (abi->passing.pass != c_pass_t::DIRECT) {
                if (abi->passing.pass == c_pass_t::MEMORY) {
                    byval.emplace_back(static_cast<unsigned>(arguments.size() + 1), type);
                }

                arguments.push_back(copy);
                continue;
            }

            // Pieces that go together are one array, the Struct's bytes from its start.
            if (abi->passing.together) {
                arguments.push_back(
                    LLVMBuildLoad2(builder(), pieces_types(abi->passing).front(), copy, ""));
                continue;
            }

            for (std::size_t k = 0; k < abi->passing.pieces.size(); ++k) {
                LLVMValueRef offset = LLVMConstInt(m_i64, abi->offsets[k], 0);
                LLVMValueRef at = LLVMBuildGEP2(builder(), LLVMInt8TypeInContext(context()), copy,
                                                &offset, 1, "");
                arguments.push_back(
                    LLVMBuildLoad2(builder(), piece_type(abi->passing.pieces[k]), at, ""));
            }
        }

        LLVMValueRef made = call(function, std::move(arguments));
        for (const auto& [position, type] : byval) {
            const unsigned kind = LLVMGetEnumAttributeKindForName("byval", 5);
            LLVMAddCallSiteAttribute(made, position,
                                     LLVMCreateTypeAttribute(context(), kind, llvm_type(type)));
        }

        if (result_memory != nullptr) {
            const unsigned kind = LLVMGetEnumAttributeKindForName("sret", 4);
            LLVMAddCallSiteAttribute(
                made, 1, LLVMCreateTypeAttribute(context(), kind, llvm_type(result_type)));
            return LLVMBuildLoad2(builder(), llvm_type(result_type), result_memory, "");
        }

        if (!signature.result) {
            return made;
        }

        // The pieces go back into the Struct's bytes, an array of them at its start.
        LLVMValueRef back = entry_alloca(result_type);
        const std::vector<c_piece_t>& pieces = signature.result->passing.pieces;
        if (signature.result->passing.together) {
            LLVMBuildStore(builder(), made, back);
            return LLVMBuildLoad2(builder(), llvm_type(result_type), back, "");
        }

        for (std::size_t k = 0; k < pieces.size(); ++k) {
            LLVMValueRef piece =
                pieces.size() == 1
                    ? made
                    : LLVMBuildExtractValue(builder(), made, static_cast<unsigned>(k), "");
            LLVMValueRef offset = LLVMConstInt(m_i64, signature.result->offsets[k], 0);
            LLVMValueRef at =
                LLVMBuildGEP2(builder(), LLVMInt8TypeInContext(context()), back, &offset, 1, "");
            LLVMBuildStore(builder(), piece, at);
        }

        return LLVMBuildLoad2(builder(), llvm_type(result_type), back, "");
    }

    /// C passes a Bool extended to a register with zeros, and on x86-64 outside Windows and on
    /// Apple's targets, an integer narrower than 32 bits too, with zeros for an unsigned one and
    /// its sign for a signed one; Windows leaves those as they are, and AArch64 outside Apple's
    /// targets a Bool too, as clang does. `index` is 0 for the result, and from 1 for the
    /// parameters.
    void extend_for_c(LLVMValueRef function, const unsigned index, const type_id_t type) {
        if (const std::string_view name = c_extension(type); !name.empty()) {
            const unsigned kind = LLVMGetEnumAttributeKindForName(name.data(), name.size());
            LLVMAddAttributeAtIndex(function, index, LLVMCreateEnumAttribute(context(), kind, 0));
        }
    }

    /// The same, for a call through a Callback.
    void extend_call_for_c(LLVMValueRef call, const unsigned index, const type_id_t type) {
        if (const std::string_view name = c_extension(type); !name.empty()) {
            const unsigned kind = LLVMGetEnumAttributeKindForName(name.data(), name.size());
            LLVMAddCallSiteAttribute(call, index, LLVMCreateEnumAttribute(context(), kind, 0));
        }
    }

    /// "zeroext" or "signext" for a value C passes extended, or empty for one it doesn't.
    [[nodiscard]] std::string_view c_extension(const type_id_t type) const {
        const type_id_t integer = is_enum(type) ? m_mir.types->get(type).inner : type;
        const bool apple = m_triple.contains("apple");
        if (integer == type_id_t::BOOL) {
            return c_abi_of(m_triple) == c_abi_t::AAPCS64 && !apple ? "" : "zeroext";
        }

        const bool small_extended =
            !m_triple.contains("windows") && (m_triple.starts_with("x86_64") || apple);
        if (!small_extended || !is_integer(integer) || bit_width(integer) >= 32) {
            return {};
        }

        return is_signed(integer) ? "signext" : "zeroext";
    }

    /// A Callback's LLVM function type: C's for its parameters and result.
    LLVMTypeRef callback_type(const type_id_t callback) {
        const type_t& info = m_mir.types->get(callback);
        std::vector<LLVMTypeRef> parameters;
        parameters.reserve(info.parameters.size());
        for (const type_id_t parameter : info.parameters) {
            parameters.push_back(llvm_type(parameter));
        }

        return LLVMFunctionType(llvm_type(info.inner), parameters.data(),
                                static_cast<unsigned>(parameters.size()), 0);
    }

    /// A Callback's address: a function by the C ABI, one for each function it calls, which
    /// passes its arguments on, and a null closure first to a lambda's code.
    LLVMValueRef value(const callback_t& node) {
        // An Extern Function is C's own, which C code calls and compares as it is.
        if (const mir_function_t& code = m_mir.functions[index(node.function)];
            !node.closure && code.external && code.external->c_abi) {
            return m_functions[index(node.function)].value;
        }

        const auto key = std::pair(std::to_underlying(node.function), node.closure);
        if (const auto found = m_thunks.find(key); found != m_thunks.end()) {
            return found->second;
        }

        const type_t& info = m_mir.types->get(node.type);
        const std::string name =
            std::format("fb.callback.{}", m_mir.functions[index(node.function)].name);
        LLVMValueRef thunk =
            LLVMAddFunction(m_module.get(), name.c_str(), callback_type(node.type));
        LLVMSetLinkage(thunk, LLVMInternalLinkage);
        extend_for_c(thunk, 0, info.inner);
        for (std::size_t i = 0; i < info.parameters.size(); ++i) {
            extend_for_c(thunk, static_cast<unsigned>(i + 1), info.parameters[i]);
        }

        LLVMBasicBlockRef current = LLVMGetInsertBlock(builder());
        LLVMValueRef outer = std::exchange(m_llvm_function, thunk);
        LLVMPositionBuilderAtEnd(builder(),
                                 LLVMAppendBasicBlockInContext(context(), thunk, "entry"));
        std::vector<LLVMValueRef> arguments;
        if (node.closure) {
            arguments.push_back(LLVMConstPointerNull(m_ptr));
        }

        for (std::size_t i = 0; i < info.parameters.size(); ++i) {
            LLVMValueRef argument = LLVMGetParam(thunk, static_cast<unsigned>(i));
            // An Enum C passes has to be one of its values.
            const type_t& parameter = m_mir.types->get(info.parameters[i]);
            if (parameter.kind == type_kind_t::ENUM && !parameter.flags) {
                const mir_function_t& code = m_mir.functions[index(node.function)];
                const std::string_view parameter_name =
                    code.locals[i + (node.closure ? 1 : 0)].name;
                panic_if(LLVMBuildNot(builder(), is_enum_value(argument, info.parameters[i]), ""),
                         std::format("C passed '{}' a value that isn't one of {}'s", parameter_name,
                                     m_mir.types->name(info.parameters[i])),
                         node.span);
            }

            arguments.push_back(argument);
        }

        // C gives a @PanicsAtCaller function no place, so its panics report where they are.
        if (m_mir.functions[index(node.function)].panics_at_caller) {
            arguments.push_back(LLVMConstNull(m_ptr));
        }

        LLVMValueRef result = call(m_functions[index(node.function)], std::move(arguments));
        if (info.inner == type_id_t::NONE) {
            LLVMBuildRetVoid(builder());
        } else {
            LLVMBuildRet(builder(), result);
        }

        m_llvm_function = outer;
        LLVMPositionBuilderAtEnd(builder(), current);
        m_thunks.emplace(key, thunk);
        return thunk;
    }

    /// Whether `value`, an Enum's integer, is one of the Enum's values.
    LLVMValueRef is_enum_value(LLVMValueRef value, const type_id_t type) {
        LLVMTypeRef integer = LLVMTypeOf(value);
        LLVMValueRef found = LLVMConstInt(m_bool, 0, 0);
        for (const enum_value_t& member : m_mir.types->get(type).values) {
            LLVMValueRef same = LLVMBuildICmp(builder(), LLVMIntEQ, value,
                                              LLVMConstInt(integer, member.bits, 0), "");
            found = LLVMBuildOr(builder(), found, same, "");
        }

        return found;
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
        if (zero_is_null(type)) {
            return LLVMConstNull(llvm_type(type));
        }

        if (info.kind == type_kind_t::ENUM) {
            return LLVMConstInt(llvm_type(type), m_mir.types->enum_zero(type), 0);
        }

        if (info.kind == type_kind_t::FIXED_ARRAY) {
            std::vector<LLVMValueRef> elements(m_mir.types->element_count(type),
                                               zero_constant(info.inner));
            return LLVMConstArray2(llvm_type(info.inner), elements.data(), elements.size());
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
        LLVMValueRef global = LLVMAddGlobal(m_module.get(), LLVMTypeOf(value), "fb.string");
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
        LLVMValueRef global = LLVMAddGlobal(m_module.get(), LLVMTypeOf(value), "fb.text");
        LLVMSetInitializer(global, value);
        LLVMSetGlobalConstant(global, 1);
        LLVMSetLinkage(global, LLVMPrivateLinkage);
        LLVMSetUnnamedAddress(global, LLVMGlobalUnnamedAddr);
        m_c_strings.emplace(text, global);
        return global;
    }

    /// An fb_location_t for a span, for panics to report. In a @PanicsAtCaller function's code,
    /// it's where its call is, when its caller gives one.
    LLVMValueRef location(const span_t span) {
        LLVMValueRef own = own_location(span);
        LLVMValueRef place = caller_place();
        if (place == nullptr) {
            return own;
        }

        LLVMValueRef none = LLVMBuildICmp(builder(), LLVMIntEQ, place, LLVMConstNull(m_ptr), "");
        return LLVMBuildSelect(builder(), none, own, place, "");
    }

    /// In the code of a @PanicsAtCaller function: the place its caller gives, the hidden
    /// parameter after the others, which is null where the caller has none. Null in other code.
    [[nodiscard]] LLVMValueRef caller_place() const {
        if (m_function == nullptr || !m_function->panics_at_caller) {
            return nullptr;
        }

        const auto at = static_cast<std::size_t>(m_function - m_mir.functions.data());
        if (m_llvm_function != m_functions[at].value) {
            return nullptr;
        }

        return LLVMGetParam(m_llvm_function, static_cast<unsigned>(m_function->parameter_count));
    }

    /// The fb_location_t of `span` itself.
    LLVMValueRef own_location(const span_t span) {
        const source_file_t& file = m_sources.get(span.file);
        const location_t start = file.location_of(span.begin, column_unit_t::CODEPOINT);
        std::array<LLVMValueRef, 3> fields{
            c_string(file.path()),
            LLVMConstInt(m_i32, start.line + 1, 0),
            LLVMConstInt(m_i32, start.column + 1, 0),
        };
        LLVMValueRef value = LLVMConstStructInContext(context(), fields.data(),
                                                      static_cast<unsigned>(fields.size()), 0);
        LLVMValueRef global = LLVMAddGlobal(m_module.get(), LLVMTypeOf(value), "fb.location");
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
        // ByRef parameter is already the address of the caller's variable. An Async Function's
        // coroutine starts before the parameters are stored, so they're in its frame.
        m_slots.clear();
        std::vector<std::pair<LLVMValueRef, LLVMValueRef>> stores;
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
                stores.emplace_back(argument, slot);
            }

            m_slots.push_back(slot);
        }

        if (m_function->is_async) {
            begin_coroutine();
        }

        for (const auto& [argument, slot] : stores) {
            LLVMBuildStore(builder(), argument, slot);
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

    /// A Class's functions for the runtime. Its destroy, when an object's last owner lets go,
    /// runs the Class's Destroy(), then those of the Classes it extends. Its release releases the
    /// object's fields, after the destroy, or instead when New threw. Its visit calls the visitor
    /// with each object the fields refer to strongly, for the cycle detector.
    void generate_class_functions(const std::size_t class_index) {
        const mir_class_t& info = m_mir.classes[class_index];
        const class_functions_t& functions = m_class_functions[class_index];
        if (functions.destroy) {
            LLVMValueRef function = functions.destroy->value;
            LLVMPositionBuilderAtEnd(builder(),
                                     LLVMAppendBasicBlockInContext(context(), function, "entry"));
            LLVMValueRef object = LLVMGetParam(function, 0);
            for (type_id_t type = info.type; type != type_id_t::INVALID;
                 type = m_mir.types->get(type).base) {
                if (const mir_class_t& current = m_mir.classes[m_class_index.at(type)];
                    current.destroy) {
                    call(m_functions[index(*current.destroy)], { object });
                }
            }

            LLVMBuildRetVoid(builder());
        }

        // The fields come after the header's two.
        const std::vector<field_t>& fields = m_mir.types->get(info.type).fields;
        LLVMValueRef release = functions.release.value;
        LLVMPositionBuilderAtEnd(builder(),
                                 LLVMAppendBasicBlockInContext(context(), release, "entry"));
        for (std::uint32_t i = 0; i < fields.size(); ++i) {
            if (m_mir.types->has_references(fields[i].type)) {
                LLVMValueRef field = LLVMBuildStructGEP2(builder(), object_type(info.type),
                                                         LLVMGetParam(release, 0), i + 2, "");
                for_each_reference(field, fields[i].type, false);
            }
        }

        // A Task's or Thread's job is the runtime's.
        if (is_task(info.type)) {
            call(m_task_release, { LLVMGetParam(release, 0) });
        }

        LLVMBuildRetVoid(builder());

        LLVMValueRef visit = functions.visit.value;
        LLVMPositionBuilderAtEnd(builder(),
                                 LLVMAppendBasicBlockInContext(context(), visit, "entry"));
        for (std::uint32_t i = 0; i < fields.size(); ++i) {
            if (holds_objects(fields[i].type)) {
                LLVMValueRef field = LLVMBuildStructGEP2(builder(), object_type(info.type),
                                                         LLVMGetParam(visit, 0), i + 2, "");
                visit_objects(field, fields[i].type, visit, c_string(field_label(info.type, i)));
            }
        }

        LLVMBuildRetVoid(builder());
    }

    /// A Class's field as leak reports name it: its name, and the Class it's declared in when a
    /// field after it has the name too, as one of a Class extending that Class can where it
    /// can't see it: "hidden (lib.Node's)".
    [[nodiscard]] std::string field_label(const type_id_t type, const std::uint32_t at) const {
        const std::vector<field_t>& fields = m_mir.types->get(type).fields;
        const std::string& name = fields[at].name;
        if (std::ranges::none_of(fields.begin() + at + 1, fields.end(),
                                 [&](const field_t& field) { return field.name == name; })) {
            return name;
        }

        // The Class that declares it is the furthest one along the bases that has it.
        type_id_t owner = type;
        for (type_id_t base = m_mir.types->get(type).base;
             base != type_id_t::INVALID && m_mir.types->get(base).fields.size() > at;
             base = m_mir.types->get(base).base) {
            owner = base;
        }

        return std::format("{} ({}'s)", name, full_name(owner));
    }

    /// Whether a value of `type` refers to objects strongly, which could make a cycle: a Class or
    /// T[], or a Struct, tuple, T?, or fixed array with one inside. A String refers to nothing,
    /// and a Weak<T> doesn't keep its object.
    [[nodiscard]] bool holds_objects(const type_id_t type) const {
        const type_t& info = m_mir.types->get(type);
        switch (info.kind) {
        case type_kind_t::CLASS:
        case type_kind_t::INTERFACE:
        case type_kind_t::ARRAY:
        case type_kind_t::FUNCTION:
        case type_kind_t::TASK:
        case type_kind_t::THREAD:
        case type_kind_t::CHANNEL:
        case type_kind_t::SCHEDULER:
            return true;
        case type_kind_t::NULLABLE:
        case type_kind_t::FIXED_ARRAY:
            return holds_objects(info.inner);
        case type_kind_t::STRUCT:
        case type_kind_t::TUPLE:
        case type_kind_t::MUTEX:
            return std::ranges::any_of(
                info.fields, [this](const field_t& field) { return holds_objects(field.type); });
        default:
            return false;
        }
    }

    /// In a visit, `function`: calls its visitor with each object in the value of `type` at
    /// `address`, and `name`, the field it's in.
    void visit_objects(LLVMValueRef address, const type_id_t type, LLVMValueRef function,
                       LLVMValueRef name) {
        if (is_nullable(type)) {
            if (!is_nullable_reference(type)) {
                address = LLVMBuildStructGEP2(builder(), llvm_type(type), address, 1, "");
            }

            visit_objects(address, inner_of(type), function, name);
            return;
        }

        if (is_object(type) || is_array(type)) {
            std::array<LLVMValueRef, 3> arguments{
                LLVMGetParam(function, 2),
                LLVMBuildLoad2(builder(), m_ptr, address, ""),
                name,
            };
            LLVMBuildCall2(builder(), m_visitor, LLVMGetParam(function, 1), arguments.data(),
                           static_cast<unsigned>(arguments.size()), "");
            return;
        }

        if (is_fixed_array(type)) {
            const type_id_t element = inner_of(type);
            LLVMValueRef count = LLVMConstInt(m_i64, m_mir.types->element_count(type), 0);
            for_each_element(address, llvm_type(element), count, [&](LLVMValueRef item) {
                visit_objects(item, element, function, name);
            });
            return;
        }

        const std::vector<field_t>& fields = m_mir.types->get(type).fields;
        for (std::uint32_t i = 0; i < fields.size(); ++i) {
            if (holds_objects(fields[i].type)) {
                LLVMValueRef field =
                    LLVMBuildStructGEP2(builder(), llvm_type(type), address, i, "");
                visit_objects(field, fields[i].type, function, name);
            }
        }
    }

    /// A T[]'s fb_class_t, made the first time it's needed. It's named like "Int32[]", its release
    /// releases the elements and frees the buffer, and its visit visits the elements.
    LLVMValueRef array_class(const type_id_t type) {
        if (const auto found = m_classes.find(type); found != m_classes.end()) {
            return found->second;
        }

        const std::string name = m_mir.types->qualified_name(type);
        const array_class_t functions{
            .type = type,
            .release = declare_internal(std::format("fb.{}.release", name), m_void, { m_ptr }),
            .visit =
                declare_internal(std::format("fb.{}.visit", name), m_void, { m_ptr, m_ptr, m_ptr }),
        };
        // Its tables, when its objects go where an Interface is expected.
        const auto tables = std::ranges::find(m_mir.array_classes, type, &mir_class_t::type);
        const bool tabled = tables != m_mir.array_classes.end();
        // Its debug form, once the program shows one.
        std::optional<function_id_t> debug = tabled ? tables->debug : std::nullopt;
        if (const auto shown = std::ranges::find(m_mir.array_debugs, type,
                                                 &std::pair<type_id_t, function_id_t>::first);
            shown != m_mir.array_debugs.end()) {
            debug = shown->second;
        }

        LLVMValueRef info = class_value(
            type, LLVMSizeOf(m_array_type), std::nullopt, functions.release, functions.visit, debug,
            tabled ? implementations(*tables)
                   : std::pair{ LLVMConstNull(m_ptr), LLVMConstInt(m_i64, 0, 0) },
            LLVMConstNull(m_ptr), {});
        const std::string global_name = std::format("fb.{}.class", name);
        LLVMValueRef global = LLVMAddGlobal(m_module.get(), LLVMTypeOf(info), global_name.c_str());
        LLVMSetInitializer(global, info);
        LLVMSetLinkage(global, LLVMInternalLinkage);
        m_classes.emplace(type, global);
        m_array_classes.push_back(functions);
        return global;
    }

    /// Each T[]'s functions: its release releases the elements and frees the buffer, and the
    /// runtime frees the object. Its visit visits the elements, which aren't in fields.
    void generate_array_functions() {
        for (const array_class_t& functions : m_array_classes) {
            const type_id_t element = inner_of(functions.type);
            LLVMValueRef release = functions.release.value;
            LLVMPositionBuilderAtEnd(builder(),
                                     LLVMAppendBasicBlockInContext(context(), release, "entry"));
            LLVMValueRef array = LLVMGetParam(release, 0);
            for_each_reference_in(array, element, false);
            call(m_array_free, { array });
            LLVMBuildRetVoid(builder());

            LLVMValueRef visit = functions.visit.value;
            LLVMPositionBuilderAtEnd(builder(),
                                     LLVMAppendBasicBlockInContext(context(), visit, "entry"));
            if (holds_objects(element)) {
                LLVMValueRef elements = LLVMGetParam(visit, 0);
                LLVMValueRef data = LLVMBuildLoad2(
                    builder(), m_ptr, LLVMBuildStructGEP2(builder(), m_array_type, elements, 4, ""),
                    "");
                for_each_element(data, llvm_type(element), count_of(elements),
                                 [&](LLVMValueRef item) {
                                     visit_objects(item, element, visit, LLVMConstNull(m_ptr));
                                 });
            }

            LLVMBuildRetVoid(builder());
        }
    }

    /// main: starts the runtime, runs the top-level code, waits for the pool's jobs, releases
    /// the Globals, and lets the runtime check that nothing leaked, with every Class's count.
    /// Debug builds keep track of the objects, so the report can point out cycles among the ones
    /// left.
    void generate_main() {
        const function_ref_t main = declare("main", m_i32, {});
        LLVMPositionBuilderAtEnd(builder(),
                                 LLVMAppendBasicBlockInContext(context(), main.value, "entry"));
        call(m_start, {});
        if (checked()) {
            call(m_track_objects, {});
        }

        call(m_functions[index(m_mir.entry)], {});
        // The coroutines run to their ends, and a job may still use a Global.
        call(m_scheduler_finish, {});
        // The pool's threads release their ThreadLocals as they end, and then the main thread
        // does its own, before the Globals, which their Destroys can use.
        call(m_pool_finish, {});
        call(m_thread_locals_release, {});
        // Then the Globals let go of what they hold, the last first, so a Destroy that runs can
        // use the ones above its object's. One that uses a Global already let go of panics where
        // the Global has a ready flag, and reads its zero otherwise.
        for (std::size_t i = m_mir.globals.size(); i-- > 0;) {
            if (m_mir.globals[i].is_thread_local) {
                continue;
            }

            if (m_ready[i] != nullptr) {
                LLVMBuildStore(builder(), LLVMConstInt(m_i32, released, 0), m_ready[i]);
            }

            if (m_mir.types->has_references(m_mir.globals[i].type)) {
                let_go(m_globals[i], m_mir.globals[i].type);
            }
        }

        std::vector<LLVMValueRef> classes;
        classes.reserve(m_mir.classes.size());
        for (const mir_class_t& info : m_mir.classes) {
            classes.push_back(m_classes.at(info.type));
        }

        for (const array_class_t& entry : m_array_classes) {
            classes.push_back(m_classes.at(entry.type));
        }

        for (const array_class_t& entry : m_channel_classes) {
            classes.push_back(m_classes.at(entry.type));
        }

        if (m_scheduler_class) {
            classes.push_back(m_classes.at(m_scheduler_class->type));
        }

        LLVMValueRef list = LLVMConstNull(m_ptr);
        if (!classes.empty()) {
            LLVMValueRef array =
                LLVMConstArray2(m_ptr, classes.data(), static_cast<std::uint64_t>(classes.size()));
            list = LLVMAddGlobal(m_module.get(), LLVMTypeOf(array), "fb.classes");
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

        for (const step_t& step : place.steps) {
            const type_t& info = m_mir.types->get(type);
            type = step.indices.empty() ? info.fields[step.field].type : info.inner;
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

        for (const step_t& step : place.steps) {
            if (!step.indices.empty()) {
                address = element_address(address, type, step);
                type = inner_of(type);
                continue;
            }

            // A Ptr's variable holds the address of what it points at.
            if (is_pointer(type)) {
                address = pointee(LLVMBuildLoad2(builder(), m_ptr, address, ""), step.span);
                type = inner_of(type);
                continue;
            }

            const std::uint32_t field = step.field;
            if (is_class(type) || is_task(type)) {
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

    /// Where an array's element is, from where the array is: a T[] holds a reference to its
    /// object, whose buffer has the elements, and a fixed array has them inline. An index out of
    /// range panics.
    LLVMValueRef element_address(LLVMValueRef address, const type_id_t type, const step_t& step) {
        // From a Ptr, the index counts T's on from where it points, or back.
        if (is_pointer(type)) {
            LLVMValueRef base = pointee(LLVMBuildLoad2(builder(), m_ptr, address, ""), step.span);
            LLVMValueRef position = index_value(step.indices[0]);
            return LLVMBuildGEP2(builder(), llvm_type(inner_of(type)), base, &position, 1, "");
        }

        if (is_array(type)) {
            LLVMValueRef array = LLVMBuildLoad2(builder(), m_ptr, address, "");
            return array_element(array, inner_of(type), index_value(step.indices[0]),
                                 index_type(step.indices[0]), step.span);
        }

        // One index counts every element of a fixed array; more go one per dimension.
        const std::vector<std::uint32_t>& sizes = m_mir.types->get(type).sizes;
        LLVMValueRef flat = nullptr;
        if (step.indices.size() == 1) {
            LLVMValueRef count = LLVMConstInt(m_i64, m_mir.types->element_count(type), 0);
            flat = check_index(index_value(step.indices[0]), index_type(step.indices[0]), count,
                               step.span);
        } else {
            for (std::size_t i = 0; i < sizes.size(); ++i) {
                LLVMValueRef size = LLVMConstInt(m_i64, sizes[i], 0);
                LLVMValueRef position = check_index(index_value(step.indices[i]),
                                                    index_type(step.indices[i]), size, step.span);
                flat = flat == nullptr
                           ? position
                           : LLVMBuildAdd(builder(), LLVMBuildMul(builder(), flat, size, ""),
                                          position, "");
            }
        }

        return LLVMBuildGEP2(builder(), llvm_type(inner_of(type)), address, &flat, 1, "");
    }

    /// What a Ptr, `address`, points at. A debug build panics at `span` when it's Null.
    LLVMValueRef pointee(LLVMValueRef address, const span_t span) {
        if (checked()) {
            panic_if(LLVMBuildIsNull(builder(), address, ""), "the Ptr is Null", span);
        }

        return address;
    }

    /// The address of a T[]'s element at `position`, an index of type `index` as an Int64, which
    /// panics when it's out of range.
    LLVMValueRef array_element(LLVMValueRef array, const type_id_t element, LLVMValueRef position,
                               const type_id_t index, const span_t span) {
        check_index(position, index, count_of(array), span);
        LLVMValueRef data = LLVMBuildLoad2(
            builder(), m_ptr, LLVMBuildStructGEP2(builder(), m_array_type, array, 4, ""), "");
        return LLVMBuildGEP2(builder(), llvm_type(element), data, &position, 1, "");
    }

    /// A T[]'s count, an Int64.
    LLVMValueRef count_of(LLVMValueRef array) {
        return LLVMBuildLoad2(builder(), m_i64,
                              LLVMBuildStructGEP2(builder(), m_array_type, array, 2, ""), "");
    }

    /// Panics unless `position`, an index of type `index` as an Int64, is from 0 to `count` minus
    /// 1, then gives it back. Unsigned, so a negative one is out of range too. The panic shows a
    /// UInt64's past an Int64's range as it is.
    LLVMValueRef check_index(LLVMValueRef position, const type_id_t index, LLVMValueRef count,
                             const span_t span) {
        LLVMValueRef function = LLVMGetBasicBlockParent(LLVMGetInsertBlock(builder()));
        LLVMBasicBlockRef fail = LLVMAppendBasicBlockInContext(context(), function, "out_of_range");
        LLVMBasicBlockRef ok = LLVMAppendBasicBlockInContext(context(), function, "in_range");
        LLVMBuildCondBr(builder(), LLVMBuildICmp(builder(), LLVMIntUGE, position, count, ""), fail,
                        ok);
        LLVMPositionBuilderAtEnd(builder(), fail);
        call(index == type_id_t::UINT64 ? m_unsigned_index_out_of_range : m_index_out_of_range,
             { position, count, location(span) });
        LLVMBuildUnreachable(builder());
        LLVMPositionBuilderAtEnd(builder(), ok);
        return position;
    }

    /// An index's value as an Int64.
    LLVMValueRef index_value(const local_id_t local) {
        return to_int64(place_t{ local });
    }

    /// An index's type.
    [[nodiscard]] type_id_t index_type(const local_id_t local) const {
        return type_of(place_t{ local });
    }

    LLVMValueRef to_int64(const operand_t& operand) {
        const type_id_t type = type_of(operand);
        return LLVMBuildIntCast2(builder(), value_of(operand), m_i64, is_signed(type) ? 1 : 0, "");
    }

    /// Runs `body` on the address of each of `count` elements of `type` from `first`, in a loop.
    template <typename Body>
    void for_each_element(LLVMValueRef first, LLVMTypeRef type, LLVMValueRef count,
                          const Body& body) {
        LLVMBasicBlockRef before = LLVMGetInsertBlock(builder());
        LLVMValueRef function = LLVMGetBasicBlockParent(before);
        LLVMBasicBlockRef head = LLVMAppendBasicBlockInContext(context(), function, "each");
        LLVMBasicBlockRef loop = LLVMAppendBasicBlockInContext(context(), function, "element");
        LLVMBasicBlockRef done = LLVMAppendBasicBlockInContext(context(), function, "done");
        LLVMBuildBr(builder(), head);
        LLVMPositionBuilderAtEnd(builder(), head);
        LLVMValueRef position = LLVMBuildPhi(builder(), m_i64, "");
        LLVMBuildCondBr(builder(), LLVMBuildICmp(builder(), LLVMIntULT, position, count, ""), loop,
                        done);
        LLVMPositionBuilderAtEnd(builder(), loop);
        body(LLVMBuildGEP2(builder(), type, first, &position, 1, ""));
        LLVMValueRef next = LLVMBuildAdd(builder(), position, LLVMConstInt(m_i64, 1, 0), "");
        LLVMBasicBlockRef end = LLVMGetInsertBlock(builder());
        LLVMBuildBr(builder(), head);
        std::array<LLVMValueRef, 2> values{ LLVMConstInt(m_i64, 0, 0), next };
        std::array<LLVMBasicBlockRef, 2> blocks{ before, end };
        LLVMAddIncoming(position, values.data(), blocks.data(), 2);
        LLVMPositionBuilderAtEnd(builder(), done);
    }

    /// Retains, or releases, every reference in a T[]'s elements.
    void for_each_reference_in(LLVMValueRef array, const type_id_t element, const bool retain) {
        if (!m_mir.types->has_references(element)) {
            return;
        }

        LLVMValueRef data = LLVMBuildLoad2(
            builder(), m_ptr, LLVMBuildStructGEP2(builder(), m_array_type, array, 4, ""), "");
        for_each_element(data, llvm_type(element), count_of(array), [&](LLVMValueRef address) {
            for_each_reference(address, element, retain);
        });
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

        if (is_object(type) || is_array(type)) {
            call(retain ? m_object_retain : m_object_release,
                 { LLVMBuildLoad2(builder(), m_ptr, address, "") });
            return;
        }

        if (is_weak(type)) {
            call(retain ? m_retain_weak : m_release_weak,
                 { LLVMBuildLoad2(builder(), m_ptr, address, "") });
            return;
        }

        if (is_fixed_array(type)) {
            const type_id_t element = inner_of(type);
            if (m_mir.types->has_references(element)) {
                LLVMValueRef count = LLVMConstInt(m_i64, m_mir.types->element_count(type), 0);
                for_each_element(address, llvm_type(element), count, [&](LLVMValueRef item) {
                    for_each_reference(item, element, retain);
                });
            }

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

    /// Releases every reference in the value of `type` at `address`, as for_each_reference does,
    /// but sets each to its zero before releasing it, and a T? that holds its T inline to Null
    /// first. A Destroy that runs meanwhile reads what's still there, or a zero, never what's
    /// been released.
    void let_go(LLVMValueRef address, const type_id_t type) {
        if (is_nullable(type) && !is_nullable_reference(type)) {
            LLVMBuildStore(builder(), LLVMConstInt(m_bool, 0, 0),
                           LLVMBuildStructGEP2(builder(), llvm_type(type), address, 0, ""));
            let_go(LLVMBuildStructGEP2(builder(), llvm_type(type), address, 1, ""), inner_of(type));
            return;
        }

        if (is_fixed_array(type)) {
            const type_id_t element = inner_of(type);
            LLVMValueRef count = LLVMConstInt(m_i64, m_mir.types->element_count(type), 0);
            for_each_element(address, llvm_type(element), count,
                             [&](LLVMValueRef item) { let_go(item, element); });
            return;
        }

        // A reference: a String, an object, a T[], or a Weak, or a T? of one, whose Null is null.
        const type_id_t inner = is_nullable(type) ? inner_of(type) : type;
        if (inner == type_id_t::STRING || is_object(inner) || is_array(inner) || is_weak(inner)) {
            LLVMValueRef reference = LLVMBuildLoad2(builder(), m_ptr, address, "");
            LLVMBuildStore(builder(), zero_constant(type), address);
            if (inner == type_id_t::STRING) {
                call(m_release, { reference });
            } else if (is_weak(inner)) {
                call(m_release_weak, { reference });
            } else {
                call(m_object_release, { reference });
            }

            return;
        }

        const std::vector<field_t>& fields = m_mir.types->get(type).fields;
        for (std::uint32_t i = 0; i < fields.size(); ++i) {
            if (m_mir.types->has_references(fields[i].type)) {
                let_go(LLVMBuildStructGEP2(builder(), llvm_type(type), address, i, ""),
                       fields[i].type);
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

    /// Marks a Global main has set, or panics where a function uses one it hasn't yet, which an
    /// earlier Global's initializer reached, or one the program has released at its end. Only a
    /// Global with a ready flag has either: in debug builds, each one main sets, and in release
    /// builds, one whose zero holds a null object.
    void generate(const ready_t& node) {
        // Only a ThreadLocal Global has a maker.
        if (const std::optional<function_ref_t>& maker = m_thread_makers[index(node.global)]) {
            make_thread_local(node, *maker);
            return;
        }

        LLVMValueRef flag = m_ready[index(node.global)];
        if (flag == nullptr) {
            return;
        }

        if (node.mark) {
            LLVMBuildStore(builder(), LLVMConstInt(m_i32, ready, 0), flag);
            return;
        }

        const std::string& name = m_mir.globals[index(node.global)].name;
        LLVMValueRef state = LLVMBuildLoad2(builder(), m_i32, flag, "");
        LLVMBasicBlockRef unready =
            LLVMAppendBasicBlockInContext(context(), m_llvm_function, "unready");
        LLVMBasicBlockRef ok = LLVMAppendBasicBlockInContext(context(), m_llvm_function, "ok");
        LLVMBuildCondBr(
            builder(),
            LLVMBuildICmp(builder(), LLVMIntEQ, state, LLVMConstInt(m_i32, ready, 0), ""), ok,
            unready);
        LLVMPositionBuilderAtEnd(builder(), unready);
        panic_if(LLVMBuildICmp(builder(), LLVMIntEQ, state, LLVMConstInt(m_i32, released, 0), ""),
                 std::format("'{}' is used after it's released, since Globals are released "
                             "last to first when the program ends",
                             name),
                 node.span);
        panic(std::format("'{}' is used before its initializer has run, since Globals "
                          "initialize in source order",
                          name),
              node.span);
        LLVMPositionBuilderAtEnd(builder(), ok);
    }

    /// Before a use of a ThreadLocal Global: the thread makes its own the first time, with its
    /// maker, and panics where its initializer reaches it, or where it's used after the thread
    /// released it.
    void make_thread_local(const ready_t& node, const function_ref_t& maker) {
        const std::size_t global = index(node.global);
        const std::string& name = m_mir.globals[global].name;
        LLVMValueRef state = LLVMBuildLoad2(builder(), m_i32, m_thread_states[global], "");
        LLVMBasicBlockRef unmade =
            LLVMAppendBasicBlockInContext(context(), m_llvm_function, "unmade");
        LLVMBasicBlockRef ok = LLVMAppendBasicBlockInContext(context(), m_llvm_function, "ok");
        LLVMBuildCondBr(
            builder(),
            LLVMBuildICmp(builder(), LLVMIntEQ, state, LLVMConstInt(m_i32, ready, 0), ""), ok,
            unmade);
        LLVMPositionBuilderAtEnd(builder(), unmade);
        panic_if(LLVMBuildICmp(builder(), LLVMIntEQ, state, LLVMConstInt(m_i32, released, 0), ""),
                 std::format("'{}' is used after it's released, since a thread releases its "
                             "ThreadLocal Globals as it ends",
                             name),
                 node.span);
        panic_if(LLVMBuildICmp(builder(), LLVMIntEQ, state, LLVMConstInt(m_i32, making, 0), ""),
                 std::format("'{}' is used before its initializer has run, since its "
                             "initializer reaches it",
                             name),
                 node.span);
        call(maker, {});
        LLVMBuildBr(builder(), ok);
        LLVMPositionBuilderAtEnd(builder(), ok);
    }

    // Terminators

    void generate(const goto_t& node) {
        LLVMBuildBr(builder(), m_blocks[index(node.target)]);
    }

    void generate(const branch_t& node) {
        LLVMBuildCondBr(builder(), value_of(node.condition), m_blocks[index(node.if_true)],
                        m_blocks[index(node.if_false)]);
    }

    /// An Async Function's coroutine: its frame, from the runtime, holds what lives across an
    /// Await. Its end frees the frame, and gives LLVM the handle.
    void begin_coroutine() {
        LLVMValueRef null = LLVMConstNull(m_ptr);
        m_coroutine_id =
            call_plain_intrinsic("llvm.coro.id", { LLVMConstInt(m_i32, 0, 0), null, null, null });
        LLVMValueRef size = call_intrinsic("llvm.coro.size", m_i64, {});
        LLVMValueRef frame = call(m_coroutine_alloc, { size });
        m_coroutine = call_plain_intrinsic("llvm.coro.begin", { m_coroutine_id, frame });
        LLVMBasicBlockRef entry = LLVMGetInsertBlock(builder());

        m_coroutine_cleanup = LLVMAppendBasicBlockInContext(context(), m_llvm_function, "free");
        m_coroutine_end = LLVMAppendBasicBlockInContext(context(), m_llvm_function, "end");
        LLVMPositionBuilderAtEnd(builder(), m_coroutine_cleanup);
        LLVMValueRef memory =
            call_plain_intrinsic("llvm.coro.free", { m_coroutine_id, m_coroutine });
        call(m_coroutine_free, { memory });
        LLVMBuildBr(builder(), m_coroutine_end);
        LLVMPositionBuilderAtEnd(builder(), m_coroutine_end);
        std::vector<LLVMValueRef> end{ m_coroutine, LLVMConstInt(m_bool, 0, 0) };
        const unsigned end_id = LLVMLookupIntrinsicID("llvm.coro.end", 13);
        if (LLVMCountParamTypes(LLVMIntrinsicGetType(context(), end_id, nullptr, 0)) == 3) {
            end.push_back(LLVMConstNull(LLVMTokenTypeInContext(context())));
        }

        call_plain_intrinsic("llvm.coro.end", std::move(end));
        LLVMBuildRet(builder(), m_coroutine);
        LLVMPositionBuilderAtEnd(builder(), entry);
    }

    /// Calls an LLVM intrinsic that isn't overloaded, like "llvm.coro.begin".
    LLVMValueRef call_plain_intrinsic(const std::string_view name,
                                      std::vector<LLVMValueRef> arguments) {
        const unsigned id = LLVMLookupIntrinsicID(name.data(), name.size());
        const function_ref_t intrinsic{
            .type = LLVMIntrinsicGetType(context(), id, nullptr, 0),
            .value = LLVMGetIntrinsicDeclaration(m_module.get(), id, nullptr, 0),
        };
        return call(intrinsic, std::move(arguments));
    }

    /// The coroutine stops; LLVM's switch says where it goes when it's resumed, or destroyed,
    /// which ends at the block that frees the frame.
    void generate(const suspend_t& node) {
        LLVMValueRef none = LLVMConstNull(LLVMTokenTypeInContext(context()));
        LLVMValueRef state =
            call_plain_intrinsic("llvm.coro.suspend", { none, LLVMConstInt(m_bool, 0, 0) });
        LLVMTypeRef i8 = LLVMInt8TypeInContext(context());
        LLVMValueRef choice = LLVMBuildSwitch(builder(), state, m_coroutine_end, 2);
        LLVMAddCase(choice, LLVMConstInt(i8, 0, 0), m_blocks[index(node.resume)]);
        LLVMAddCase(choice, LLVMConstInt(i8, 1, 0), m_blocks[index(node.cancel)]);
    }

    void generate(const return_t& node) {
        // An Async Function's Return ends its coroutine, whose result is in its Task.
        if (m_function->is_async) {
            LLVMBuildBr(builder(), m_coroutine_cleanup);
            return;
        }

        if (node.value) {
            LLVMBuildRet(builder(), value_of(*node.value));
        } else {
            LLVMBuildRetVoid(builder());
        }
    }

    void generate(const panic_t& node) {
        // A message with a NUL in it goes as a String, which is shown whole.
        if (node.text || node.message.contains('\0')) {
            LLVMValueRef text = node.text ? value_of(*node.text) : static_string(node.message);
            call(m_panic_string, { text, location(node.span) });
            LLVMBuildUnreachable(builder());
            return;
        }

        panic(node.message, node.span);
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
        // Ptrs, CStrings, and Callbacks compare their addresses.
        if (is_pointer(type) || type == type_id_t::CSTRING || is_callback(type)) {
            return LLVMBuildICmp(builder(), node.op == binary_op_t::EQUAL ? LLVMIntEQ : LLVMIntNE,
                                 left, right, "");
        }

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
        if (node.overflows) {
            return overflows(node.op, type, left, right);
        }

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

    /// Whether integer + - * overflows, from LLVM's intrinsics, or / divides by zero or computes
    /// MIN / -1: a Bool.
    LLVMValueRef overflows(const binary_op_t op, const type_id_t type, LLVMValueRef left,
                           LLVMValueRef right) {
        const bool is_signed_type = is_signed(type);
        LLVMTypeRef llvm = LLVMTypeOf(left);
        if (op == binary_op_t::DIVIDE) {
            LLVMValueRef by_zero =
                LLVMBuildICmp(builder(), LLVMIntEQ, right, LLVMConstNull(llvm), "");
            if (!is_signed_type) {
                return by_zero;
            }

            const unsigned long long min = 1ULL << (bit_width(type) - 1);
            LLVMValueRef is_min =
                LLVMBuildICmp(builder(), LLVMIntEQ, left, LLVMConstInt(llvm, min, 0), "");
            LLVMValueRef is_minus_one =
                LLVMBuildICmp(builder(), LLVMIntEQ, right, LLVMConstAllOnes(llvm), "");
            return LLVMBuildOr(builder(), by_zero,
                               LLVMBuildAnd(builder(), is_min, is_minus_one, ""), "");
        }

        std::string_view intrinsic =
            is_signed_type ? "llvm.smul.with.overflow" : "llvm.umul.with.overflow";
        if (op == binary_op_t::ADD) {
            intrinsic = is_signed_type ? "llvm.sadd.with.overflow" : "llvm.uadd.with.overflow";
        } else if (op == binary_op_t::SUBTRACT) {
            intrinsic = is_signed_type ? "llvm.ssub.with.overflow" : "llvm.usub.with.overflow";
        }

        LLVMValueRef result = call_intrinsic(intrinsic, llvm, { left, right });
        return LLVMBuildExtractValue(builder(), result, 1, "");
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
    /// either way; any other gets the converted value and the flag. A T? as a U? of the same
    /// depth keeps its flag, and in a deeper one it's wrapped, so a U?? holds its Null.
    LLVMValueRef to_nullable(LLVMValueRef value, const type_id_t from, const type_id_t to,
                             const span_t span) {
        if (from == to || is_nullable_reference(to)) {
            return value;
        }

        LLVMValueRef holds = LLVMConstInt(m_bool, 1, 0);
        type_id_t from_inner = from;
        if (is_nullable(from) && nullable_depth(from) == nullable_depth(to)) {
            if (is_nullable_reference(from)) {
                holds = LLVMBuildIsNotNull(builder(), value, "");
            } else {
                holds = LLVMBuildExtractValue(builder(), value, 0, "");
                value = LLVMBuildExtractValue(builder(), value, 1, "");
            }

            from_inner = inner_of(from);
        }

        value = is_nullable(inner_of(to)) ? to_nullable(value, from_inner, inner_of(to), span)
                                          : convert(value, from_inner, inner_of(to), span);
        LLVMValueRef result = LLVMGetUndef(llvm_type(to));
        result = LLVMBuildInsertValue(builder(), result, holds, 0, "");
        return LLVMBuildInsertValue(builder(), result, value, 1, "");
    }

    /// Between numbers, or from a Bool to an integer. An object of a Class is the same
    /// reference as an object of the one it extends, or of an Interface it implements, as an
    /// array is, and an Enum is its integer type.
    LLVMValueRef convert(LLVMValueRef value, const type_id_t from, const type_id_t to,
                         const span_t span) {
        if (is_enum(from) || is_enum(to)) {
            return convert(value, is_enum(from) ? inner_of(from) : from,
                           is_enum(to) ? inner_of(to) : to, span);
        }

        // A Weak<T> is made from the reference, or from null for Null.
        LLVMTypeRef target = llvm_type(to);
        if (from == to || ((is_object(from) || is_array(from)) && is_object(to)) || is_weak(to)) {
            return value;
        }

        // A Ptr, CString, or Callback is an address, which an integer holds.
        const auto is_address = [this](const type_id_t type) {
            return is_pointer(type) || type == type_id_t::CSTRING ||
                   m_mir.types->get(type).kind == type_kind_t::CALLBACK;
        };
        const bool from_address = is_address(from);
        const bool to_address = is_address(to);
        if (from_address && to_address) {
            return value;
        }

        if (from_address) {
            return LLVMBuildPtrToInt(builder(), value, target, "");
        }

        if (to_address) {
            return LLVMBuildIntToPtr(builder(), value, target, "");
        }

        if (from == type_id_t::BOOL) {
            return LLVMBuildZExt(builder(), value, target, "");
        }

        if (from == type_id_t::CHAR) {
            return LLVMBuildIntCast2(builder(), value, target, 0, "");
        }

        if (to == type_id_t::CHAR) {
            return to_char(value, from, span);
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

    /// An integer of type `from` as a Char, which panics at `span` unless it's a Unicode scalar
    /// value: from 0 to 0x10FFFF, but not a surrogate, 0xD800 to 0xDFFF. As an unsigned 64-bit
    /// number, a negative one is past them all.
    LLVMValueRef to_char(LLVMValueRef value, const type_id_t from, const span_t span) {
        LLVMValueRef wide = LLVMBuildIntCast2(builder(), value, m_i64, is_signed(from) ? 1 : 0, "");
        const auto at_most = [&](const std::uint64_t limit) {
            return LLVMBuildICmp(builder(), LLVMIntULE, wide, LLVMConstInt(m_i64, limit, 0), "");
        };
        LLVMValueRef surrogate = LLVMBuildAnd(
            builder(),
            LLVMBuildICmp(builder(), LLVMIntUGE, wide, LLVMConstInt(m_i64, 0xD800, 0), ""),
            at_most(0xDFFF), "");
        LLVMValueRef fits =
            LLVMBuildAnd(builder(), at_most(0x10FFFF), LLVMBuildNot(builder(), surrogate, ""), "");
        panic_if(LLVMBuildNot(builder(), fits, ""),
                 "the value isn't a Unicode scalar value, which a Char holds", span);
        return LLVMBuildTrunc(builder(), wide, m_i32, "");
    }

    LLVMValueRef value(const address_of_t& node) {
        return address_of(node.place);
    }

    /// A Ptr's steps are its T's size, and a count of them is an Int64.
    LLVMValueRef value(const pointer_t& node) {
        const type_id_t type = type_of(node.left);
        LLVMTypeRef element = llvm_type(inner_of(type));
        LLVMValueRef left = value_of(node.left);
        if (is_pointer(type_of(node.right))) {
            return LLVMBuildPtrDiff2(builder(), element, left, value_of(node.right), "");
        }

        LLVMValueRef count = to_int64(node.right);
        if (node.op == binary_op_t::SUBTRACT) {
            count = LLVMBuildNeg(builder(), count, "");
        }

        return LLVMBuildGEP2(builder(), element, left, &count, 1, "");
    }

    /// A call through the Method table goes to the slot's function in the table of the
    /// object's Class.
    LLVMValueRef value(const call_t& node) {
        if (const auto found = m_c_signatures.find(index(node.function));
            found != m_c_signatures.end()) {
            return call_c_structs(m_functions[index(node.function)], found->second, node);
        }

        std::vector<LLVMValueRef> arguments;
        arguments.reserve(node.arguments.size());
        for (const call_argument_t& argument : node.arguments) {
            arguments.push_back(argument.by_ref ? address_of(std::get<place_t>(argument.value))
                                                : value_of(argument.value));
        }

        // A @PanicsAtCaller function takes where its call is, or null where no code calls it. In
        // another one's code, it takes the place that one's caller gives, which is null where
        // the caller has none, so its panics report where they are.
        if (m_mir.functions[index(node.function)].panics_at_caller) {
            LLVMValueRef place = caller_place();
            if (place == nullptr) {
                place = node.place ? own_location(*node.place) : LLVMConstNull(m_ptr);
            }

            arguments.push_back(place);
        }

        function_ref_t function = m_functions[index(node.function)];
        if (node.interface != type_id_t::INVALID && node.slot) {
            function.value = interface_method(arguments[0], node.interface, *node.slot);
        } else if (node.slot) {
            function.value = method_of(arguments[0], *node.slot);
        }

        return call(function, std::move(arguments));
    }

    /// The function in a slot of the table that an object's Class has for an Interface, which
    /// the runtime finds.
    LLVMValueRef interface_method(LLVMValueRef object, const type_id_t interface,
                                  const std::uint32_t slot) {
        LLVMValueRef table = call(m_interface_methods, { object, interface_info(interface) });
        LLVMValueRef position = LLVMConstInt(m_i64, slot, 0);
        LLVMValueRef entry = LLVMBuildGEP2(builder(), m_ptr, table, &position, 1, "");
        return LLVMBuildLoad2(builder(), m_ptr, entry, "");
    }

    /// The function in a slot of an object's Method table, which its Class's info holds.
    LLVMValueRef method_of(LLVMValueRef object, const std::uint32_t slot) {
        LLVMValueRef type_field = LLVMBuildStructGEP2(builder(), m_object_header, object, 1, "");
        LLVMValueRef type = LLVMBuildLoad2(builder(), m_ptr, type_field, "");
        std::array<LLVMValueRef, 3> path{
            LLVMConstInt(m_i32, 0, 0),
            LLVMConstInt(m_i32, methods_field, 0),
            LLVMConstInt(m_i64, slot, 0),
        };
        LLVMValueRef entry = LLVMBuildGEP2(builder(), m_class_header, type, path.data(),
                                           static_cast<unsigned>(path.size()), "");
        return LLVMBuildLoad2(builder(), m_ptr, entry, "");
    }

    /// An Atomic's operation, in one order that every thread sees. A Bool goes in and out of
    /// its byte.
    LLVMValueRef value(const atomic_t& node) {
        const bool flag = inner_of(node.type) == type_id_t::BOOL;
        LLVMTypeRef storage = llvm_type(node.type);
        const auto in = [&](const operand_t& operand) {
            LLVMValueRef given = value_of(operand);
            return flag ? LLVMBuildZExt(builder(), given, storage, "") : given;
        };
        const auto out = [&](LLVMValueRef held) {
            return flag ? LLVMBuildTrunc(builder(), held, m_bool, "") : held;
        };

        if (node.op == atomic_op_t::MAKE || !node.target) {
            return in(node.operands.front());
        }

        constexpr LLVMAtomicOrdering order = LLVMAtomicOrderingSequentiallyConsistent;
        const auto bytes = static_cast<unsigned>(LLVMGetIntTypeWidth(storage) / 8);
        LLVMValueRef address = address_of(*node.target);
        const auto modify = [&](const LLVMAtomicRMWBinOp op) {
            return out(
                LLVMBuildAtomicRMW(builder(), op, address, in(node.operands.front()), order, 0));
        };

        switch (node.op) {
        case atomic_op_t::LOAD: {
            LLVMValueRef loaded = LLVMBuildLoad2(builder(), storage, address, "");
            LLVMSetOrdering(loaded, order);
            LLVMSetAlignment(loaded, bytes);
            return out(loaded);
        }
        case atomic_op_t::STORE: {
            LLVMValueRef stored = LLVMBuildStore(builder(), in(node.operands.front()), address);
            LLVMSetOrdering(stored, order);
            LLVMSetAlignment(stored, bytes);
            return stored;
        }
        case atomic_op_t::ADD:
            return modify(LLVMAtomicRMWBinOpAdd);
        case atomic_op_t::SUBTRACT:
            return modify(LLVMAtomicRMWBinOpSub);
        case atomic_op_t::EXCHANGE:
            return modify(LLVMAtomicRMWBinOpXchg);
        default: {
            LLVMValueRef pair = LLVMBuildAtomicCmpXchg(builder(), address, in(node.operands[0]),
                                                       in(node.operands[1]), order, order, 0);
            return LLVMBuildExtractValue(builder(), pair, 1, "");
        }
        }
    }

    /// A new Channel, of a Class made for its type, or a new Scheduler.
    LLVMValueRef value(const channel_t& node) {
        if (m_mir.types->get(node.type).kind == type_kind_t::SCHEDULER) {
            return call(m_scheduler_new, { scheduler_class(node.type) });
        }

        LLVMValueRef size = LLVMSizeOf(llvm_type(inner_of(node.type)));
        return call(m_channel_new, {
                                       channel_class(node.type),
                                       size,
                                       value_of(node.capacity),
                                       location(node.span),
                                   });
    }

    /// A Channel type's fb_class_t, made the first time it's needed. Threads share its objects.
    LLVMValueRef channel_class(const type_id_t type) {
        if (const auto found = m_classes.find(type); found != m_classes.end()) {
            return found->second;
        }

        const std::string name = m_mir.types->qualified_name(type);
        const array_class_t functions{
            .type = type,
            .release = declare_internal(std::format("fb.{}.release", name), m_void, { m_ptr }),
            .visit =
                declare_internal(std::format("fb.{}.visit", name), m_void, { m_ptr, m_ptr, m_ptr }),
        };
        LLVMValueRef info = class_value(type, LLVMSizeOf(m_channel_type), std::nullopt,
                                        functions.release, functions.visit, std::nullopt,
                                        { LLVMConstNull(m_ptr), LLVMConstInt(m_i64, 0, 0) },
                                        LLVMConstNull(m_ptr), {}, true);
        const std::string global_name = std::format("fb.{}.class", name);
        LLVMValueRef global = LLVMAddGlobal(m_module.get(), LLVMTypeOf(info), global_name.c_str());
        LLVMSetInitializer(global, info);
        LLVMSetLinkage(global, LLVMInternalLinkage);
        m_classes.emplace(type, global);
        m_channel_classes.push_back(functions);
        return global;
    }

    /// The Scheduler's fb_class_t, made the first time it's needed. Its objects stay on the main
    /// thread.
    LLVMValueRef scheduler_class(const type_id_t type) {
        if (const auto found = m_classes.find(type); found != m_classes.end()) {
            return found->second;
        }

        const array_class_t functions{
            .type = type,
            .release = declare_internal("fb.Scheduler.release", m_void, { m_ptr }),
            .visit = declare_internal("fb.Scheduler.visit", m_void, { m_ptr, m_ptr, m_ptr }),
        };
        std::array fields{ m_object_header, m_ptr };
        LLVMTypeRef object = LLVMStructTypeInContext(context(), fields.data(),
                                                     static_cast<unsigned>(fields.size()), 0);
        LLVMValueRef info =
            class_value(type, LLVMSizeOf(object), std::nullopt, functions.release, functions.visit,
                        std::nullopt, { LLVMConstNull(m_ptr), LLVMConstInt(m_i64, 0, 0) },
                        LLVMConstNull(m_ptr), {}, false);
        LLVMValueRef global = LLVMAddGlobal(m_module.get(), LLVMTypeOf(info), "fb.Scheduler.class");
        LLVMSetInitializer(global, info);
        LLVMSetLinkage(global, LLVMInternalLinkage);
        m_classes.emplace(type, global);
        m_scheduler_class = functions;
        return global;
    }

    /// The Scheduler's functions: its release frees what the runtime keeps for it, and its visit
    /// visits nothing, since the runtime holds its Tasks.
    void generate_scheduler_functions() {
        if (!m_scheduler_class) {
            return;
        }

        const array_class_t& functions = *m_scheduler_class;
        LLVMValueRef release = functions.release.value;
        LLVMPositionBuilderAtEnd(builder(),
                                 LLVMAppendBasicBlockInContext(context(), release, "entry"));
        call(m_scheduler_free, { LLVMGetParam(release, 0) });
        LLVMBuildRetVoid(builder());
        LLVMValueRef visit = functions.visit.value;
        LLVMPositionBuilderAtEnd(builder(),
                                 LLVMAppendBasicBlockInContext(context(), visit, "entry"));
        LLVMBuildRetVoid(builder());
    }

    /// Each Channel type's functions: its release releases the elements left, then frees the
    /// ring and the lock, and the runtime frees the object. Its visit goes over the elements left
    /// in the ring, for the leak report, once no thread uses the Channel.
    void generate_channel_functions() {
        for (const array_class_t& functions : m_channel_classes) {
            const type_id_t element = inner_of(functions.type);
            LLVMValueRef release = functions.release.value;
            LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(context(), release, "entry");
            LLVMBasicBlockRef next = LLVMAppendBasicBlockInContext(context(), release, "next");
            LLVMBasicBlockRef one = LLVMAppendBasicBlockInContext(context(), release, "one");
            LLVMBasicBlockRef done = LLVMAppendBasicBlockInContext(context(), release, "done");
            LLVMValueRef channel = LLVMGetParam(release, 0);
            LLVMPositionBuilderAtEnd(builder(), entry);
            LLVMBuildBr(builder(), next);
            LLVMPositionBuilderAtEnd(builder(), next);
            LLVMValueRef left = call(m_channel_take_left, { channel });
            LLVMBuildCondBr(builder(), LLVMBuildIsNull(builder(), left, ""), done, one);
            LLVMPositionBuilderAtEnd(builder(), one);
            if (m_mir.types->has_references(element)) {
                for_each_reference(left, element, false);
            }

            LLVMBuildBr(builder(), next);
            LLVMPositionBuilderAtEnd(builder(), done);
            call(m_channel_free, { channel });
            LLVMBuildRetVoid(builder());

            // The visit goes over the elements in the ring, oldest first, for the leak report,
            // once no thread uses the Channel.
            LLVMValueRef visit = functions.visit.value;
            LLVMPositionBuilderAtEnd(builder(),
                                     LLVMAppendBasicBlockInContext(context(), visit, "entry"));
            if (holds_objects(element)) {
                LLVMValueRef index = LLVMBuildAlloca(builder(), m_i64, "index");
                LLVMBuildStore(builder(), LLVMConstInt(m_i64, 0, 0), index);
                LLVMBasicBlockRef head = LLVMAppendBasicBlockInContext(context(), visit, "head");
                LLVMBasicBlockRef each = LLVMAppendBasicBlockInContext(context(), visit, "each");
                LLVMBasicBlockRef end = LLVMAppendBasicBlockInContext(context(), visit, "end");
                LLVMBuildBr(builder(), head);
                LLVMPositionBuilderAtEnd(builder(), head);
                LLVMValueRef at = LLVMBuildLoad2(builder(), m_i64, index, "");
                LLVMValueRef item = call(m_channel_element, { LLVMGetParam(visit, 0), at });
                LLVMBuildCondBr(builder(), LLVMBuildIsNull(builder(), item, ""), end, each);
                LLVMPositionBuilderAtEnd(builder(), each);
                visit_objects(item, element, visit, LLVMConstNull(m_ptr));
                LLVMBuildStore(builder(),
                               LLVMBuildAdd(builder(), at, LLVMConstInt(m_i64, 1, 0), ""), index);
                LLVMBuildBr(builder(), head);
                LLVMPositionBuilderAtEnd(builder(), end);
            }

            LLVMBuildRetVoid(builder());
        }
    }

    /// Takes or lets go of a Mutex, through the runtime.
    LLVMValueRef value(const lock_t& node) {
        LLVMValueRef address = address_of(node.target);
        if (node.unlock) {
            return call(m_mutex_unlock, { address });
        }

        return call(m_mutex_lock, { address, location(node.span) });
    }

    /// A function value's code is in slot 0 of its object's Method table. It takes the object
    /// first, then the arguments.
    LLVMValueRef value(const invoke_t& node) {
        if (is_callback(type_of(node.callee))) {
            return call_callback(node);
        }

        const type_t& func = m_mir.types->get(type_of(node.callee));
        std::vector<LLVMTypeRef> parameters{ m_ptr };
        std::vector<LLVMValueRef> arguments{ value_of(node.callee) };
        for (std::size_t i = 0; i < node.arguments.size(); ++i) {
            parameters.push_back(llvm_type(func.parameters[i]));
            arguments.push_back(value_of(node.arguments[i]));
        }

        const function_ref_t code{
            .type = LLVMFunctionType(llvm_type(func.inner), parameters.data(),
                                     static_cast<unsigned>(parameters.size()), 0),
            .value = method_of(arguments.front(), 0),
        };
        return call(code, std::move(arguments));
    }

    /// A Callback's call, by the C ABI, through its address.
    LLVMValueRef call_callback(const invoke_t& node) {
        const type_id_t type = type_of(node.callee);
        const type_t& info = m_mir.types->get(type);
        std::vector<LLVMValueRef> arguments;
        arguments.reserve(node.arguments.size());
        for (const operand_t& argument : node.arguments) {
            arguments.push_back(value_of(argument));
        }

        // A debug build panics on a Null Callback, as on a Null Ptr's Value.
        LLVMValueRef callee = value_of(node.callee);
        if (checked()) {
            panic_if(LLVMBuildIsNull(builder(), callee, ""), "the Callback is Null", node.span);
        }

        LLVMValueRef made =
            call({ .type = callback_type(type), .value = callee }, std::move(arguments));
        extend_call_for_c(made, 0, info.inner);
        for (std::size_t i = 0; i < info.parameters.size(); ++i) {
            extend_call_for_c(made, static_cast<unsigned>(i + 1), info.parameters[i]);
        }

        return made;
    }

    LLVMValueRef value(const aggregate_t& node) {
        LLVMValueRef result = LLVMGetUndef(llvm_type(node.type));
        for (std::size_t i = 0; i < node.fields.size(); ++i) {
            result = LLVMBuildInsertValue(builder(), result, value_of(node.fields[i]),
                                          static_cast<unsigned>(i), "");
        }

        return result;
    }

    /// A new T[] holding the elements, or a fixed array's value made of them.
    LLVMValueRef value(const array_t& node) {
        const type_id_t element = inner_of(node.type);
        if (is_fixed_array(node.type)) {
            LLVMValueRef result = LLVMGetUndef(llvm_type(node.type));
            for (std::size_t i = 0; i < node.elements.size(); ++i) {
                result = LLVMBuildInsertValue(builder(), result, value_of(node.elements[i]),
                                              static_cast<unsigned>(i), "");
            }

            return result;
        }

        LLVMValueRef count = LLVMConstInt(m_i64, node.elements.size(), 0);
        LLVMValueRef size = LLVMSizeOf(llvm_type(element));
        LLVMValueRef array = call(m_array_new, { array_class(node.type), size, count });
        for (std::size_t i = 0; i < node.elements.size(); ++i) {
            LLVMValueRef position = LLVMConstInt(m_i64, i, 0);
            LLVMValueRef data = LLVMBuildLoad2(
                builder(), m_ptr, LLVMBuildStructGEP2(builder(), m_array_type, array, 4, ""), "");
            LLVMBuildStore(builder(), value_of(node.elements[i]),
                           LLVMBuildGEP2(builder(), llvm_type(element), data, &position, 1, ""));
        }

        return array;
    }

    LLVMValueRef value(const zero_t& node) {
        return zero_constant(node.type);
    }

    LLVMValueRef value(const new_t& node) {
        return call(m_object_new, { m_classes.at(node.type) });
    }

    LLVMValueRef value(const is_t& node) {
        LLVMValueRef is = call(is_interface(node.type) ? m_object_implements : m_object_is,
                               { value_of(node.value), type_info(node.type) });
        return LLVMBuildICmp(builder(), LLVMIntNE, is, LLVMConstNull(m_i32), "");
    }

    /// A Class's fb_class_t, or an Interface's fb_interface_t.
    LLVMValueRef type_info(const type_id_t type) {
        return is_interface(type) ? interface_info(type) : m_classes.at(type);
    }

    /// The runtime checks the object's Class, and reports the one it found. For 'As?', a wrong
    /// Class gives Null instead, and for 'As T?', Null isn't checked. For an Interface, the
    /// Class has to implement it.
    LLVMValueRef value(const downcast_t& node) {
        LLVMValueRef object = value_of(node.value);
        const bool nullable = is_nullable(node.type);
        const type_id_t target = nullable ? inner_of(node.type) : node.type;
        LLVMValueRef type = type_info(target);
        const function_ref_t& is_function =
            is_interface(target) ? m_object_implements : m_object_is;
        const function_ref_t& expect_function =
            is_interface(target) ? m_object_expect_implements : m_object_expect;
        if (node.or_null) {
            LLVMValueRef is = call(is_function, { object, type });
            LLVMValueRef fits = LLVMBuildICmp(builder(), LLVMIntNE, is, LLVMConstNull(m_i32), "");
            return LLVMBuildSelect(builder(), fits, object, LLVMConstNull(m_ptr), "");
        }

        if (!nullable) {
            call(expect_function, { object, type, location(node.span) });
            return object;
        }

        LLVMBasicBlockRef check =
            LLVMAppendBasicBlockInContext(context(), m_llvm_function, "check");
        LLVMBasicBlockRef done = LLVMAppendBasicBlockInContext(context(), m_llvm_function, "done");
        LLVMValueRef null = LLVMBuildICmp(builder(), LLVMIntEQ, object, LLVMConstNull(m_ptr), "");
        LLVMBuildCondBr(builder(), null, done, check);
        LLVMPositionBuilderAtEnd(builder(), check);
        call(expect_function, { object, type, location(node.span) });
        LLVMBuildBr(builder(), done);
        LLVMPositionBuilderAtEnd(builder(), done);
        return object;
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
        case runtime_function_t::FORMAT_INT:
            return call(m_format_int, { value_of(node.arguments[0]), value_of(node.arguments[1]) });
        case runtime_function_t::FORMAT_UINT:
            return call(m_format_uint,
                        { value_of(node.arguments[0]), value_of(node.arguments[1]) });
        case runtime_function_t::FORMAT_FLOAT:
            return call(m_format_float,
                        {
                            value_of(node.arguments[0]),
                            LLVMBuildZExt(builder(), value_of(node.arguments[1]), m_i32, ""),
                            value_of(node.arguments[2]),
                        });
        case runtime_function_t::DEBUG_STRING:
            return call(m_string_debug, { value_of(node.arguments[0]) });
        case runtime_function_t::DEBUG_CHAR:
            return call(m_char_debug, { value_of(node.arguments[0]) });
        case runtime_function_t::DEBUG_OBJECT:
            return call(m_debug_object,
                        { value_of(node.arguments[0]), value_of(node.arguments[1]) });
        case runtime_function_t::DEBUG_INDENT:
            return call(m_debug_indent, { value_of(node.arguments[0]) });
        case runtime_function_t::FORMAT_TEXT:
            return call(m_format_text,
                        {
                            value_of(node.arguments[0]),
                            value_of(node.arguments[2]),
                            LLVMBuildZExt(builder(), value_of(node.arguments[1]), m_i32, ""),
                        });
        case runtime_function_t::CSTRING_NEW:
            return call(m_cstring_new, { value_of(node.arguments[0]) });
        case runtime_function_t::CSTRING_FREE:
            return call(m_cstring_free, { value_of(node.arguments[0]) });
        case runtime_function_t::STRING_FROM_CSTRING:
            return call(m_string_from_cstring, { value_of(node.arguments[0]) });
        case runtime_function_t::ERROR_UNCAUGHT:
            return call(m_error_uncaught,
                        { value_of(node.arguments[0]), location(node.span.value_or(span_t{})) });
        case runtime_function_t::DEBUG_BUILD:
            return LLVMConstInt(m_bool, checked() ? 1 : 0, 0);
        // A Throw's place is its own, even in a @PanicsAtCaller function's code.
        case runtime_function_t::ERROR_THROWN:
            return call(m_error_thrown, {
                                            value_of(node.arguments[0]),
                                            own_location(node.span.value_or(span_t{})),
                                        });
        case runtime_function_t::WEAK_LOAD:
            return call(m_load_weak, { value_of(node.arguments[0]) });
        case runtime_function_t::OBJECT_DISCARD:
            return call(m_discard, { value_of(node.arguments[0]) });
        case runtime_function_t::COMPARE:
            return call(m_compare, { value_of(node.arguments[0]), value_of(node.arguments[1]) });
        case runtime_function_t::HASH:
            return hash(node.arguments[0]);
        case runtime_function_t::HASH_COMBINE:
            return call(m_hash_combine,
                        { value_of(node.arguments[0]), value_of(node.arguments[1]) });
        case runtime_function_t::STRING_CHARS:
            return call(m_string_chars,
                        { value_of(node.arguments[0]), value_of(node.arguments[1]) });
        case runtime_function_t::STRING_BYTES:
            return call(m_string_bytes,
                        { value_of(node.arguments[0]), value_of(node.arguments[1]) });
        case runtime_function_t::TASK_START:
            return call(m_task_start, { value_of(node.arguments[0]) });
        case runtime_function_t::THREAD_START:
            return call(m_task_start_thread, { value_of(node.arguments[0]) });
        case runtime_function_t::TASK_WAIT:
            return call(m_task_wait, { value_of(node.arguments[0]) });
        case runtime_function_t::TASK_DONE:
            return LLVMBuildICmp(builder(), LLVMIntNE,
                                 call(m_task_done, { value_of(node.arguments[0]) }),
                                 LLVMConstInt(m_i32, 0, 0), "");
        case runtime_function_t::CHANNEL_SEND:
            return call(m_channel_send, {
                                            value_of(node.arguments[0]),
                                            address_of(std::get<place_t>(node.arguments[1])),
                                            location(node.span.value_or(span_t{})),
                                        });
        case runtime_function_t::CHANNEL_TRY_SEND:
        case runtime_function_t::CHANNEL_RECEIVE:
        case runtime_function_t::CHANNEL_TRY_RECEIVE: {
            const function_ref_t* function = &m_channel_try_receive;
            if (node.function == runtime_function_t::CHANNEL_TRY_SEND) {
                function = &m_channel_try_send;
            } else if (node.function == runtime_function_t::CHANNEL_RECEIVE) {
                function = &m_channel_receive;
            }

            LLVMValueRef done =
                call(*function, {
                                    value_of(node.arguments[0]),
                                    address_of(std::get<place_t>(node.arguments[1])),
                                });
            return LLVMBuildICmp(builder(), LLVMIntNE, done, LLVMConstInt(m_i32, 0, 0), "");
        }
        case runtime_function_t::CHANNEL_CLOSE:
            return call(m_channel_close, { value_of(node.arguments[0]) });
        case runtime_function_t::ASYNC_BEGIN:
            return call(m_task_begin, { value_of(node.arguments[0]) });
        case runtime_function_t::ASYNC_FINISH:
            return call(m_task_finish, { value_of(node.arguments[0]) });
        case runtime_function_t::AWAIT:
            return call(m_await, {
                                     m_coroutine,
                                     value_of(node.arguments[1]),
                                     value_of(node.arguments[0]),
                                 });
        case runtime_function_t::WAIT_FRAME:
            return call(m_wait_frame, { value_of(node.arguments[0]) });
        case runtime_function_t::WAIT_TIME:
            return call(m_wait_time,
                        {
                            value_of(node.arguments[0]),
                            value_of(node.arguments[1]),
                            LLVMBuildZExt(builder(), value_of(node.arguments[2]), m_i32, ""),
                        });
        case runtime_function_t::SCHEDULER_RUN:
            return call(m_scheduler_run,
                        { value_of(node.arguments[0]), value_of(node.arguments[1]) });
        case runtime_function_t::SCHEDULER_TICK:
            return call(m_scheduler_tick,
                        { value_of(node.arguments[0]), value_of(node.arguments[1]) });
        case runtime_function_t::SCHEDULER_IDLE:
            return LLVMBuildICmp(builder(), LLVMIntNE,
                                 call(m_scheduler_idle, { value_of(node.arguments[0]) }),
                                 LLVMConstInt(m_i32, 0, 0), "");
        case runtime_function_t::PARALLEL_FOR:
            return call(m_parallel_for, {
                                            value_of(node.arguments[0]),
                                            value_of(node.arguments[1]),
                                            value_of(node.arguments[2]),
                                        });
        default:
            return array_call(node);
        }
    }

    /// A value's hash from the runtime: a String's of its bytes, an object's of which object it
    /// is, a float's of its value, and anything else's of its integer bits.
    LLVMValueRef hash(const operand_t& operand) {
        const type_id_t type = type_of(operand);
        LLVMValueRef value = value_of(operand);
        if (type == type_id_t::STRING) {
            return call(m_hash_string, { value });
        }

        if (is_object(type)) {
            return call(m_hash_object, { value });
        }

        if (is_float(type)) {
            LLVMValueRef wide =
                LLVMBuildFPExt(builder(), value, LLVMDoubleTypeInContext(context()), "");
            return call(m_hash_float, { wide });
        }

        const type_id_t integer = is_enum(type) ? inner_of(type) : type;
        LLVMValueRef bits = is_signed(integer) ? LLVMBuildSExt(builder(), value, m_i64, "")
                                               : LLVMBuildZExt(builder(), value, m_i64, "");
        return call(m_hash_integer, { bits });
    }

    /// A T[]'s operations. The runtime moves the elements around, and this stores, retains, and
    /// releases them. Values are read before the array changes, since they may be its elements.
    LLVMValueRef array_call(const runtime_call_t& node) {
        LLVMValueRef array = value_of(node.arguments[0]);
        const type_id_t element = inner_of(type_of(node.arguments[0]));
        LLVMValueRef size = LLVMSizeOf(llvm_type(element));
        switch (node.function) {
        case runtime_function_t::ARRAY_COUNT:
            return LLVMBuildTrunc(builder(), count_of(array), m_i32, "");
        case runtime_function_t::ARRAY_ADD: {
            LLVMValueRef value = value_of(node.arguments[1]);
            LLVMValueRef where = location(node.span.value_or(span_t{}));
            return LLVMBuildStore(builder(), value, call(m_array_push, { array, size, where }));
        }
        case runtime_function_t::ARRAY_INSERT: {
            LLVMValueRef position = to_int64(node.arguments[1]);
            LLVMValueRef value = value_of(node.arguments[2]);
            LLVMValueRef where = location(node.span.value_or(span_t{}));
            LLVMValueRef slot = call(m_array_insert, { array, size, position, where });
            return LLVMBuildStore(builder(), value, slot);
        }
        case runtime_function_t::ARRAY_REMOVE_AT: {
            LLVMValueRef position = to_int64(node.arguments[1]);
            LLVMValueRef address = array_element(
                array, element, position, type_of(node.arguments[1]), node.span.value_or(span_t{}));
            if (m_mir.types->has_references(element)) {
                for_each_reference(address, element, false);
            }

            return call(m_array_remove, { array, size, position });
        }
        case runtime_function_t::ARRAY_CLEAR:
            for_each_reference_in(array, element, false);
            return call(m_array_clear, { array });
        case runtime_function_t::ARRAY_CLONE: {
            LLVMValueRef copy = call(m_array_clone, { array, size });
            for_each_reference_in(copy, element, true);
            return copy;
        }
        case runtime_function_t::ARRAY_CAPACITY: {
            LLVMValueRef capacity = LLVMBuildLoad2(
                builder(), m_i64, LLVMBuildStructGEP2(builder(), m_array_type, array, 3, ""), "");
            return LLVMBuildTrunc(builder(), capacity, m_i32, "");
        }
        case runtime_function_t::ARRAY_RESERVE: {
            LLVMValueRef capacity = to_int64(node.arguments[1]);
            LLVMValueRef where = location(node.span.value_or(span_t{}));
            return call(m_array_reserve, { array, size, capacity, where });
        }
        case runtime_function_t::ARRAY_TRIM:
            return call(m_array_trim, { array, size });
        default:
            std::unreachable();
        }
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
        case type_id_t::CHAR:
            return call(m_from_char, { value });
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

std::expected<std::string, std::string> emit_ir(const mir_module_t& module,
                                                const source_manager_t& sources,
                                                const file_id_t main,
                                                const codegen_options_t& options) {
    codegen_t codegen(module, sources, sources.get(main), options);
    if (std::expected<void, std::string> built = codegen.build(); !built) {
        return std::unexpected(built.error());
    }

    return codegen.ir();
}

std::expected<void, std::string> emit_object(const mir_module_t& module,
                                             const source_manager_t& sources, const file_id_t main,
                                             const codegen_options_t& options,
                                             const std::filesystem::path& path) {
    codegen_t codegen(module, sources, sources.get(main), options);
    if (std::expected<void, std::string> built = codegen.build(); !built) {
        return built;
    }

    return codegen.write_object(path);
}

} // namespace fb
