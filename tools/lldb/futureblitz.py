"""FutureBlitz's values in lldb and CodeLLDB: Strings, arrays, Maps, nullables, and Classes as
their values, not their bytes.

Load it with `command script import <path>/futureblitz.py`, which the VS Code extension does. It
reads what the compiler's debug info says about each type (compiler/src/codegen.cpp's
debug_type()): a String, a Class, and a T[] are their names for a pointer to their object, whose
header comes first, in a member named "header.counts", and for a Class's or a T[]'s object
"header.class" after it; a T[]'s object, "fb." and its name, has its Count and its Items after
the header; a String's, "fb.String", its Length, with its UTF-8 bytes after it; a value T? is a
struct of HasValue and Value, and a String?, Class?, or T[]? a struct of its Value, each named
by its type, which ends in "?". A Class's object shows the Fields of its own Class, whose struct
is "fb." and the full name its header's fb_class_t has, so an object seen through a Class it
extends, or an Interface, shows them all.

The summaries hide a pointer's value, its address, as `type summary add --no-value` does. lldb's
`frame variable` shows a pointer's children only with a pointer depth, as `-P 1` gives.
"""

import lldb

CATEGORY = "futureblitz"
# The start of std's Map's struct's name, its full name's.
MAP = "fb.std.collections.map.Map<"
# The most elements, entries, or bytes a summary shows, and the most children a value has.
SHOWN = 16
CHILDREN = 1000
TEXT_BYTES = 4096


def _read_int(process, address, size):
    error = lldb.SBError()
    value = process.ReadUnsignedFromMemory(address, size, error)
    return value if error.Success() else None


def _object_struct(sbtype):
    """The struct a String's, Class's, or T[]'s pointer points at, or None for another type."""
    sbtype = sbtype.GetCanonicalType()
    if not sbtype.IsPointerType():
        return None

    pointee = sbtype.GetPointeeType().GetCanonicalType()
    if pointee.GetNumberOfFields() == 0:
        return None

    if pointee.GetFieldAtIndex(0).GetName() != "header.counts":
        return None

    return pointee


def _field_names(struct):
    return [struct.GetFieldAtIndex(i).GetName() for i in range(struct.GetNumberOfFields())]


def _kind(sbtype):
    """What a type is to the script: "string", "array", "map", "object", "nullable", or None."""
    struct = _object_struct(sbtype)
    if struct is not None:
        names = _field_names(struct)
        name = struct.GetName()
        if name == "fb.String" and names == ["header.counts", "Length"]:
            return "string"

        if (name.endswith("[]")
                and names[:5] == ["header.counts", "header.class", "Count", "Capacity", "Items"]):
            return "array"

        if name.startswith(MAP) and "Entries" in names and "First" in names:
            return "map"

        if len(names) >= 2 and names[1] == "header.class":
            return "object"

        return None

    canonical = sbtype.GetCanonicalType()
    if (canonical.IsTypeComplete() and canonical.GetName().endswith("?")
            and _field_names(canonical) in (["HasValue", "Value"], ["Value"])):
        return "nullable"

    return None


def _is_kind(kind):
    def recognizes(sbtype, internal_dict):
        return _kind(sbtype) == kind

    return recognizes


is_string = _is_kind("string")
is_array = _is_kind("array")
is_map = _is_kind("map")
is_object = _is_kind("object")
is_nullable = _is_kind("nullable")


def _quoted(data):
    """A String's bytes as FutureBlitz writes the String: in quotes, with escapes."""
    text = data.decode("utf-8", "replace")
    escapes = {"\"": "\\\"", "\\": "\\\\", "\n": "\\n", "\r": "\\r", "\t": "\\t", "\0": "\\0"}
    out = []
    for character in text:
        if character in escapes:
            out.append(escapes[character])
        elif ord(character) < 0x20 or ord(character) == 0x7F:
            out.append("\\u{%x}" % ord(character))
        else:
            out.append(character)

    return "\"" + "".join(out) + "\""


def string_text(valobj):
    """A String's value as text, or None for Null."""
    address = valobj.GetValueAsUnsigned(0)
    if address == 0:
        return None

    process = valobj.GetProcess()
    length = _read_int(process, address + 8, 8)
    if length is None:
        return "<unreadable>"

    error = lldb.SBError()
    shown = min(length, TEXT_BYTES)
    data = process.ReadMemory(address + 16, shown, error) if shown > 0 else b""
    if not error.Success():
        return "<unreadable>"

    text = _quoted(data)
    return text + "..." if shown < length else text


