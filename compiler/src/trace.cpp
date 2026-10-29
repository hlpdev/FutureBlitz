#include "fb/trace.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <llvm-c/Core.h>
#include <llvm-c/DebugInfo.h>
#include <map>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace fb {
namespace {

/// The table's name, which fill_trace_table() finds it by.
constexpr const char* table_name = "fb.(trace)";
/// A marker's frame when its call has no place, which a trace leaves out, and the parent of a
/// frame that isn't inlined.
constexpr std::uint32_t no_frame = 0xFFFFFFFF;

enum class format_t : std::uint8_t {
    COFF,
    ELF,
    MACHO,
};

format_t format_of(const std::string_view triple) {
    if (triple.contains("windows")) {
        return format_t::COFF;
    }

    return triple.contains("apple") ? format_t::MACHO : format_t::ELF;
}

/// The section the markers go in, as `.pushsection` takes it in inline assembly, which spells a
/// '$' "$$". On Windows the linker puts the parts of ".fbtrace" in the order of what follows
/// their '$', so the table's start is in "$a" and its end in "$z"; ELF's linker gives a section
/// whose name C could spell a start and a stop, and Apple's gives every section one.
std::string_view section_of(const format_t format) {
    switch (format) {
    case format_t::COFF:
        return ".fbtrace$$m,\"dr\"";
    case format_t::ELF:
        return "fb_trace,\"aR\"";
    case format_t::MACHO:
        break;
    }

    return "__TEXT,__fb_trace,regular,no_dead_strip";
}

/// A marker's entry: where the code after it is, from the entry, and its frame's index.
std::string marker(const format_t format, const std::uint32_t frame) {
    return std::format(".pushsection {}\n.long 0f - .\n.long {}\n.popsection\n0:",
                       section_of(format), static_cast<std::int32_t>(frame));
}

/// One place in a trace: a function's name, its file, and the line of the call in it, each name
/// an offset into the table's text, and the frame of the call it's inlined into, or no_frame.
struct frame_t {
    std::uint32_t name = 0;
    std::uint32_t file = 0;
    std::uint32_t line = 0;
    std::uint32_t parent = no_frame;
};

/// The frames that markers point at, and the names they use, each once.
class frames_t {
public:
    explicit frames_t(LLVMContextRef context) : m_context(context) {}

    /// The frame of a call at `location`, after those of the calls it's inlined into.
    std::uint32_t frame(LLVMMetadataRef location) {
        if (const auto found = m_locations.find(location); found != m_locations.end()) {
            return found->second;
        }

        LLVMMetadataRef inlined = LLVMDILocationGetInlinedAt(location);
        const std::uint32_t parent = inlined != nullptr ? frame(inlined) : no_frame;
        LLVMMetadataRef scope = LLVMDILocationGetScope(location);
        unsigned length = 0;
        const char* file = LLVMDIFileGetFilename(LLVMDIScopeGetFile(scope), &length);
        const frame_t made{
            .name = text(name_of(subprogram_of(scope))),
            .file = text(std::string_view(file, length)),
            .line = LLVMDILocationGetLine(location),
            .parent = parent,
        };
        const std::array key{ made.name, made.file, made.line, made.parent };
        const auto [at, added] =
            m_index.try_emplace(key, static_cast<std::uint32_t>(m_frames.size()));
        if (added) {
            m_frames.push_back(made);
        }

        m_locations.emplace(location, at->second);
        return at->second;
    }

    [[nodiscard]] const std::vector<frame_t>& frames() const {
        return m_frames;
    }

    [[nodiscard]] const std::string& strings() const {
        return m_strings;
    }

private:
    LLVMContextRef m_context;
    std::vector<frame_t> m_frames;
    /// Each frame's index, by what it holds, since a line's calls at other columns are one frame.
    std::map<std::array<std::uint32_t, 4>, std::uint32_t> m_index;
    std::unordered_map<LLVMMetadataRef, std::uint32_t> m_locations;
    /// The names, each ending in a NUL, and where each starts.
    std::string m_strings;
    std::unordered_map<std::string, std::uint32_t> m_offsets;

    std::uint32_t text(const std::string_view name) {
        const auto [at, added] =
            m_offsets.try_emplace(std::string(name), static_cast<std::uint32_t>(m_strings.size()));
        if (added) {
            m_strings += name;
            m_strings += '\0';
        }

        return at->second;
    }

