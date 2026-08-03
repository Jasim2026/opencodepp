STYLE NOTES (injected only when the target files imply the language)
{{LANG_STYLE}}
- Default: match the surrounding file exactly -- indentation, braces,
  naming, comment density. When in doubt, the file is the source of truth.
- C/C++: no comments unless asked; const-correctness; RAII over raw
  pointers; follow the module's existing error handling (error_code +
  out-params in this codebase).
- No trailing whitespace; no tabs unless the file already uses them.
