# Music Hub Plugin

## Purpose & links

- DAW plugin (JUCE, CMake) for Music Hub: slot A plays the DAW's live mix, slots B to D play versions of a Music Hub track. Talks to the API of `robinchoice/music-hub`, by default https://hub.pleasance.org, overridable with `MUSICHUB_URL`.
- Distributed as unsigned prereleases on GitHub, no hosting.

## Checks

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build --target MusicHubTests --parallel 4 &&
ctest --test-dir build --output-on-failure
```

## Deploy

- A push to `main` runs `build.yml`: builds and tests on macOS, Windows and Linux and uploads the packages as artifacts. Verify: `gh run watch`.
- A tag `v*` additionally creates a GitHub prerelease with the packages. That is a release: only on Robin's go, following the release rules in `~/.claude/rules/git.md`.

## Pitfalls

- Releases are signed annotated tags. The tag message body becomes the release notes, so write them there (German, like v0.1.0).
- Keep `project(... VERSION ...)` in `CMakeLists.txt` in line with the tag.
- v0.1.0 talks to https://hub.diespaetzles.lol. That domain stays up until users have a newer version.
