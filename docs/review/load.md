# The review of M6 step 17's Load<T>(value) and Save(x): built-ins converting std.core's Value to and from any type, with @Rename, @Skip, @Optional, @Flatten, @Renamed, OnLoaded, and Loadable/Saveable

23 confirmed findings: 3 high, 11 medium, 9 low. Each was reproduced by a skeptic. Fix every one with a test that fails without the fix, highs first, and set its Status line: fixed in <commit>, or dropped with why (only if it turns out not to reproduce, or the spec settles it the other way).

## lower-1 (high): A failed Load runs the half-made Class's Destroy, which crashes on a Field Load never filled

Status: fixed in 01edbb23: load_fields keeps a half-made Class in its scope as a discard_t, a new cleanup that lets go of its fields without its Destroy (OBJECT_DISCARD), until every Field is filled; core_load_destroy fails a Load before and after the Field with no zero, and its OnLoaded's error, which runs Destroy on the whole object

Where: compiler/src/lower.cpp:

load_fields makes the Class with new_object(type) and does not run New, then fills the Fields one by one. When an earlier Field's entry fails (WRONG_TYPE, MISSING, OUT_OF_RANGE), the error path releases `made`. That runs the object's Destroy() while later Fields with no zero (a Class, an array, a Map) are still null. A Destroy that reads such a Field then dereferences null. The same input with a valid Count loads and destroys fine (prints 'destroying 2'). The same crash happens in debug and release.

```
Import std.core

Class Item
    Field Name: String = "item"
End Class

Class Box
    Field Count: Int
    Field Thing: Item

    New()
        Thing = Item()
    End New

    Method Destroy()
        Print("destroying", Thing.Name.Bytes.Count)
    End Method
End Class

Try
    Local b = Load<Box>(Value({"Count": Value("x")}))
Catch e As ParseError
    Print(e.Message)
End Try
Print("done")
```

Expected: Count: expected Int32, found "x"
done
(and no Destroy run on an object Load didn't finish, or none that can see a null Field)

Actual: error: g.exe exited with code 0xC0000005 (segfault), in debug and in release

Suggested fix: Don't run Destroy for an object that Load gave up on: free its fields without its Destroy. Or have Load fill every Field before anything can fail, which isn't possible for a Field without a zero, so skipping Destroy is the cleaner fix. Add a program test with a Destroy that reads a Class Field, and an error before that Field.

Skeptic: I ran the program in debug and release, and both segfault (0xC0000005). In a variant (l1b.fb) the Destroy only prints, and the output is 'destroy runs' and then 'Count: expected Int32, found "x"'. So the failed Load does release the half-made Box and run its Destroy while Thing, which has no zero, is still null. In lower.cpp, load_fields makes the Class with new_object(type) and doesn't run New, then adds `made` to the scope's releases before any Field loads. Spec 13 says each Field starts at its default, but a Class Field with no default has none, so Destroy can see a null in a non-nullable Field.

## lower-2 (high): Load of a non-@Flags Enum from an integer makes a value that is no member

Status: fixed in 01edbb23: LoadEnum takes an integer for an Enum without @Flags only when it's a member's, else WRONG_TYPE naming the members; core_load_errors loads Value(1), Value(7), and Value(-1) as a Color

Where: std/core/load.fb:228

LoadEnum takes any integer within the underlying type's range (signed_range of info.inner), and lowering casts it to the Enum unchecked. Spec 06 says that for an Enum without @Flags the integer must be one of its values, and that others panic. Load therefore makes a Color that equals no member: it prints 7 and compares False with RED and GREEN, so Selects fall through. It should be a ParseError, since this is bad input and not a bug in the program.

```
Import std.core

Enum Color
    RED, GREEN
End Enum

Local c = Try Load<Color>(Value(7))
Print(c, c == Color.RED, c == Color.GREEN)
```

Expected: uncaught ParseError: expected Color, one of RED, or GREEN, found 7 (or an OUT_OF_RANGE naming its members)

Actual: 7 False False

Suggested fix: For an Enum without @Flags, after reading the integer, check that it is one of `values` (the bits array LoadEnum already gets), and give WRONG_TYPE or OUT_OF_RANGE otherwise. Add the case to core_load_errors.fb (Value(7) and Value(-1) for Color).

Skeptic: Debug and release both print '7 False False'. With a Select that covers every member (l2b.fb), the program panics with 'no Case matched at l2b.fb:8:5'. Spec 06 says that for an Enum without @Flags the integer must be one of its values, and that others panic. So Load makes a value the language says can't exist, and bad input ends in a panic further on instead of a ParseError.

## sema-1 (high): A generic @Skip (or @Optional) Field whose instance's T has no zero isn't checked, and Load leaves it Null, which crashes

Status: fixed in 01edbb23: lowering checks each instance's Struct or Class with fields_problem(), analysis's rules shared, which says a @Skip or @Optional Field needs a default when its type has no zero, at the Load<T> call; mir_tests has Held<Node> with each

Where: compiler/src/sema.cpp:299

check_load_attributes skips the needs-a-default check when the Field's type has type parameters in it (`inside.empty()`), and neither check_converted (which skips parts with parameters) nor lowering's instance code checks it again for the instance. The spec says each instance's T is checked, and that a Field @Skip or @Optional leaves at its default needs one. For Box<Node>, Load leaves the non-nullable @Skip Field Item uninitialized (Null), and reading it gives an access violation in both debug and release builds. The non-generic version of the same Class is a compile error. For @Optional, Load throws a misleading 'missing, and Box<Node>'s Field Item isn't @Optional' instead (the Field is @Optional).

```
Import std.core

Class Node
    Field Name: String = "n"
End Class

Class Box<T>
    Field Label: String = "b"
    @Skip Field Item: T

    New(item: T)
        Item = item
    End New
End Class

Local b = Try Load<Box<Node>>(Value({"Label": Value("x")}))
Print(b.Label)
Print(b.Item.Name)
```

Expected: A compile error at the Load<Box<Node>> call, like the non-generic one: "@Skip leaves 'Item' at its default when Load makes Box<Node>, and Node has no zero, so it needs a default of its own"

Actual: Compiles. Running it prints nothing and exits with code 0xC0000005 (debug and --release). With @Optional in place of @Skip: uncaught ParseError: Item: missing, and Box<Node>'s Field Item isn't @Optional

Suggested fix: When lowering builds an instance's Load or Save, run the same per-Field checks analysis runs (the needs-a-default check for @Skip/@Optional, @Flatten's type, Map keys, Abstract, key clashes) on the instance's concrete types, and report them at the call. Or have check_converted check them on concrete instances as lowering meets them. Add sema or driver tests for Box<Node> with @Skip and with @Optional.

