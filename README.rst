dryfinder
=========

A small C++20 command-line tool that scans a set of files selected by
shell-style globs (including ``**``) and finds identical, *repeated
blocks* of at least ``N`` lines appearing in two or more places (across
files or within the same file). It prints the findings as YAML, sorted by
block length (descending) and then by number of occurrences.

Build
-----

.. code-block:: bash

   ./build.sh

The binary will be at ``./build/dryfinder``.

Usage
-----

.. code-block:: bash

   ./build/dryfinder --min-lines 9 "./foo/**/*.cpp" "*.c"

Options:

- ``--min-lines N`` (required): Minimum number of lines in a block to be
  considered a duplicate seed.
- ``--debug`` (optional): Print diagnostic information to **stderr**.
- Globs: One or more glob patterns. Supports ``*``, ``?``, and ``**``
  (recursive). Bracket ``[]`` classes are not supported. Patterns are
  matched relative to a computed **base directory** (portion before the
  first glob-char).
- The program outputs YAML to **stdout**.

Output schema (example)
-----------------------

.. code-block:: yaml

   blocks:
     - lines: 12
       bytes: 423
       occurrences: 3
       hits:
         - file: path/to/a.cpp
           start_line: 10
           end_line: 21
         - file: path/to/b.cpp
           start_line: 100
           end_line: 111
         - file: path/to/c.hpp
           start_line: 5
           end_line: 16
       content: |
         // the repeated 12-line block...

Notes & Limitations
-------------------

- Matching is **exact line-by-line** (after normalizing CRLF to LF and
  removing a UTF-8 BOM if present on the first line).
- Seeds are found using ``--min-lines``; each seed group is **maximally
  extended** both backward and forward as long as *all* occurrences in
  that group keep matching. Identical maximal blocks discovered via
  different seeds are de-duplicated and their hits merged.
- For performance and simplicity, bracket character classes like
  ``[a-z]`` in globs are **not** supported. Use multiple patterns if
  needed.
- Very large repositories may take a while to scan; consider narrowing
  your globs. Use ``--debug`` to see progress.
