# Writes OUTPUT, the C++ of the zones the tz database's zic compiled into ZONEINFO: their TZif
# bytes, one zone after another, and each zone's and link's name, sorted, with where its zone's
# bytes are. DATA is tzdata's folder, whose Zone and Link lines name them, and VERSION its
# version.

set(sources africa antarctica asia australasia europe northamerica southamerica etcetera backward)
set(zones "")
set(links "")
foreach(source ${sources})
  file(STRINGS "${DATA}/${source}" lines REGEX "^(Zone|Link)[ \t]")
  foreach(line ${lines})
    string(REGEX REPLACE "[ \t]+" ";" fields "${line}")
    list(GET fields 0 kind)
    list(GET fields 1 first)
    if(kind STREQUAL "Zone")
      list(APPEND zones "${first}")
    else()
      # "Link TARGET NAME"
      list(GET fields 2 second)
      list(APPEND links "${second}")
      set(target_of_${second} "${first}")
    endif()
  endforeach()
endforeach()

# Each zone's bytes, from a line of its own, 16 to a line, and where they are. CMake's regular
# expressions have no counts, so a line's 32 digits are spelled out.
list(SORT zones)
string(REPEAT "[0-9a-f]" 32 line)
set(data "")
set(at 0)
foreach(zone ${zones})
  file(READ "${ZONEINFO}/${zone}" hex HEX)
  string(LENGTH "${hex}" length)
  math(EXPR size "${length} / 2")
  string(REGEX REPLACE "(${line})" "\\1\n" hex "${hex}")
  string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
  string(REGEX REPLACE "\n$" "" bytes "${bytes}")
  string(APPEND data "${bytes}\n")
  set(offset_of_${zone} ${at})
  set(size_of_${zone} ${size})
  math(EXPR at "${at} + ${size}")
endforeach()

# A link names a zone, or another link that does.
set(names ${zones} ${links})
list(SORT names)
set(entries "")
foreach(name ${names})
  set(zone "${name}")
  foreach(step RANGE 8)
    if(DEFINED offset_of_${zone} OR NOT DEFINED target_of_${zone})
      break()
    endif()
    set(zone "${target_of_${zone}}")
  endforeach()
  if(NOT DEFINED offset_of_${zone})
    message(FATAL_ERROR "the tz database's ${name} names no zone")
  endif()
  string(APPEND entries "    { \"${name}\", ${offset_of_${zone}}, ${size_of_${zone}} },\n")
endforeach()

file(WRITE "${OUTPUT}" "// Made by third_party/embed_tz.cmake from the IANA tz database ${VERSION}, as its zic
// compiled it.

#include \"tz_data.hpp\"

namespace {

const unsigned char bytes[] = {
${data}};

const fb::tz::entry_t table[] = {
${entries}};

} // namespace

namespace fb::tz {

std::span<const unsigned char> data() {
    return bytes;
}

std::span<const entry_t> entries() {
    return table;
}

std::string_view version() {
    return \"${VERSION}\";
}

} // namespace fb::tz
")
