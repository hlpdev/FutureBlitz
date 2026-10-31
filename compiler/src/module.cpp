#include "fb/module.hpp"

#include <algorithm>
#include <cassert>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fb {

std::string module_name(std::string_view path) {
    if (path.ends_with(".fb")) {
        path.remove_suffix(3);
    }

    std::string name(path);
    std::ranges::replace(name, '/', '.');
    std::ranges::replace(name, '\\', '.');
    return name;
}

module_id_t module_table_t::add(module_t module) {
    const auto id = static_cast<module_id_t>(m_modules.size());
    const auto file = std::to_underlying(module.file);
    if (m_by_file.size() <= file) {
        m_by_file.resize(file + 1);
    }

    m_by_file[file] = id;
    m_modules.push_back(std::move(module));
    return id;
}

const module_t& module_table_t::get(const module_id_t id) const {
    const auto index = std::to_underlying(id);
    assert(index < m_modules.size());
    return m_modules[index];
}

std::optional<module_id_t> module_table_t::of(const file_id_t file) const {
    const auto index = std::to_underlying(file);
    return index < m_by_file.size() ? m_by_file[index] : std::nullopt;
}

bool module_table_t::is_main(const file_id_t file) const {
    const std::optional<module_id_t> module = of(file);
    return !module || get(*module).is_main;
}

std::optional<module_id_t> module_table_t::main() const {
    const auto found = std::ranges::find_if(m_modules, &module_t::is_main);
    return found != m_modules.end()
               ? std::optional(static_cast<module_id_t>(found - m_modules.begin()))
               : std::nullopt;
}

package_id_t module_table_t::add_package(std::string name, const bool standard,
                                         std::optional<std::vector<std::string>> natives) {
    const auto id = static_cast<package_id_t>(m_packages.size());
    m_packages.push_back(std::move(name));
    m_natives.push_back(std::move(natives));
    m_dependencies.emplace_back();
    m_tested.emplace_back();
    if (standard) {
        m_standard = id;
    }

    return id;
}

void module_table_t::add_dependency(const package_id_t package, const package_id_t on) {
    const auto index = std::to_underlying(package);
    assert(index < m_dependencies.size());
    m_dependencies[index].push_back(on);
}

bool module_table_t::depends_on(const package_id_t package, const package_id_t on) const {
    const auto index = std::to_underlying(package);
    assert(index < m_dependencies.size());
    return std::ranges::contains(m_dependencies[index], on);
}

void module_table_t::add_tests(const package_id_t package, const package_id_t tested) {
    const auto index = std::to_underlying(package);
    assert(index < m_tested.size());
    m_tested[index] = tested;
}

bool module_table_t::sees_package(const package_id_t here, const package_id_t declared) const {
    const auto index = std::to_underlying(here);
    assert(index < m_tested.size());
    return here == declared || m_tested[index] == declared;
}

bool module_table_t::is_standard(const package_id_t id) const {
    return m_standard == id;
}

const std::optional<std::vector<std::string>>&
module_table_t::natives(const package_id_t id) const {
    const auto index = std::to_underlying(id);
    assert(index < m_natives.size());
    return m_natives[index];
}

const std::string& module_table_t::package_name(const package_id_t id) const {
    const auto index = std::to_underlying(id);
    assert(index < m_packages.size());
    return m_packages[index];
}

std::optional<std::string_view> module_table_t::std_home(const std::string_view name,
                                                         const bool array_method) const {
    const auto found = std::ranges::find_if(m_std_names, [&](const std_name_t& declared) {
        return declared.name == name && declared.array_method == array_method &&
               declared.extended.empty();
    });
    return found != m_std_names.end() ? std::optional<std::string_view>(found->module)
                                      : std::nullopt;
}

std::optional<std::string_view> module_table_t::std_member_home(const std::string_view type,
                                                                const std::string_view name) const {
    const auto found = std::ranges::find_if(m_std_names, [&](const std_name_t& declared) {
        return declared.extended == type && declared.name == name;
    });
    return found != m_std_names.end() ? std::optional<std::string_view>(found->module)
                                      : std::nullopt;
}

} // namespace fb
