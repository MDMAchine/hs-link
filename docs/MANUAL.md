# HOT-STEP LINK user manual

Version 0.5.14. This manual covers the plugin (VST3 and CLAP, which behave the same) and the Standalone application together; differences between the two are called out in each section.

---

## Overview

HOT-STEP LINK sends generation requests to a local HOT-Step server and plays back the results inside your DAW session. It does not generate audio itself: everything computational happens in HOT-Step. The plugin's job is to capture source audio, describe what you want, trigger generation, and let you audition and export the takes that come back.

---

## Status bar

When the plugin opens it connects to HOT-Step and starts a heartbeat every 3 seconds. The status bar at the top of the window shows whether the connection is live. Any good answer from the server counts as a sign of life: a heartbeat, a job poll, a submit, a backend switch or a finished take download. If no good answer has arrived for 10 seconds, or three heartbeats in a row fail, the plugin switches to offline state and shows the offline overlay with a Retry button. A server that has really gone away is therefore shown as offline 6 to 10 seconds later.

The status also shows which model is loaded in HOT-Step (`engineModel`) and whether a base (non-turbo) model is available. Mode availability follows from that: if only a turbo model is loaded, `lego` and `repaint` are greyed out.

The pill colour and label also track the current job state: grey `offline`, white `idle`, amber `queued`, blue `rendering`, and red `error`. The backend name appears inside the label when a job is running (`rendering (SA3)`). This was added in v0.5.12.

---

## Server URL and key

The Server URL field sets the HOT-Step address. The default is `http://127.0.0.1:3001`. Change it if HOT-Step is on a different machine or port.

The Key field holds the `X-DAW-Key` value. If your HOT-Step instance has `HOTSTEP_DAW_KEY` set in `config.yaml`, enter the same value here. The key is saved to your user application data folder (`HOT-Step/LINK.settings`), not to the project file. This means the key does not move with a session you share.

Both fields are in plugin state and are saved and restored with your DAW project normally.

---

## Modes

Three modes are available for MVP:

**text2music** generates a new clip from the caption alone. No source audio is needed. Available with any backend including turbo models.

**repaint** modifies an existing clip while keeping its structure. Requires a source clip and a region (start and end in seconds). Also requires a base (non-turbo) model in HOT-Step (`non_turbo_available`). The region defaults to the host loop region when one is set.

**lego** extends or varies a clip in a semantically consistent way. Requires a source clip and a base model. No region is needed.

When no base model is available, `repaint` and `lego` are greyed out and cannot be selected. Load a base model in HOT-Step to unlock them.

---

## Source clip

For `repaint` and `lego` the plugin needs source audio.

**In the plugin (VST3):** click Capture to arm recording. When the host plays, the plugin records the stereo output of the track it is inserted on. Recording stops at the loop end or when transport stops. A 32-second ring buffer handles faster-than-realtime bounces. After recording stops, the source is resampled offline to 48 kHz float WAV using a Lagrange interpolator. The source label updates when the file is ready.

Click Capture again while armed to cancel before recording starts.

**In the Standalone:** there is no track to record from. Either drag a WAV file onto the window or click Drop WAV... to open a file chooser. The file is used as-is; no resampling is applied until a generate request goes out.

---

## Region (repaint only)

In the plugin, the region is taken from the host loop points when the host reports a loop. The region start and end are shown in seconds relative to the start of the source capture. If the loop is shorter than the source or the capture spans multiple bars, set it manually in the region fields.

In the Standalone, set the start and end times directly in the region fields (seconds).

The region must satisfy `0 <= start < end <= source length`. A validation error is shown if the values are out of range.

---

## Backend

The backend selector lets you choose which HOT-Step backend processes the request. `Auto` leaves the choice to whatever is active in HOT-Step. Selecting a specific backend makes it the active backend in HOT-Step when you press Generate, if it is not already: the status pill, which otherwise names the active backend (`Online (SA3)`, `Online (YuE2)`, `Online (ACE: <model>)`), reads `Switching to SA3...` for a moment, then the job is sent. The pill also shows this briefly when that backend is already active, while the plugin asks HOT-Step which backend is active; nothing is switched in that case. This changes the backend HOT-Step itself shows as active, so the next job from the HOT-Step window uses it too. Choosing a backend in the box or on a role card does not switch anything by itself; only Generate does. The first job after a switch takes longer, because HOT-Step frees the old model and loads the new one inside that job. If the switch fails, that job fails with one message starting `Could not switch to`, and the jobs queued behind it for the same backend are cancelled with a reason shown on each card; fix the cause and press Generate again. Jobs set to Auto are not cancelled.

**Expected times (warm, keep-models-loaded on, GPU0 RTX 4070 Ti Super):**

