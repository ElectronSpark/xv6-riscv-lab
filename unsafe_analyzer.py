import glob
import os
import re

def analyze_unsafe(filepath):
    with open(filepath, "r") as f:
        content = f.read()
    
    lines = content.splitlines()
    total_lines = len(lines)
    if total_lines == 0:
        return 0.0, 0, 0

    unsafe_lines = set()
    
    # Simple brace matching parser
    clean_content = content
    # Remove single line comments
    clean_content = re.sub(r"//.*", "", clean_content)
    # Remove block comments
    clean_content = re.sub(r"/\*.*?\*/", "", clean_content, flags=re.DOTALL)
    
    pattern = re.compile(r"\bunsafe\b")
    for match in pattern.finditer(clean_content):
        start_idx = match.start()
        post = clean_content[start_idx:]
        brace_pos = post.find("{")
        semi_pos = post.find(";")
        
        if brace_pos != -1 and (semi_pos == -1 or brace_pos < semi_pos):
            depth = 0
            abs_start = start_idx + brace_pos
            line_start = content[:abs_start].count("\n")
            
            for j in range(abs_start, len(clean_content)):
                c = clean_content[j]
                if c == "{":
                    depth += 1
                elif c == "}":
                    depth -= 1
                    if depth == 0:
                        abs_end = j
                        line_end = content[:abs_end].count("\n")
                        for l in range(line_start, line_end + 1):
                            unsafe_lines.add(l)
                        break
        else:
            line_idx = content[:start_idx].count("\n")
            unsafe_lines.add(line_idx)
            
    # Also add lines that literally contain unsafe
    for i, line_content in enumerate(lines):
        # Ignore comments
        stripped = line_content.split("//")[0]
        if "unsafe" in stripped:
            unsafe_lines.add(i)

    unsafe_len = len(unsafe_lines)
    return (unsafe_len / total_lines) * 100, unsafe_len, total_lines

rs_files = glob.glob("**/*.rs", recursive=True)
results = []
for f in rs_files:
    if os.path.isdir(f) or "target/" in f:
        continue
    pct, ul, tot = analyze_unsafe(f)
    results.append((f, pct, ul, tot))

results.sort(key=lambda x: x[1], reverse=True)
for f, pct, ul, tot in results:
    if pct > 0:
        print(f"{f}: {pct:.1f}% ({ul}/{tot} lines are inside unsafe context)")
