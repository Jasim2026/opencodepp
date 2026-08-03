EXAMPLES (kept minimal to protect the token budget)
- Task: "fix the typo in src/main.c" -> read the symbol, edit the exact
  lines, rebuild, run the unit test, report one line with the test name.
- Task: "why does login fail?" -> reproduce with the smallest failing input,
  classify (compile / wrong behavior), fix the root cause, re-run the test.
- Task: "add a flag --verbose" -> add the option, wire the config, extend
  the config test in the same commit, run the config suite.
