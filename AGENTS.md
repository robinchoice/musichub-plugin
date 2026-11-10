# Music Hub Plugin

## Purpose & links

- DAW plugin (JUCE, CMake) for Music Hub: slot A plays the DAW's live mix, slots B to D play versions of a Music Hub track. Armed in the editor, it records the next offline bounce of its input (FLAC, `<data dir>/captures/`) and uploads it as the next version of the chosen track. Talks to the API of `robinchoice/music-hub`, by default https://hub.pleasance.org, overridable with `MUSICHUB_URL`.
- Distributed as unsigned prereleases on GitHub, no hosting.

## Checks

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build --target MusicHubTests --parallel 4 &&
ctest --test-dir build --output-on-failure
```

- The tests include `HubSession` against a fake hub on localhost, so Linux needs the libcurl headers. CI runs them a second time under ThreadSanitizer.
- On macOS, CI also validates the AU with `auval`, for both arm64 and x86_64 (via Rosetta).

## Deploy

- A push to `main` runs `build.yml`: builds and tests on macOS, Windows and Linux and uploads the packages as artifacts. Verify: `gh run watch`.
- A tag `v*` additionally creates a GitHub prerelease with the packages. That is a release: only on Robin's go, following the release rules in `~/.claude/rules/git.md`.

## Pitfalls

- Bounce capture only sees offline renders the host announces through `setNonRealtime` (Logic offline bounce, Ableton export). Realtime bounces are not captured, and Pro Tools is out until the AAX is PACE-signed.
- Releases are signed annotated tags. The tag message body becomes the release notes, so write them there (German, like v0.1.0).
- Keep `project(... VERSION ...)` in `CMakeLists.txt` in line with the tag.
- v0.1.0 talks to https://hub.diespaetzles.lol. That domain stays up until users have a newer version.

## Deviations from the standard

- The UI is German only, without an English switch, until the plugin gets a language toggle. New texts stay German like the existing ones.
