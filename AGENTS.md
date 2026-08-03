# Agent Instructions

Read `docs/DEVELOPMENT.md`, `docs/PROTOCOL.md`, and `docs/RELEASING.md` before
changing protocol fields, saved configuration, version metadata, or release
behavior.

- Treat `../PixelBro84-Configurator` as the coordinated sibling project.
- Develop new minor versions on matching `vX.Y` branches in both repositories.
- Keep `main` as the latest accepted stable release.
- Gate configurator features by protocol version and preserve older firmware
  compatibility whenever possible.
- Never change an existing saved-schema layout; add and migrate a new version.
- Build both production and diagnostic firmware after behavioral changes.
- Verify production UF2 metadata with `picotool` before a release.
- Preserve unrelated and uncommitted work. Stage files explicitly.
- Do not commit, push, tag, publish a release, flash hardware, or deploy a site
  unless the user authorizes that action.
- Keep durable process knowledge in the Markdown guides, not only in this file
  or in conversation history.