def string_summary(valobj, internal_dict):
    text = string_text(valobj)
    return "Null" if text is None else text


class StringProvider:
    """A String, which is its text, with no children, not its object's members."""

    def __init__(self, valobj, internal_dict):
        pass

    def update(self):
        return False

    def num_children(self):
        return 0

    def has_children(self):
        return False

    def get_child_index(self, name):
        return -1

    def get_child_at_index(self, index):
        return None


def _shown(value):
    """How a child shows in its owner's summary: its summary, else its value."""
    summary = value.GetSummary()
    if summary:
        return summary

    shown = value.GetValue()
    return shown if shown is not None else value.GetTypeName()


class ArrayProvider:
    """A T[]'s elements as its children, "[0]", "[1]", and so on."""

    def __init__(self, valobj, internal_dict):
        self.valobj = valobj
        self.count = 0
        self.items = 0
        self.element = None

    def update(self):
        self.count = 0
        address = self.valobj.GetValueAsUnsigned(0)
        if address == 0:
            return False

        array = self.valobj.Dereference()
        self.count = max(0, array.GetChildMemberWithName("Count").GetValueAsSigned(0))
        items = array.GetChildMemberWithName("Items")
        self.items = items.GetValueAsUnsigned(0)
        self.element = items.GetType().GetPointeeType()
        return False

    def num_children(self):
        return min(self.count, CHILDREN)

    def has_children(self):
        return self.count > 0

    def get_child_index(self, name):
        try:
            return int(name.strip("[]"))
        except ValueError:
            return -1

    def get_child_at_index(self, index):
        if index < 0 or index >= self.num_children():
            return None

        return self.valobj.CreateValueFromAddress(
            "[%d]" % index, self.items + index * self.element.GetByteSize(), self.element)


def array_summary(valobj, internal_dict):
    if valobj.GetValueAsUnsigned(0) == 0:
        return "Null"

    provider = ArrayProvider(valobj.GetNonSyntheticValue(), internal_dict)
    provider.update()
    shown = [_shown(provider.get_child_at_index(i)) for i in range(min(provider.count, SHOWN))]
    more = ", ..." if provider.count > SHOWN else ""
    return "[" + ", ".join(shown) + more + "]"


def _class_info(valobj, at):
    """What the fb_class_t of the object a reference points at has `at` bytes in, from its
    header: its name at 0, its full name at 8, and its objects' size at 16."""
    address = valobj.GetValueAsUnsigned(0)
    process = valobj.GetProcess()
    type_info = _read_int(process, address + 8, 8)
    if not type_info:
        return None

    return _read_int(process, type_info + at, 8)


def _class_name(valobj, at=0):
    """The name of the Class of the object a reference points at, or with `at` 8, its full name,
    which tells it from another module's of its name."""
    name = _class_info(valobj, at)
    if not name:
        return None

    error = lldb.SBError()
    text = valobj.GetProcess().ReadCStringFromMemory(name, 512, error)
    return text if error.Success() else None


class ObjectProvider:
    """A Class's object's Fields as its children, those of the Class it's an object of, which
    can be one that extends the reference's own."""

    def __init__(self, valobj, internal_dict):
        self.valobj = valobj
        self.object = None
        self.fields = []

    def update(self):
        self.object = None
        self.fields = []
        address = self.valobj.GetValueAsUnsigned(0)
        if address == 0:
            return False

        # The object's own Class's struct, which its size checks is the one.
        struct = _object_struct(self.valobj.GetType())
        name = _class_name(self.valobj, 8) or _class_name(self.valobj)
        size = _class_info(self.valobj, 16)
        if name is not None:
            found = self.valobj.GetTarget().FindFirstType("fb." + name)
            if found.IsValid() and found.GetByteSize() == size:
                struct = found

        self.object = self.valobj.CreateValueFromAddress("object", address, struct)
        self.fields = [field for field in _field_names(struct) if not field.startswith("header.")]
        return False

    def num_children(self):
        return len(self.fields)

    def has_children(self):
        return bool(self.fields)

    def get_child_index(self, name):
        return self.fields.index(name) if name in self.fields else -1

    def get_child_at_index(self, index):
        if self.object is None or index < 0 or index >= len(self.fields):
            return None

        return self.object.GetChildMemberWithName(self.fields[index])


