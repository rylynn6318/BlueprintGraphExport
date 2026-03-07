# FAQ

## Does the plugin support DataTable export?

Yes. The current supported scope includes Blueprint metadata/graphs, DataAsset export, and DataTable row export.

## Why are docs written under `Saved/` by default?

The default is intentionally non-invasive so first-time users do not immediately clutter the project root with generated files.

## Does the plugin rebuild everything on every editor startup?

Not by default. Save-triggered export is the default editor workflow. Startup full sync is opt-in because scanning large projects on startup can be expensive.

## Can I store markdown under `Docs/` instead?

Yes. Set `DocumentationRootDir` to a repository-tracked location after initial verification.

## Does the commandlet require the editor?

It runs through `UnrealEditor-Cmd.exe`, so it still uses Unreal Editor binaries, but it is designed for unattended/headless execution.
