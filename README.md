# Writing-line harness against the PS4 11.00 libSceFont

Evidence for AnyPS5 PR 3260 (`core/libs/prx/libSceFont/src/WritingLine.cpp`). Everything here is our own code. No
firmware file is included: you supply the PS4 11.00 `libSceFont.sprx` as a plain (decrypted) ELF yourself. The
file used for the numbers below has sha256
`2e57d191aa3a26aa6682cb8181c29adf3952b6f16ff2ca60124e4a890a49ca9d`. The function addresses in the sources
(`0x19830` Create, `0x198f0` WritesOrder, `0x19e40` GetOrderingSpace, `0x19f40` RefersRenderStep, `0x1a1a0`
Clear, `0x1a2a0` GetRenderMetrics, `0x1a2f0` Destroy) belong to that file only.

This is PS4 behaviour. The PS5 library was not measured.

## What it does

The module is mapped into the process, its one import used here (`memset`) is pointed at the host's, and its
writing-line exports are called directly with a small fake library object that supplies `malloc` and `free`.
Linux x86-64 only.

- `vectors.cpp` runs the repository test `core/libs/tests/GuestFontWritingLine.cpp`, unmodified apart from a
  line-number trace, with the PS4 module in place of the repository implementation. `vectors.txt` is its output:
  under each `test line N` are the calls that assertion made and what the PS4 module returned. The run ends with
  `font writing line: ok` only if every assertion of the test holds on the module's output.
- `diff.cpp` and `diff2.cpp` link the repository implementation and the PS4 module side by side and compare them
  call for call on random sequences of writes, steps, metric reads, space reads and clears. `diff2` adds levels up
  to 126, inexact and special floats, and a caller that writes into the returned step.

## Where the repository implementation does not follow the module

Lines in `vectors.txt` that start with `harness:` mark these; they are decisions of the harness, not module output.

- The module returns `0x80460002` for a null line, an attribute whose low byte is not `0xFA`, a level above 126, a
  null orderer and a null metrics output. The repository implementation throws for all of them, because the codes
  are not confirmed for the PS5. The harness calls the module, prints its return value and throws.
- Attribute 0 is accepted by the repository implementation as `0xFA` with level 0. The module rejects it. The
  harness passes `0xFA` to the module in its place.
- Form `0x10` and a creation detail with id `0x0FD5` are accepted by the repository implementation. The module
  rejects both. The harness passes form `0x12` and a null detail.
- Handles that are not live lines throw in the repository implementation. The harness throws without calling the
  module, which would read the memory.
- `sceFontWritingLineGetOrderingSpace` with the second output set and the fourth null throws. The module writes
  through the null pointer there.
- `sceFontDestroyWritingLine` of a null pointer or null line returns 0 without calling the module.

## Run

```sh
sh build.sh <AnyPS5 checkout> <PS4 11.00 libSceFont.sprx> [output directory]
```

Needs `g++` with C++20. It builds three programs into the output directory (default `out/` beside the script),
writes `vectors.txt` there and prints one line per differential run. `diff2` arguments are
`module sequences maxLevel floatMode(0 grid, 1 inexact, 2 specials) writeStep maxLength seed [levelWalk]`.

Output on the PR head this was generated from (see `run.log`): 20,000 sequences / 411,762 calls,
50,000 / 2,024,854, 100,000 / 4,049,638 and 20,000 / 806,277 with special floats, all matching. In the last run
232 results differ only in the sign or payload of a NaN, which the harness counts and accepts.
