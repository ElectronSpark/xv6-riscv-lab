"""Regression tests for source metrics; run with `python3 -m unittest discover -s scripts -p 'test_unsafe_analyzer.py'`."""

import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
import unsafe_analyzer as analyzer


class RustLexingTests(unittest.TestCase):
    def test_comments_keep_original_offsets_and_lines(self):
        source = "// a very long comment before the block\n/* first\n second */\nfn f() {\n    unsafe {\n        work();\n    }\n}\n"
        masked = analyzer.mask_non_code(source)
        self.assertEqual(len(masked), len(source))
        self.assertEqual([i for i, char in enumerate(masked) if char == "\n"],
                         [i for i, char in enumerate(source) if char == "\n"])
        metric, lines = analyzer.unsafe_line_numbers(source)
        self.assertEqual(lines, {5, 6, 7})
        self.assertEqual(metric.unsafe_blocks, 1)
        self.assertEqual(metric.total_lines, 8)

    def test_nested_comments_never_supply_keywords_or_delimiters(self):
        source = "/* outer unsafe { /* inner */ unsafe fn fake() { } */\nfn safe() {}\n"
        metric = analyzer.analyze_source(source)
        self.assertEqual(metric.unsafe_keywords, 0)
        self.assertEqual(metric.unsafe_lines, 0)

    def test_all_string_forms_hide_keywords_and_braces(self):
        source = r'''fn f() {
    let a = "escaped \" quote; unsafe { // }";
    let b = b"unsafe /* { */";
    let c = c"unsafe {";
    let d = r###"unsafe { "## still raw //"###;
    let e = br#"unsafe }"#;
    let f = cr##"unsafe {"##;
    let g = r"unsafe {";
}'''
        metric = analyzer.analyze_source(source)
        self.assertEqual(metric.unsafe_keywords, 0)
        self.assertEqual(metric.unsafe_lines, 0)

    def test_characters_lifetimes_and_raw_identifiers_are_distinct(self):
        source = r'''fn f<'a>(s: &'a str) -> &'a str {
    let a = '}'; let b = '"'; let c = '\''; let d = '\\';
    let e = '\u{7b}'; let f = b'{';
    let r#unsafe = s;
    'label: loop { break 'label; }
    r#unsafe
}'''
        self.assertEqual(analyzer.analyze_source(source).unsafe_keywords, 0)
        self.assertIn("'a", analyzer.mask_non_code(source))

    def test_unicode_identifiers_do_not_split_into_unsafe_keyword(self):
        metric = analyzer.analyze_source("fn f() { let éunsafe = 0; let e\u0301unsafe = 1; }")
        self.assertEqual(metric.unsafe_keywords, 0)

    def test_string_comment_markers_and_character_quotes_do_not_confuse_lexer(self):
        source = "fn f() { let x = '\"'; let y = \"/* unsafe */\"; unsafe { work(); } }"
        metric = analyzer.analyze_source(source)
        self.assertEqual(metric.unsafe_keywords, 1)
        self.assertEqual(metric.unsafe_blocks, 1)

    def test_unterminated_input_is_reported_instead_of_silently_mismeasured(self):
        for source in ('/* nested /* */', 'let x = "unfinished', 'r##"unfinished"#', 'unsafe {'):
            with self.subTest(source=source), self.assertRaises(ValueError):
                analyzer.analyze_source(source)