| Backend | 10 s, 1 take | 30 s, 1 take | 30 s, 4 takes (per take) |
|---|---|---|---|
| ACE turbo | ~4 s | ~4 s | ~3 s |
| ACE base | ~4 s | ~6 s | ~5 s |
| SA3 | ~4 s | ~6 s | 1 take only |
| YuE2 | ~82 s | ~65 s | ~32 s |

Times are from a controlled bench on that specific GPU. Cold-start (model reload) adds 6 to 18 seconds for ACE and 40 to 77 seconds for YuE2. Your times will vary.

**SA3 note:** SA3 ignores the takes setting and always returns one take per job. Requesting four takes sends four separate jobs internally.

**YuE2 note:** YuE2 ignores `duration_s` and generates roughly 51 to 68 seconds of audio regardless of the requested length. The facade trims the result to the requested duration with a 10 ms fade-out. Expect generation to take 1 to 2 minutes even for short clips. If you request 10 seconds, the full ~65 second render still happens before trimming.

**VRAM note (SA3 + YuE2 together):** if keep-models-loaded is on in HOT-Step and you switch from SA3 to YuE2 (or vice versa), both models may try to stay resident simultaneously. On a 16 GB GPU this can cause an out-of-memory error for YuE2 batch jobs. If you see a VRAM error, unload models between backends in HOT-Step's settings or turn keep-models-loaded off.

---

## Role cards

Three cards at the top of the Generate plate are shortcuts for the Backend, Takes and ACE model settings. Clicking a card changes those settings exactly as if you had set them by hand, and nothing else:

| Card | Sets | Leaves alone |
|---|---|---|
| SA3, quick sketch | Backend = SA3, Takes = 1 | everything else |
| YuE2, batch-4 take garden | Backend = YuE2, Takes = 4 | everything else |
| ACE base, lego / repaint | Backend = ACE, ACE model = Base | Takes, Mode and everything else |

The ACE card does not change the Mode; its subtitle only names the modes that need ACE. It sets ACE model to Base even when Base is greyed out in the ACE model selector (the server reports no base model); the card does not check the server.

The highlighted card follows the Backend setting and is not stored on its own: SA3, YuE2 or ACE lights the matching card, Auto lights none. Changing the Backend selector, loading a preset, switching A / B or reopening a project updates the highlight. The Backend selector stays in the Setup plate; the cards sit on top of it.

## ACE model

Picks which ACE-Step DiT the server uses. Auto sends nothing (the server keeps its loaded model), Turbo sends `model: "turbo"`, Base sends `model: "base"`. The field is sent only when Backend is ACE; SA3 and YuE2 never get it. Lego mode always sends Base (the server rejects turbo with lego), so Turbo is greyed out with a tooltip while lego is selected. Base is greyed out when the server reports no base model. The job status line shows the DiT the server actually used (`DiT <name>`) when the server reports it.

## Duration and takes

The Duration slider sets the requested clip length in seconds (sent as `duration_s`). For YuE2 this is used only as a trim ceiling; the full render time is independent of duration.

The Takes slider sets how many takes to request. SA3 ignores this and returns one. ACE runs takes as serial jobs. YuE2 batch-4 is the most efficient for generating multiple takes.

## Guidance and steps

Two rotary knobs in the Generate row let you tune the generation engine directly. Both default to Auto.

**Guidance (0 to 20, default Auto).** Controls how strongly the engine follows your caption. Higher values push the result closer to what you wrote; lower values give the engine more freedom to interpret it. At Auto the knob is at its far-left position and no guidance value is sent, so the server uses its own default. When you move the knob off Auto, the value is sent as `guidance` in the request, clamped to the 0 to 20 range.

**Steps (1 to 300, default Auto).** Sets the number of diffusion steps. More steps can improve detail at the cost of a longer render time; fewer steps are faster but may be rougher. At Auto the knob is at its far-left position and no steps value is sent. When set, the value is sent as `steps` in the request.

Sessions saved before v0.5.14 load both controls as Auto.

---

## Caption

The Caption box describes the music you want. It is saved with the project, so it is still there after a project reload or after closing and reopening the plugin window.

## Lyrics

The Lyrics box under Caption takes multi-line lyrics (section tags like [verse] are passed through as typed). Leave it empty, or tick Instrumental, and no lyrics are sent, so the server makes an instrumental. Ticking Instrumental keeps the text but greys the box out. Lyrics are saved with the project.

## Seed and key

