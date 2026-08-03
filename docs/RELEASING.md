# PixelBro84 Release Guide

This checklist creates a reproducible firmware release and a UF2 asset that
the PixelBro84 Configurator can detect.

## 1. Prepare matching release branches

Create the same minor-version branch in both repositories, for example:

```sh
git switch main
git pull --ff-only
git switch -c v1.3
```

Do this separately in `PixelBro84` and `PixelBro84-Configurator`. Keep `main`
as the latest accepted stable version. Do not mix unrelated working-tree files
into a release commit.

## 2. Update release metadata

For firmware version `X.Y.Z`, update and cross-check:

- `src/project_info.h`
- `CMakeLists.txt` (`pico_set_program_version`)
- PIO source header comments
- the version and date in `README.md`
- `CHANGELOG.md`

If protocol or saved settings changed, update `docs/PROTOCOL.md`, retain the
old schema migration, and test the configurator against the previous stable
firmware.

## 3. Build a clean production UF2

```sh
cmake -S . -B build-production \
  -DPICO_BOARD=waveshare_rp2040_zero \
  -DENABLE_USB_DIAGNOSTICS=OFF \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build-production --clean-first --parallel
cmake -E copy \
  build-production/PixelBro84.uf2 \
  build-production/PixelBro84-vX.Y.Z.uf2
```

Verify the embedded identity and record the checksum:

```sh
picotool info -a build-production/PixelBro84-vX.Y.Z.uf2
shasum -a 256 build-production/PixelBro84-vX.Y.Z.uf2
git diff --check
```

`picotool` must report the intended version, the RP2040 family, the
`waveshare_rp2040_zero` board, and a Release build.

## 4. Test before publishing

- Flash the release candidate and verify startup, shutdown, input gating, LED
  styles, animation effects, tests, save, power cycle, and reconnect.
- Connect the current configurator to the previous stable firmware and verify
  its supported controls still work.
- Connect it to the release candidate and verify every new control.
- In the configurator repository run:

```sh
npm run lint
npm run build
npm run build:worker
```

## 5. Commit and push

Inspect the exact files, stage only the release scope, and push the release
branch:

```sh
git status --short
git diff --check
git add -- <explicit release files>
git diff --cached --check
git commit -m "Release PixelBro84 firmware vX.Y.Z"
git push origin vX.Y
```

Committing, pushing, tagging, publishing a GitHub Release, and deploying the
configurator are separate external actions. Confirm that each is intended.

## 6. Publish the GitHub Release

The configurator queries GitHub's `releases/latest` endpoint and selects a
release asset whose filename ends in `.uf2`. A release intended for automatic
upgrade detection must be published—not a draft or prerelease.

```sh
gh release create vX.Y.Z \
  build-production/PixelBro84-vX.Y.Z.uf2 \
  --repo A2ThreeD/PixelBro84 \
  --target vX.Y \
  --title "PixelBro84 vX.Y.Z" \
  --latest \
  --notes "Release notes"
```

Do not move or replace a published tag. If a released binary needs a fix,
publish a new patch version.

## 7. Verify GitHub and the configurator path

```sh
gh release view vX.Y.Z \
  --repo A2ThreeD/PixelBro84 \
  --json tagName,isDraft,isPrerelease,url,assets

gh api repos/A2ThreeD/PixelBro84/releases/latest \
  --jq '{tag_name, draft, prerelease, assets: [.assets[].name]}'
```

Confirm the latest response contains the correct tag and versioned UF2. Then
connect a device running the previous firmware to the deployed configurator;
it should offer the new release when `X.Y.Z` is greater than the connected
version.

## 8. Finish the coordinated release

- Publish/deploy the matching configurator version when required.
- Verify the custom domain and Web Serial connection on the production site.
- Merge the accepted firmware and configurator release branches into `main`.
- Keep old UF2 assets available so users can recover or intentionally
  downgrade.
