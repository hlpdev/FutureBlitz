#pragma once

#include "fb/diagnostic_render.hpp"
#include "fb/options.hpp"
#include "fb/registry.hpp"
#include "fb/style.hpp"
#include "fb/terminal.hpp"

#include <cstdio>
#include <format>
#include <string_view>

namespace fb {

/// Stage views go to stdout so they pipe cleanly. Everything else goes to stderr, like cargo.
class console_t {
public:
    /// Each stream gets its own style, since only one of them may be a terminal.
    explicit console_t(const color_t color)
        : m_out{ .color = use_color(color, stream_t::STANDARD_OUTPUT) },
          m_err{ .color = use_color(color, stream_t::STANDARD_ERROR) } {}

    [[nodiscard]] const style_t& out() const {
        return m_out;
    }

    [[nodiscard]] const style_t& err() const {
        return m_err;
    }

    /// How diagnostics go to stderr: in its colors, with FB_COLUMNS's columns.
    [[nodiscard]] render_options_t diagnostics() const {
        return { .color = m_err.color, .columns = m_columns };
    }

    static void print(const std::string_view text) {
        std::fwrite(text.data(), 1, text.size(), stdout);
    }

    static void print_error(const std::string_view text) {
        // Views and errors stay in order when both streams go to the same place.
        std::fflush(stdout);
        std::fwrite(text.data(), 1, text.size(), stderr);
    }

    /// status(), for what a compilation says as it works, as it runs a build step.
    [[nodiscard]] status_t statuses() const {
        return [this](const std::string_view verb, const std::string_view message) {
            status(verb, message);
        };
    }

    /// "   Compiling game.fb", with the verb right-aligned like cargo's.
    void status(const std::string_view verb, const std::string_view message) const {
        const std::string label = m_err.paint(std::format("{:>12}", verb), tone_t::SUCCESS);
        print_error(std::format("{} {}\n", label, message));
    }

    void error(const std::string_view message) const {
        print_error(std::format("{}: {}\n", m_err.paint("error", tone_t::ERROR),
                                m_err.paint(message, tone_t::BOLD)));
    }

    void warning(const std::string_view message) const {
        print_error(std::format("{}: {}\n", m_err.paint("warning", tone_t::WARNING), message));
    }

    void note(const std::string_view message) const {
        print_error(std::format("{}: {}\n", m_err.paint("note", tone_t::NOTE), message));
    }

private:
    style_t m_out;
    style_t m_err;
    column_unit_t m_columns = diagnostic_columns();
};

} // namespace fb
