# Rig files

wfview `.rig` files, used unchanged. The client picks the file whose
`CIVAddress` matches the model ID the radio announces (Transceiver ID,
`19 00`); only Icom files (`Manufacturer=0`) are considered, as wfview does.

## Sources

| Files | Source | Commit |
|---|---|---|
| all except `X6100.rig` and `IC-7110.rig` | wfview, branch `main`, `rigs/` (https://github.com/wf-group/wfview) | `ae0e493` |
| `X6100.rig` | libx6100, `contrib/wfview/X6100.rig` (https://github.com/roelandjansen/libx6100) | `9b75edb` |
| `IC-7110.rig` | libx6100_app: a **draft**, made here from `IC-705.rig` and the facts in RS-BA1's IC-7110 definition (`docs/ic7110_rig_draft_20261127.md`); untested, until wfview has one | - |

Licence: GPL-3.0, as the projects they come from.

## Updating

Copy the files over from the sources above, unchanged, and update the
commit column. Do not edit rig files here; fix them upstream.

```
cp -- ../wfview/rigs/*.rig rigs/
cp -- ../libx6100/contrib/wfview/X6100.rig rigs/
```