def object_summary(valobj, internal_dict):
    if valobj.GetValueAsUnsigned(0) == 0:
        return "Null"

    name = _class_name(valobj)
    return name if name is not None else valobj.GetTypeName()


class MapProvider:
    """A Map's entries as its children, in the order they were added, each named by its key."""

    def __init__(self, valobj, internal_dict):
        self.valobj = valobj
        self.entries = []

    def update(self):
        self.entries = []
        if self.valobj.GetValueAsUnsigned(0) == 0:
            return False

        fields = ObjectProvider(self.valobj, None)
        fields.update()
        map_object = fields.object
        entries = map_object.GetChildMemberWithName("Entries").GetNonSyntheticValue()
        array = ArrayProvider(entries, None)
        array.update()
        at = map_object.GetChildMemberWithName("First").GetValueAsSigned(-1)
        while 0 <= at < array.count and len(self.entries) < CHILDREN:
            entry = array.get_child_at_index(at)
            self.entries.append(entry)
            at = entry.GetChildMemberWithName("After").GetValueAsSigned(-1)

        return False

    def num_children(self):
        return len(self.entries)

    def has_children(self):
        return bool(self.entries)

    def get_child_index(self, name):
        for i in range(len(self.entries)):
            if self._name(i) == name:
                return i

        return -1

    def _name(self, index):
        return "[" + _shown(self.entries[index].GetChildMemberWithName("Key")) + "]"

    def get_child_at_index(self, index):
        if index < 0 or index >= len(self.entries):
            return None

        value = self.entries[index].GetChildMemberWithName("Value")
        return value.CreateValueFromAddress(self._name(index), value.GetLoadAddress(),
                                            value.GetType())


def map_summary(valobj, internal_dict):
    if valobj.GetValueAsUnsigned(0) == 0:
        return "Null"

    provider = MapProvider(valobj.GetNonSyntheticValue(), internal_dict)
    provider.update()
    shown = []
    for entry in provider.entries[:SHOWN]:
        shown.append(_shown(entry.GetChildMemberWithName("Key")) + ": "
                     + _shown(entry.GetChildMemberWithName("Value")))

    more = ", ..." if len(provider.entries) > SHOWN else ""
    return "{" + ", ".join(shown) + more + "}"


def _nullable_value(valobj):
    """A T?'s value, or None for Null: a value T?'s when its HasValue is set, and a String?'s,
    Class?'s, or T[]?'s when its reference isn't null."""
    plain = valobj.GetNonSyntheticValue()
    value = plain.GetChildMemberWithName("Value")
    has = plain.GetChildMemberWithName("HasValue")
    held = has.GetValueAsUnsigned(0) if has.IsValid() else value.GetValueAsUnsigned(0)
    if held == 0:
        return None

    # Its children are those the script gives it, a Class's Fields, not its object's members.
    value.SetPreferSyntheticValue(True)
    return value


class NullableProvider:
    """A T?'s value's children when it holds one, and none for Null."""

    def __init__(self, valobj, internal_dict):
        self.valobj = valobj
        self.value = None

    def update(self):
        self.value = _nullable_value(self.valobj)
        return False

    def num_children(self):
        return self.value.GetNumChildren() if self.value is not None else 0

    def has_children(self):
        return self.num_children() > 0

    def get_child_index(self, name):
        return self.value.GetIndexOfChildWithName(name) if self.value is not None else -1

    def get_child_at_index(self, index):
        return self.value.GetChildAtIndex(index) if self.value is not None else None


def nullable_summary(valobj, internal_dict):
    value = _nullable_value(valobj)
    return "Null" if value is None else _shown(value)


def __lldb_init_module(debugger, internal_dict):
    module = __name__
    for kind, summary, provider in (
        ("string", "string_summary", "StringProvider"),
        ("array", "array_summary", "ArrayProvider"),
        ("map", "map_summary", "MapProvider"),
        ("object", "object_summary", "ObjectProvider"),
        ("nullable", "nullable_summary", "NullableProvider"),
    ):
        recognizer = "%s.is_%s" % (module, kind)
        # Without the value, a String's, Class's, or T[]'s address.
        debugger.HandleCommand(
            "type summary add -w %s -e --no-value -F %s.%s --recognizer-function %s"
            % (CATEGORY, module, summary, recognizer))
        if provider is not None:
            debugger.HandleCommand("type synthetic add -w %s -l %s.%s --recognizer-function %s"
                                   % (CATEGORY, module, provider, recognizer))

    debugger.HandleCommand("type category enable %s" % CATEGORY)
