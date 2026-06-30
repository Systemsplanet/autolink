# developer.md

General engineering principles for this project. These are project-agnostic and
explain the *why*. Project-specific operational rules — build commands, file
layout, delivery, gotchas — live in `AGENTS.md`, which defers here for rationale.

## Design

- Build **deep modules**: lots of behavior behind a small interface at a clean seam.
- An interface is *everything a caller must know*: signature, invariants, ordering, error modes, config, perf — not just the type.
- Deletion test: if deleting a module makes complexity reappear across N callers, it earns its keep; if complexity vanishes, it was a pass-through — inline it.
- One adapter = a hypothetical seam; two = a real one. Don't add a seam until something actually varies across it.
- Design it twice: sketch ≥2 radically different interfaces, pick on depth, locality, seam placement.
- Decide which modules a change touches *before* writing the spec.
- Prototype throwaway code to resolve state/business-logic or UI questions before committing to a design.
- Keep a shared-vocabulary doc and short ADRs for hard or surprising decisions.

## Code

- **Prefer composition over inheritance.** Compose deep modules from small, swappable parts; don't build class hierarchies. Inheritance only at the user-extension boundary (an injected interface). Function-pointer callbacks or `unique_ptr<Interface>` over virtual bases.
- No inheritance for code reuse — inject collaborators and delegate instead.
- Accept dependencies, don't construct them inside (enables swapping and testing).
- Return results; avoid side effects and in-place mutation.
- Small surface area: fewer methods, fewer params, more hidden inside.
- Minimal code for the current need; no speculative features.
- **One concern per unit.** A tool or module that mixes two jobs is brittle; if it grows to two, split before three.
- **Short names.** `b`, `n`, `i`, `e`, `ok`, `seq`, `cb` — not `m_messageBufferLength`.
- Name everything from the shared vocabulary; same word everywhere.
- Comments only for non-obvious invariants or surprising decisions. No version references in code. EST timestamps in logs.
- **Logging:** version at startup, wire-op results, state-change causes, error resolutions. Never hot-path chatter.

## Test

- Test behavior through public interfaces, never implementation details. A rename that breaks a green test means it tested the wrong thing.
- Vertical slices: one test → minimal code to pass → repeat. Never write all tests then all code.
- Start with a tracer bullet: one test proving the path end-to-end.
- Don't mock internal collaborators or test private methods.
- Refactor only when green; never while red.
- You can't test everything — prioritize critical paths and complex logic.
- At least one test class per real class.
- Every fix gets a regression test that fails when the fix is reverted. Toggle off → red, toggle on → green. Green/green means the test is useless.
- Separate pure decision logic from I/O: decisions are pure functions returning enums, table-tested exhaustively; side effects in the caller.
- Make time and scheduling injectable — never busy-wait on wall-clock in a unit test.
- A unit test runs in **under one second**. Anything slower, or that crosses process/network/filesystem, is an integration test.

## Debug

- Build a tight, red-capable feedback loop **first**: one command you've already run that drives the real code path and asserts the exact symptom — deterministic, fast, runnable unattended. No loop, no hypothesis.
- Reproduce, then minimise to the smallest scenario still going red; cut one thing at a time, re-run after each.
- Generate 3–5 ranked, falsifiable hypotheses before testing any one ("if X is the cause, then Y will fix it").
- Instrument one variable at a time. Prefer a debugger over logs. Tag debug logs with a unique prefix so cleanup is one grep.
- Performance: measure a baseline first, then bisect. Logs are usually the wrong tool.
- Write the regression test before the fix *if* a correct seam exists; if none does, that absence is itself the finding — flag it.
- Cleanup before done: original repro gone, tagged logs removed, throwaways deleted, correct hypothesis recorded in the commit.

## Support

- Invest in design daily; periodically scan the codebase for deepening opportunities before it becomes a ball of mud.
- When a bug had no good test seam, fix it, then hand the architectural finding off for refactor — decide after the fix, with more information.
- On handoff, compact the work into a doc another dev can resume from.
- Move issues through an explicit triage state machine.

## Shared

- Grill the plan before building: resolve every branch of the decision tree until you and the work are aligned.
- Break work into independently-grabbable vertical slices, not horizontal layers.
- One ubiquitous language across code, docs, and conversation.
- Small deliberate steps. The rate of feedback is your speed limit.

## Project structure

- `build/` holds build scripts and config files.
- Top-level folders reflect the *type* of project (e.g. `firmware/`, `web/`, `lib/`, `tools/`).
- Keep fewer than 7 files per folder; split when it grows past that.
- One test class per real class.
- Unit tests under one second each; everything else lives under an integration-test tree.
