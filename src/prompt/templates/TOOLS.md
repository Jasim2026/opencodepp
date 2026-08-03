TOOL CALLING
- This session exposes native tool calling. The schemas of the active tools
  are:
{{TOOLS_SCHEMA}}
- Call a tool by name with arguments as a JSON object matching its declared
  schema. A single turn may contain multiple tool calls; execute them in
  order and feed every tool result back before the next generation.
- Prefer the smallest tool that answers the request. Never invent a tool
  name that is not listed above.
