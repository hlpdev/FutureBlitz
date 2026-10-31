// fb-lsp's benchmark, docs/tooling.md's M11 step 0: how long each request takes on a large
// package, from the edit, at the 50th and 95th percentile, against M11's budgets. The whole one is
// hidden, as it takes minutes on a release build: `fb_lsp_tests "[bench]"` runs it, with
// FB_LSP_BENCH_REPORT naming a folder for its report, lsp-bench.md and lsp-bench.json, and
// FB_LSP_BENCH_SAMPLES how many times each thing is timed. A missed budget fails it only with
// FB_LSP_BENCH_ENFORCE set, as CI's lsp-bench job has it, since a PC busy with builds misses some.
// ctest runs a small one, which checks that each measurement works.

#include "client.hpp"
#include "fb/build_config.hpp"
#include "fb/lsp/protocol.hpp"
#include "fb/lsp/workspace.hpp"
#include "fb/utf8.hpp"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

using fb::lsp::json_t;
using fb::lsp::test::client_t;
using fb::lsp::test::folder_t;

namespace {

using steady_t = std::chrono::steady_clock;
using milliseconds_t = std::chrono::duration<double, std::milli>;

/// One thing the benchmark times, with its budget, which the 50th percentile is in and the 95th
/// in one and a half times, to meet it.
struct metric_t {
    std::string name;
    std::string what;
    std::chrono::milliseconds budget{};
    /// Whether a miss fails the benchmark: for what met its budget when the benchmark came, and
    /// for the rest once the step of M11 that's for it meets it.
    bool enforced = false;
    /// Each time, in milliseconds.
    std::vector<double> samples;
};

/// The metrics, in the report's order, with docs/tooling.md's budgets.
std::vector<metric_t> metrics() {
    using std::chrono::milliseconds;
    return {
        {
            .name = "syntax_error",
            .what = "a syntax error shown, from the keystroke",
            .budget = milliseconds(50),
            .enforced = true,
        },
        {
            .name = "type_error",
            .what = "a type error shown, from the last keystroke",
            .budget = milliseconds(300),
            .enforced = true,
        },
        {
            .name = "completion_member",
            .what = "completion after `.`, from the keystroke",
            .budget = milliseconds(100),
            .enforced = true,
        },
        {
            .name = "completion_word",
            .what = "completion on a word being typed, from the keystroke",
            .budget = milliseconds(100),
            .enforced = true,
        },
        {
            .name = "signature_help",
            .what = "signature help after `(`, from the keystroke",
            .budget = milliseconds(100),
            .enforced = true,
        },
        {
            .name = "hover",
            .what = "hover over a name",
            .budget = milliseconds(100),
            .enforced = true,
        },
        {
            .name = "code_action",
            .what = "the quick fixes at an error",
            .budget = milliseconds(100),
            .enforced = true,
        },
        {
            .name = "references",
            .what = "find references across the package",
            .budget = milliseconds(1000),
            .enforced = true,
        },
        {
            .name = "rename",
            .what = "a rename across the package",
            .budget = milliseconds(1000),
            .enforced = true,
        },
    };
}

/// The value at the `fraction` percentile of `samples`, by nearest rank.
double percentile(std::vector<double> samples, const double fraction) {
    if (samples.empty()) {
        return 0;
    }

    std::ranges::sort(samples);
    const auto rank =
        static_cast<std::size_t>(std::ceil(fraction * static_cast<double>(samples.size())));
    return samples[std::clamp<std::size_t>(rank, 1, samples.size()) - 1];
}

/// Whether a metric's times meet its budget: the 50th percentile in it, and the 95th in one and a
/// half times it.
bool met(const metric_t& metric) {
    const auto budget = static_cast<double>(metric.budget.count());
    return percentile(metric.samples, 0.5) <= budget &&
           percentile(metric.samples, 0.95) <= budget * 1.5;
}

/// What the benchmark runs on: a folder the client opens, the file in it that's edited, and
/// where in that file it types and asks, each a piece of its text it finds.
struct workload_t {
    std::string name;
    std::filesystem::path root;
    std::filesystem::path file;
    /// A line of a Function's body, before which lines are typed, with its indentation.
    std::string line;
    std::string indent;
    /// A Local of that Function, for a member's completion, and a Function to call.
    std::string local;
    std::string function;
    /// A name to hover over, declared in the file and used across the package, which is found
    /// and renamed.
    std::string symbol;
    /// How many .fb files the folder has, and their lines.
    std::size_t files = 0;
    std::size_t lines = 0;
    /// How long the client's first check of the whole folder took.
    double initial = 0;
    /// Where an edit's time goes: how long each check of the edited file alone took, with what
    /// it imports, and how many modules that was.
    std::vector<double> checks;
    std::size_t modules = 0;
    std::vector<metric_t> metrics;
};

/// How many layers and modules a layer the generated program has, and how many Functions of
/// work each module has.
struct shape_t {
    std::size_t layers = 10;
    std::size_t width = 30;
    std::size_t works = 6;
};

/// A module of the generated program: `layer`'s `index`th, which imports three modules of the
/// layer below and calls their Functions.
std::string module_text(const shape_t& shape, const std::size_t layer, const std::size_t index) {
    const std::string own = std::format("{}_{}", layer, index);
    std::vector<std::size_t> below;
    if (layer > 0) {
        for (const std::size_t other : { index, index + 1, (index * 7) + 3 }) {
            const std::size_t module = other % shape.width;
            if (std::ranges::find(below, module) == below.end()) {
                below.push_back(module);
            }
        }
    }

    std::ostringstream out;
    out << std::format("//! The generated program's module {}, which fb-lsp's benchmark edits.\n\n",
                       own);
    out << "Import std.core\nImport std.collections\n";
    for (const std::size_t module : below) {
        out << std::format("Import bench.l{}.m{}\n", layer - 1, module);
    }

    out << std::format(R"(
/// A point of module {0}.
Public Struct Point{0}
    Public Field X: Int
    Public Field Y: Int

    /// Its length, squared.
    Public Method Squared(): Int
        Return X * X + Y * Y
    End Method
End Struct

/// How big a number is.
Public Enum Size{0}
    SMALL, MEDIUM, LARGE
End Enum

/// Module {0}'s points, and their total.
Public Class Store{0}
    Field Points: Point{0}[] = []
    Field Total: Int

    /// `count` points along a line.
    Public New(count: Int)
        For n = 0 Until count
            Add(Point{0}(n, n * 2))
        End For
    End New

    /// How many points it has.
    Public Property Count: Int
        Get => Points.Count
    End Property

    /// Adds `point`, and its length to the total.
    Public Method Add(point: Point{0})
        Points.Add(point)
        Total += point.Squared()
    End Method

    /// How big `n` is.
    Public Method SizeOf(n: Int): Size{0}
        If n < 10 Then Return Size{0}.SMALL
        If n < 100 Then Return Size{0}.MEDIUM
        Return Size{0}.LARGE
    End Method

    /// The sum of every point's coordinates.
    Public Method Sum(): Int
        Local sum = 0
        For point In Points
            sum += point.X + point.Y
        End For
        Return sum
    End Method
End Class

/// A point from `n`, from the layer below's.
Public Function Make{0}(n: Int): Point{0}
)",
                       own);
    if (below.empty()) {
        out << std::format("    Return Point{}(n, n + 1)\n", own);
    } else {
        out << std::format("    Local below = Make{}_{}(n)\n", layer - 1, below.front());
        out << std::format("    Return Point{}(below.X + n, below.Y)\n", own);
    }