The Seed field fixes the generation seed (0 to 9007199254740927, sent as `seed`). A larger number is refused: the footer of the Collection panel reads `Seed too large: max 9007199254740927` the field keeps the number you typed with a red outline, the seed in effect stays, and Generate is blocked (the job line reads the same text and no job is sent) until the field holds a valid seed or is empty. Tick Random (or leave the field blank) to let the server pick a new seed each generate; the seed is then omitted from the request. Dice rolls a new fixed seed. When the server reports the seed used for each take, the job line shows it (for example `seed 11, 12`). When it reports the same seed for every take of a job with more than one take, the line reads `seed 11 requested (per-take seeds not reported)`: the server reported one seed for the whole batch, it is the seed that was requested, and per-take seeds were not reported, so the takes may or may not share it. In a list, a take without a reported seed reads `unknown` (`seed 5, unknown, 7`). When no seed was reported for any take, as with Random, the job line shows no seed at all.

The Key menu sends a musical key such as `A minor` (field `key`). Auto omits it. Both settings are saved with the plugin state and are not automatable.

**Variation (0 to 100, added v0.5.13).** At 0 every Generate call sends the same fixed seed. Higher values step the seed by 8 to 1000 per generate and per "+4 more like take N", derived deterministically from the base seed. A random seed (no fixed seed set) is not affected by the Variation slider. This lets you explore variations of a fixed starting point without having to adjust the seed by hand each time.

## Generate and cancel

Click Generate to submit the request. The job label shows the current state: queued, running, and progress as it polls every second. A queue position is shown when other jobs are ahead.

Click Cancel to send a cancel request. The server stops the current take and skips pending ones. Already-completed takes remain available. The job ends in state `cancelled` (all takes cancelled) or `partial` (some takes completed before cancel).

---

## Generation queue

The Queue panel, below the job card, lists every generate job in the current session. Jobs are sent to the server one at a time, in the order they were submitted.

**Queueing more jobs.** While a job is running, clicking Generate submits a new job rather than blocking. The new job waits in the queue until the earlier one finishes. Up to 8 jobs can wait at a time. If 8 are already waiting, the job label shows "Queue full: 8 jobs waiting" and the submit is rejected until a slot opens.

**What each card shows.** Every generate job appears as a compact row:
- A job number (`#1`, `#2`, ...) and the caption (or "(no caption)" if empty)
- A state label: `queued` (waiting its turn), `switching` (asking the server which backend is active and, if it is another one, to make the chosen backend active, before the request is sent; a job never reads `sending` before `switching`), `sending` (the request is being sent to the server, including the upload of a source clip), `running` (accepted by the server, or finished there with takes still downloading), `done`, `partial` (cancelled after some takes had completed; those takes are kept), `failed`, or `cancelled`. The job card header above the queue shows the same word for the selected job (`Job partial`, `Job sending`); only during a backend switch does it differ, reading `Switching to SA3...` where the row reads `switching`
- A takes counter showing how many takes have downloaded out of the total requested (`2/4`, for example)

**Removing a waiting job.** A job that has not yet been sent to the server shows a small `x` on the right side of its card. Clicking `x` removes it from the queue; it is never sent. The `x` goes away as soon as the plugin starts sending the job (state `sending`): from then on the job cannot be removed, only cancelled. Jobs that are running or finished do not show `x` either.

**Cancelling the running job.** Click the Cancel button in the job card area. This cancels the job currently in flight. If the job is still in state `sending`, the cancel is kept until the server has answered with a job id and is then sent for that id. Once that job ends (as `cancelled` or `partial`), the next waiting job in the queue starts automatically.

**Finished jobs and their takes.** Once a job finishes, its card stays in the list. At most 8 finished cards are kept. When a ninth job finishes, failed and cancelled cards that have no takes are removed first (oldest first, except the card of the job that finished most recently, so its error stays readable; this is the job that ended last, wherever its card sits in the list), then the oldest remaining finished cards. Removing a card removes only the card: the takes of that job stay in the Takes grid, starred or not, and their files stay in the stable folder. Clicking a finished card filters the Takes grid to show only the takes from that job; this works only while the card is still in the list. Clicking the same card again clears the filter and shows takes from all jobs.

**Queue is not saved.** The queue exists only in memory. When the plugin is closed, when the DAW unloads the plugin, or when the project is closed, all queued and finished job records are lost, and so is the Takes grid's list. Every take is copied to the stable folder as soon as its download completes, whether or not the plugin window is open, so the files of downloaded takes remain on disk. The exception is a take whose copy fails; see the next paragraph.