    /// A metadata node's operand, or null.
    [[nodiscard]] LLVMMetadataRef operand(LLVMMetadataRef node, const unsigned at) const {
        LLVMValueRef value = LLVMMetadataAsValue(m_context, node);
        const unsigned count = LLVMGetMDNodeNumOperands(value);
        if (at >= count) {
            return nullptr;
        }

        std::vector<LLVMValueRef> operands(count);
        LLVMGetMDNodeOperands(value, operands.data());
        return operands[at] != nullptr ? LLVMValueAsMetadata(operands[at]) : nullptr;
    }

    /// The subprogram a scope is in: itself, or the one a lexical block's scope is in, its
    /// second operand.
    [[nodiscard]] LLVMMetadataRef subprogram_of(LLVMMetadataRef scope) const {
        while (scope != nullptr && LLVMGetMetadataKind(scope) != LLVMDISubprogramMetadataKind) {
            scope = operand(scope, 1);
        }

        return scope;
    }

    /// A subprogram's name, its third operand, which the C API has no call for. The code the
    /// compiler makes itself, whose subprogram is at line 0, has none, which a trace leaves out.
    [[nodiscard]] std::string_view name_of(LLVMMetadataRef subprogram) const {
        if (subprogram == nullptr || LLVMDISubprogramGetLine(subprogram) == 0) {
            return {};
        }

        LLVMMetadataRef name = operand(subprogram, 2);
        if (name == nullptr) {
            return {};
        }

        unsigned length = 0;
        const char* text = LLVMGetMDString(LLVMMetadataAsValue(m_context, name), &length);
        return text != nullptr ? std::string_view(text, length) : std::string_view();
    }
};

/// Whether `call` runs code that can be on the stack under a trace: a call of a function, not
/// of an intrinsic or of inline assembly.
bool is_traced_call(LLVMValueRef call) {
    if (LLVMIsACallInst(call) == nullptr && LLVMIsAInvokeInst(call) == nullptr) {
        return false;
    }

    LLVMValueRef callee = LLVMGetCalledValue(call);
    if (LLVMIsAInlineAsm(callee) != nullptr) {
        return false;
    }

    return LLVMIsAFunction(callee) == nullptr || LLVMGetIntrinsicID(callee) == 0;
}

/// Whether a function has a call that must be a tail call, which the backend can't make a plain
/// one. Compiled code has none, but what LLVM makes could.
bool has_must_tail_call(LLVMValueRef function) {
    for (LLVMBasicBlockRef block = LLVMGetFirstBasicBlock(function); block != nullptr;
         block = LLVMGetNextBasicBlock(block)) {
        for (LLVMValueRef instruction = LLVMGetFirstInstruction(block); instruction != nullptr;
             instruction = LLVMGetNextInstruction(instruction)) {
            if (LLVMIsACallInst(instruction) != nullptr &&
                LLVMGetTailCallKind(instruction) == LLVMTailCallKindMustTail) {
                return true;
            }
        }
    }

    return false;
}

/// The table's start and end: on Windows, entries in the sections around the markers'; on the
/// others, the symbols the linker gives the markers' section.
std::array<LLVMValueRef, 2> bounds(LLVMModuleRef module, const format_t format) {
    LLVMContextRef context = LLVMGetModuleContext(module);
    LLVMTypeRef i32 = LLVMInt32TypeInContext(context);
    if (format == format_t::COFF) {
        std::array<LLVMValueRef, 2> made{};
        constexpr std::array<const char*, 2> sections{ ".fbtrace$a", ".fbtrace$z" };
        for (std::size_t i = 0; i < made.size(); ++i) {
            std::array<LLVMValueRef, 2> entry{
                LLVMConstInt(i32, 0, 0),
                LLVMConstInt(i32, no_frame, 0),
            };
            LLVMValueRef value = LLVMConstArray2(i32, entry.data(), entry.size());
            made[i] = LLVMAddGlobal(module, LLVMTypeOf(value),
                                    i == 0 ? "fb.(trace start)" : "fb.(trace end)");
            LLVMSetInitializer(made[i], value);
            LLVMSetGlobalConstant(made[i], 1);
            LLVMSetLinkage(made[i], LLVMPrivateLinkage);
            LLVMSetSection(made[i], sections[i]);
            LLVMSetAlignment(made[i], 4);
        }

        return made;
    }

    // An entry that marks nothing, so the section is there in a binary without calls.
    const std::string empty =
        std::format(".pushsection {}\n.long 0\n.long -1\n.popsection\n", section_of(format));
    LLVMAppendModuleInlineAsm(module, empty.data(), empty.size());
    const std::array<std::string, 2> names =
        format == format_t::ELF
            ? std::array<std::string, 2>{ "__start_fb_trace", "__stop_fb_trace" }
            : std::array<std::string, 2>{
                  "\x01section$start$__TEXT$__fb_trace",
                  "\x01section$end$__TEXT$__fb_trace",
              };
    std::array<LLVMValueRef, 2> found{};
    for (std::size_t i = 0; i < found.size(); ++i) {
        found[i] = LLVMAddGlobal(module, LLVMInt8TypeInContext(context), names[i].c_str());
        LLVMSetVisibility(found[i], LLVMHiddenVisibility);
    }

    return found;
}

/// On ELF, puts each function llvm.global_ctors runs as the binary loads in .init_array, which
/// the C runtime runs. LLVM's C API makes a target that writes them in .ctors, which lld keeps
/// as a section of its own, and nothing runs.
void use_init_array(LLVMModuleRef module) {
    LLVMValueRef ctors = LLVMGetNamedGlobal(module, "llvm.global_ctors");
    if (ctors == nullptr) {
        return;
    }

    LLVMContextRef context = LLVMGetModuleContext(module);
    LLVMTypeRef pointer = LLVMPointerTypeInContext(context, 0);
    std::vector<LLVMValueRef> kept;
    LLVMValueRef list = LLVMGetInitializer(ctors);
    for (int i = 0; list != nullptr && i < LLVMGetNumOperands(list); ++i) {
        LLVMValueRef entry = LLVMGetOperand(list, static_cast<unsigned>(i));
        const auto priority =
            static_cast<unsigned>(LLVMConstIntGetZExtValue(LLVMGetOperand(entry, 0)));
        // A priority's section, as LLVM names it, which the linker sorts.
        const std::string section =
            priority == 65535 ? ".init_array" : std::format(".init_array.{:05}", priority);
        LLVMValueRef run = LLVMAddGlobal(module, pointer, "fb.(runs at load)");
        LLVMSetInitializer(run, LLVMGetOperand(entry, 1));
        LLVMSetLinkage(run, LLVMPrivateLinkage);
        LLVMSetSection(run, section.c_str());
        LLVMSetAlignment(run, 8);
        kept.push_back(run);
    }

    LLVMDeleteGlobal(ctors);
    // Kept, though nothing refers to them.
    LLVMValueRef used = LLVMGetNamedGlobal(module, "llvm.compiler.used");
    if (used != nullptr) {
        LLVMValueRef existing = LLVMGetInitializer(used);
        for (int i = 0; existing != nullptr && i < LLVMGetNumOperands(existing); ++i) {
            kept.push_back(LLVMGetOperand(existing, static_cast<unsigned>(i)));
        }

        LLVMDeleteGlobal(used);
    }

    LLVMValueRef all = LLVMConstArray2(pointer, kept.data(), kept.size());
    used = LLVMAddGlobal(module, LLVMTypeOf(all), "llvm.compiler.used");
    LLVMSetInitializer(used, all);
    LLVMSetLinkage(used, LLVMAppendingLinkage);
    LLVMSetSection(used, "llvm.metadata");
}

/// The table's type: its markers' start and end, its frames, and their names.
LLVMTypeRef table_type(LLVMContextRef context) {
    LLVMTypeRef pointer = LLVMPointerTypeInContext(context, 0);
    std::array<LLVMTypeRef, 4> fields{ pointer, pointer, pointer, pointer };
    return LLVMStructTypeInContext(context, fields.data(), static_cast<unsigned>(fields.size()), 0);
}

} // namespace