    out << "End Function\n";
    for (std::size_t work = 0; work < shape.works; ++work) {
        out << std::format(R"(
/// Step {1} of module {0}'s work.
Public Function Work{0}_{1}(store: Store{0}, n: Int): Int
    Local total = 0
    For step = 0 Until n
        Local p = Make{0}(step + {1})
        store.Add(p)
        If store.SizeOf(step) == Size{0}.LARGE Then Continue
        total += p.Squared() + store.Count
    End For
)",
                           own, work);
        for (const std::size_t module : below) {
            out << std::format("    total += Make{}_{}(n).Squared()\n", layer - 1, module);
        }

        out << "    Return total\nEnd Function\n";
    }

    return out.str();
}

/// The generated program: `shape`'s layers of modules, each importing some of the layer below's,
/// and a main file that imports the top layer's.
std::vector<std::pair<std::string, std::string>> program(const shape_t& shape) {
    std::vector<std::pair<std::string, std::string>> files{
        { "project.toml", "[project]\nname = \"bench\"\nversion = \"0.1.0\"\n" },
    };
    std::string main = "Import std.core\n";
    for (std::size_t layer = 0; layer < shape.layers; ++layer) {
        for (std::size_t index = 0; index < shape.width; ++index) {
            files.emplace_back(std::format("l{}/m{}.fb", layer, index),
                               module_text(shape, layer, index));
        }
    }

    const std::size_t top = shape.layers - 1;
    for (std::size_t index = 0; index < shape.width; ++index) {
        main += std::format("Import bench.l{}.m{}\n", top, index);
    }

    main += "\nLocal total = 0\n";
    for (std::size_t index = 0; index < shape.width; ++index) {
        main += std::format("total += Make{}_{}({}).Squared()\n", top, index, index);
    }

    main += "Print(\"{total}\")\n";
    files.emplace_back("main.fb", main);
    return files;
}