**When a take cannot be saved.** If the stable folder cannot be written (disk full, Windows Controlled Folder Access, a path that is too long, an offline network drive), the take is still shown in the Takes grid with a `NOT SAVED` badge. You can audition it, star it and use Export WAV on it. It cannot be dragged to the DAW until it is saved; a drag attempt shows the reason in the cell. A notice under the Takes grid says how many takes could not be saved, which folder could not be written and which temporary folder holds them. The plugin tries the save again in the background every 10 seconds, slowing to once a minute while it keeps failing; the Retry save button tries at once. When the save succeeds the badge and the notice go away. A saved take stays in the grid even if its file is later deleted or moved by hand; clicking it or exporting it then shows that the file is missing in the Collection footer. No new save attempt is started while the plugin closes, and a save that is already running is stopped: the copy of the audio stops part-way (it is checked about every megabyte), no further file operation is started, and on Windows the plugin asks the system to cancel the file operation that is in progress. Closing a project therefore does not wait for a take to finish copying. Whether a network drive that does not answer lets go of an operation in progress is up to the system; if it does not, closing waits for that one operation. A save stopped this way removes its partial file and the take stays unsaved. If the drive does not answer the removal within a second, a file ending in `.part` can be left in the take folder; the plugin never shows or loads it and it can be deleted. A take that finishes downloading while the plugin closes is not saved either; it is handled like any other unsaved take.

A finished download is renamed from `<name>.wav.part` to `<name>.wav`. If another program holds the file at that moment (a virus scanner, a search indexer, a sync tool), the plugin tries the rename again for about 1.5 seconds. If it still fails, the take is kept and nothing is shown as failed: it is saved to the stable folder under its normal `.wav` name and can be auditioned, starred, exported and dragged like any other take. Only its copy in the temporary folder keeps the `.part` ending. If it cannot be saved either, it is handled like any other unsaved take, including the recovery at the next start, where it is recovered under its `.wav` name.

