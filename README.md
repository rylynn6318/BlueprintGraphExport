# BlueprintGraphExport

Editor-only Unreal plugin for exporting Blueprint and DataAsset documentation as Markdown and JSON.

## Defaults

The plugin ships with default settings in `Config/DefaultEditor.ini`.

- `OutputBaseDir=.` means the project directory that contains the `.uproject` file.
- Relative output paths are resolved against `OutputBaseDir`.
- Default Markdown mirror root: `Docs/AssetMirror`
- Default JSON mirror root: `Saved/BlueprintGraphAnalysis`
- Default startup manifest: `Saved/BlueprintGraphAnalysis/StartupSyncManifest.json`
- Default asset scan root: `/Game`

Projects can override these values in their own `Config/DefaultEditor.ini`.

## Current Commands

- `BlueprintGraphExport.SyncAllDocs`
  Runs a full documentation rebuild for all configured `RootAssetPaths`.

## Automatic Behavior

- Save hook: re-exports supported assets when they are saved.
- Startup sync: rebuilds documentation on editor startup only when the mirror is stale.

## Notes

- `ExportBlueprintsUnderPathToJson` and related helper scripts use configured roots. Legacy aggregate scripts that only support a single root use the first configured root path.
- Relative config paths are project-root relative, not plugin-root relative.