void keep_frames(LLVMModuleRef module) {
    LLVMContextRef context = LLVMGetModuleContext(module);
    constexpr std::string_view uwtable = "uwtable";
    const unsigned uwtable_kind = LLVMGetEnumAttributeKindForName(uwtable.data(), uwtable.size());
    LLVMAttributeRef unwinds = LLVMCreateEnumAttribute(context, uwtable_kind, 2);
    constexpr std::string_view no_tail_calls = "disable-tail-calls";
    constexpr std::string_view yes = "true";
    LLVMAttributeRef stays = LLVMCreateStringAttribute(
        context, no_tail_calls.data(), static_cast<unsigned>(no_tail_calls.size()), yes.data(),
        static_cast<unsigned>(yes.size()));
    // Apple's ABI has every function that calls keep a frame record that x29, or rbp, points
    // at, as clang's do, which its tools walk the stack by.
    constexpr std::string_view frame_pointer = "frame-pointer";
    constexpr std::string_view non_leaf = "non-leaf";
    LLVMAttributeRef framed = LLVMCreateStringAttribute(
        context, frame_pointer.data(), static_cast<unsigned>(frame_pointer.size()), non_leaf.data(),
        static_cast<unsigned>(non_leaf.size()));
    const char* triple = LLVMGetTarget(module);
    const bool apple = triple != nullptr && std::string_view(triple).contains("apple");
    const auto at_function = static_cast<LLVMAttributeIndex>(LLVMAttributeFunctionIndex);
    for (LLVMValueRef function = LLVMGetFirstFunction(module); function != nullptr;
         function = LLVMGetNextFunction(function)) {
        if (LLVMIsDeclaration(function) != 0) {
            continue;
        }

        LLVMAddAttributeAtIndex(function, at_function, unwinds);
        if (apple) {
            LLVMAddAttributeAtIndex(function, at_function, framed);
        }
        if (!has_must_tail_call(function)) {
            LLVMAddAttributeAtIndex(function, at_function, stays);
        }
    }
}

