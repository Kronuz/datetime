# AGENTS.md

Working notes for agents modifying this repository. For the design read
`ARCHITECTURE.md`; for usage read `README.md`. This file covers the repo layout,
how to build and test, the invariants you must not break, and the traps.

## Repo map

```
datetime.h                   Datetime namespace API: tm_t / clk_t / Format, the parsers/formatters, DatetimeError hierarchy. Header.
datetime.cc                  The pure-core implementation: ISO 8601 + date-math + time + timedelta parsing/formatting. Compiled.
test/test.cc                 Runnable smoke test: ISO parses, date math, timestamp round-trip, time/timedelta, throwing paths, predicates.
CMakeLists.txt               STATIC library `datetime` (+ alias datetime::datetime); FetchContents strict-stox; CTest test `datetime`.
LICENSE                      MIT, Copyright (c) 2015-2019 Dubalu LLC.
README.md                    What it is, scope, install, usage.
ARCHITECTURE.md              Parse paths, numeric core, the decoupling delta from Xapiand.
```

This is a compiled library (one `.cc`), not header-only. The CMake target is a
`STATIC` library that links `strict_stox::strict_stox` PUBLIC and requests
`cxx_std_20`.

## Build and run the test

```sh
cmake -B build && cmake --build build && ctest --test-dir build
```

Expected output ends with `all datetime tests passed`, exit 0. The test target is
`datetime_test`; the registered CTest name is `datetime`.

## Conventions

- **C++20.** The target requests `cxx_std_20` to stay uniform with the sibling
  libraries and because the code uses `std::format`. Don't drop it.
- **One external dependency: strict-stox.** The only third-party include is
  `strict_stox.hh`. Do not add `hashes` / `phf` / MsgPack back; that machinery
  belongs to the Xapiand glue, not here (see "Standalone vs. Xapiand").
- **Filenames are stable.** `datetime.h` / `datetime.cc` keep their original
  Xapiand names so a consumer that already `#include "datetime.h"` just needs
  this repo on the include path. Don't rename them.
- Tabs for indentation, double quotes in code, no em dashes in prose.

## Load-bearing invariants

- **The exception hierarchy is `std::runtime_error`-based.** `DatetimeError :
  std::runtime_error`, and `DateISOError` / `TimeError` / `TimedeltaError` :
  `DatetimeError`. Names and the message-string constructors are the contract:
  Xapiand catches these exact types. Don't reintroduce a `ClientError` /
  located-exception base, and don't change which parser throws which subclass
  (datetime/date-math -> `DatetimeError`, time -> `TimeError`, timedelta ->
  `TimedeltaError`).
- **`Iso8601Parser` returns a `Format`, it does not throw.** The `INVALID` case
  is load-bearing: `process_date_datetime` falls through to the regex on
  `INVALID`. If you make it throw, the regex fallback dies.
- **Date-math rounding direction.** `/unit` rounds *up* (end of unit, fsec =
  `DATETIME_MAX_FSEC`); `//unit` rounds *down* (start of unit, fsec 0). The test
  pins both; keep them straight.
- **`strict_*` nothrow variant drives the scanners.** The hand-written ISO/time
  scanners call `strict_stoul(&errno_save, ...)` and branch on `errno_save`
  rather than catching. Keep using the nothrow overload inside the scanners; the
  throwing overload would change control flow.
- **Year > 0 domain.** `timegm` / `timestamp` / `toordinal` are proleptic
  Gregorian from year 1; `isValidDate` rejects year < 1. Don't feed year 0.

## How to extend

- **Add a format/unit.** Extend the relevant length case in `Iso8601Parser` (both
  overloads) or a `computeDateMath` unit. Keep the nothrow `strict_*` + branch
  pattern and renormalize via `timegm`/`gmtime_r` after a math op.
- **Always extend the smoke test.** `test/test.cc` is the only executable check.
  Cover the new happy path and at least one malformed input that must throw.

## Traps

- **`/d` rounds to end-of-day, not start.** Easy to get backwards when writing a
  test expectation (this exact mistake is why the test documents both `/d` and
  `//d`).
- **`computeTimeZone` is date math under the hood.** A `+05:30` offset is applied
  as `-5h -30m`, so an offset shifts the stored `tm_t` to the UTC instant (and
  can roll the date). That is intended; don't "fix" it to keep wall-clock fields.
- **Two `Iso8601Parser` overloads must stay in sync.** One fills a `tm_t`, one
  only validates. A format accepted by one but not the other will make
  `isDatetime` and `DatetimeParser` disagree.

## Standalone vs. Xapiand

This is a standalone extraction from
[Xapiand](https://github.com/Kronuz/Xapiand), and specifically the **pure
string/number half** of `src/datetime.{h,cc}`. The delta is decoupling:
exceptions reparented to `std::runtime_error`; `THROW(...)` -> `throw
X(std::format(...))`; `strings::format` -> `std::format`; `repr(...)` quoting
inlined; logging dropped.

The **MsgPack structured-object half** (the `process_date_*` static helpers and
the `DatetimeParser(const MsgPack&)` / `time_to_double(const MsgPack&)` /
`timedelta_to_double(const MsgPack&)` overloads) was *not* extracted. It is the
only user of MsgPack, the `RESERVED_*` tokens, and `phf::make_phf` / `hh(...)`,
and it lives in Xapiand as glue (`datetime_msgpack.{h,cc}`) that pulls the value
out of the MsgPack and delegates to this library's string/double parsers. Keep
that boundary: anything MsgPack- or reserved-token-shaped does not belong here.
