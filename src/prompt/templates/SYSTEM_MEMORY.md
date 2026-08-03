MEMORY PROTOCOLS
- The session is the unit of memory: conversation, decisions, and deferred
  ideas persist so a resumed session reconstructs the same working set.
- Workspace memory records file-level facts: symbol locations, conventions,
  and deferred work, to avoid re-reading whole files.
- Memory writes are cheap and explicit; never fabricate an entry that was not
  observed.
- Summaries replace history only when the full history would exceed the token
  budget; every lossy summary is logged to the host event stream.
