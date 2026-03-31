# Minecraft Legacy Remake

Current project layout:

- `Minecraft legacy/`: main Visual Studio project, source, third-party code, and runtime assets.
- `Minecraft legacy/assets/`: working assets used by the app at runtime.
- `Source_4jd/`: local-only reference archive with isolated legacy assets and notes.

`Source_4jd/` is intentionally ignored by git and must not be used as a runtime asset path.
If you reuse something from that archive, copy it into the working project tree first.
