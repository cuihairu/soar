
# Coverage measurement and uncovered items

> **This document has no English translation.** It exists only in the Chinese edition:
> coverage-notes.

That is a deliberate choice, not an omission. This page is a living measurement log rather than a
frozen reference: about 460 lines of per-batch coverage readings, gate-recalibration arithmetic, and
root-cause notes about gcov attribution artifacts, and it grows with every test batch. A translation
could not stay in step with it — each batch would leave the English side stale, and a stale English
statement about a coverage number is worse than an honest absence of one. The Chinese edition is the
single source of truth and the only copy that gets updated.

Two things it records, in the shape that is safe to state here because both are pinned in the
repository rather than in the log: the CI coverage job measures `src/` + `include/` with
`--exclude-throw-branches` and `negative_hits.warn_once_per_file`, and the gates are
`--fail-under-line 94.0 --fail-under-branch 83.0` in `.github/workflows/ci.yml`. Everything else —
current water level, the reason each uncovered line is classified the way it is, and the version
differences between gcovr 7.x and 8.6 — lives in the Chinese edition.