class UnsafeScopeTests(unittest.TestCase):
    def test_nested_unsafe_scopes_are_not_double_counted(self):
        source = "unsafe fn f() {\n    unsafe {\n        work();\n    }\n}\n"
        metric, lines = analyzer.unsafe_line_numbers(source)
        self.assertEqual(metric.unsafe_functions, 1)
        self.assertEqual(metric.unsafe_blocks, 1)
        self.assertEqual(lines, set(range(1, 6)))
        self.assertEqual(metric.unsafe_lines, 5)

    def test_unsafe_impl_trait_and_extern_do_not_cover_safe_bodies(self):
        source = '''unsafe impl Send for T {
    fn f() { safe(); }
}
unsafe trait Trait {
    fn g() { safe(); }
    unsafe fn h();
}
unsafe extern "C" {
    fn external();
}
'''
        metric = analyzer.analyze_source(source)
        self.assertEqual(metric.unsafe_impls, 1)
        self.assertEqual(metric.unsafe_traits, 1)
        self.assertEqual(metric.unsafe_externs, 1)
        self.assertEqual(metric.unsafe_function_declarations, 1)
        self.assertEqual(metric.unsafe_functions, 0)
        self.assertEqual(metric.unsafe_lines, 0)

    def test_explicit_unsafe_body_inside_impl_still_counts(self):
        source = "unsafe impl Trait for T {\n    fn f() { unsafe { work(); } }\n}"
        metric, lines = analyzer.unsafe_line_numbers(source)
        self.assertEqual(metric.unsafe_impls, 1)
        self.assertEqual(metric.unsafe_blocks, 1)
        self.assertEqual(lines, {2})

    def test_unsafe_extern_function_and_const_generic_body(self):
        source = '''unsafe extern "C" fn foo<const N: usize = { 1 }>(
    callback: unsafe fn(),
) -> [u8; { 2 }] where X: Fn() -> Y {
    work();
}
'''
        metric, lines = analyzer.unsafe_line_numbers(source)
        self.assertEqual(metric.unsafe_functions, 1)
        self.assertEqual(metric.unsafe_function_declarations, 1)
        self.assertEqual(lines, {3, 4, 5})

    def test_function_pointer_never_claims_enclosing_safe_function_body(self):
        source = "fn safe(callback: unsafe extern \"C\" fn()) {\n    safe_work();\n}\ntype Handler = unsafe fn();"
        metric = analyzer.analyze_source(source)
        self.assertEqual(metric.unsafe_functions, 0)
        self.assertEqual(metric.unsafe_function_declarations, 2)
        self.assertEqual(metric.unsafe_lines, 0)

    def test_u_macro_all_delimiters_and_qualified_paths(self):
        source = "fn f() {\n    u! { one(); }\n    crate::u!(two());\n    u![three()];\n}"
        metric, lines = analyzer.unsafe_line_numbers(source)
        self.assertEqual(metric.unsafe_macros, 3)
        self.assertEqual(metric.unsafe_keywords, 0)
        self.assertEqual(lines, {2, 3, 4})

    def test_macro_definitions_are_measured_as_written_not_expanded(self):
        source = "macro_rules! u { ($($x:tt)*) => { unsafe { $($x)* } }; }\nu!{work();}"
        metric = analyzer.analyze_source(source)
        self.assertEqual(metric.unsafe_blocks, 1)
        self.assertEqual(metric.unsafe_macros, 1)
        self.assertEqual(metric.unsafe_lines, 2)

    def test_empty_source_and_compatibility_interface(self):
        self.assertEqual(analyzer.analyze_source("").report()["percentage"], 0.0)
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "empty.rs"
            path.write_text("")
            self.assertEqual(analyzer.analyze_unsafe(path), (0.0, 0, 0))


class ReportingTests(unittest.TestCase):
    def test_build_directories_are_pruned_and_explicit_paths_deduplicated(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for directory in ("src", "target", "build", "build_qemu", "cmake-build-debug"):
                (root / directory).mkdir()
                (root / directory / "module.rs").write_text("unsafe { f(); }")
            paths = list(analyzer.rust_files([root, root / "src/module.rs"]))
            self.assertEqual(paths, [(root / "src/module.rs").resolve()])
            self.assertEqual(analyzer.make_report([root])["totals"]["files"], 1)

    def test_json_reports_and_baseline_deltas_are_reproducible(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "source.rs"
            path.write_text("fn f() {}\n")
            command = [sys.executable, str(ROOT / "unsafe_analyzer.py"), str(path), "--json"]
            before = subprocess.check_output(command, text=True)
            self.assertEqual(before, subprocess.check_output(command, text=True))
            baseline = Path(temporary) / "baseline.json"
            baseline.write_text(before)
            path.write_text("fn f() { unsafe { g(); } }\n")
            after = json.loads(subprocess.check_output(command + ["--baseline", str(baseline)], text=True))
            self.assertEqual(after["delta"]["unsafe_keywords"], 1)
            self.assertEqual(after["delta"]["unsafe_blocks"], 1)
            self.assertEqual(after["delta"]["unsafe_lines"], 1)
            self.assertEqual(after["delta"]["total_lines"], 0)

    def test_import_does_not_print_or_scan_files(self):
        output = subprocess.check_output([sys.executable, "-c", "import unsafe_analyzer"], cwd=ROOT)
        self.assertEqual(output, b"")


if __name__ == "__main__":
    unittest.main()