void add_trace_table(LLVMModuleRef module) {
    LLVMContextRef context = LLVMGetModuleContext(module);
    LLVMTypeRef pointer = LLVMPointerTypeInContext(context, 0);
    LLVMTypeRef type = table_type(context);
    LLVMValueRef table = LLVMAddGlobal(module, type, table_name);
    LLVMSetInitializer(table, LLVMConstNull(type));
    LLVMSetLinkage(table, LLVMInternalLinkage);

    // The runtime's fb_trace_add(), which a library beside the executable takes from it.
    LLVMTypeRef add_type = LLVMFunctionType(LLVMVoidTypeInContext(context), &pointer, 1, 0);
    LLVMValueRef add = LLVMGetNamedFunction(module, "fb_trace_add");
    if (add == nullptr) {
        add = LLVMAddFunction(module, "fb_trace_add", add_type);
    }

    LLVMTypeRef adds_type = LLVMFunctionType(LLVMVoidTypeInContext(context), nullptr, 0, 0);
    LLVMValueRef adds = LLVMAddFunction(module, "fb.(add trace)", adds_type);
    LLVMSetLinkage(adds, LLVMInternalLinkage);
    LLVMBuilderRef builder = LLVMCreateBuilderInContext(context);
    LLVMPositionBuilderAtEnd(builder, LLVMAppendBasicBlockInContext(context, adds, "entry"));
    LLVMBuildCall2(builder, add_type, add, &table, 1, "");
    LLVMBuildRetVoid(builder);
    LLVMDisposeBuilder(builder);

    // It runs as the binary loads, before main, or as the executable loads a library beside it.
    LLVMTypeRef i32 = LLVMInt32TypeInContext(context);
    std::array<LLVMTypeRef, 3> entry_fields{ i32, pointer, pointer };
    LLVMTypeRef entry_type = LLVMStructTypeInContext(context, entry_fields.data(),
                                                     static_cast<unsigned>(entry_fields.size()), 0);
    std::vector<LLVMValueRef> entries;
    LLVMValueRef existing = LLVMGetNamedGlobal(module, "llvm.global_ctors");
    if (existing != nullptr) {
        LLVMValueRef initializer = LLVMGetInitializer(existing);
        for (int i = 0; initializer != nullptr && i < LLVMGetNumOperands(initializer); ++i) {
            entries.push_back(LLVMGetOperand(initializer, static_cast<unsigned>(i)));
        }

        LLVMDeleteGlobal(existing);
    }

    std::array<LLVMValueRef, 3> entry{ LLVMConstInt(i32, 65535, 0), adds, LLVMConstNull(pointer) };
    entries.push_back(
        LLVMConstStructInContext(context, entry.data(), static_cast<unsigned>(entry.size()), 0));
    LLVMValueRef list = LLVMConstArray2(entry_type, entries.data(), entries.size());
    LLVMValueRef ctors = LLVMAddGlobal(module, LLVMTypeOf(list), "llvm.global_ctors");
    LLVMSetInitializer(ctors, list);
    LLVMSetLinkage(ctors, LLVMAppendingLinkage);
}