/// The workload of the generated program: a module in its middle layer, which the layers above
/// import, directly or not.
workload_t program_workload(const folder_t& folder, const shape_t& shape) {
    const std::string own = std::format("{}_{}", shape.layers / 2, shape.width / 2);
    return {
        .name = std::format("a program of {} modules", shape.layers * shape.width),
        .root = folder.root(),
        .file = folder.path(std::format("l{}/m{}.fb", shape.layers / 2, shape.width / 2)),
        .line = "    Return total\n",
        .indent = "    ",
        .local = "store",
        .function = "Make" + own,
        .symbol = "Make" + own,
    };
}

/// The workload of std's own folder, editing std.text.fuzzy.
workload_t std_workload() {
    const std::filesystem::path root = fb::path_from_utf8(fb::build_config::std_directory);
    return {
        .name = "std",
        .root = root,
        .file = root / "text" / "fuzzy.fb",
        .line = "        Return Best(wanted, folded, chars, firsts)\n",
        .indent = "        ",
        .local = "chars",
        .function = "Best",
        .symbol = "Best",
    };
}

/// A file's text.
std::string read(const std::filesystem::path& path) {
    const std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

/// How many .fb files a folder has, and their lines.
std::pair<std::size_t, std::size_t> size_of(const std::filesystem::path& root) {
    std::size_t files = 0;
    std::size_t lines = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        if (entry.is_regular_file() && entry.path().extension() == ".fb") {
            ++files;
            lines += static_cast<std::size_t>(std::ranges::count(read(entry.path()), '\n'));
        }
    }

    return { files, lines };
}

/// The file's text with `typed` typed on a line of its own before the workload's line, and the
/// offset of the typed line's end in it.
std::pair<std::string, std::size_t> typing(const std::string& text, const workload_t& workload,
                                           const std::string& typed) {
    std::string typed_text = text;
    const std::size_t line = typed_text.find(workload.line);
    REQUIRE(line != std::string::npos);
    const std::string inserted = workload.indent + typed + "\n";
    typed_text.insert(line, inserted);
    return { typed_text, line + inserted.size() - 1 };
}

/// textDocument and position params at `offset` in `text`.
json_t at_offset(const std::string& uri, const std::string& text, const std::size_t offset) {
    const std::string_view before = std::string_view(text).substr(0, offset);
    const std::size_t line_start = before.rfind('\n') + 1;
    const json_t position =
        client_t::position(static_cast<std::uint32_t>(std::ranges::count(before, '\n')),
                           static_cast<std::uint32_t>(offset - line_start));
    return { { "textDocument", { { "uri", uri } } }, { "position", position } };
}

/// Times one request, in milliseconds, which must have a result.
template <typename Ask>
double timed(Ask&& ask) {
    const steady_t::time_point start = steady_t::now();
    std::forward<Ask>(ask)();
    return milliseconds_t(steady_t::now() - start).count();
}

