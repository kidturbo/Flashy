# Wiki Starter Files

This folder contains Markdown pages ready to drop into the Flashy GitHub Wiki.

## How to publish these to the wiki

GitHub Wikis are stored in a separate git repository (`<repo>.wiki.git`) that lives alongside the main repo. To publish:

1. **Enable the Wiki tab** on GitHub:
   - Go to repo Settings > Features > check *"Wikis"*

2. **Initialize the wiki** by creating a first page in the GitHub web UI:
   - Click the Wiki tab > *"Create the first page"*
   - Give it a title like "Home" and save (content doesn't matter, will be overwritten)

3. **Clone the wiki repo:**
   ```bash
   git clone https://github.com/kidturbo/Flashy.wiki.git
   cd Flashy.wiki
   ```

4. **Copy the files from `wiki/` in the main repo:**
   ```bash
   cp ../Flashy/wiki/*.md .
   # Don't copy this README.md — it's just instructions
   rm README.md
   ```

5. **Commit and push:**
   ```bash
   git add -A
   git commit -m "Initial wiki content"
   git push origin master
   ```
   > Note: the wiki repo's default branch is `master`, not `main`.

6. The pages will appear in the Wiki tab immediately.

## Keeping the wiki in sync

Any time you update pages in `wiki/` in the main repo, repeat step 4–5 to propagate changes. Alternatively, edit the wiki directly on GitHub's web UI for small fixes.

## File inventory

| File | Purpose |
|------|---------|
| `Home.md` | Landing page — shown by default when visitors click the Wiki tab |
| `Hardware-Assembly.md` | Building the Feather + SD stack, breaking the 120Ω terminator, OBD-II + bench wiring |
| `Getting-Started.md` | Firmware flash, first serial connection, LED codes |
| `Downloads.md` | Explains what's in `Flashy-Tool.zip` |
| `Documentation-Index.md` | Navigation hub — links to HTML docs and other wiki pages |