Skeptic: Ran the program in rv-load/sema/s1.fb. It compiles, then exits with 0xC0000005 in both the debug and the --release build. With @Optional in place of @Skip (s1b.fb) it throws 'uncaught ParseError: Item: missing, and Box<Node>'s Field Item isn't @Optional' at s1b.fb:16:15, even though the Field is @Optional. Spec 13 says 'In generic code, each instance's T is checked' and 'A Field that @Skip or @Optional leaves at its default has one', and neither holds for Box<Node>.

## data-1 (medium): A @Flatten Field's keys aren't checked against the other Fields' keys, so Save drops data with no error

Status: fixed in 01edbb23, with sema-3: fields_problem() expands a @Flatten Field into its type's keys, so Marker's 'X' and 'At.X' clash; a sema test has it

Where: compiler/src/sema.cpp:7290

clashing_keys() skips @Flatten Fields altogether (load_key gives nullopt for them) and never looks at the keys the flattened type adds. When a flattened Field's own Field has the same key as an outer Field, Save puts both under one key (Put overwrites the first), and Load fills both Fields from that one entry. Spec 13 says two Fields can't have one key and an old key can be no other's. The same gap lets @Rename("X") on an outer Field clash with a flattened X (Save writes {"X": 0} for two Fields), and lets a flattened Field's @Renamed("Y") read an outer Field's key Y.

```
Import std.core

Struct Place
    Field X: Int
    Field Y: Int
End Struct

Class Marker
    Field X: Int
    @Flatten Field At: Place
End Class

Local m = Marker()
m.X = 1
m.At = Place(2, 3)
Print(Save(m))
Local back = Try Load<Marker>(Save(m))
Print(back.X, back.At.X)
```

Expected: A compile error saying Marker's Fields 'X' and 'At.X' both have the key "X" (as c1/c3-style clashes already get)

