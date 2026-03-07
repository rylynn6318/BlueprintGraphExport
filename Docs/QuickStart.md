# Quick Start

## Install

1. Copy the plugin into your project's `Plugins/BlueprintGraphExport` folder.
2. Open the project in Unreal Editor.
3. Enable the plugin if Unreal prompts for it.

## First Export

1. Open `Project Settings > Plugins > Blueprint Graph Export`.
2. Confirm `RootAssetPaths` points to the asset roots you want to export.
3. Leave the default output paths under `Saved/BlueprintGraphExport` if you want a safe first run.
4. Leave startup full sync disabled unless your project explicitly wants startup rebuilds.
5. Run the console command:

```text
BlueprintGraphExport.SyncAllDocs
```

After the first run, day-to-day editor usage can rely on the default save-triggered export path.

## What You Get

- Markdown mirror under `Saved/BlueprintGraphExport/Docs`
- JSON mirror under `Saved/BlueprintGraphExport/Json`
- Sync manifest under `Saved/BlueprintGraphExport/StartupSyncManifest.json`

## Recommended Next Step

After the first successful run, move `DocumentationRootDir` to a repository-tracked `Docs/` folder only if you intentionally want generated markdown checked into source control.
