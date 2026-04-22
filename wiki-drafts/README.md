# Wiki Drafts

Scratch area for wiki pages that aren't ready for publication yet. Drop in photos, notes, and draft content here. When a page is ready, move it to `../wiki/` (for the GitHub Wiki) or to `../docs/` (for the Pages site if it's styled HTML).

## Folders

- `images/` — drop photos here while drafting. Move to `../docs/images/hardware/` or `../docs/images/wiring/` when finalizing.

## Current drafts

| File | Status | What it covers |
|------|--------|---------------|
| `BAM-Mode-Wiring.md` | In progress | Bench wiring for BAM (Boot Assist Mode) entry — T87A first, other modules to follow |

## Workflow

1. Draft the page as `<Topic>.md` in this folder
2. Drop reference photos into `images/` with clear names
3. Review and iterate
4. When ready:
   - Move final photos to `../docs/images/wiring/` (create if needed)
   - Move the final `.md` to `../wiki/<Topic>.md`
   - Update image paths in the `.md` to point to the `docs/images/...` location
   - Commit + push, then propagate to the actual wiki repo (see `../wiki/README.md`)
