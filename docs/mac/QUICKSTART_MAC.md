# HOT-STEP LINK beta for Mac: quick start

HOT-STEP LINK is an AU/VST3/CLAP plugin and Standalone app that sends generation requests to a running local HOT-Step server and brings the takes back into your DAW. It makes no audio by itself: HOT-Step must be running and reachable (default http://127.0.0.1:3001).

This build is universal (Apple Silicon and Intel) and needs macOS 11 or newer.

## Install (manual copy)

- AU: copy `AU/HOT-STEP LINK.component` to `~/Library/Audio/Plug-Ins/Components/`
- VST3: copy `VST3/HOT-STEP LINK.vst3` to `~/Library/Audio/Plug-Ins/VST3/`
- CLAP: copy `CLAP/HOT-STEP LINK.clap` to `~/Library/Audio/Plug-Ins/CLAP/`
- Standalone: copy `Standalone/HOT-STEP LINK.app` to `/Applications/`

Factory presets are built into the plugin; no preset files need installing.

## Not notarized: clear the quarantine flag

These files are ad-hoc signed only (a local signature with no Apple developer ID) and are not notarized, so macOS Gatekeeper still blocks them when they come from a download. After copying, run this in Terminal once per bundle:

```
xattr -dr com.apple.quarantine ~/Library/Audio/Plug-Ins/Components/"HOT-STEP LINK.component"
xattr -dr com.apple.quarantine ~/Library/Audio/Plug-Ins/VST3/"HOT-STEP LINK.vst3"
xattr -dr com.apple.quarantine ~/Library/Audio/Plug-Ins/CLAP/"HOT-STEP LINK.clap"
xattr -dr com.apple.quarantine /Applications/"HOT-STEP LINK.app"
```

If you skip this, macOS may say the file "is damaged" or "can't be opened", or the DAW may silently skip the plugin.

## Rescan in your DAW

- Logic Pro: AU only. Logic validates new AUs on launch. If LINK does not show up, open Settings > Plug-in Manager, select it and click "Reset & Rescan Selection". A failed scan usually means the quarantine step was missed.
- Ableton Live: Settings > Plug-Ins, turn on "Use Audio Units v2" and/or "Use VST3 Plug-in System Folders", then click "Rescan".
- Reaper: Settings > Plug-ins > VST (or AU / CLAP), then "Re-scan".

## Known gaps on Mac

- Audition playback inside the plugin is untested on Mac. If it misbehaves, drag or export the take and play it in the DAW.
- Logic may block file drops from the plugin window. Use Export WAV and import from disk instead.

## Reporting bugs

Report bugs by replying to the email or Discord message you got this download from.
