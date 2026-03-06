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

- `UnrealEditor-Cmd.exe <Project>.uproject -run=BlueprintGraphExportSync`
  Runs the official headless/CI sync entrypoint.

## Headless / CI

Recommended invocation:

```text
UnrealEditor-Cmd.exe <Project>.uproject -run=BlueprintGraphExportSync -Unattended -NoSplash -NullRHI
```

Supported arguments:

- `-Roots=/Game,/Game/UI`
- `-DocsRoot=<path>`
- `-JsonRoot=<path>`
- `-ManifestPath=<path>`
- `-SummaryPath=<path>`
- `-OnlyIfStale`
- `-Force`
- `-PrettyJson`
- `-CompactJson`

Argument precedence is `CLI override > project settings > plugin defaults`.

Summary output defaults to `Saved/BlueprintGraphAnalysis/BlueprintGraphExportRunSummary.json`.

Exit codes:

- `0`: sync succeeded or was skipped because outputs are already current
- `1`: sync failed during execution
- `2`: invalid arguments or invalid configuration

## Automatic Behavior

- Save hook: re-exports supported assets when they are saved.
- Startup sync: rebuilds documentation on editor startup only when the mirror is stale.

## Mermaid Visualization

- Blueprint markdown documents can include inline Mermaid graph visualizations.
- This is enabled by default through `bIncludeGraphVisualizationInMarkdown=True`.
- Large graphs skip inline visualization when they exceed `MaxVisualizationNodeCount` (default `80`).
- Mermaid rendering depends on the markdown viewer or site consuming the generated docs. The plugin only emits Mermaid source blocks.

## Notes

- `ExportBlueprintsUnderPathToJson` and related helper scripts use configured roots. Legacy aggregate scripts that only support a single root use the first configured root path.
- Relative config paths are project-root relative, not plugin-root relative.