void fill_trace_table(LLVMModuleRef module, const std::string_view triple) {
    LLVMValueRef table = LLVMGetNamedGlobal(module, table_name);
    if (table == nullptr) {
        return;
    }

    LLVMContextRef context = LLVMGetModuleContext(module);
    const format_t format = format_of(triple);
    LLVMTypeRef asm_type = LLVMFunctionType(LLVMVoidTypeInContext(context), nullptr, 0, 0);
    // What optimizing made keeps the frames too.
    keep_frames(module);
    frames_t frames(context);
    LLVMBuilderRef builder = LLVMCreateBuilderInContext(context);
    for (LLVMValueRef function = LLVMGetFirstFunction(module); function != nullptr;
         function = LLVMGetNextFunction(function)) {
        if (LLVMIsDeclaration(function) != 0) {
            continue;
        }

        const bool described = LLVMGetSubprogram(function) != nullptr;
        for (LLVMBasicBlockRef block = LLVMGetFirstBasicBlock(function); block != nullptr;
             block = LLVMGetNextBasicBlock(block)) {
            for (LLVMValueRef instruction = LLVMGetFirstInstruction(block); instruction != nullptr;
                 instruction = LLVMGetNextInstruction(instruction)) {
                if (!is_traced_call(instruction)) {
                    continue;
                }

                LLVMMetadataRef location =
                    described ? LLVMInstructionGetDebugLoc(instruction) : nullptr;
                const std::string text =
                    marker(format, location != nullptr ? frames.frame(location) : no_frame);
                LLVMValueRef code = LLVMGetInlineAsm(asm_type, text.data(), text.size(), "", 0, 1,
                                                     0, LLVMInlineAsmDialectATT, 0);
                // With no place of its own: a line's row in the line table starts at the code
                // that has it, and the marker's entry would take the row instead.
                LLVMPositionBuilderBefore(builder, instruction);
                LLVMInstructionSetDebugLoc(LLVMBuildCall2(builder, asm_type, code, nullptr, 0, ""),
                                           nullptr);
            }
        }
    }

    LLVMDisposeBuilder(builder);

    // The frames, then their names.
    LLVMTypeRef i32 = LLVMInt32TypeInContext(context);
    std::array<LLVMTypeRef, 4> frame_fields{ i32, i32, i32, i32 };
    LLVMTypeRef frame_type = LLVMStructTypeInContext(context, frame_fields.data(),
                                                     static_cast<unsigned>(frame_fields.size()), 0);
    std::vector<LLVMValueRef> made;
    made.reserve(frames.frames().size());
    for (const frame_t& frame : frames.frames()) {
        std::array<LLVMValueRef, 4> fields{
            LLVMConstInt(i32, frame.name, 0),
            LLVMConstInt(i32, frame.file, 0),
            LLVMConstInt(i32, frame.line, 0),
            LLVMConstInt(i32, frame.parent, 0),
        };
        made.push_back(LLVMConstStructInContext(context, fields.data(),
                                                static_cast<unsigned>(fields.size()), 0));
    }

    LLVMValueRef frame_list = LLVMConstArray2(frame_type, made.data(), made.size());
    LLVMValueRef frames_global = LLVMAddGlobal(module, LLVMTypeOf(frame_list), "fb.(trace frames)");
    LLVMSetInitializer(frames_global, frame_list);
    LLVMSetGlobalConstant(frames_global, 1);
    LLVMSetLinkage(frames_global, LLVMPrivateLinkage);
    const std::string& strings = frames.strings();
    LLVMValueRef text = LLVMConstStringInContext2(context, strings.data(), strings.size(), 0);
    LLVMValueRef text_global = LLVMAddGlobal(module, LLVMTypeOf(text), "fb.(trace names)");
    LLVMSetInitializer(text_global, text);
    LLVMSetGlobalConstant(text_global, 1);
    LLVMSetLinkage(text_global, LLVMPrivateLinkage);

    const std::array<LLVMValueRef, 2> ends = bounds(module, format);
    std::array<LLVMValueRef, 4> fields{ ends[0], ends[1], frames_global, text_global };
    LLVMSetInitializer(table, LLVMConstStructInContext(context, fields.data(),
                                                       static_cast<unsigned>(fields.size()), 0));
    LLVMSetGlobalConstant(table, 1);
    if (format == format_t::ELF) {
        use_init_array(module);
    }
}

} // namespace fb