Actual: {"X": 2, "Y": 3}
2 2   (Marker.X's 1 is lost on Save, and Load puts 2 in both Fields)

Suggested fix: In clashing_keys, for a @Flatten Field, recurse into its Struct's or Class's keys (and old keys, for Load) and add them to the same list, so a clash across the flatten is reported like any other.

Skeptic: I ran rv-load/data/d1.fb in debug. It printed {"X": 2, "Y": 3} and then 2 2, so Marker.X's 1 is lost on Save and Load puts 2 in both Fields. In sema.cpp, load_key gives nullopt for a @Flatten Field, and clashing_keys skips any Field with no key, so it never looks at the flattened type's keys. Spec 13 says two Fields can't have one key, and a Field's old key is no other's.

## data-2 (medium): @Flatten is accepted on a Map or Value Field, and Load gives it every key, the other Fields' too

Status: fixed in 01edbb23, with sema-4: @Flatten on a Map or a Value is an error that they're converted whole; a sema test has each, and their T?s

Where: compiler/src/sema.cpp:7254

The @Flatten check only asks is_struct || is_class, and Map<String, V> and Value are Classes, so it passes. Spec 13 says @Flatten puts a Struct's or Class's Field's own Fields in the Object. For a Map or Value Field, Save puts its entries in the Object, and Load loads the Map or Value from the whole Object, so it also gets every sibling Field's entry. Here Extra comes back holding "Name". A @Flatten Value Field gets the same: {"Name": "a", "k": 1}.

```
Import std.core
Import std.collections

Class Bag
    Field Name: String = "a"
    @Flatten Field Extra: Map<String, String>

    New()
        Extra = {"k": "v"}
    End New
End Class

Local b = Bag()
Print(Save(b))
Local back = Try Load<Bag>(Save(b))
Print(back.Name, back.Extra)
```

Expected: A compile error that @Flatten goes on a Struct's or Class's Field whose Fields join the others, not a Map or Value; or, if this is meant as a catch-all, Extra holding only the keys no other Field takes: {k: v}

Actual: {"Name": "a", "k": "v"}
a {Name: a, k: v}

Suggested fix: Reject @Flatten on a Field whose type converts itself, as Map<String, V>, Value, T[], or a Loadable/Saveable do, or give it leftover-keys behavior and say so in spec 13.

Skeptic: I ran d2.fb. It printed {"Name": "a", "k": "v"} and then a {Name: a, k: v}: Extra comes back holding Name, so Save then Load doesn't give back what was saved. The check in sema.cpp is only is_struct || is_class, and Map is a Class, so it passes. Spec 13 and the check's own diagnostic say @Flatten puts the Field's own Fields with the others. A Map's entries aren't Fields.

## data-3 (medium): Save of a @Flags value with bits no member has writes only the member names, so the bits are lost

Status: fixed in 01edbb23, with lower-3: Save writes a @Flags value with bits no member has as its integer; core_load round-trips 9 As Layer

Where: compiler/src/lower.cpp:7750

save_body writes an Enum as its ToString text. For a @Flags value that mixes member bits with others, like 9 (WORLD | 8), that text is "WORLD". Load reads that back as 1. Load<Layer>(Value(9)) accepts 9, so Save then Load doesn't give back what Load made. Spec 13 says a value that isn't a member's is saved as an integer.

```
Import std.core

@Flags Enum Layer
    WORLD, PLAYER, ENEMY
End Enum

Local odd = Try Load<Layer>(Value(9))
Print(odd As Int, Save(odd))
Print((Try Load<Layer>(Save(odd))) As Int)
```

Expected: 9 9 (or "WORLD | 8", any form Load reads back)
9

Actual: 9 "WORLD"
1

Suggested fix: In Save's Enum code, when a @Flags value has bits outside the members' (value & ~all_members != 0), save the integer, or add the leftover bits to the text in a form LoadEnum reads.

Skeptic: I ran d3.fb. It printed 9 "WORLD" and then 1, so a value Load accepts doesn't survive Save then Load. Spec 13 says a @Flags Enum is saved as its flags ("WORLD | PLAYER"), and a value that isn't a member's is saved as an integer. 9 isn't a member's value or a joining of members, but Save writes "WORLD".

## data-5 (medium): A ParseError thrown with path: from FromValue or OnLoaded loses its Message on the way out of Load

Status: fixed in 01edbb23, with lower-5: ParseError's New keeps its message as Plain, and a path given to it starts its Message, so Within keeps the text; core_load_errors has a FromValue's error with path: "inner", which comes out as "T.inner: bad tag"

Where: std/core/error.fb:104

ParseError's Public New takes `path`. Within sets Plain = Message only when At is "". When a type's own FromValue or OnLoaded throws a ParseError with a non-empty path, Plain stays "", and the outer step rewrites Message to "{path}: ", which throws away the text the user wrote.

```
Import std.core

Struct Tagged Implements Loadable
    Field N: Int

    Static Method FromValue(value: Value): Tagged Throws ParseError
        Throw ParseError(ParseErrorKind.OTHER, "bad tag", path: "inner")
    End Method
End Struct

Class Holder
    Field T: Tagged
End Class

Try
    Local h = Load<Holder>(Value({"T": Value(1)}))
Catch e As ParseError
    Print("[{e.Path ?? "Null"}] {e.Message}")
End Try
```

Expected: [T.inner] T.inner: bad tag

Actual: [T.inner] T.inner: 

Suggested fix: Set Plain from Message whenever Plain hasn't been set yet (e.g. track it with a Bool, or set Plain = message in New), not only when At is ""; or make the path parameter Package.

Skeptic: I ran d5.fb. It printed [T.inner] T.inner: with the user's text "bad tag" gone. In std/core/error.fb, ParseError's Public New takes path: and sets At = path. Within sets Plain = Message only when At is "", so with At = "inner", Plain stays "" and Message becomes "T.inner: ". Spec 13 says the Message starts with the path, followed by the error's text.

## lower-3 (medium): Save of a @Flags value with bits outside its named flags loses them, so Load gives back another value

Status: fixed in 01edbb23, with data-3: core_load round-trips ~Layer.WORLD, which is saved as -2

Where: compiler/src/lower.cpp:

save_body writes an Enum through its text_function (ToString). For a @Flags value that mixes named flags with other bits, like ~Layer.WORLD or 9 As Layer, that text drops the other bits. Spec 13 says Save writes 'an integer for a value that isn't a member's'. Here Save(~Layer.WORLD) writes "PLAYER | ENEMY", so the value -2 comes back as 6. A value made only of unnamed bits (8) is written as "8" and round-trips, but a mixed one doesn't.

```
Import std.core

@Flags Enum Layer
    WORLD, PLAYER, ENEMY
End Enum

Local all = ~Layer.WORLD
Local back = Try Load<Layer>(Save(all))
Print(Save(all), (all As Int), (back As Int), back == all)
```

Expected: "-2" -2 -2 True (or some text that keeps every bit)

Actual: "PLAYER | ENEMY" -2 6 False

Suggested fix: In Save, write a @Flags value as its names only when its named flags cover all of its bits, and otherwise write its integer, as the spec says. Load already reads that integer. Add a ~ value and a 9 As Layer to core_load.fb's round trips.

Skeptic: Debug and release both print '"PLAYER | ENEMY" -2 6 False'. With 9 As Layer (l3b.fb) the output is '"WORLD" 9 1 False', so the unnamed bit 8 is dropped. Spec 13 says Save writes an integer for a value that isn't a member's. Here Save writes text that drops bits, and the round trip gives back a different value.

## lower-4 (medium): Load<Int64> takes "-9223372036854775809" (and down to about -9223372036854776832) as Int64's least

Status: fixed in 01edbb23: fb_value_scalar gives integer text past every integer type's range its own code, 5, which LoadSigned and LoadUnsigned throw OUT_OF_RANGE for; core_load_errors loads "-9223372036854775809" as an Int64 and "-18446744073709551616" as a UInt64, and the runtime's test has the code

Where: std/core/load.fb:136

fb_value_scalar reads all-digit text that is past Int64's range as a Double. Every value from -2^63-1 down to about -2^63-1024 rounds to exactly -2^63. LoadSigned's whole-Double check (real >= -9223372036854775808.0) then takes that Double as Int64.Min. So an out-of-range integer loads silently as a wrong number. The positive side gives OUT_OF_RANGE as it should ("9223372036854775808" fits a UInt64), so the two ends behave differently.

```
Import std.core

Print(Try Load<Int64>(Value("-9223372036854775809")))
Try
    Print(Load<Int64>(Value("9223372036854775808")))
Catch e As ParseError
    Print(e.Kind)
End Try
```

Expected: uncaught ParseError: expected Int64, from -9223372036854775808 to 9223372036854775807, found "-9223372036854775809" (OUT_OF_RANGE)

Actual: -9223372036854775808
OUT_OF_RANGE

Suggested fix: In fb_value_scalar, give integer text that is past both Int64's and UInt64's ranges its own code (an integer out of range), or mark it, so LoadSigned and LoadUnsigned report OUT_OF_RANGE and don't take its rounded Double as whole. Add a test for both ends.

Skeptic: Debug and release both print '-9223372036854775808' for "-9223372036854775809", and OUT_OF_RANGE for "9223372036854775808". Spec 13 says Load throws OUT_OF_RANGE for a number too big for its type. Instead, an out-of-range integer loads silently as Int64's least value, and the two ends behave differently.

## lower-5 (medium): ParseError.Within drops the Message of a ParseError made with a path

Status: fixed in 01edbb23, with data-5

Where: std/core/error.fb:104

ParseError's Public New takes `path:`, but Within sets Plain (the Message without the path) only when At is "". A ParseError that a FromValue or OnLoaded throws with its own path, like path: "mark", keeps Plain at "". On the way out, Within sets Message to "{path}: " with the text gone. Its Message also doesn't start with its path before that.

```
Import std.core

Struct Grade Implements Loadable
    Field Mark: Int

    Static Method FromValue(value: Value): Grade Throws ParseError
        Throw ParseError(ParseErrorKind.OTHER, "bad mark", path: "mark")
    End Method
End Struct

Class Report
    Field Grade: Grade
End Class

Try
    Local r = Load<Report>(Value({"Grade": Value(3)}))
Catch e As ParseError
    Print("[{e.Path ?? "Null"}] {e.Message}")
End Try
```

Expected: [Grade.mark] Grade.mark: bad mark

Actual: [Grade.mark] Grade.mark: 

Suggested fix: In Within, set Plain from Message whenever Plain is still "", or set Plain in New. Or make New's path parameter Package, so only load.fb's code passes it. Add a test.

Skeptic: Debug and release both print '[Grade.mark] Grade.mark: ' with the text gone. In std/core/error.fb, Within sets Plain = Message only when At is "", but the Public New sets At from its path parameter and leaves Plain at "". So Message becomes "{path}: " plus nothing. Spec 13 says the Message starts with the path and that a FromValue's error keeps its own text.

## sema-2 (medium): Generic code converts what sema rejects: Map<Int, V> as the Map's internal Fields, and a @Flatten T instanced as Int

Status: fixed in 01edbb23: lowering's instance check, with sema-1, says a Map's keys are Strings, that an Abstract Class can't be loaded, and that a @Flatten Field is a Struct or Class; mir_tests saves and loads a Map<Int32, Int32> through generic code, saves a Wrap<Int32> with a @Flatten T, and loads an Abstract Shape

Where: compiler/src/sema.cpp:558

check_converted skips every part with type parameters in it, and lowering's instance path only reports types it can't handle at all (cannot_convert). So for an instance it never applies the Map-keys-are-Strings rule, the @Flatten-needs-a-Struct-or-Class rule, or the Abstract rule. A Map<Int, Int> passed through a generic Save is written as Map's private implementation Fields (Entries, Buckets, Hash, Shift, ...), and a generic Load of one expects those Fields. Non-generic Save(m) on the same Map is a compile error. A @Flatten Field of type T in Box<Int> compiles and panics with a message about a ToValue that Int doesn't have.

```
Import std.core
Import std.collections

Function Put<T>(x: T): Value => Save(x)
Function Get<T>(v: Value): T Throws ParseError => Try Load<T>(v)

Struct Box<T>
    Field Label: String
    @Flatten Field Inner: T
End Struct

Local m: Map<Int, Int> = {1: 2}
Print(Put(m))
Local n = Try Get<Map<Int, Int>>(Value({"1": Value(2)}))
Print(Save(Box<Int>("b", 5)))
```

Expected: Compile errors, as for the non-generic calls: "Save can't write Map<Int32, Int32>; a Map's keys are an Object's, which are Strings" (and the same for Load), and "@Flatten goes on a Struct's or Class's Field whose own type is a Struct or Class ...; 'Inner' is Int32" for Box<Int>

Actual: Compiles. Put(m) prints {"Entries": [{"Key": 1, "Value": 2, "Hash": 6238072747940578789, "Next": -1, "Before": -1, "After": -1}], "Buckets": [-1, -1, -1, -1, 0, -1, -1, -1], "Shift": 61, "First": 0, "Last": 0, "Changes": 1, "Fallback": null}. Get throws ParseError: Entries: missing, and Map<Int32, Int32>'s Field Entries isn't @Optional. Save(Box<Int>(...)) panics: a @Flatten Field's Int32 is saved as an Object, and its ToValue gave 5

Suggested fix: Check each instance's concrete type with the same rules as check_converted (Map<K, V> with K not String, Abstract, @Flatten's own type, key clashes) when lowering first builds its Load or Save, and report at the generic Load<T>/Save call. Add tests for each case.

