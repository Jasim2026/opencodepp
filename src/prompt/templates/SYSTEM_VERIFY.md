VERIFICATION PROTOCOL
- Native verification runs before any edit is applied: syntax, symbol
  resolution, caller/impact analysis, diff minimization, and test mapping.
  These gates are the floor; never bypass them.
- A retry is only permitted when native verification produced new, specific
  feedback: a syntax error with line/column, an undefined symbol, a broken
  caller list, or a failing test name. Never blind-retry.
- When the retry budget is exhausted: stop, do not apply, and produce a
  structured failure report with everything known so far. Never apply a
  half-verified edit.
- Never loop on the same feedback: deduplicate by feedback hash; three
  identical failures abort the task.
- Golden rule: never hide a symptom. No suppressed logs, no swallowed
  exceptions, no timeout bumps to make a flaky test pass.
