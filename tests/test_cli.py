"""Exercise public CLI behavior and parse its actual output, without a shell."""
import json
import pathlib
import subprocess
import sys
import tempfile

program = str(pathlib.Path(sys.argv[1]).resolve())

def run(args=(), data=b"", code=0, cwd=None):
    result = subprocess.run([program, *args], input=data, capture_output=True, cwd=cwd, timeout=15)
    assert result.returncode == code, (args, result.returncode, result.stderr)
    return result

assert run(["-"], b"hello\r\n").stdout == b"<p>hello</p>\n"
assert run(["--", "-"], b"hello").stdout == b"<p>hello</p>\n"
assert run(["--max-input-bytes", "3"], b"abc").stdout == b"<p>abc</p>\n"
assert run(["--max-input-bytes", "3"], b"abcd", code=1).stdout == b""
assert run(["--max-input-bytes", "65536"], b"a" * 65536).stdout.startswith(b"<p>")
run(["--max-input-bytes", "65536"], b"a" * 65537, code=1)
run(["--max-nodes", "2"], b"x", code=1)
run(["--max-nesting", "2"], b"**x**", code=1)
run(["--validate-utf8"], b"\xff", code=1)
assert run(["--safe"], b"<b>x</b>").stdout == b"<p>&lt;b&gt;x&lt;/b&gt;</p>\n"
assert run(["--html5"], b"a  \nb\n\n---").stdout == b"<p>a<br>\nb</p>\n<hr>\n"
assert run(["--soft-break-as-space"], b"a\nb").stdout == b"<p>a b</p>\n"
for args in [["--to"], ["--to", "bad"], ["--unknown"], ["a", "b"],
             ["--max-nodes"], ["--max-nodes", "-1"], ["--max-nodes", ""],
             ["--max-nodes", "1x"], ["--max-nesting", "999999999999999999999999999999"]]:
    run(args, code=2)
with tempfile.TemporaryDirectory() as directory:
    pathlib.Path(directory, "--input.md").write_bytes(b"**file**\n")
    assert run(["--", "--input.md"], cwd=directory).stdout == b"<p><strong>file</strong></p>\n"
    run(["missing.md"], code=2, cwd=directory)
for markdown in [b"", b"**x**", b"![a *b*](u) after", b"- [x] done\n\n| a |\n| - |\n| b |\n",
                 b"bad \xff\xc0\xaf\xed\xa0\x80", "中文 😀".encode()]:
    pretty = run(["--to", "ast"], markdown).stdout
    compact = run(["--to", "ast", "--compact"], markdown).stdout
    assert json.loads(pretty) == json.loads(compact)
    assert b"\n" not in compact
    assert run(["--to", "events"], markdown).stdout.startswith(b"enter document\n")
print("CLI regression checks passed")