Skeptic: Ran s2.fb. Put(m) printed Map's internal Fields ({"Entries": [...], "Buckets": [...], "Shift": 61, ...}), and Get<Map<Int, Int>> threw 'Entries: missing, and Map<Int32, Int32>'s Field Entries isn't @Optional'. A non-generic Save(m) (s2c.fb) is a compile error: 'a Map's keys are an Object's, which are Strings'. s2b.fb, Save(Box<Int>("b", 5)) with @Flatten Field Inner: T, compiles and panics: 'a @Flatten Field's Int32 is saved as an Object, and its ToValue gave 5'. This breaks spec 13's rule that each instance's T is checked, and the rule that only Map<String, V> converts.

## sema-3 (medium): Keys of a @Flatten Field's Fields aren't checked against the others, so two Fields share a key and Save drops one

Status: fixed in 01edbb23, with data-1: the sema test has an outer/flattened clash, two @Flatten Fields of one type, a @Rename onto a flattened key, and a @Renamed key that's a flattened one's

Where: compiler/src/sema.cpp:321

load_key returns nullopt for a @Flatten Field, and clashing_keys only compares the type's own keys. The flattened Struct's or Class's Fields, which go in the same Object, are never compared with the outer Fields, with each other across two @Flatten Fields, or with @Renamed old keys. The spec says two Fields can't have one key. Here Save writes "Name" twice, so the later one wins and the outer Name is lost, and Load fills both Fields from the one entry. With two @Flatten Fields of one type plus a @Rename onto the same key, the saved Value doesn't load back at all.

