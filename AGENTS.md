# Repository conventions

## Documentation: state, not a log

`README.md` is the user-facing overview: purpose, supported capabilities,
configuration, build and installation entry points. Put implementation details,
capacity and memory accounting, synchronization, ABI formats, IORegistry
properties and debugging procedures in `DEBUG.md`, and link to them from the
README instead of duplicating them there.

`README.md`, `DEBUG.md` and `PLAN.md` are state documents. They say what is
true of this driver now, not what happened round by round.

- **State only.** State facts that hold at the time of writing.
  Do not record "round N passed / round N failed". Once a defect is fixed
  its failure history is dropped, because the reader needs to know whether
  the combination works now.
- **Attribution decides inclusion.** A failure that follows the driver must be recorded;
  it is a defect in this driver. A failure that follows one physical sample — the same
  hardware error under every driver, every firmware revision and every cold boot — is a
  property of that sample: it is not listed, and it must not be used to excuse a
  driver defect.
- **No version or commit pinning.** The header carries a date and the driver
  version only. Do not write `at commit <hash>` or round-specific report
  filenames into the body or the sources: the hash goes stale immediately, and
  the round names turn a state document back into a log.
- **Update the docs when the test ends, not before the next one.** Finishing a
  verification round is part of that round: before committing or reporting,
  revise `README.md`, `DEBUG.md` and `PLAN.md` so they state what is true now —
  newly verified scope, closed gaps, hardware behaviors discovered — and correct
  every statement the results made stale. Superseded claims ("unverified", "not
  yet supplied", "fails closed on X") are rewritten or removed, never left
  standing beside their replacement.

## Deployment: the ESP and nothing else

This driver is installed on the ESP only, never write to `/Library/Extensions`
or any other system volume, however much shorter the load path would be.
A driver kept on the system volume is not an option here.

- The active ESP is the **internal SATA disk's** EFI partition. Confirm it before
  writing: `diskutil info` shows `Device Location: Internal` for the whole disk.
- Never write OpenCore files to a USB disk's EFI. The USB sticks in this machine
  are install and rescue media, not boot configuration.
- The plugin is injected from `EFI/OC/Kexts` through a `Kernel > Add` entry.
  Back up `config.plist` next to itself before editing, `plutil -lint` the result,
  and verify that the only difference from the backup is the entry that was added.

## Identifiers: no milestone-round labels

Milestone numbers (M0…M7) are planning vocabulary and belong in `PLAN.md`
only. Property keys, class names, methods, members, macros, local variables,
log lines and comments must describe what the code does, not when it was
written. Never introduce an identifier such as `runM3b`, `fM4TmfCount` or
`MPRM3bFailure`; use `probeCapacity`, `fTmfCount`, `MPRCapacityProbeFailure`
instead. Diagnostics keep their meaning across refactors, and a state
document does not turn into a log.