If the takes are still unsaved when the plugin closes, their temporary session folder is kept and is not removed by the 24 hour cleanup while it holds a take that has not been saved. The next time the plugin starts (in any project), it takes over that folder once the instance that made it is no longer running, and saves each unsaved take into `Documents/MDMAchine/HOT-STEP LINK/takes/recovered_<session>/`, the same way it saves a new take, retrying in the background if the save fails. The same applies if a project is closed in the instant between a take finishing its download and its save to Documents. A take that is already recorded as saved (a line in the folder's `saved.log` with its name and size) is not copied again. A `.part` file that holds a complete download is recovered too, under its `.wav` name. Capture recordings (`source_<number>.wav`) and files of zero bytes are never treated as takes. Once every take in the folder is saved, the folder is removed. It is left alone, and one line naming the folder and the first file in the way is written to the log, when it holds anything the plugin cannot prove is safe to delete: a file or folder it does not know, a cut-short `.part`, or a take with no saved record. Such a folder stays until you look at it and delete it yourself. Until a take has been saved or recovered, its only copy is in the temporary folder, so use Export WAV for anything you need to keep while the notice is showing.

---

## Takes grid

Takes appear as cells in the scrollable grid as they download. Each cell shows a waveform thumbnail, a star toggle, a `+` button for the Collection and an `x` button to delete the take. Click a cell to select that take for audition. Click the star to mark it as a keeper.

**Fit-to-bars score (added v0.5.13).** Each cell shows a `fit 0-100` score in the lower right corner once the tempo has been measured. It is the product of how close the take's length is to a whole number of host bars and how close the measured tempo is to the host tempo. A score near 100 means the take is close to a whole number of bars at the host tempo. The score is 0 when no host tempo is set.

Measured tempo: after a take has downloaded, the plugin measures its tempo in the background and shows a small tag in the lower left corner of the waveform. While it works the tag reads `bpm ...`. Then it shows the measured tempo to two decimals and how far the beats wander from a fixed grid at that tempo, for example `123.77 bpm  6 ms`. Green: steady and sure. Amber: usable, check it by ear (the drift is above 15 ms or the detector is less sure). Red `wanders 85 ms`: the tempo moves too much for one number. Red `tempo ?`: no beat was found (pads, noise, very short takes); no number is shown then. The tempo in the take's file name is the one that was asked for, not the measured one. A mark between the tempo and the drift says how the measured tempo relates to the one that was asked for: `x2` (for example `140.02 bpm  x2  4 ms`) means the take plays at double the asked tempo, `/2` at half of it. No mark: the same octave as asked, or a tempo that is no half or double of it. A take whose file cannot be read (damaged, empty, removed during the read) shows `tempo ?` too. The measurement can be off by half or double; it only reads the take and never changes the file.

Tempo from the file name or the host: when the measurement gives no tempo it stands behind (`tempo ?` or `wanders`), the plugin looks for a tempo in the take's file name, and when the name has none, takes the tempo of the host. The tag then names where the tempo comes from, in amber: `120 bpm (file name)` or `98 bpm (host)`. A measured tempo always wins and reads as before, without a source. Accepted in the file name, upper or lower case: `120bpm`, `120_bpm`, `120-bpm`, `120 bpm`, `bpm120`, `bpm_120`, `bpm-120`, `bpm 120`, and the same with a decimal point (`120.5bpm`). The tempo must stand on its own (not inside a longer word or number, so `x2120bpm` is not a tempo), lie between 40 and 300, and carry `bpm`: a bare number is not a tempo. When the name holds several, the first one counts. The name is the take's own file name, the one shown in the cell; for takes made by the plugin that is the tempo that was asked for. The host tempo is used only while the host reports a position, must lie between 40 and 300, and is taken once, when the measurement ends: the take keeps it and does not follow later tempo changes of the host. With a file name or host tempo the take editor shows beats on the ruler and Snap to grid works; the beats are counted from the start of the file, since no downbeat was measured. `tempo ?` stays only for a take with no measured tempo, none in the name and no host tempo; grid snap is off only then. Note: a take made by Stretch to fit keeps the tempo before the stretch first in its name, so if its measurement is not sure, the file name gives that older tempo.

The grid keeps the takes of every job of the current session, including jobs whose queue card has been removed. It holds up to 256 takes. Past that, the oldest take that is not starred and not selected for audition leaves the grid; its file in the stable folder is not deleted. Starred takes never leave the grid during a session.

Starred takes are saved in the plugin state so they survive project reload. The star state persists even after you restart the DAW.

**Deleting a take.** Each cell has three small buttons at the top right: `x` (delete), `+` (keep in the Collection) and `*` (star). Click `x` to delete that take. It leaves the grid at once, and a line under the grid reads `Deleted <name>. It moves to the Deleted folder in a few seconds.` with an Undo button. For about 8 seconds Undo puts the take back where it was; nothing has happened to its file yet. After that the take's WAV is moved into a folder named `Deleted` that sits in the same folder as the WAV. The take does not come back into the grid by itself; a saved take can be got back by moving the file out of `Deleted` by hand. A take with the `NOT SAVED` badge can be undone only within those 8 seconds: its file is gone when the plugin closes (see below). If the file cannot be moved, it is left where it is, the take is kept in the grid and the footer of the Collection panel reads `Could not move <name> to the Deleted folder. The take was kept.`

**The Deleted folders.** LINK does not use the Windows Recycle Bin, and it does not delete a saved take or a Collection item itself. A deleted file is renamed into a `Deleted` folder next to where it was:

- A saved take: `Deleted` inside the stable folder that holds the take's WAV. It stays there until you empty that folder by hand.
- A take with the `NOT SAVED` badge: `Deleted` inside that session's temporary folder (`HOTSTEP_LINK/session_...` in the system temp folder). That session folder is temporary: it is removed when the plugin closes, and its `Deleted` folder goes with it. So a deleted unsaved take can be undone only within the undo window, and its file is gone when the plugin closes. No session folder is left behind in the system temp folder for it.
- An item removed from the Collection: `Deleted` inside the Collection folder (`Documents/MDMAchine/HOT-STEP LINK/collection/Deleted`), the WAV and its info file. They stay there until you empty that folder by hand. If the WAV is in use or cannot be moved, nothing is moved (`Could not remove <name>: audio file in use or could not be moved, both files kept`). If the info file cannot be moved, the WAV is put back and the item stays in the Collection (`Could not remove <name>: info file could not be moved, both files kept`). In the rare case that the WAV cannot be put back either, the item is removed, the WAV is in `Deleted`, and the footer says that its info file is still in the Collection folder.

LINK never empties the `Deleted` folder of saved takes or of the Collection, and never overwrites a file in a `Deleted` folder: a second file with the same name is stored as `name (2).wav`, `name (3).wav` and so on. You empty these two kinds of folder by hand when you want the disk space back. Files inside `Deleted` are not shown in the grid or in the Collection.

- Only one delete can be undone at a time. Deleting a second take moves the first one to its Deleted folder straight away, and so does closing the plugin window. Closing the project does it too.
- A starred take cannot be deleted. The footer of the Collection panel says `Unstar this take to delete it`. Remove the star first.
- A click on `x` that is refused (starred take, a take that is still being saved) changes nothing else: a take you deleted just before keeps its Undo and its time.
- If a deleted take is starred again before its file is moved (for example the host restores an earlier plugin state that had the star), the file is not moved. The take comes back into the grid and the footer of the Collection panel says `Kept <name>: it is starred. Unstar it to delete it.`
- Two takes with the same name are deleted one by one: `x` acts on the cell you clicked.
- A take you added to the Collection with `+` can be deleted. The Collection keeps its own copy of the WAV, and that copy is not touched.
- If the take being deleted is the one selected for audition, audition is switched off first.
- If the file cannot be moved to the Deleted folder, for example because the DAW has the clip open after you dragged it to a track, or because the folder cannot be created, the take comes back into the grid and the footer of the Collection panel says so. The file stays where it was.
- A take whose file is already gone is simply removed from the grid. That is done only when LINK can see that the file is not there: the file is missing from its folder, or its folder itself was removed and a folder above it can still be read. If the drive cannot be reached (drive unplugged, network share gone, no access) or the path runs through a link or mount point, the take is kept in the grid with the `Could not move ...` message.
- A take that has just finished downloading can refuse for an instant with `This take is still being saved. Try again in a moment`.
- A take with the `NOT SAVED` badge has no copy in the stable folder; deleting it moves its temporary file to the Deleted folder of the session's temporary folder, which is removed when the plugin closes (see above).
- Takes of jobs that are still running are not affected by deleting another take.

Deleting does not remove the copy in the temporary session folder of a take that was saved; that folder is removed when the plugin closes, as before. The delete button and the Undo row were built without a look on screen; their fit at the smallest window size has not been checked by eye.

---

## Collection

The Collection is a curated store of takes you want to keep across sessions. It appears as a chip bar at the bottom of the plugin window (added v0.5.12; it was previously a panel inside the Setup plate).

**Adding a take.** Click the `+` button on any take cell. The footer shows `Added to Collection: <name>` briefly. A copy of the WAV and a small info file (`.info`) are written to `Documents/MDMAchine/HOT-STEP LINK/collection/`. The collection keeps its own copy; deleting the take from the grid does not remove the collection copy.

**The chip bar.** Shows the total count and, while a filter is active, the filtered count. Filter chips: `ALL`, `SA3`, `YuE2`, `ACE`, and a `*` (starred) toggle. A search box filters by name, caption and tags. One chip per saved take is visible while scrolling the bar. Click a chip to select that item; click again to play it back (audition works the same way as takes in the grid). Drag a chip to drop the WAV onto a DAW track.

**Removing an item.** Click the `x` on a chip. The WAV and its info file are moved into a `Deleted` subfolder inside the collection folder. LINK never empties that folder automatically; delete the files by hand when you want the space back.

---

## Take editor

Click **Edit take** in the Takes header row to open the take editor for the selected take. The editor covers the Generate plate area. The Setup, Queue, and Preview plates stay active while the editor is open.

**What the editor shows.** The take name and tempo tag, sample rate, channel count and duration in the info row. A seconds ruler (beats when the tempo is known). A warp lane reserved for a future feature. A loop lane. The waveform (two lanes for stereo, one for mono or more than two channels). A scroll bar, a readout, and rows for snap, fit, zoom and buttons.

**Crop handles.** Two vertical handles mark the start and end of the crop region. The area outside the crop is dimmed. Drag a handle (within 6 px of the line, or by its grip tab) to move it; handles cannot get closer than 20 ms to each other. Click **Apply crop** to render a new take from the crop region; the result line names it. Click **Reset** to put the handles back at the file ends. Click **Cancel** to restore the crop points to where they were when the editor was opened. Click **Close** to keep the current points and close the editor.

**Snap to grid and Zero crossing.** Two toggles under the readout. When Snap to grid is on, dragging a handle or a loop flag snaps to the nearest beat of the measured tempo. When Zero crossing is on, Apply crop moves each cut to the nearest zero crossing within 3 ms. Both toggles reset to off each time the plugin window opens and are not saved in project state.

**Loop lane.** Click or drag in the loop lane to set a loop region (start and end flags with a bar between them). Right-click or press Reset to clear the loop. The readout shows the crop region and the loop region. The Repeats box (1 to 64, default 4) sets how many times the loop plays in the exported file.

**Export loop.** Renders the loop region, repeated the number of times in the Repeats box, to a new take. The result line shows the new take name and warns in amber when the loop end and start do not join cleanly (amber under a seam correlation of -0.30, red under -0.60). Export loop does not apply crop or fit settings.

**Fit to bars.** A toggle in the fit row. When on, the readout shows how many whole bars the crop region covers at the given tempo and how much is left over, for example `2 bars, +999.5 ms`. When the leftover would make the stretch more than 10 % either way the readout turns amber and adds a second line, for example `far off: stretch 25 %`. The tempo field (30 to 300 BPM) starts at the host tempo and switches to what you type. Apply crop with Fit to bars on sends the bar count and tempo to the renderer, which records them in the result line for the new take.

**Stretch to fit.** Appears after an Apply crop with Fit to bars on makes a new take whose crop left a remainder of at least half a frame. One click stretches that take from the crop tempo to the target bars tempo, making it exactly whole bars long (pitch is kept, using Signalsmith Stretch). The stretched take appears as a new entry in the takes grid with `_st<target>bpm` in its name. The original crop take is not changed.

**Take editor audition.** The editor has its own Play, Stop and Loop controls (in the zoom row). Play plays the loop region when one is set, or the whole take. It uses the editor's own clock, independent of the host transport, and adds to the plugin output. There is a 5 ms fade at start and stop. Loop is on by default.

**Zoom row.** Zoom in and out, All (fit the whole take), Crop (fit the crop region). The zoom is local to the editor session and is not saved.

---

## Audition

The Preview panel (right column) draws the waveform of the selected take once it has loaded, with a white playhead line that follows audition playback. With no take selected it reads "Select a take to see its waveform" and audio passes through.

Click Play to audition the selected take. Play is shown in every mode and always restarts the take from 0. By default the preview runs on its own clock, so it plays while the host transport is stopped (since v0.5.8). Preview stops when the plugin window closes.

**Follow host transport:** this plugin parameter (off by default) brings back the older host-gated preview: the take plays only while the host plays, in sync with the host transport, starting at the playhead position where it was requested (the anchor PPQ).

**Level match:** the Level match toggle normalises the take's RMS to approximately -18 dBFS. This makes quick A/B comparisons more useful. The gain applied is shown next to the toggle. Level match is off by default; you can automate it via the plugin parameter.

The take is resampled to the host rate on load (Lagrange interpolator). There is a 10 ms crossfade on take switch to avoid clicks.

**Standalone:** audition uses an app-owned transport clock rather than a host playhead. Click Play to start and stop. The same level-match and crossfade behaviour applies.

---

## Drag to timeline and Export WAV

Drag any take cell out of the grid and drop it directly onto a track in your DAW. The dragged file is the take's copy in the stable folder (`Documents/MDMAchine/HOT-STEP LINK/takes/<session>/`), so it persists after the plugin closes and the temp session folder is cleaned up. A take with the `NOT SAVED` badge has no such copy yet and cannot be dragged.

The stable file name encodes the caption, backend, take number, and BPM, and ends in a 6-character id that is different for every take. For example: `lo_fi_beat_sa3_t1_120bpm_a3f9c2.wav`.

If a name is already taken in the stable folder, the plugin appends `_2`, `_3`, and so on.

Click Export WAV to save the selected take via a file chooser. MIDI export is not available in this version.

**Drag to timeline note:** this uses the JUCE external drag API (`performExternalDragDropOfFiles`). It was included in the Ableton Live hand test on 2026-10-07 (per `docs/RELEASE_READINESS.md`). Reaper and Bitwig have not been tested by hand; sandboxed hosts (Logic Pro, Bitwig) may block the file drop.

---

## Presets and A / B

The header holds a preset box, Save, Del, and the A, B and A=B buttons. They cover the generate settings: caption, lyrics, Instrumental, BPM, duration, takes, seed (or Random), key, mode, backend and ACE model.

- A / B: two settings slots. Click B to store what is on screen in A and load B; click A to go back. A slot you have never used starts as a copy of the other. A=B (shown as B=A while B is active) copies the active slot into the other one. Both slots and the active letter are saved with the project.
- Presets: type a name in the box and press Save. Pick a name from the list to load it into the active slot. Del removes the preset named in the box. Names keep only letters, digits, spaces, '-' and '_', so a name cannot point outside the preset folder.
- Presets are XML files, one per preset, in the user app-data folder under `HOT-STEP LINK/Presets` (on Windows `%APPDATA%\HOT-STEP LINK\Presets`).

## Planned features (coming soon)

No panel is a COMING SOON placeholder at the moment.

## Standalone differences

In Standalone mode there is no host playhead:

- BPM and key fields are manual input fields (not read from a DAW).
- The transport is app-owned. Use the Play button to control audition.
- Source clips are loaded by drag-and-drop or the Drop WAV button; capture is not available.
- The job queue persists across individual windows (stored in app data).
- An audio device picker is available under the standard Standalone settings gear.

---

## Offline overlay

When the server is unreachable the overlay covers the plugin UI with the last error and a Retry button. Clicking Retry resets the backoff and attempts a heartbeat immediately. The plugin otherwise retries automatically with backoff: 1 s, 2 s, 4 s, capped at 5 s.

Jobs are not held back until the connection returns. While the overlay shows it covers the controls, so no new job can be started. A job that was added before the overlay appeared and has not been sent yet is still sent when its turn comes, whatever the pill says. If the server does not answer, that job ends as failed (with `connection failed`, or with a `Could not switch to ...` message when the job had to ask for a backend first) and is not sent again. Press Generate again once the pill is back online.

---

## Known limits

These items are not yet proven in practice. They are listed so you know what to expect and what to report.

- **Host testing.** Ableton Live was tested by hand on 2026-10-07 (scanning, loading, generating, drag to timeline). Reaper, Samplitude and Bitwig have not been tested by hand; basic host operations such as state save and restore and transport sync are unverified in those hosts.
- **Backend switch, 10 s timeout.** The 10 s wait that ends a switch attempt against a server that does not answer is not covered by any test. The test fake returns code 0 immediately; the actual wait has not been measured.
- **Backend switch, cancel in flight.** When Cancel is pressed while the switch POST is in flight, the plugin calls `cancel()` on the HTTP request. This path is not covered by any test, because the test fake does not create a real socket.
- **Backend switch, live server.** The backend switch has not been checked against a live HOT-Step server. Its `/api/backends` and `/api/backends/active` endpoints were designed for this use, but the round trip, the timing and the `backend_mismatch` error from the generate facade have not been observed in a real session.
- **Effect of a switch on a running render.** When HOT-Step switches backends, any render already in progress on the server continues under the old backend (the switch frees that backend's memory once the job ends). The plugin does not know when that job ends; the effect on takes in progress is not known.
- **Drag to DAW timeline.** Drag to timeline uses the JUCE external drag API (`performExternalDragDropOfFiles`). Tested in Ableton Live on 2026-10-07; Reaper and Bitwig have not been tested by hand. Sandboxed hosts (Logic Pro, Bitwig) may block the file drop.

See [docs/HOST_TEST.md](HOST_TEST.md) for the checklist that covers these items when first tested in a host.

---

## Troubleshooting

**Plugin shows offline immediately.** Confirm HOT-Step is running and the Node server is on port 3001 (or the configured port). Try `http://127.0.0.1:3001/api/health` in a browser; it should return `{ "ok": true }`. If you have a firewall or antivirus blocking loopback traffic to Node, add an exception.

**400 error on generate.** The request was rejected by the facade. Common causes:
- `lego` or `repaint` submitted without a source file. Set a source clip first.
- `repaint` submitted with an invalid region (start >= end, or end past the source length).
- `backend_mismatch`: the backend changed in HOT-Step between the plugin's switch and its job (someone picked another one in the HOT-Step window). Press Generate again.

**`Could not switch to ...`.** The plugin asked HOT-Step to make the chosen backend active and that did not work. `no answer`: HOT-Step did not reply within 10 s; check that it is running, then press Generate again. `has no ... backend`: this HOT-Step build does not have that backend; pick another backend or Auto. `HOT-Step is busy`: a job is running; wait for it to finish, then press Generate again. `HOT-Step refused`: HOT-Step said no for another reason; switch the backend in the HOT-Step window or pick Auto, then press Generate again. In all cases, other jobs waiting for the same backend are also cancelled with a reason shown on each card.

**Mode buttons greyed out.** HOT-Step reports no base model available (`non_turbo_available: false`). Only turbo models are loaded. Load a base (non-turbo) DiT model in HOT-Step to unlock `repaint` and `lego`.

**YuE2 clips are trimmed shorter than expected.** YuE2 ignores the requested duration and generates roughly 51 to 68 seconds. The facade trims to the requested length. If the YuE2 render is shorter than your requested duration (unusual), the clip is returned as-is without padding. This is expected behavior.

**SA3 returns one take even when Takes is set above 1.** SA3 does not support batched takes. The facade submits separate jobs internally and assembles them, but if only one completes before the others, the partial result contains one take. This is expected.

**Out-of-memory error on YuE2 with keep-models-loaded on.** See the VRAM note in the Backend section above.

---

## File locations

| File type | Location |
|---|---|
| Stable takes (persisted) | `Documents/MDMAchine/HOT-STEP LINK/takes/<session>/` |
| Temp session folder (active session) | `<system temp>/HOTSTEP_LINK/session_<date>_<time>_<6 characters>/` |
| DAW key | `<AppData>/HOT-Step/LINK.settings` |

Temp session folders from previous plugin instances are cleaned up shortly after startup if they are older than 24 hours, but only when the instance that made them is no longer running and every take in them is recorded as saved. Each instance holds a lock file (`owner.lock`) in its session folder while it runs; a folder whose lock is held is never cleaned up or recovered from by another instance, in the same host or in another program. An instance that could not create its lock file is left alone by the other instances in the same host, and it tries again to create the lock in the background. A folder from an older version of the plugin has no lock file: if it has a `hslink_unsaved.txt` list of unsaved takes, a newer instance recovers those takes into `recovered_<session>/` at once and then deletes only the list; without a list, the folder is not touched until its newest file is a day old. Each saved take gets a line in `saved.log` in its folder, and a folder that holds a take without such a line is not removed, whatever its age; if the stable folder stays unwritable, these folders stay until the takes are saved or you delete them yourself. The 24 hour cleanup follows the same keep rule as recovery: a folder holding a file it does not know, an unknown subfolder or a cut-short `.part` is left alone and named once in the log. A file of zero bytes is never treated as a take. When the plugin closes it deletes its own temp session folder, except when the folder still holds the only copy of a take: a take whose copy to the stable folder failed (the copy is retried in the background, but not at close), or a starred take with no stable copy.