```
Import std.core

Struct Inner
    Field Name: String
End Struct

Class Outer
    Field Name: String = "outer"
    @Flatten Field Part: Inner

    New()
        Part = Inner("inner")
    End New
End Class

Print(Save(Outer()))
Local o = Try Load<Outer>(Save(Outer()))
Print(o.Name)
Print(o.Part.Name)
```

Expected: A compile error at Save(Outer()) and Load<Outer>: Outer's Fields 'Name' and Part's 'Name' both have the key "Name"

Actual: Compiles and prints {"Name": "inner"}, then inner, then inner (the outer Name is lost). With @Flatten Field Outer: Size, @Flatten Field Inner: Size, and @Rename("Width") Field Title: String, Save gives {"Width": "t", "Height": 2} and Load throws Width: expected Int32, found "t"

Suggested fix: In clashing_keys, expand each @Flatten Field into its type's keys, recursively and with inheritance, and compare them with the other keys and with @Renamed old keys. Name both Fields in the error. Add sema tests for an outer/flattened clash and for two @Flatten Fields of one type.

Skeptic: Ran s3.fb. It compiles and prints {"Name": "inner"}, then inner, then inner, so the outer Name is lost on Save and both Fields load from one entry. Spec 13 says @Flatten puts the Field's own Fields 'in the Object with the others' and that 'two Fields can't have one key'. Nothing checks the flattened keys against the outer ones.

## sema-4 (medium): @Flatten is accepted on a Map<String, V> or Value Field, which isn't a Struct or Class of Fields, and round trips fail or panic

Status: fixed in 01edbb23, with data-2

Where: compiler/src/sema.cpp:309

The @Flatten check only asks is_struct || is_class, and std.collections' Map and std.core's Value are both Classes. Both are converted specially (as an Object, or copied), not Field by Field. The spec says @Flatten puts a Struct's or Class's Field's own Fields in the Object. For a Map, Save merges its entries into the outer Object, but Load hands the whole Object, outer keys included, to the Map, so the Value Save made doesn't load back. For a Value holding a scalar, Save panics with a message about a ToValue that Value doesn't have.

```
Import std.core
Import std.collections

Class Holder
    Field Name: String = "h"
    @Flatten Field Extra: Map<String, Int>

    New()
        Extra = {"a": 1}
    End New
End Class

Class Other
    Field Name: String = "o"
    @Flatten Field Data: Value

    New()
        Data = Value(5)
    End New
End Class

Print(Save(Holder()))
Local h = Try Load<Holder>(Save(Holder()))
Print(h.Extra.Count)
Print(Save(Other()))
```

Expected: Compile errors on both @Flatten attributes: "@Flatten goes on a Struct's or Class's Field whose own type is a Struct or Class ...; 'Extra' is Map<String, Int32>" (and 'Data' is Value). Otherwise, a round trip that gives 1.

Actual: Prints {"Name": "h", "a": 1}, then uncaught ParseError: Name: expected Int32, found "h". Save(Other()) panics: a @Flatten Field's Value is saved as an Object, and its ToValue gave 5

Suggested fix: In check_load_attributes, also reject @Flatten when the Field's type is loading.value or an instance of loading.map. Arguably also when it implements Loadable or Saveable, since those panic at runtime today. Add these to the attribute test.

Skeptic: Ran s4.fb. Save(Holder()) printed {"Name": "h", "a": 1}, and Load<Holder> of that same Value threw 'Name: expected Int32, found "h"', so Load can't read back what Save wrote. Save(Other()), a @Flatten Field of type Value holding 5, panics: 'a @Flatten Field's Value is saved as an Object, and its ToValue gave 5'. Spec 13's @Flatten is about a Struct's or Class's own Fields, and Map and Value are converted specially, not Field by Field. Sema lets both through.

## sema-5 (medium): A missing Field with a default or zero throws MISSING, against decisions.md and milestones row 17

Status: fixed in COMMIT: the user chose decisions.md 635's rule, so a Field the Object leaves out takes its default, else its zero, else it's MISSING, and @Optional, which that rule makes needless, is gone; spec 13 and stdlib.md say so, core_load_errors loads an Object without a Field with a default and one with a zero, and is MISSING a Class Field with neither, nested and at the top

Where: docs/spec/13-compile-time.md:262

decisions.md (line 635) settles that reading data makes a T Field by Field: "the data's value, else the Field's default, else its zero, else an error naming it". Milestones row 17 says the same. The step instead makes every Field that isn't @Optional or a T? required: a Field with a default (Speed = 1.5) or a zero (Lives: Int) throws ParseError MISSING when the Object leaves it out. The new spec 13 text documents the stricter rule, but neither decisions.md nor milestones.md was updated, so the docs contradict each other and the settled decision. decisions.md line 771 also still names the attribute @Default, which the step calls @Optional.

```
Import std.core

Class Player
    Field Name: String = "nobody"
    Field Speed: Double = 1.5
    Field Lives: Int
End Class

Local p = Try Load<Player>(Value({"Name": Value("ann")}))
Print(p.Name, p.Speed, p.Lives)
```

