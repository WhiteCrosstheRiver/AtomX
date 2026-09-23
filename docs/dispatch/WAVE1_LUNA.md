# AtomX Wave1 — functional OVITO parity (NO UI rearrange)

Use **GPT-5.6 Luna / light** only. Do **not** redesign or reorder the UI chrome. Wire new capabilities into the **existing** Add modification / pipeline / property panel patterns.

## Goal
Close the highest-value gaps vs OVITO Basic shown in:
- `docs/dispatch/ovito-shots/ovito-pipeline-cna.png` (pipeline with Create bonds, Common neighbor analysis, Slice + CNA params)
- `docs/dispatch/ovito-shots/ovito-add-modification-menu.png` (full Add modification menu)

Also read `docs/FEATURE_MATRIX.md` and `vendor/ovito-reference/`.

## Implement end-to-end this wave (priority order)
1. **Color coding** — map a particle property to a colormap (auto/manual range); colors visible in viewport; works as a pipeline modifier.
2. **Common neighbor analysis (CNA)** — adaptive and/or fixed cutoff; structure types FCC/HCP/BCC/ICO/Other; color-by-structure; counts if inspector pattern exists.
3. **Create bonds** — cutoff / pair rules; store bonds; render cylinders or lines in the existing renderer (no layout change).
4. If time remains: **Assign color**, and/or promote Coordination analysis / RDF into real pipeline modifiers.

## Hard constraints
- Functional only — no panel moves, no new window chrome, no viewport grid redesign.
- No fake stubs that claim success without computing.
- Keep build green (`build.ps1` / existing tests).
- Small commits; **git push** to origin on your working branch; open/update PR if that is the repo workflow.
- Update FEATURE_MATRIX only for what actually works.
- End with a short leftover list vs the OVITO Add modification menu for Wave2.

## Done when
- Modifiers appear in existing Add modification UI and run in the pipeline (toggle/reorder/delete like current modifiers).
- Visible correct effect on sample structures / XYZ.
- Pushed to remote.
