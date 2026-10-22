# Copilot Instructions

## Project Style Guide

This library is written to be easy for another engineer to read and reason about. Favor clear C++ over clever abstractions, and keep the code aligned with the patterns already used in the current `RealRHI` API.

### Naming and API shape
- Use `RealRHI` as the namespace and keep the public API straightforward and domain-specific.
- Use PascalCase for types and method names, e.g. `Device`, `CreateWindow`, `GetShaderDirectory`, `IsDebugEnabled`, `Submit`.
- Use `m_` for member variables and `s_` for static constants or shared state, matching the current Vulkan implementation files.
- Use camelCase for local variables and parameters, e.g. `queueFamilyIndices`, `outBuffer`, `enableValidationLayer`.
- Prefer descriptive names over abbreviations unless the abbreviation is already established in the codebase.
- Keep `Create...`, `Get...`, `Init...`, `Is...`, and `Submit...` naming patterns consistent so the API reads naturally.

### Readability and structure
- Keep code easy to scan: one responsibility per function, small helpers, and early returns for failure cases instead of deeply nested branches.
- Write for a human reader first. Do not optimize for cleverness if it makes the code harder to understand.
- Follow the current style of explicit initialization, direct control flow, and clear ownership boundaries.
- Match the existing tab-based indentation and brace placement used in the library's C++ headers and Vulkan implementation files.
- Keep spacing consistent and readable throughout the codebase: use whitespace deliberately around operators, after commas, and add clear newlines between logical blocks so the intent remains obvious at a glance.
- Always use braces for `if` statements, even when there is only one statement after them, to keep the control flow explicit and easy to audit.
- Keep functions and classes focused on a single concern, and avoid creating layers of indirection that hide the true behavior.
- Prefer straightforward logic and small, readable condition blocks over dense metaprogramming or hidden state.

### Class member organization
- Separate member function declarations from member variables in class headers.
- Use distinct access-specifier sections for function declarations and member variables, even when both sections are `private:`.
- The access specifiers themselves provide the separation; do not add section-label comments such as `// Functions` or `// Variables`.
- Keep nested types and aliases before declarations that depend on them. Do not expose internal implementation types publicly solely to resolve declaration ordering.
- Apply the same separation to other access levels when appropriate; keep the order clear and consistent.

### Headers and includes
- Use `#pragma once` for headers.
- Keep local headers grouped first and standard library includes after them, as seen in the current code.
- Prefer cleaning up include dependencies instead of relying on broad forward declarations when a header can be made self-contained.
- Keep the external interface stable and understandable; avoid unnecessary public surface area.

### Error handling and debugging
- Return `Result` for operations that can fail, and keep failure paths explicit.
- Surface meaningful debug messages when a Vulkan or graphics operation fails or a parameter is invalid.
- Prefer clear validation logic and straightforward error propagation over swallowing failures.

### Comments
- Comments should explain why a piece of code exists, not restate what the code already says.
- Add comments only when they clarify a constraint, a workaround, a platform quirk, a performance decision, or a design reason that would otherwise be unclear.
- If a comment is needed, make it about intent or rationale: why this code is here, why this behavior is required, or what tradeoff it represents.
- Do not add comments that merely describe obvious code, such as "initialize buffer" or "check if result is success."

Good examples:
- `// Keep the debug callback stable across device lifetime; it captures the owning device.`
- `// Vulkan requires a dedicated allocator for this path; avoid reusing the global pool across queue families.`

Avoid examples:
- `// Create the buffer`
- `// Check if the result is success`
- `// Initialize the allocator`

### Human readability checklist
Before finalizing a change, ask:
- Would a teammate understand the purpose of this code on first read?
- Are the names specific enough to explain the intent in the domain language?
- Is the flow easy to follow without hidden behavior or unnecessary indirection?
- Are comments explaining intent instead of describing obvious code?
- Does the result feel consistent with the rest of the library's existing style?

The default standard in this repo is: clear, direct, maintainable C++ that is easy for humans to read and easy for another engineer to extend safely.