Expected: ann 1.5 0 (per decisions.md and the milestones row: else the Field's default, else its zero)

Actual: uncaught ParseError: Speed: missing, and Player's Field Speed isn't @Optional at missingdef.fb:9:15

Suggested fix: The user needs to pick a rule. Either make Load fall back to the default or zero and error only without one (then @Optional mostly matters for types without a zero), or update decisions.md lines 635 and 771 and milestones row 17 to say a missing Field is MISSING unless @Optional or a T?, and that the attribute is @Optional.

Skeptic: Ran s5.fb. It throws 'uncaught ParseError: Speed: missing, and Player's Field Speed isn't @Optional' at s5.fb:9:15. decisions.md line 635 settles that Json.Read<T>, Args.Parse<T>, and db.Query<T> fill a T with 'the data's value, else the Field's default, else its zero, else an error naming it', and milestones row 17 says the same ('each Field gets the data's value, else its default, else its zero'). The step's new spec 13 text documents the stricter MISSING rule, but decisions.md and the milestones row weren't changed, and decisions.md line 771 still says @Default. The code contradicts the settled decision and the milestones row, so the user has to pick one rule.

## data-6 (low): An error from a top-level type's OnLoaded or FromValue has Path Null, though the docs say "" for the Value itself

Status: fixed in 01edbb23: Load<T>'s own error path gives a ParseError with no path the path "" (LoadAtTop); core_load_errors has a top-level FromValue's and OnLoaded's

Where: compiler/src/lower.cpp:7110

Spec 13 says a Load error's Path is where in the Value it is, or "" for the Value itself, and that an error FromValue or OnLoaded throws keeps the path to where it was thrown. Within only runs on the way out of an entry or element. So when OnLoaded or FromValue of the T that Load<T> is called on throws, the ParseError keeps the Null it was made with, and code that tells Load errors apart by Path != Null misses it.

```
Import std.core

Class Checked
    Field Count: Int

    Method OnLoaded() Throws ParseError
        If Count < 0 Then Throw ParseError(ParseErrorKind.OUT_OF_RANGE, "Count is below 0")
    End Method
End Class

Struct Grade Implements Loadable
    Field Mark: Int

    Static Method FromValue(value: Value): Grade Throws ParseError
        Throw ParseError(ParseErrorKind.OTHER, "no grade")
    End Method
End Struct

Try
    Local c = Load<Checked>(Value({"Count": Value(-1)}))
Catch e As ParseError
    Print("[{e.Path ?? "Null"}] {e.Message}")
End Try

Try
    Local g = Load<Grade>(Value(1))
Catch e As ParseError
    Print("[{e.Path ?? "Null"}] {e.Message}")
End Try
```

Expected: [] Count is below 0
[] no grade

Actual: [Null] Count is below 0
[Null] no grade

Suggested fix: In lower_load's top-level error path (or in FromValue's and OnLoaded's error check), set a ParseError's At to "" when it's Null, or change the docs to say a type's own error at the top keeps Null.

Skeptic: I ran d6.fb. It printed [Null] Count is below 0 and then [Null] no grade. Spec 13 says Path is where in the Value the error is, or "" for the Value itself, and an error FromValue or OnLoaded throws keeps the path to where it was thrown. At the top level that path is "", not Null, and ParseError's own comment in error.fb says Null is for another error.

## data-7 (low): A missing @Renamed Field is reported at its old key, not its own

Status: fixed in 01edbb23, with lower-7: a @Renamed Field the Object has under neither key is missing at its own key; core_load_errors has one

Where: compiler/src/lower.cpp:7616

load_field sets `step` to the old key before it knows whether the old entry exists. When neither key is there, the MISSING error's Path and Message name the old key ("colour"), which current saves never write, and not the key the Field has now ("Color").

```
Import std.core

Class Player
    @Renamed("colour") Field Color: String
End Class

Try
    Local p = Load<Player>(Value.Object())
Catch e As ParseError
    Print("[{e.Path ?? "Null"}] {e.Message}")
End Try
```

Expected: [Color] Color: missing, and Player's Field Color isn't @Optional

Actual: [colour] colour: missing, and Player's Field Color isn't @Optional

Suggested fix: Set step to the old key only when the old key's entry is found; otherwise keep the Field's own key for the MISSING error.

Skeptic: I ran d7.fb. It printed [colour] colour: missing, and Player's Field Color isn't @Optional. The Path and Message name the old key that current saves never write, while the text names Color. Spec 13 says the Path says where in the Value the error is. Reporting the old, fallback key is misleading, and it's a small fix.

## data-9 (low): Spec 13's Load and Save example doesn't compile

Status: fixed in 01edbb23: spec 13's Settings has a New that sets Keys, and core_load_spec runs the example as written

Where: docs/spec/13-compile-time.md:211

The Settings example declares `Field Keys: Map<String, Key>` with no New. The compiler rejects that, since a Map has no zero. Defining Key as an Enum, the class as written fails.

```
Import std.core
Import std.collections

Enum Key
    UP, DOWN
End Enum

Class Settings
    @Rename("player_name") Field Name: String
    @Optional Field Volume: Double = 0.8
    @Skip Field Cache: Int[] = []
    Field Keys: Map<String, Key>

    Method OnLoaded() Throws ParseError
        If Volume > 1.0 Then Throw ParseError(ParseErrorKind.OUT_OF_RANGE, "Volume is past 1")
    End Method
End Class

Local settings = Try Load<Settings>(Value({"player_name": Value("a"), "Keys": Value.Object()}))
Local saved = Save(settings)
Local loaded = Try Load<Settings>(saved)
Print(saved)
```

Expected: {"player_name": "a", "Volume": 0.8, "Keys": {}}

Actual: error: Settings needs a New that assigns 'Keys', since Map<String, Key> has no zero value

Suggested fix: Give the example a New that assigns Keys, or make Keys a Map<String, Key>?, or another type with a zero.

Skeptic: I ran d9.fb, which is spec 13's Settings class as written, plus a Key Enum. It fails with: Settings needs a New that assigns 'Keys', since Map<String, Key> has no zero value. Spec 13's own example doesn't compile.

## lower-6 (low): Save's depth panic reports std/core/load.fb's line, not the Save call

Status: fixed in 01edbb23: Save's helpers hand a depth past MostDepth, and a @Flatten Field's ToValue that isn't an Object, back as an Error, which the generated code passes out, and Save(x) panics with its Message at the call; panic_save_deep and panic_save_flattened now say their programs' lines

Where: std/core/load.fb:343

stdlib.md's conventions say that a bug std panics on panics at the caller's line (@PanicsAtCaller). SaveArray and SaveObject panic inside the generated Save functions, so the panic names std/core/load.fb:343:32 and not the user's Save(x). panic_save_flattened shows the same thing (load.fb:363:9). The .out files encode this, but it breaks the convention and doesn't tell the user which Save went wrong.

```
Import std.core

Class Node
    Field Next: Node?
End Class

Local node = Node()
node.Next = node
Print(Save(node))
```

Expected: panic: Save goes 256 levels deep into Fields and elements, and no further; ... at p.fb:9:7

Actual: panic: Save goes 256 levels deep into Fields and elements, and no further; a Class that holds itself, inside it, can't be saved at std/core/load.fb:343:32

Suggested fix: Pass the Save call's place through the generated Save functions, as @PanicsAtCaller passes a caller's place, or panic with the span lower_save has. Update panic_save_deep.out and panic_save_flattened.out.

Skeptic: Debug and release both panic with '... can't be saved at std/core/load.fb:343:32', not at the user's Save call. stdlib.md's conventions say a bug std panics on panics at the caller's line (@PanicsAtCaller). The place names std's internals and doesn't say which Save failed.

## lower-7 (low): A MISSING @Renamed Field's Path names its old key, not its key

Status: fixed in 01edbb23, with data-7

Where: compiler/src/lower.cpp:

load_field's fallback block always sets `step` to the @Renamed key, whether or not the old entry is there. When neither key is there, the MISSING error's Path and Message name the old key ("colour"), which the program no longer writes. The error should name the Field's current key ("Color"). Nothing tests a missing @Renamed Field.

```
Import std.core

Class Settings
    @Renamed("colour") Field Color: String
End Class

Try
    Local s = Load<Settings>(Value.Object())
Catch e As ParseError
    Print("[{e.Path ?? "Null"}] {e.Message}")
End Try
```

Expected: [Color] Color: missing, and Settings's Field Color isn't @Optional

Actual: [colour] colour: missing, and Settings's Field Color isn't @Optional

Suggested fix: Set `step` to the old key only when the old key's entry is found, or reset it to the key in the absent block before LoadMissing. Add the case to core_load_errors.fb.

Skeptic: Debug and release both print '[colour] colour: missing, and Settings's Field Color isn't @Optional'. In lower.cpp, load_field's fallback block always assigns `step` the @Renamed key before it checks whether that entry was found. So when neither key is there, the MISSING error names the old key, which the program no longer writes, and not the Field's own key, Color.

## lower-8 (low): For one Value alone read as a T[], the error's Path has an [0] that isn't in the Value

Status: fixed in 01edbb23: LoadWithinIndex leaves out the index of a Value alone read as a T[] (LoadAlone), in load_array and LoadBytes; core_load_errors checks the path finds the Value for a lone Object, a lone byte, and an element of a real Array

Where: compiler/src/lower.cpp:

When LoadElements (or LoadBytes) treats a lone Value as a one-element array, an error inside it still goes through LoadWithinIndex(0). The Path then says Sizes[0].Height, but Sizes is an Object, so Value.Path(e.Path) finds nothing. Spec 13 says Path 'says where in the Value it is'. core_load_errors.out encodes the same thing (dims[0].Height for a lone Object).

```
Import std.core

Struct Size
    Field Width: Int
    Field Height: Int
End Struct

Class Options
    Field Sizes: Size[] = []
End Class

Local v = Value({"Sizes": Value({"Width": Value(1), "Height": Value("x")})})
Try
    Local o = Load<Options>(v)
Catch e As ParseError
    Local path = e.Path ?? ""
    Print(path, v.Path(path) == Null)
End Try
```

Expected: Sizes.Height False

Actual: Sizes[0].Height True

Suggested fix: Have LoadElements say whether it wrapped a lone Value, and skip the index step for that one element, in load_array and in LoadBytes. Or document that the path counts the lone Value as element 0.

Skeptic: Debug and release both print 'Sizes[0].Height True', so v.Path(e.Path) finds nothing in the Value. Spec 13 says the Path says where in the Value the error is, but for a lone Value that Load reads as a T[], the path has an [0] that isn't in the Value.

## lower-9 (low): An Enum on UInt64 can't be loaded from an integer past Int64's largest, and the error gives the wrong range

Status: fixed in 01edbb23: LoadEnum reads an Enum on a UInt64 with LoadUnsigned; core_load loads Big.HUGE from its integer

Where: compiler/src/lower.cpp:

For an Enum, load_body passes signed_range(info.inner) to LoadEnum, and that range cuts a UInt64 at INT64_MAX. A member like HUGE = 18446744073709551615 loads by name, but not by its integer. The error also says 'from 0 to 9223372036854775807', which is wrong for the Enum's type.

```
Import std.core

Enum Big: UInt64
    SMALL = 1, HUGE = 18446744073709551615
End Enum

Try
    Print(Load<Big>(Value(18446744073709551615 As UInt64)))
Catch e As ParseError
    Print(e.Message)
End Try
```

Expected: HUGE

Actual: expected Big, from 0 to 9223372036854775807, found 18446744073709551615

Suggested fix: For an Enum on UInt64, read the integer with LoadUnsigned, not LoadSigned. Combined with lower-2's member check, that covers every member.

Skeptic: Debug and release both print 'expected Big, from 0 to 9223372036854775807, found 18446744073709551615'. Spec 13 says Load takes an Enum as an integer, but an Enum on UInt64 is cut at INT64_MAX, so its member HUGE can't be loaded from its integer, and the error states the wrong range for the type.

## sema-6 (low): An OnLoaded that a Class or Struct gets from an Interface isn't checked: one with parameters is silently never run

Status: fixed in 01edbb23: an Interface's OnLoaded is checked as a Struct's or Class's is, but a Struct's copy of it needn't be ByRef, as spec 13 now says; a sema test has one with a parameter

Where: compiler/src/sema.cpp:3804

check_on_loaded runs only from check_member_function, for a Struct's or Class's own members. An Interface's default Method OnLoaded(x: Int): Int is accepted. A Class that implements the Interface without its own OnLoaded gets it, and Load silently doesn't run it. The spec says OnLoaded takes and gives nothing, and the same Method declared in the Class is an error. A Struct also gets a non-ByRef OnLoaded this way, which the spec says must be ByRef.

```
Import std.core

Interface Checked
    Method OnLoaded(x: Int): Int
        Print("interface OnLoaded {x}")
        Return x
    End Method
End Interface

Class C Implements Checked
    Field X: Int
End Class

Local c = Try Load<C>(Value({"X": Value(1)}))
Print(c.X)
```

Expected: A compile error at the Interface's OnLoaded: "OnLoaded takes no parameters and returns nothing; Load<T> runs it" (the error the same Method gets in a Class)

Actual: Compiles and prints 1. The OnLoaded is never run and nothing is reported.

Suggested fix: Apply check_on_loaded's signature rules to an Interface's OnLoaded too, or to the OnLoaded each Struct or Class ends up with, inherited or from an Interface default. Decide whether a Struct may get a non-ByRef one from an Interface. Add a test.

Skeptic: Ran s6.fb. It compiles and prints 1, and the Interface's OnLoaded(x: Int): Int never runs and nothing is reported. The same Method declared in the Class (s6c.fb) is the error 'OnLoaded takes no parameters and returns nothing; Load<T> runs it'. s6b.fb shows an Interface's no-parameter OnLoaded does run, for a Class and for a Struct (where it isn't ByRef). So OnLoaded's signature check is skipped for an OnLoaded a type gets from an Interface, and a wrong one is silently ignored. It's a corner case, so I rated it low.

## sema-7 (low): An Interface's OnLoaded that throws a non-ParseError is reported once per type that implements it

Status: fixed in 01edbb23: check_loaded_type skips an OnLoaded a type got from an Interface, which the Interface's own check reports once; the sema test has two Classes implementing one

Where: compiler/src/sema.cpp:402

check_loaded_type walks each type_stmt's members, and an Interface default's symbol appears in the Interface and in every Class that implements it. So one bad Throws gives the same error at the same span 1 + N times.

```
Import std.core

Interface Checked
    Method OnLoaded() Throws IOError
        Print("interface OnLoaded")
    End Method
End Interface

Class C Implements Checked
    Field X: Int
End Class

Class D Implements Checked
    Field X: Int
End Class

Print("ok")
```

Expected: One error: ifaceon3.fb:4:12: error: OnLoaded throws only ParseErrors, which Load<T>, which runs it, throws; IOError isn't one

Actual: The same error three times at 4:12, then: couldn't build ifaceon3.fb because of 3 errors

Suggested fix: Skip members whose declaration isn't in this type_stmt (an Interface default copied into an implementer), or keep a set of OnLoaded symbols already checked. Add a test.

Skeptic: Ran s7.fb. The same error, 's7.fb:4:12: error: OnLoaded throws only ParseErrors ... IOError isn't one', appears three times at one span (once for the Interface, once for C, once for D), then 'couldn't build s7.fb because of 3 errors'. That's a duplicated diagnostic.

## Refuted by the skeptics (no action)

- sema-8: An instance's can't-convert error doesn't say where the bad part is or which instance it is. Ran s8.fb. The build fails with 's8.fb:7:55: error: Load can't make Func(): Int32 here; ...' at the generic Load<T> call. The bad instance is rejected at compile time and the error names the type that can't convert. The lane's settled call is that generic instances are checked in lowering. lower.cpp:7188 cannot_convert reports through m_module.error, lowering's usual one-error mechanism, and other lowering errors don't name the instance either. The 'says which part can't and where' in the brief is about Sema's checks. A missing Field path and instance name is a diagnostic-quality wish, not a contradiction of spec 13, which only says the compiler checks it can. Refuted.
- data-4: @Optional can't go on a Map (or other Class) Field: the error asks for a default the language rejects. It reproduces: both errors appear for d4.fb. But spec 02 line 180 says a field's default must be known at compile time, or be [], so a {} default is outside the language by spec. Spec 13 says a Field that @Optional leaves at its default needs a type with a zero or a default of its own. A Map has neither, so rejecting @Optional here matches the spec, and Map<...>? is the way that works. This is a design limit, not a bug against the docs.
- data-8: std's own collections (Set, Deque, ...) are saved as their private internals, and Set<Int> can't be saved. It reproduces: Deque saves as {"Front": [], "Back": [1, 2], "Changes": 2}, Set<String> as {"Items": {"a": true}}, and Set<Int> fails to compile. But stdlib.md (line 707 on) says Load and Save cover Structs and Classes by reflection, "every Field included, Private ones too". Spec 13 lists only T[], UInt8[], and Map<String, V> as having their own forms, and says a Map's keys must be Strings. The behavior is what the docs say. A natural form for std's collections would be a design change, not a bug.
- data-10: The milestones row says a missing Field takes its default, else its zero; Load throws MISSING instead. It reproduces: d10.fb prints MISSING Port: missing, and Config's Field Port isn't @Optional. But in milestones.md row 17 the parenthesis "(each Field gets the data's value, else its default, else its zero, else an error names it)" describes std.os's Args.Parse<T>(), not Load<T>. Load's rule is set by spec 13 and stdlib.md, and the behavior matches them. Args.Parse isn't part of this change, so the row doesn't contradict it.