/// Runs the workload's measurements, `samples` times each, the editor's delays `delay` and
/// `dependents_delay`.
void measure(workload_t& workload, const std::size_t samples, const std::chrono::milliseconds delay,
             const std::chrono::milliseconds dependents_delay) {
    std::tie(workload.files, workload.lines) = size_of(workload.root);
    workload.metrics = metrics();
    const auto metric = [&](const std::string_view name) -> metric_t& {
        return *std::ranges::find(workload.metrics, name, &metric_t::name);
    };

    const steady_t::time_point start = steady_t::now();
    // As VS Code, which watches the files.
    client_t client({
        .folders = { workload.root },
        .delay = delay,
        .dependents_delay = dependents_delay,
        .watches = true,
    });
    client.settle();
    workload.initial = milliseconds_t(steady_t::now() - start).count();

    const std::string uri = fb::lsp::uri_of(workload.file);
    const std::string text = read(workload.file);
    client.open(uri, text);
    client.settle();
    INFO(client.diagnostics(uri).dump());
    REQUIRE(client.diagnostics(uri).empty());

    // An edit's diagnostics, from its didChange to their publication, then the file as it was,
    // with every check of it and of what imports it done before the next.
    const auto shown = [&](const std::string& typed) {
        const std::string broken = typing(text, workload, typed).first;
        const steady_t::time_point edited = steady_t::now();
        client.change(uri, broken);
        const json_t diagnostics = client.published(uri);
        const double took = milliseconds_t(steady_t::now() - edited).count();
        INFO(typed);
        CHECK(!diagnostics.empty());
        client.change(uri, text);
        client.settle();
        return took;
    };

    // A request right after the keystroke that makes it, from that didChange to its response.
    const auto typed_request = [&](const std::string& typed, const std::string& method) {
        const auto [typed_text, end] = typing(text, workload, typed);
        json_t result;
        const double took = timed([&] {
            client.change(uri, typed_text);
            result = client.request(method, at_offset(uri, typed_text, end));
        });
        INFO(method << " after " << typed << ": " << result.dump());
        CHECK(!result.is_null());
        client.change(uri, text);
        client.settle();
        return took;
    };

    for (std::size_t sample = 0; sample < samples; ++sample) {
        metric("syntax_error").samples.push_back(shown("Local wrong = (1 +"));
        metric("type_error").samples.push_back(shown("Local wrong: Int = \"text\""));
        metric("completion_member")
            .samples.push_back(typed_request(workload.local + ".", "textDocument/completion"));
        metric("completion_word")
            .samples.push_back(
                typed_request(workload.local.substr(0, 2), "textDocument/completion"));
        metric("signature_help")
            .samples.push_back(
                typed_request(workload.function + "(", "textDocument/signatureHelp"));
    }

    // The rest, at rest: the file as it was, and every check done.
    const std::size_t use = text.find(workload.symbol + "(");
    REQUIRE(use != std::string::npos);
    const json_t name_at = at_offset(uri, text, use + 1);
    const std::string broken = typing(text, workload, "Local wrong: Int = \"text\"").first;
    for (std::size_t sample = 0; sample < samples; ++sample) {
        metric("hover").samples.push_back(
            timed([&] { CHECK(client.request("textDocument/hover", name_at).is_object()); }));

        json_t references_params = name_at;
        references_params["context"] = { { "includeDeclaration", true } };
        metric("references").samples.push_back(timed([&] {
            CHECK(!client.request("textDocument/references", references_params).empty());
        }));

        json_t rename_params = name_at;
        rename_params["newName"] = workload.symbol + "Renamed";
        metric("rename").samples.push_back(timed(
            [&] { CHECK(client.request("textDocument/rename", rename_params).is_object()); }));
    }

    // The quick fixes at an error, once it's shown.
    client.change(uri, broken);
    const json_t diagnostics = client.published(uri);
    REQUIRE(!diagnostics.empty());
    const json_t actions_params{
        { "textDocument", { { "uri", uri } } },
        { "range", diagnostics[0]["range"] },
        { "context", { { "diagnostics", diagnostics } } },
    };
    for (std::size_t sample = 0; sample < samples; ++sample) {
        metric("code_action").samples.push_back(timed([&] {
            CHECK(client.request("textDocument/codeAction", actions_params).is_array());
        }));
    }

    client.change(uri, text);
    client.settle();
    const std::string key = fb::lsp::file_key(workload.file);
    for (const fb::lsp::check_record_t& check : client.server().workspace().history()) {
        if (check.roots.size() == 1 && fb::lsp::file_key(check.roots.front()) == key) {
            workload.checks.push_back(static_cast<double>(check.took.count()));
            workload.modules = check.modules;
        }
    }
}

/// The report, in Markdown.
std::string markdown(const std::vector<workload_t>& workloads) {
    std::string out = "# fb-lsp's benchmark\n\nEach time at the 50th and 95th percentile, in "
                      "milliseconds, against docs/tooling.md's M11 budgets, each met with the "
                      "50th in it and the 95th in one and a half times it.\n";
    for (const workload_t& workload : workloads) {
        out += std::format("\n## {}\n\n{} files, {} lines; the first check of the whole folder "
                           "took {:.0f} ms. The edited file is {}: a check of it alone, with "
                           "the {} modules it has with what it imports, std's among them, took "
                           "{:.0f} ms at the 50th percentile and {:.0f} ms at the 95th, over {} "
                           "checks.\n\n",
                           workload.name, workload.files, workload.lines, workload.initial,
                           fb::to_utf8(workload.file.lexically_relative(workload.root)),
                           workload.modules, percentile(workload.checks, 0.5),
                           percentile(workload.checks, 0.95), workload.checks.size());
        out += "| What | p50 | p95 | Budget | |\n|---|---|---|---|---|\n";
        for (const metric_t& metric : workload.metrics) {
            // A miss that fails the benchmark stands out.
            std::string_view status = "met";
            if (!met(metric)) {
                status = metric.enforced ? "**missed**" : "missed";
            }

            out += std::format("| {} | {:.1f} | {:.1f} | {} | {} |\n", metric.what,
                               percentile(metric.samples, 0.5), percentile(metric.samples, 0.95),
                               metric.budget.count(), status);
        }
    }

    return out;
}

