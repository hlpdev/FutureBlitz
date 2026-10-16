#include "fb/diagnostic.hpp"

#include <algorithm>
#include <utility>

namespace fb {

diagnostic_t& diagnostic_bag_t::report(diagnostic_t diagnostic) {
    if (diagnostic.severity == severity_t::ERROR) {
        ++m_error_count;
    }

    return m_diagnostics.emplace_back(std::move(diagnostic));
}

void diagnostic_bag_t::sort() {
    std::ranges::stable_sort(m_diagnostics, [](const diagnostic_t& a, const diagnostic_t& b) {
        return std::pair(a.span.file, a.span.begin) < std::pair(b.span.file, b.span.begin);
    });
}

void diagnostic_bag_t::clear() {
    m_diagnostics.clear();
    m_error_count = 0;
}

} // namespace fb
