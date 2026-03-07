# BlueprintGraphExport

Editor-only Unreal plugin for exporting Blueprint and DataAsset documentation as Markdown and JSON, including headless CI sync.

## Support Matrix

Currently supported:

- Blueprint metadata
- Blueprint graph, node, pin, and link export
- DataAsset metadata and property export
- DataTable metadata and row export
- Headless commandlet sync

Not currently supported:

- BehaviorTree export
- AnimBlueprint-specific semantic analysis
- WidgetBlueprint-specific semantic analysis beyond generic Blueprint metadata

## Defaults

The plugin ships with default settings in `Config/DefaultEditor.ini`.

- `OutputBaseDir=.` means the project directory that contains the `.uproject` file.
- Relative output paths are resolved against `OutputBaseDir`.
- Default Markdown mirror root: `Saved/BlueprintGraphExport/Docs`
- Default JSON mirror root: `Saved/BlueprintGraphExport/Json`
- Default startup manifest: `Saved/BlueprintGraphExport/StartupSyncManifest.json`
- Default asset scan root: `/Game`
- Default automation path: export on asset save
- Startup full sync is disabled by default and can be enabled per project if your team accepts the startup scan cost.

Projects can override these values in their own `Config/DefaultEditor.ini`.

Full setup and usage docs:

- `Docs/QuickStart.md`
- `Docs/CI.md`
- `Docs/FAQ.md`

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

Summary output defaults to `Saved/BlueprintGraphExport/BlueprintGraphExportRunSummary.json`.

Exit codes:

- `0`: sync succeeded or was skipped because outputs are already current
- `1`: sync failed during execution
- `2`: invalid arguments or invalid configuration

## Automatic Behavior

- Save hook: re-exports supported assets when they are saved. This is the default editor workflow.
- Startup sync: optional. When enabled, it rebuilds documentation on editor startup only when the mirror is stale.

## Notes

- `ExportBlueprintsUnderPathToJson` and related helper scripts use configured roots. Legacy aggregate scripts that only support a single root use the first configured root path.
- Relative config paths are project-root relative, not plugin-root relative.
- If you want repository-tracked markdown outputs, point `DocumentationRootDir` at a project `Docs/` folder after initial setup.
- For large projects, prefer save-triggered export or explicit manual/CI sync over startup full sync.
