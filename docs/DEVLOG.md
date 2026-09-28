# Development log

Where shipped work, architecture decisions, and technical history get
recorded — what was built, why, and what's still unverified. This is a
running project record, not a design-reasoning doc.

## UI/UX reset: environment picker removed, one shared top bar for both modes

Decided to stop Productivity Mode work for now and start a general app
UI/UX pass instead. First target: the in-VR "environment picker" (a grid
letting Gaming mode pick Passthrough / a black void / one of four bundled
360° photos as the backdrop). Removed entirely — the session is now
hard-locked to Passthrough. Along the way, found the picker's own "open"
icon secretly shared its swapchain/texture with Productivity Mode's
exit-door button (the code literally commented "reuses the env-button
swapchain slot") — flagged by the user as exactly the wrong way to build
menu UI.

**The replacement is one shared top-bar module, not a Gaming-only feature.**
Direct instruction: "The ONLY difference between Gmode and PMode is how
they render and connect to the pc and how many screens. Other then that
they both have the same top center menu module." PMode's existing bar
(`productivityMenuBarPose()`/`productivityMenuItemPose()`) was already the
right shape — modular item count, anchored above the screen(s) by its own
comment — so it was generalized rather than replaced: `topBarPose(ctx)` now
branches on `ctx->productivityMode` for the anchor (above Gaming's single
resizable screen, or above PMode's centre screen), `topBarItemPose()` lays
out items identically either way, and one shared hit-test function
(`updateTopBar()`, native) is called from both modes' input paths — Gaming
gained the exit/brightness icons it never had before, PMode's old bespoke
bar and its borrowed texture slot are gone. The bar's own texture is
renamed `topBarSwapchain` and now holds the whole icon strip, not one
borrowed cell.

**Two items today: Exit and a passthrough brightness slider.** Tapping
the brightness icon opens a track+thumb above it; dragging is absolute
position (not relative delta, unlike the screen move/resize handles),
right = full passthrough (default), left = full black, persisted to a new
`vr_passthrough_level` preference and restored at session start. The dim
effect is a full-surround black sphere
(`XR_TYPE_COMPOSITION_LAYER_EQUIRECT2_KHR`, `radius = 0` per spec = infinite
sphere) submitted behind everything else, alpha = `1 - level`, re-uploaded
only when the value actually changes. The equirect extension was already
detected/enabled in this codebase (`ctx->equirectSupported`) but had never
actually been used until now. Exit needed zero Java-side changes —
`Game.onVrExitRequested()` had already been written defensively to handle
non-Productivity sessions even though nothing outside PMode could reach it
before.

**Icons are user-supplied PNGs, not drawn in code.** Exit is a plain white
door/arrow glyph; brightness is a two-tone glyph (a white rounded rect
partly occluding a black bracket) depicting a bright screen against a dark
room, matching what the slider actually does. Both live at
`res/drawable-nodpi/ic_topbar_*.png`, decoded and drawn into one wide
strip bitmap in `XrRenderer.buildTopBarArt()`, uploaded as a single
texture. Exit is index 0, which the existing placement math already put on
the left — confirmed, no change needed. A soft, mostly-see-through pill
(`BlurMaskFilter`, ~31% opaque black, baked into the same texture behind
the icons) was added after the user pointed out plain white icons would
vanish against a bright wall; alpha/blur radius still needs a real
on-device check against an actual wall.

**New app icon**: the placeholder purple adaptive-icon background
(`ic_launcher_background`) was replaced with a proper adaptive icon built
from a user-supplied pinwheel image — background color matched to the
icon's own grey, foreground scaled to the standard safe zone, plus a
flattened legacy icon for older launchers. The bundled wordmark text was
flagged as unreadable at real 48px launcher size (confirmed with an
upscaled mockup) but kept as-is at the user's call.

Native (`externalNativeBuildNonRootDebug`) and the full app
(`assembleNonRootDebug`) both build clean; installed to the connected
Quest 3 and launches to PC-select with no crash. **Not yet verified**: the
actual in-VR behavior (bar placement/legibility in both modes, the slider
drag feel, whether the dim level reads right) needs a real PC-connected
session, which wasn't available while building this.

One real mid-session mistake worth recording: a large native edit (the
picker removal) was left half-done — a whole render block was deleted but
a second block still referencing the deleted identifiers wasn't caught
before reporting the task in progress. Found by grepping for the deleted
identifiers and confirming the file still compiled with
`externalNativeBuildNonRootDebug`; fixed by finishing the removal. For
native/compiled code, "I removed X" is a claim to verify with a real
build, not report from having read the diff.

## Third top-bar module: screen curvature, made live-adjustable in-VR

Added a curve slider to the top bar, same pattern as brightness: tap the
icon, drag right for a tighter (~180°) wrap, left for flat. This exposed a
real pre-existing constraint - `curvature` was already a feature (a flat
Settings-only seekbar driving a cylinder-vs-quad screen layer,
`XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR`), but it lived entirely as a
per-frame Java parameter threaded through `nativeUpdateInput`/
`nativeEndFrame`/`updatePlacement`. Made it native-owned live state instead
(`ctx->curveAmount`, mirroring `ctx->passthroughLevel`) so the in-VR slider
has something to actually mutate - removed the parameter from all three
function signatures, seeded from the existing `seekbar_vr_curvature`
preference at session start (`nativeSetCurvature`, called once, same as
`nativeSetPassthroughLevel`), written back to that *same* preference key
(not a new one) so the flat Settings screen and the in-VR slider stay one
source of truth.

**One real wrinkle, worth the note**: `updatePlacement()`'s existing
curvature-seed formula also resets screen position/pose (it's the same
branch that places the screen on first launch or when the flat Settings
distance/size sliders move). Dragging the new in-VR curve slider must
never snap the screen back to the default position, so it writes
`ctx->screenRadius` directly instead, using `ctx->lastDistance` (already
tracked every frame regardless) rather than going through that reseed
path.

**The slider itself is now shared infrastructure, not brightness-specific.**
`ctx->sliderOpen` (bool) became `ctx->openSlider` (an item index, or
`TOPBAR_NO_SLIDER`) plus `ctx->grabSliderTarget` (a snapshot taken when a
drag starts, so it keeps controlling the same value even if the other hand
touches a different icon mid-drag) - the track/thumb chrome and the
open/close/drag state machine are the same for both modules, just
retargeted by index. Adding a fourth slider-based module later is a
two-line change (a new index constant, a case in `applySliderValue()`/
`markSliderDirty()`), not a new subsystem.

Full app build clean, installed and launches with no crash. Same caveat
as everything else in this doc: the actual on-device drag feel hasn't
been checked yet.

## Fourth top-bar module: keyboard toggle - a tap, not a slider

User's own request going in: "let's go big" on adding a way to bring up
the on-screen keyboard - but with a real insight that made this small
instead of a from-scratch 3D keyboard: Quest's system IME can appear as an
overlay inside an immersive session on its own once a key-event-consuming
view has focus, and this app already has exactly that. Found `Game.java`
already implements `GameGestures.toggleKeyboard()` (used by the existing
flat/touch-mode gesture), which just calls
`InputMethodManager.toggleSoftInput()` - and `onKeyDown()`/`onKeyUp()`
already route through `KeyboardTranslator` into the same host-keyboard-
event pipeline a physical Bluetooth keyboard uses, regardless of Gaming vs
Productivity mode. So the whole feature is: a 4th tap-only bar icon (no
slider - mirrors Exit's pattern exactly) wired to a new
`IN_KEYBOARD_TOGGLE` slot → `InputListener.onVrKeyboardToggleRequested()`
→ `runOnUiThread(this::toggleKeyboard)` (hopped to the UI thread, since
`toggleKeyboard()` normally only runs from a touch-gesture callback, not
the render thread this fires from). Zero new text-input plumbing.

Icon started as a procedural placeholder, then replaced same day with
user-supplied art (`ic_topbar_keyboard.png`) - same cleanup as the curve
icon needed: source was a near-white light grey (`244,244,244`) with soft
noisy edges, recolored to solid white and denoised to match the rest of
the set.

**Real unverified assumption, flagged explicitly**: that Quest's system
keyboard actually renders as a visible, legible overlay *within* this
app's immersive OpenXR session when toggled this way, not just in flat
2D activities. This is standard behavior for well-behaved immersive
Android/Quest apps in general, and nothing here should prevent it, but it
hasn't been seen working in this specific app yet - first thing to check
alongside the rest of the top bar.

## This build is Gaming-only - Productivity Mode moved to a private repo

Decided to split development: Productivity Mode continues in a new
private repo, `r0mn-creator/moonlight-vr-pmode` (a full-history copy of
this repo, made by mirror-pushing rather than GitHub's Fork feature, which
won't fork a repo into the account that already owns it - local clone at
`/home/roman/Android/MoonlightVR-PMode`). This public repo's `master`
stays Gaming-only for now.

Scope was deliberately kept small: "you only need to remove what the user
can see... Gmode works just fine." So only the user-visible surface was
touched - the Gaming/Productivity tab bar on PC-select is hidden
(`activity_pc_view.xml`'s `modeTabBar` set to `visibility="gone"`,
`PcView.java`'s `initializeModeTabs()` call commented out) - and nothing
underneath was removed. All the PMode code (multi-process
`PModeScreenService`s, AIDL, native `productivityMode` render/input
paths) is still physically in this repo, just permanently unreachable
since nothing can set `productivityMode = true` anymore. Trivially
reversible when PMode is ready to merge back.

The shared top-bar work (Exit, brightness, curve, keyboard) shipped here
too, deliberately - none of it is Productivity-specific, it was built to
serve both modes identically from the start.

## Fifth top-bar module: 3D effect toggle, live in both directions

Same "on/off, no slider" shape as Exit and the keyboard button, but this
one turned out to have a real architectural wrinkle worth recording.

**The setting itself**: Productivity Mode used to have its own Depth
Off/On buttons in the (now-hidden) drawer panel - a per-launch toggle, not
a persisted preference, and Productivity-specific. Promoted that concept
into a real, simple, mode-agnostic Settings checkbox
(`checkbox_vr_depth_effect`, "3D Effect", on by default) that both modes
now share - separate from the existing `list_vr_depth_source` dropdown,
which stays as the advanced/debug test-pattern picker (flat/ramp/blob/
eyetest/shifttest) it always was.

**Why a live toggle isn't "just another flag the render reads"**: the
mono-vs-stereo decision was baked into the actual OpenXR swapchain's size
at session start (`videoWidth * 2` for stereo, `videoWidth` for mono, in
`initSwapchain()`) - not something a per-frame flag can change, since you
can't resize a live swapchain. Fix: always initialize as at least
`DEPTH_MODE_MODEL`-capable (never truly `OFF`) so the swapchain is always
allocated stereo-sized, regardless of the saved toggle state - this costs
nothing new in practice, since the existing default was already "model"
for virtually everyone. The live toggle then only has to flip two much
lighter things each way: `ctx->depthEffectOn` (a new native flag that
zeroes `separation` before `renderVideoFrame()`, collapsing the whole warp
to a flat pass in one place rather than gating occlusion/upsample/
disparity separately), and the Java-side MiDaS inference thread's actual
running state (saves the ~13.5ms/frame GPU cost when off).

**Second wrinkle**: `startDepthThread()`/`stopDepthThread()` were written
assuming a start-once/stop-once lifecycle per session (`stopDepthThread()`
blocks on `join()`, and `depthExit` was never reset back to `false`
anywhere). The live toggle needs both to run repeatedly within one
session, and `stopDepthThread()`'s blocking join can't run on the render
thread without stalling the whole VR view for however long it takes.
Fixed with `reconcileDepthThread()`: a small dedicated worker thread that
serializes start/stop calls (only one in flight at a time - both touch the
same depth EGL context, so they can't be allowed to race), and
`startDepthThread()` now resets `depthExit`/`depthPending`/`depthBusy`
before spinning up a fresh thread. The visual change itself
(`nativeSetDepthEffect`) is applied immediately regardless of how long the
thread reconciliation takes, so the toggle always *feels* instant even
though the GPU-cost saving lags a moment behind it.

Icon started as a placeholder pair (filled "lenses" for on, outline-only
for off), then replaced same day with user-supplied art (an isometric "3D"
block, already clean - full opacity at 255 white, no denoising needed
unlike the last few icons). Only one asset was provided, so the "off"
state is the same art at 45% alpha rather than a separately fabricated
variant.

## Ambient glow v1: the dim sphere tints itself from the screen's colour

User's idea: like bias lighting behind a real TV, the darker the room gets
(brightness slider), the more the surrounding passthrough should pick up
ambient light coloured like whatever's on screen, instead of just fading
to flat black.

**v1 scope, deliberately simple**: one averaged colour for the whole
frame, not per-edge/positional matching. `computeGlowColor()` reuses the
existing box-filter downscale technique (`DOWNSCALE_FRAGMENT_SRC`, already
used for the depth model's input) at a tiny `GLOW_TEX_SIZE` (8x8) target,
own program/FBO since the depth model's version only exists in
`DEPTH_MODE_MODEL` sessions and glow needs to work with the 3D effect off
too. Read back and averaged in C into `ctx->glowR/G/B`, which the
passthrough dim sphere's texture now uses instead of hardcoded black - the
existing alpha curve (`1 - passthroughLevel`) already means "more visible
as the room darkens" needed zero new logic, just a new colour underneath
it.

Only runs while the dim sphere is actually visible
(`passthroughLevel < 0.999`), so the default full-passthrough experience
costs nothing extra. Re-uploads the tiny dim texture every frame while
visible (video content changes every frame; the texture is 4x4, cheap
regardless).

**Follow-up same day: a dedicated on/off toggle, contextual to the
brightness slider.** User: "we need a button to toggle on and off glow...
This toggle appears and disappears with the slider that darkens the room."
Not a topbar module (it isn't part of the fixed icon row) - its own small
icon, positioned next to the brightness slider's track specifically
(`topBarGlowTogglePose()`, built off `topBarSliderPose(ctx,
TOPBAR_BRIGHTNESS_INDEX)` - never appears next to curve's slider, since
glow has no meaning there), only hit-tested while that slider is open.
Turning it off skips `computeGlowColor()`'s per-frame downscale entirely
(not just the visual result) and the dim sphere falls back to flat black
immediately rather than freezing on a stale colour. Persisted
(`vr_glow_enabled`, on by default), same restore/persist shape as the
other toggles. Icon is user-supplied art with two states - a glowing
white outline for on, a plain grey outline for off - recreated once after
the first version's blur wasn't pronounced enough to read clearly.

**Next evolution (discussed, not started)**: true room-scale glow, where
light would bounce off the user's *actual* walls using Quest's scanned
room geometry rather than a generic surrounding sphere. Researched what
that would take:
- APIs: `XR_FB_scene` + `XR_FB_spatial_entity*` for anchors/planes,
  `XR_META_spatial_entity_mesh` for the actual triangle mesh (mesh is what
  you'd need for real bounce/reflection math - the semantic plane API only
  gives crude wall/floor rectangles).
- Hard prerequisite: only works if the user already ran Quest's own
  system-level Space Setup room scan - this app cannot trigger a scan
  itself, only query whatever's already there. Must degrade gracefully
  when absent (i.e. this can only ever be an optional layer over the v1
  sphere effect, never a replacement for it).
- Needs the `com.oculus.permission.USE_SCENE` manifest permission plus a
  runtime consent prompt.
- Considered stable/shipped for third-party apps as of 2025-2026, not
  beta-flagged.
- Real lift: this app currently uses zero spatial-entity APIs (just
  passthrough + controllers), so this is closer to "new subsystem" (mesh
  query, LOD/simplification budget for arbitrary user-room complexity,
  raycast/reflection math against it) than "new render pass." Quest 3 gets
  a meaningfully better mesh than Quest 2/Pro if that ever matters.

## On-device test pass: findings, then a round of fixes

First real headset session against the new top bar. Findings (verbatim
in spirit): 3D toggle worked; keyboard icon did nothing; the glow was "an
overpowering white that fills the room" and needed a short throw instead;
sliders/icons were too small to grab reliably, wanted a round thumb and a
thinner track; curve wanted more curve at max; the darkness slider itself
was fine on a second look ("I take it back... it's the glow that's too
strong"); exit worked perfectly; the corner resize handles needed more
room off the screen edge to grab. Two polish asks came out of the same
session: a short fade instead of an instant flash on exit, and whether
Quest gives this app the same scheduling priority a "real" VR game gets.

**Sizing.** `CORNER_HOVER` 1.5->2.2, `TOPBAR_ITEM_SIZE_M` 0.10->0.20,
`SLIDER_TRACK_WIDTH_M` 0.28->0.56, `SLIDER_THUMB_SIZE_M` 0.045->0.09.
Track height went the other way, 0.03->0.022 - doubling everything
uniformly would have made the track read as a fat bar instead of a slim
line with a big grabbable thumb on it. Curve's tight end
(`CURVE_RADIUS_MIN_MULT`) tightened from 1.0x to 0.6x viewing distance;
both call sites (`updatePlacement()`'s seed and `applySliderValue()`'s
live update) now read the same two named constants so they can't drift
apart again.

**Glow vs. darkness, actually separated.** The v1 design above tinted the
same full-surround dim sphere the brightness slider drives - which is
exactly why it read as "fills the room": a colour applied to something
already covering the entire passthrough view has no way to stay short-
throw. Reverted the dim sphere to flat black, always. The glow is now its
own small quad (`updateGlowHalo()`, 64x64 alpha-only) hugging the screen's
own rectangle with a soft `GLOW_MARGIN_FRAC` (0.18) falloff - transparent
directly behind the screen (which draws over it regardless, composition
layers are submission-order, not depth-tested) and fading to nothing a
short distance past the screen's edge. Submitted between the dim sphere
and the screen itself, Gaming mode only.

**Keyboard - root cause found, one manifest line.** `toggleSoftInput()`
was already correctly wired (confirmed via `Game.java`/`activity_game.xml`
- a focusable, focused `StreamView` exists in the hierarchy either way).
The actual cause: Horizon OS gates the system-keyboard-overlay compositor
feature behind an opt-in `<uses-feature>` flag. Without it the OS silently
refuses to composite the keyboard overlay during an active immersive
session - the `InputMethodManager` call still "succeeds", nothing is ever
drawn. Confirmed via Unity/Unreal's identical error message
("Oculus overlay keyboard is disabled, add
'oculus.software.overlay_keyboard' feature request..."), and it's a
manifest flag rather than an engine API, so it applies the same way to
this native OpenXR app. Fix: added
`<uses-feature android:name="oculus.software.overlay_keyboard"
android:required="false"/>` to `AndroidManifest.xml`. (Separately, Meta
also has a much heavier `XR_META_VIRTUAL_KEYBOARD_EXTENSION_NAME` API for
rendering their own 3D floating keyboard model in-scene - not needed here,
that's a different feature from showing the plain system IME overlay.)

**Fade in/out, ~2s.** New independent whole-view fade sphere
(`fadeSwapchain`/`ctx->fadeAlpha`/`FADE_IN`/`FADE_OUT`), same tiny
alpha-only equirect recipe as the dim sphere but submitted dead last in
the layer array so painter's-algorithm order puts it in front of
literally everything - the screen included, not just the room (the dim
sphere alone can't do this: it's submitted first/backmost, so the screen
always draws over it). Session start (`XR_SESSION_STATE_READY`, right
after `xrBeginSession` succeeds) seeds it fully black and eases to clear
over `FADE_DURATION_NS` (2s). Pressing the Exit icon no longer raises
`IN_EXIT_PRESSED` immediately - it starts a `FADE_OUT` instead (captures
the current alpha as the ease-from point, so pressing exit mid fade-in
doesn't jump); once that reaches full black, `nativeEndFrame` sets
`ctx->fadeOutComplete`, and the *next* `nativeUpdateInput` call is what
actually raises `IN_EXIT_PRESSED` - checked ahead of both modes' input
handling and any focus/placement early-return, so exit can't get stuck
mid-fade if the session loses focus. This means `finish()` only ever
lands on a frame the user can no longer see anything of.

**Performance level.** Added `XR_EXT_performance_settings` (detected like
every other optional extension here, enabled conditionally, `enabledExts`
bumped from 9 to 10 slots to fit it). At the same `XR_SESSION_STATE_READY`
point the fade kicks off, also calls
`xrPerfSettingsSetPerformanceLevelEXT()` for both `CPU_EXT` and `GPU_EXT`
domains at `SUSTAINED_HIGH_EXT` (not `BOOST_EXT`, which the spec frames as
a short-burst allowance - a stream runs for the whole session). This is
additive to, not a replacement for, the existing
`com.oculus.intent.category.VR` intent filter that already marks this as
an immersive app to the scheduler; it's an explicit ask rather than
hoping the runtime's default pick is generous. Whether it produces a
measurable difference (this device may already have picked a high level
on its own, same as the shipping-config-costs-nothing finding elsewhere
in this project) is unverified.

## Live on-device round two: real bugs found by actually using it

A second headset pass, this time exercising the fixes above for real
(brightness/curve sliders, corner handles, glow) instead of just reasoning
through them. Found several real bugs the first pass's static reading
missed entirely:

**Slider thumb rendered as a hard square, not a circle - and the glow
halo didn't fade, it just stopped dead at a sharp edge.** Same root
cause in both places: `XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT`'s
blend equation (`Color_dst = Color_src + Color_dst*(1-Alpha_src)`) expects
premultiplied source color - i.e. color must already shrink toward black
as alpha shrinks toward zero. The slider track already did this
(`p[0]=p[1]=p[2]=a`); the thumb didn't (`p[0]=p[1]=p[2]=255` regardless of
`a`), and the glow halo didn't either (fixed `r,g,b` regardless of `a`).
A "transparent" texel with full-strength color still adds that color at
full strength under this blend mode, so both rendered as if alpha were
pinned at 255 everywhere except the literal quad boundary. Fixed both to
premultiply, matching the track. Also, while chasing "the glow is too
big and doesn't follow the curve": the halo was always a flat quad even
when the screen itself renders as a curved cylinder - added a matching
cylinder path (same radius/axis math as the screen's own, `centralAngle`/
`aspectRatio` scaled by the same margin factor) so it actually hugs a
curved screen instead of a flat rectangle sitting in front of one.
Shrunk `GLOW_MARGIN_FRAC` further (0.18->0.10), bumped `GLOW_HALO_TEX`
64->128, and switched the linear alpha ramp to a smoothstep ease so the
fade reads as a fade instead of a ramp over a handful of texels.

**Dragging the slider thumb didn't work, and it also snapped the corner
resize handle.** `updateTopBar()` sets `ctx->grabMode = GRAB_SLIDER` and
owns that grab's entire lifecycle itself - but `applyGrab()` (the
screen's own move/resize state machine) ran unconditionally every frame
regardless, and its very first check (`if (ctx->grabMode != GRAB_NONE)`)
doesn't know what `GRAB_SLIDER` is. It read `ctx->grabByTrigger`/
`ctx->grabHand`, both meaningless for a slider grab, decided the grab
had been released, reset `grabMode` to `GRAB_NONE`, and then - same
frame, same stray hover state - could immediately hand it to
`GRAB_RESIZE` if the ray also happened to land in the screen's corner
zone. Fixed by skipping `applyGrab()` entirely while `grabMode ==
GRAB_SLIDER`; that state is `updateTopBar()`'s alone now.

**Corner resize handle sat half on top of the screen's own corner.**
`local.x/y` placed it exactly at the corner (`±0.5*screenWidth`), not
outside it. Added `CORNER_GAP_FRAC` (same standoff convention as the
move bar's `BAR_GAP_FRAC`) so it now sits fully clear of the picture.

**Glow-toggle icon didn't get the 2x sizing pass the rest of the top bar
got.** `GLOW_TOGGLE_SIZE_M` was missed when `TOPBAR_ITEM_SIZE_M` doubled
earlier - doubled it too (0.06->0.12).

**Brightness icon was rendering as a blank white block.** The pre-made
asset was authored as a filled two-tone glyph (opaque black curve +
opaque white fill, both alpha 255) instead of this app's actual
convention for every other top-bar icon (a white glyph, alpha-shaped,
transparent everywhere else). Replaced with a plain sun glyph matching
that convention.

**Exit fade shortened 2s -> 1s** per feedback that 2 felt long.

**Keyboard: root-caused fully, then pulled from the top bar anyway.**
The `oculus.software.overlay_keyboard` manifest fix from the previous
round was real and correct - confirmed via logcat that the entire chain
fires end to end: native hit-test -> `IN_KEYBOARD_TOGGLE` -> Java
dispatch -> `toggleKeyboard()` -> `InputMethodManager.toggleSoftInput()`
-> Horizon OS's own `KeyboardInputMethodService` logging
`onShowInputRequested package: com.limelight.debug`. The OS genuinely
accepts the request. But the panel never actually appears, and Horizon
OS's own logs show why:
```
W DynamicObjectClient: FIXME: failed to enable keyboard tracking
W DynamicObjectClient: Failed to enable KeyboardTrackingFidelity.
```
literally a `FIXME` left in Meta's shipped code, right after
`ObjectTrackingEngine::startTrackingKeyboard()` registers the keyboard
successfully. Earlier in the same log: `"Incompatible features Keyboard
and SurfaceInputs are requested ON -- stopping keyboard tracking"` - this
app requests hand-tracking (Horizon calls it "SurfaceInputs" internally),
and the user was in fact on hand-tracking (bare hands, no controllers)
during every failed test. Everything points to a genuine Horizon OS
limitation: the system keyboard's positioning can't come up while
hand-tracking is the active input mode, independent of anything this app
does. Decision: pull the keyboard icon from the top bar for now
(`TOPBAR_ITEM_COUNT` 5->4, `TOPBAR_KEYBOARD_INDEX` removed, indices
renumbered) rather than ship a button that silently does nothing for
hand-tracking users - who are apparently the norm, not the exception.
The underlying `IN_KEYBOARD_TOGGLE` plumbing (Java `toggleKeyboard()`,
`onVrKeyboardToggleRequested()`, the manifest flag) is left in place,
just unreachable - re-adding the icon later is the only step needed if
Horizon OS ever fixes this, or if a controller-only code path is worth
carrying separately.

**This build is now the release build, not debug.** Per the user: this
fork isn't a debug/test app, it's the actual Virtual Moonlight people
use - `com.limelight.debug` has been uninstalled from the test device in
favour of the signed `nonRootRelease` build (`com.limelight.unofficial`,
per upstream Moonlight's own applicationId convention for third-party
release builds - see the big comment in `app/build.gradle`). All testing
from here on should target that build, not debug.

## Not yet verified / next up

- The corner-handle widening (`CORNER_HOVER`) is a symmetric hover-zone
  change, not an asymmetric outward shift - may not fully match "expand
  the handles off the screen edge" if that meant something more specific.
- The 3D-effect toggle specifically: repeated on/off cycling within one
  session (does `reconcileDepthThread()` actually behave under rapid
  double-taps, does the depth EGL context survive several start/stop
  cycles cleanly) has only been reasoned through, not run on a headset.
- Further UI/UX targets beyond the top bar: the flat 2D screens (PC-select,
  Settings) haven't had a pass yet, and are actually easier to iterate on
  without a headset worn (adb can drive/screenshot a normal Activity in a
  way it can't drive an immersive OpenXR session).
- Performance level (`SUSTAINED_HIGH_EXT`): still no way to confirm from
  outside a session whether it changed anything measurable.

## beta06: multi-corner glow, and the tab-bar mystery finally solved

**The old mode tab bar mystery, root-caused.** Several sessions back, an
on-device build kept showing the Gaming/Productivity tab bar on the
PC-select screen despite the portrait layout (`layout/activity_pc_view.xml`)
having `modeTabBar` set to `visibility="gone"` back in `bfc2d1ef`, verified
byte-for-byte in the installed APK. Never re-investigated at the time since
it wasn't blocking anything. Root cause: Horizon OS renders that 2D panel
app in **landscape**, and `layout-land/activity_pc_view.xml` is a
completely separate resource file - `bfc2d1ef` only ever touched the
portrait one. The landscape file's `modeTabBar` never had a visibility
attribute at all (defaulting to visible), while `productivityPanel`
right below it in the same file *did* get hidden, which is presumably
why this got missed - it looked done. Fixed.

**Ambient glow now samples eight regions around the frame, not one
average for the whole screen.** The v1 downscale (`GLOW_TEX_SIZE` 8x8 box
filter) used to average every texel into one `(r,g,b)` - a flat wash, not
real bias lighting. First pass split that into four corners
(bilinearly blended); per feedback that more regions would read better,
went straight to eight - four corners plus four edge midpoints, sampled
from a 3x3 grid (`GLOW_TEX_SIZE` moved 8->9 so it divides evenly into
nine 3x3 blocks, the centre one discarded) and blended per halo texel via
inverse-distance weighting across all eight (`GLOW_SAMPLE_TL`..`BR`),
which generalises to any sample count cleanly instead of needing bespoke
interpolation math. Also caught and fixed a real bug in the same pass:
the corner/edge sampling had top and bottom inverted (this app's
downscale shader already un-flips the frame relative to raw GL's
bottom-up `glReadPixels` convention, so the original code's assumption
was backwards) - confirmed on-device with a scene that had an
unambiguous red light source at the top, which the glow was showing at
the bottom before the fix.

**Both fixes (landscape tab-bar, 8-region glow with the corrected
orientation) confirmed working on-device** against the release build,
including the same red-light bar scene used to catch the orientation bug
in the first place.

## Not yet verified / next up

- The 3D-effect toggle specifically: repeated on/off cycling within one
  session (does `reconcileDepthThread()` actually behave under rapid
  double-taps, does the depth EGL context survive several start/stop
  cycles cleanly) has only been reasoned through, not run on a headset.
- Further UI/UX targets beyond the top bar: the flat 2D screens (PC-select,
  Settings) haven't had a pass yet, and are actually easier to iterate on
  without a headset worn (adb can drive/screenshot a normal Activity in a
  way it can't drive an immersive OpenXR session).
- The keyboard icon remains removed pending a Horizon OS fix or further
  investigation into hand-tracking frequency.
- GitHub release: beta05 was replaced with the top-bar-bugfix build;
  beta06 (everything below, plus the landscape tab-bar fix + 8-region
  glow above) is committed and pushed to `master` but not yet cut as its
  own GitHub release.

## beta06 continued: XR_FB_hand_tracking_aim, XR_FB_passthrough research, PC-select redesign

**Adopted `XR_FB_hand_tracking_aim`.** Detected/enabled like every other
optional extension here. `jointPinching()` chains an
`XrHandTrackingAimStateFB` onto the same `xrLocateHandJointsEXT` call it
already makes (no extra API call) and now prefers Meta's own aim pose over
`buildHandRay()`'s hand-rolled shoulder-ray math, and the continuous
per-finger `pinchStrengthIndex` (with hysteresis, same shape as the
existing `PINCH_ON_M`/`PINCH_OFF_M`) over the raw thumb/index joint-gap
distance - falls back to the original approach entirely on runtimes
without the extension (e.g. Pico). Likely explains why this app's
hand-tracking ray looked different from other apps' - confirmed NOT
expected to fix the keyboard-under-hand-tracking limitation (separate,
OS-level "frequency" setting per Meta's docs) - not yet confirmed on a
headset whether the ray/pinch feel actually improved.

**Researched, parked: `XR_FB_passthrough` would fix the double-tap-exits-
app bug, but costs Quest-only.** User's finding: double-tapping the side
of the headset (meant to toggle passthrough without leaving the app)
instead exits the app entirely, leaving it in passthrough with the app
closed. Root cause: this app implements passthrough via the generic
`XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND` blend mode, not Meta's own
`XR_FB_passthrough` extension - a Khronos forum thread indicates Quest's
system-level passthrough gesture depends on an app having an active,
proper `XR_FB_passthrough` session to toggle, which this app never
creates. The real fix (`xrCreatePassthroughFB` + a dedicated passthrough
composition layer, replacing the blend-mode toggle) is real engineering
work, not a flag flip, and - per the user's own framing - "once we do it
this app is pretty much for Quest only," since `XR_FB_passthrough` is
Meta-specific and this codebase currently also supports Pico. Explicitly
parked; see `project_moonlight_vr_fb_passthrough_lead` in memory.

**PC-select screen redesigned**: rounded `ml_surface` cards (matching old
desktop Moonlight's card look, restyled to this app's actual palette - no
new colors) replace the bare top-left icon grid, real centering (both
axes, backing off vertically once content overflows one screen so it
falls back to a normal scrollable grid with the next row peeking in at
the bottom), and hold-to-favorite (2s, persisted, star badge, favorites
sort first in normal reading order). Two real bugs found and fixed along
the way:
- `RelativeLayout`'s own `android:gravity` is a no-op for centering
  children - the container had relied on it the whole time. Only a
  child's own `layout_center*` attributes do anything.
- `GridView`'s `wrap_content` width/height does NOT shrink to
  `numColumns*columnWidth` despite the name - confirmed via logcat
  (`numColumns` correctly read back as set, `getWidth()` still reported
  the full available width). Centering is computed by hand instead:
  `numColumns` driven off the real PC count (capped at 4), leftover
  space applied as `GridView` padding.
- Giving the card's background container `focusable="true"` (to give the
  press/focus state selector something to key off) silently broke the
  `GridView`'s own `OnItemClickListener` - a focusable/clickable
  descendant intercepts the touch before the parent `AdapterView`'s own
  click handling ever sees it. `duplicateParentState="true"` gets the
  same visual feedback without stealing the event - a real, easy-to-repeat
  mistake worth remembering for any future custom list/grid item
  backgrounds in this app.

Iterated live against an HTML mockup artifact first (several rounds:
removed an unrequested stats bar becoming the "keep it" default, fixed
the wordmark to say "Virtual Moonlight" not upstream's "Moonlight", fixed
an unreadable dark-on-dark render caused by the browser's own forced-dark
heuristic fighting the page's intentional dark palette - fixed with an
explicit `color-scheme: dark`) before touching real Android layouts,
which caught the wrong-app-name and wrong-color-scheme mistakes for free
without needing a device round-trip.

## v1.0: pointer polish, a real favorites bug, and out of beta

**In-VR laser pointer restyled to match Quest's own system pointer.**
Beam width 0.010m -> 0.004m, cursor dot 0.022m -> 0.014m, matching Meta's
own documented convention for a system-style laser (0.003-0.005m) - ours
read noticeably thicker before. Studied an actual reference screenshot of
Quest's system pointer (visible on the PC-select screen, which is a 2D
panel Quest renders its own pointer for) closely and found two more real
details worth matching: the dot doesn't touch the beam at all (a visible
gap), and the beam fades at both ends rather than being a uniform line -
this app's beam already faded both ends (`lengthFade` in
`uploadPointerArt()`), so that part needed no change. The gap did need
one: pulled the beam's rendered geometry back 2.5cm from the actual
target point (leaving `start` anchored at the hand) rather than widening
the texture's own alpha fade, since which physical end of the texture
maps to hand-vs-target isn't guaranteed (an existing comment on
`lengthFade` already flags this) - a geometric pullback from the known
target-side endpoint sidesteps that ambiguity entirely. Also: the cursor
dot used to disappear completely while aiming at nothing (`beamFree`,
"no target, no cursor" was the original reasoning) - now it stays
visible but bigger (0.022m) than the on-target size, since hiding it
entirely read as the pointer vanishing rather than just being imprecise
right now. **Confirmed via research, not guessed**: there is no OpenXR
API for an immersive session to borrow the system's own pointer
rendering - that mechanism is exclusive to 2D panel apps, so matching its
*style* by hand is the actual ceiling here, not a config to unlock.

**Real bug found in the new favorites feature**: the star badge
disappeared after exiting a streaming session. Diagnosis: the toggle
wrote via `SharedPreferences.apply()`, whose disk write is asynchronous -
favoriting a card is immediately followed by launching straight into a
heavy new Activity, a real window for Android to reclaim the process
under memory pressure before that write ever reaches disk. Switched to
`commit()` for this one write (rare enough that the synchronous cost is
free). Not independently reproduced/confirmed by re-testing the exact
sequence, but this is a real, known class of bug that matches the
symptom precisely.

**Shipped as v1.0** (`versionName`/`versionCode` bumped, GitHub release
published, non-prerelease) - the first release considered stable enough
to leave the beta line, covering everything in this file since beta05.

## v2.0: ultra-wide mode, rounded/feathered screen edges

**New top-bar toggle: ultra-wide streaming.** Widens the requested
`StreamConfiguration` resolution to a 21:9 aspect (same vertical quality
tier, just wider), purely client-side - Sunshine/Apollo already just
captures/encodes whatever resolution gets negotiated at connect time, so
no server-side changes were needed to add the toggle itself. Resolution
can only be negotiated once per connection, so flipping the toggle
triggers a full reconnect (re-launches the stream with the same Intent
extras) rather than resizing live.

**Important host-side requirement, easy to miss**: this toggle only
produces a genuinely wider *picture* if the host actually has something
ultra-wide to capture. Sunshine/Apollo streams whatever the selected
display's native output is - it does not stretch or crop to match
whatever resolution the client asks for. So:
- If the PC's real monitor is a normal 16:9 (or 4:3) display, turning on
  Ultra-Wide here without doing anything else on the host will not
  widen the field of view - you'll just get that same 16:9 image
  letterboxed/pillarboxed into a wider frame, not a wider picture.
- To actually get a wider picture, the host needs a **virtual display**
  set to an ultra-wide (21:9 or wider) resolution, and Sunshine/Apollo
  configured to capture *that* display for this client - not the
  physical monitor.
- The one exception: if the PC is already outputting to a real physical
  ultra-wide monitor and that's the display being streamed, no virtual
  display is needed - the source is already the right shape and this
  toggle just matches the client's request to it.

**Rounded, feathered screen edges + an ambient glow halo.** The floating
screen's corners are now softly rounded (`SCREEN_CORNER_RADIUS_FRAC`) and
its edges fade out over a wider band (`SCREEN_EDGE_FEATHER_FRAC`) rather
than cutting off sharply, so the screen dissolves into the glow halo
around it instead of showing a hard rectangle line. Implemented as a
rounded-box SDF in the compositing shader, applied to the video quad's
alpha (premultiplied to match the layer's
`XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT` blend mode). The
feather width was widened and the halo's own outward margin
(`GLOW_MARGIN_FRAC`) nudged up to match, after the first pass read as
still having a visible seam where the crisp video met the halo.

**Landscape PC-select layout was missing the version number.** The
portrait layout (`layout/activity_pc_view.xml`) already had a
`versionLabel` reading `BuildConfig.VERSION_NAME` in the top-right
corner; the landscape variant (`layout-land/activity_pc_view.xml`) - the
one actually shown on a headset, since headsets run landscape - never
had the matching view added, so the version number was invisible on
every real device despite the code already being there to set it. Added
it to the landscape layout too, sized at 16sp after an initial 11sp
proved too small to read comfortably on-headset.