/// The report, in JSON.
json_t report_json(const std::vector<workload_t>& workloads) {
    json_t out = json_t::array();
    for (const workload_t& workload : workloads) {
        json_t measured = json_t::array();
        for (const metric_t& metric : workload.metrics) {
            measured.push_back({
                { "name", metric.name },
                { "budget_ms", metric.budget.count() },
                { "enforced", metric.enforced },
                { "p50_ms", percentile(metric.samples, 0.5) },
                { "p95_ms", percentile(metric.samples, 0.95) },
                { "met", met(metric) },
                { "samples_ms", metric.samples },
            });
        }

        out.push_back({
            { "name", workload.name },
            { "files", workload.files },
            { "lines", workload.lines },
            { "initial_ms", workload.initial },
            { "modules", workload.modules },
            { "checks_ms", workload.checks },
            { "metrics", std::move(measured) },
        });
    }

    return { { "workloads", std::move(out) } };
}

/// An environment variable's value, when it's set and not empty.
std::optional<std::string> environment(const char* name) {
    const char* value = std::getenv(name); // NOLINT(concurrency-mt-unsafe)
    if (value == nullptr || *value == '\0') {
        return std::nullopt;
    }

    return value;
}

} // namespace

TEST_CASE("fb-lsp's benchmark, against M11's budgets", "[.][bench]") {
    const std::size_t samples = std::stoul(environment("FB_LSP_BENCH_SAMPLES").value_or("20"));
    const shape_t shape;
    const folder_t folder(program(shape));
    std::vector<workload_t> workloads{ program_workload(folder, shape), std_workload() };
    for (workload_t& workload : workloads) {
        measure(workload, samples, std::chrono::milliseconds(150), std::chrono::milliseconds(400));
    }

    const std::string report = markdown(workloads);
    std::cout << report;
    if (const std::optional<std::string> folder_name = environment("FB_LSP_BENCH_REPORT")) {
        const std::filesystem::path out = fb::path_from_utf8(*folder_name);
        std::filesystem::create_directories(out);
        std::ofstream(out / "lsp-bench.md", std::ios::binary) << report;
        std::ofstream(out / "lsp-bench.json", std::ios::binary)
            << report_json(workloads).dump(2) << "\n";
    }

    if (!environment("FB_LSP_BENCH_ENFORCE")) {
        return;
    }

    for (const workload_t& workload : workloads) {
        for (const metric_t& metric : workload.metrics) {
            if (metric.enforced) {
                INFO(workload.name << ": " << metric.what);
                CHECK(met(metric));
            }
        }
    }
}

TEST_CASE("the benchmark measures each request, on a small program", "[lsp]") {
    const shape_t shape{ .layers = 2, .width = 2, .works = 1 };
    const folder_t folder(program(shape));
    workload_t workload = program_workload(folder, shape);
    measure(workload, 1, std::chrono::milliseconds(0), std::chrono::milliseconds(0));
    for (const metric_t& metric : workload.metrics) {
        INFO(metric.name);
        CHECK(metric.samples.size() == 1);
    }

    // The edited file's checks, which import std's modules and the layer below's.
    CHECK(!workload.checks.empty());
    CHECK(workload.modules > 3);

    const std::string report = markdown({ workload });
    CHECK(report.contains("| a syntax error shown, from the keystroke |"));
    const json_t json = report_json({ workload });
    CHECK(json["workloads"][0]["files"] == (shape.layers * shape.width) + 1);
    CHECK(json["workloads"][0]["metrics"].size() == workload.metrics.size());

    // Percentiles by nearest rank.
    CHECK(percentile({ 4, 1, 3, 2 }, 0.5) == 2);
    // Met with the 50th percentile in the budget and the 95th in one and a half times it.
    CHECK(met({ .budget = std::chrono::milliseconds(10), .samples = { 10, 10, 15 } }));
    CHECK(!met({ .budget = std::chrono::milliseconds(10), .samples = { 10, 11, 11 } }));
    CHECK(!met({ .budget = std::chrono::milliseconds(10), .samples = { 1, 1, 16 } }));
    CHECK(percentile({ 4, 1, 3, 2 }, 0.95) == 4);
    CHECK(percentile({}, 0.95) == 0);
}
