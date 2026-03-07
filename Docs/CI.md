# CI Usage

## Recommended Command

```text
UnrealEditor-Cmd.exe <Project>.uproject -run=BlueprintGraphExportSync -Unattended -NoSplash -NullRHI
```

## Useful Arguments

- `-Roots=/Game,/Game/UI`
- `-DocsRoot="Saved/BlueprintGraphExport/Docs"`
- `-JsonRoot="Saved/BlueprintGraphExport/Json"`
- `-ManifestPath="Saved/BlueprintGraphExport/StartupSyncManifest.json"`
- `-SummaryPath="Saved/BlueprintGraphExport/BlueprintGraphExportRunSummary.json"`
- `-OnlyIfStale`
- `-Force`
- `-PrettyJson`
- `-CompactJson`

## Exit Codes

- `0`: sync succeeded or was skipped because outputs are already current
- `1`: sync failed during execution
- `2`: invalid arguments or invalid configuration

## Recommended CI Flow

1. Build the plugin or project editor target.
2. Run the commandlet with explicit output paths.
3. Archive the markdown, JSON, manifest, and summary outputs.
4. Fail the job if the exit code is not `0`.
