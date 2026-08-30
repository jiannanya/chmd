# CommonMark conformance fixture

`spec.json` is the unmodified conformance fixture downloaded from:

<https://spec.commonmark.org/0.31.2/spec.json>

It contains 652 examples from **CommonMark Spec 0.31.2**, authored by John MacFarlane and the CommonMark contributors. The specification is published under the Creative Commons Attribution-ShareAlike 4.0 International license:

<https://creativecommons.org/licenses/by-sa/4.0/>

The fixture is used only by `run_spec.py`; it is not linked into or distributed as part of the `chmd` runtime library.

`test_extensions.cpp` contains independent regression coverage for tables, task list items,
strikethrough spans, feature switches, AST metadata, event metadata, and mixed extension boundaries.

SHA-256: `d431b29d97b6f73e69d547109cf5081578fac931e72afe95639ebe766c1b2a20`

Run it directly with:

```sh
python tests/run_spec.py --program build/chmd --program-arg=--commonmark --spec tests/spec.json
```
