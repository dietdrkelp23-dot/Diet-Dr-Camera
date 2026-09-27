# CBPC startup hook inspection - September 24, 2026

> Historical audit. The fixes below are retained in 1.2.1; the native movement
> experiments were subsequently removed. See RELEASE-CHECKLIST.md and
> RUNTIME-MATRIX.md for current validation.

The supplied DietDrCamera 1.2.0 log identifies an entry detour into CBPC on
Skyrim 1.7.104.0. DDC deliberately stops during PostLoad hook validation,
before installing its instruction hooks.

The failing **caller** is Skyrim's main-update function (AE ID 36564,
RVA `0x658870`). DDC is looking for its call to `ScrapHeap::KeepPages`
(AE ID 68150, RVA `0xCDFE00`). The caller's first bytes are:

```text
FF 25 00 00 00 00 10 42 92 DD FE 7F 00 00 90 90 90 90 90
```

`FF 25 00 00 00 00` jumps through the eight-byte pointer immediately after
the instruction. That pointer is `0x7FFEDD924210`. The same log places
`cbp.dll` at `0x7FFEDD8E0000`, so the destination is **cbp.dll+0x44210**.
SKSE identifies that DLL as CBPC. This is direct address evidence, not an
inference from CBPC merely appearing in the module list.

The old DDC decoder treats those eight pointer bytes as instructions and
rejects the reported bytes. This can depend on the address assigned by ASLR:
some other pointers happen to decode successfully. The Address Library
header in this report matches the runtime; the camera hook into TDM also
passes validation before DDC reaches this failure.

The development fix recognizes this exact entry-jump format, records its
destination, and excludes only the inline pointer from instruction decoding.
The jump must target executable memory outside the Skyrim image. DDC leaves
the entry detour intact and still requires the expected native call at a
decoded instruction boundary. This does not disable validation or substitute
an unchecked fixed offset.

Regression checks include the exact reported pointer, truncated pointers,
fake calls inside a pointer, duplicate/missing calls, and invalid body code.
Four mapped-image scenarios cover the entry detour, a conflicting KeepPages
call, a non-executable detour target, and a detour into another engine function.
The mapped-image fixture pads whole displaced instructions, matching the
19-byte patched prologue independently checked on 1.7.104. Its synthetic
pointer contains an invalid x64 opcode to reproduce the old bug consistently.

The original scanner fails the reported-byte regression and the 1.7.104
mapped-image regression. After the fix, all **504 offline scenarios pass
across 21 Steam executable versions**, including all four new scenarios on
every available version. The two GOG executable fixtures remain unavailable.
No game code or synthetic callback runs in these checks.

The Release DLL builds successfully. `RuntimeHookChecks`,
`RuntimeLayoutChecks`, `DiagnosticsChecks`, and `RuntimeBinaryChecks` pass.
The development DLL and matching PDB are preserved in the evidence directory's
`candidate/` folder, with hashes in `verification.json`. This candidate uses
the current development workspace; it has not been installed or published.

Evidence, original source snapshots, and the isolated patch are under
`build/diagnostics/cbpc-entry-20260924-194000/`. In particular,
`repro-parser.log`, `repro-image-deterministic.log`, and
`runtime-matrix/matrix.json` record the before/after results.

The affected user's complete mod setup still needs an in-game retest. These
checks establish the scanner fix, not gameplay compatibility for every mod.
