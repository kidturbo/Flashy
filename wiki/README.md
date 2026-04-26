# Wiki maintainer notes — DO NOT sync to public wiki

> **This file is maintainer-only.** It lives in the main repo's `wiki/`
> folder for convenience (next to the public pages it documents) but
> is **explicitly excluded** from the [sync workflow](../.github/workflows/sync-wiki.yml),
> so it never appears on github.com/kidturbo/Flashy/wiki.

## Workflow

```
   wiki/*.md  in main repo  ──► [push to main] ──►  github.com/kidturbo/Flashy/wiki
                                  (CI sync, ~15s)
```

The public wiki at <https://github.com/kidturbo/Flashy/wiki> is mirrored
from this folder. The mirror runs on every push to `main` that touches
`wiki/**` or the workflow file itself, plus a manual "Run workflow"
button on the [Actions tab](https://github.com/kidturbo/Flashy/actions/workflows/sync-wiki.yml).

## How to update wiki content going forward

**Always edit in this repo. Never edit on github.com/kidturbo/Flashy/wiki directly.**

1. Edit the page locally — e.g. `wiki/Getting-Started.md`.
2. Commit and push to `main` (or merge a PR that touches it).
3. The [`Sync wiki/ -> GitHub Wiki`](https://github.com/kidturbo/Flashy/actions/workflows/sync-wiki.yml)
   workflow fires automatically; the wiki is updated within ~15 seconds.
4. Verify by visiting the page in the wiki UI. Look for the "Mirrored
   from `wiki/<file>.md`" banner near the top — that confirms it came
   from the sync, not a manual edit.

If a direct edit ever sneaks in (e.g. a collaborator edits via the wiki
UI), pull the updated content out of the wiki repo and merge it back
into `wiki/` here:

```bash
git clone https://github.com/kidturbo/Flashy.wiki.git /tmp/flashy-wiki
diff -w --strip-trailing-cr wiki/<page>.md /tmp/flashy-wiki/<page>.md
# manually merge anything unique from the live wiki into wiki/<page>.md
git add wiki/<page>.md && git commit -m "wiki: pull back direct edit" && git push
# the next sync overwrites the live wiki with the merged content
```

## Adding a new wiki page

1. Drop `wiki/New-Page.md` in this folder. Use the same banner pattern
   as the existing pages (look at `wiki/Home.md` line 1 for the format).
2. Add a navigation entry to `wiki/Home.md` and/or `wiki/Documentation-Index.md`
   so visitors can find it.
3. Commit + push. The sync will pick it up automatically.

The page must be valid Markdown. GitHub Wikis support GFM tables, fenced
code, anchors via `<a name="...">`, and basic HTML inline (`<sub>`,
`<sup>`, `<details>`, etc.).

## Binary attachments (zips, images, etc.)

The sync workflow uses an **additive** copy — files in the live wiki
that aren't in `wiki/*.md` are preserved (not deleted). So binary
attachments uploaded directly via the wiki UI (like
`T87A-Patched-Library-v1.zip`) survive the sync.

**Recommendation:** upload binary attachments directly via the wiki UI,
not through this folder. The main repo doesn't need 11 MB zips
checked into git history.

## What's in this folder

| File | Mirrored to wiki? | Purpose |
|------|-------------------|---------|
| `Home.md` | yes | Wiki landing page |
| `Hardware-Assembly.md` | yes | Build the Feather + SD stack |
| `Getting-Started.md` | yes | First flash, first read, LED codes |
| `Downloads.md` | yes | Release zip contents + T87A library |
| `Documentation-Index.md` | yes | Nav hub linking to all HTML docs |
| `Advanced-Commands.md` | yes | Full serial command reference |
| `README.md` | **no — maintainer-only** | This file |

## Workflow source

[`.github/workflows/sync-wiki.yml`](../.github/workflows/sync-wiki.yml)
— uses Andrew-Chen-Wang/github-wiki-action-style logic with the
auto-provided `GITHUB_TOKEN` (no PAT or secret to manage). Excludes
this `README.md` via `find ... -not -name 'README.md'`.
