#!/usr/bin/env python3
"""Keep netatop in the foreground and propagate BPF attach errors to Docker."""
from pathlib import Path

source = Path("netatop.c")
code = source.read_text()
edits = [
    ("\tif ( fork() )\n\t\texit(0);\n\tsetsid();\n", "\t/* Container supervisor owns this foreground process. */\n"),
    ("\t\tcleanup(skel);\n\t}\n}\n\nvoid bpf_destroy", "\t\tcleanup(skel);\n\t\texit(EXIT_FAILURE);\n\t}\n}\n\nvoid bpf_destroy"),
]
for before, after in edits:
    if code.count(before) != 1:
        raise SystemExit(f"netatop source changed; expected exactly one {before!r}")
    code = code.replace(before, after, 1)
if "#include <stdlib.h>" not in code:
    code = code.replace("#include <stdio.h>\n", "#include <stdio.h>\n#include <stdlib.h>\n", 1)
source.write_text(code)

# glibc 2.43 returns const char * for strchr(const char *, ...) in this build.
# Both vendored libbpf copies use the result only for pointer arithmetic.
for libbpf in (Path("libbpf/src/libbpf.c"), Path("bpftool/libbpf/src/libbpf.c")):
    code = libbpf.read_text()
    before = "\t\t\tchar *next_path;\n\t\t\tint seg_len;\n"
    after = "\t\t\tconst char *next_path;\n\t\t\tint seg_len;\n"
    if code.count(before) != 1:
        raise SystemExit(f"libbpf source changed: {libbpf}")
    libbpf.write_text(code.replace(before, after, 1))
