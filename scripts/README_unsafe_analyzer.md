# Unsafe source metrics

Run from the repository root:

```sh
python3 unsafe_analyzer.py
python3 unsafe_analyzer.py --json > /tmp/unsafe-before.json
python3 unsafe_analyzer.py --baseline /tmp/unsafe-before.json
python3 unsafe_analyzer.py kernel/net.rs kernel/sync --json
python3 -m unittest discover -s scripts -p 'test_unsafe_analyzer.py'
```

The default scan includes only `kernel/**/*.rs`, excluding `target`, `build`,
`build_*`, `build-*`, and `cmake-build-*` directories. Explicit file/directory
arguments use the same exclusions. Files are deduplicated and JSON output is
sorted for reproducible comparisons. `--baseline` accepts JSON from the same
schema version and reports aggregate changes, including added/removed files.
Compare the same selected paths and configurations of this analyzer each time.

The analyzer reports separate counts for `unsafe` keywords, blocks, functions
with bodies, function declarations/pointer types, impls, traits, extern blocks,
and known `u!` invocations. Comment and literal contents do not contribute
keywords or delimiters. Nested block comments, escaped strings, raw strings,
byte/C strings, characters, raw identifiers, and lifetimes are handled without
moving the remaining source positions.

`unsafe_lines` is the union of physical source lines spanned by unsafe block
bodies, unsafe function bodies, and `u!` arguments. Their delimiters, comments,
and blank lines inside those bodies contribute to this measure. Overlapping
scopes count each line once. Unsafe impls, traits, extern blocks, and function
pointer types do **not** make their enclosing bodies unsafe. The percentage is
`100 * unsafe_lines / total_lines`; sites per 1,000 lines uses the sum of
`unsafe` keywords and `u!` invocations. Different measures answer different
questions and should not be interchanged with keyword density in another
kernel.

This is a lexical source census, not a Rust compiler, macro expander, or safety
proof. It counts every `cfg` branch, tests embedded in production source, and
macro definitions as written. A definition containing `unsafe` is counted once
at its definition, and known `u!` calls are counted separately; expansion sites
for other macros are not inferred. Macro metavariables and unusual generated
syntax may not classify like compiled Rust. Function signature recognition
skips balanced parameters and const generics but does not perform Rust type or
name resolution. Syntax errors such as unmatched delimiters and unterminated
comments/strings produce a diagnostic instead of a partial report.

Moving an unsafe block into a safe-looking wrapper can reduce these counts
while leaving an unsound API. Source metrics must accompany reviews of
ownership, synchronization, raw-pointer contracts, and runtime regressions.
