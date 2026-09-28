// OpenXR presentation for the decoded video stream. The decoder feeds a
// SurfaceTexture whose OES texture lives in the EGL context created here.
// Each new video frame is drawn into a single swapchain that the compositor
// shows on a quad (or cylinder) visible to both eyes. No projection layers,
// the compositor does all the reprojection work.

#include <jni.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <time.h>
#include <math.h>

#include <android/log.h>
#include <sys/system_properties.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>

#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#define TAG "moonlight-xr"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

#ifndef GL_FRAMEBUFFER_SRGB_EXT
#define GL_FRAMEBUFFER_SRGB_EXT 0x8DB9
#endif

#define STATS_LOG_INTERVAL_FRAMES 300

// Pico ships its controller bindings behind an extension. Older headers may
// not have the name, and the runtime may not offer it at all
#ifndef XR_BD_CONTROLLER_INTERACTION_EXTENSION_NAME
#define XR_BD_CONTROLLER_INTERACTION_EXTENSION_NAME "XR_BD_controller_interaction"
#endif

// Beam and cursor art share one small swapchain, beam on top, dot below
#define PTR_TEX_W 64
#define PTR_BEAM_H 256
#define PTR_DOT_H 64
#define PTR_TEX_H (PTR_BEAM_H + PTR_DOT_H)

#define HAND_LEFT  0
#define HAND_RIGHT 1
#define HAND_COUNT 2
// Some headsets aim by looking rather than by pointing. Gaze is a third source
// of a ray, so the pointing code counts it alongside the two hands and
// everything downstream stays the same. Only the hands carry buttons.
#define SRC_GAZE  HAND_COUNT
#define SRC_COUNT (HAND_COUNT + 1)

// Trigger and grip are analog, and a single threshold chatters around the
// crossing, so presses and releases use different ones
#define PRESS_ON   0.65f
#define PRESS_OFF  0.35f
// Thumbstick travel before it counts as a scroll, and how fast a full
// deflection winds the wheel
#define SCROLL_DEADZONE 0.30f
#define SCROLL_CLICKS_PER_SEC 7.5f

// One euro filter on the hit point. A hand at rest still shakes, and at 3 m
// that tremor is several pixels of cursor, so the cutoff drops when the
// pointer is still and rises with speed to keep fast moves from lagging.
#define POINTER_MIN_CUTOFF 1.2f
#define POINTER_BETA 8.0f
#define POINTER_D_CUTOFF 1.0f
// A gap this long means the pointer left the screen or changed hands, and
// filtering across it would slide the cursor in from where it used to be
#define POINTER_RESET_NS 250000000L

// The pointer waits for deliberate movement before it appears, so knocking a
// controller does not throw a laser across the picture, and it goes away again
// once a controller has been put down
#define POINTER_WAKE_SEC 0.5f
#define POINTER_SLEEP_SEC 9.0f
// Metres per second and radians per second. A resting hand manages about a
// tenth of these.
#define POINTER_MOVE_SPEED 0.06f
#define POINTER_TURN_SPEED 0.35f

#define VR_BUTTON_LEFT   0x1
#define VR_BUTTON_RIGHT  0x2
#define VR_BUTTON_MIDDLE 0x4

// Slots in the float array handed back to Java each frame
#define IN_HIT      0
#define IN_U        1
#define IN_V        2
#define IN_BUTTONS  3
#define IN_SCROLL   4
#define IN_POINTER  5
#define IN_POSE_DIRTY 6
// Passthrough brightness slider, both modes - only sent when a drag just
// ended (IN_PASSTHROUGH_DIRTY), same one-shot pattern as IN_POSE_DIRTY.
#define IN_PASSTHROUGH_LEVEL 7
// x y z, then the orientation quaternion, then width and cylinder radius
#define IN_POSE     8
// Top menu bar exit button, pressed this frame - shared by both modes
#define IN_EXIT_PRESSED 18
// Spatial audio: -1..1 left/right balance and 0..1 distance gain, both
// relative to the user's head and the centre screen. Neutral (0, 1) outside
// Productivity mode, so Gaming's audio is never touched.
#define IN_AUDIO_PAN 19
#define IN_AUDIO_GAIN 20
// Phase 2: pointer/click control for the 3 PMode screens themselves (Phase 1
// only ever hit-tested the exit button). -1 in IN_PMODE_SCREEN means no
// screen is currently under the ray.
#define IN_PMODE_HIT     21
#define IN_PMODE_SCREEN  22
#define IN_PMODE_U       23
#define IN_PMODE_V       24
#define IN_PMODE_BUTTONS 25
// Reuses slot 17 (the environment-picker's old pick slot, retired with it) -
// see IN_PASSTHROUGH_LEVEL above.
#define IN_PASSTHROUGH_DIRTY 17
// Screen curvature slider, same one-shot pattern as IN_PASSTHROUGH_*
#define IN_CURVE_LEVEL 26
#define IN_CURVE_DIRTY 27
// Top bar keyboard button, pressed this frame - shows/hides the system
// soft keyboard, same as the flat-mode gesture already does
#define IN_KEYBOARD_TOGGLE 28
// Top bar 3D-effect button, pressed this frame - a request to flip it, not
// the resulting state (Java owns and echoes the real value back via
// nativeSetDepthEffect, same fire-and-forget shape as IN_KEYBOARD_TOGGLE)
#define IN_DEPTH_TOGGLE 29
// Glow on/off, next to the brightness slider - only meaningful (and only
// hit-tested) while that slider is open
#define IN_GLOW_TOGGLE 30
// Top bar ultra-wide button, pressed this frame - same fire-and-forget shape
// as IN_KEYBOARD_TOGGLE. Always routed to Java's InputListener, never
// handled natively, since widening the stream needs a full reconnect.
#define IN_ULTRAWIDE_TOGGLE 31
#define IN_SLOTS    32

// Grab thresholds for the grip, and the range a resize is allowed to reach
#define SCREEN_MIN_WIDTH 0.8f
#define SCREEN_MAX_WIDTH 8.0f

// What the ray is over. Handles only show while hovered, which is how spatial
// panels usually behave: nothing visible until you go looking for it.
#define HOVER_NONE   0
#define HOVER_SCREEN 1
#define HOVER_BAR    2
#define HOVER_CORNER 3

#define GRAB_NONE   0
#define GRAB_MOVE   1
#define GRAB_RESIZE 2
#define GRAB_SLIDER 3

// All as a fraction of screen width, so the handles keep their proportions as
// the screen is resized
#define BAR_WIDTH_FRAC  0.14f
// Height follows the art rather than being picked separately. The two used to
// disagree by 2.5x, which stretched the rounded ends into a slab.
#define BAR_HEIGHT_FRAC (BAR_WIDTH_FRAC * (float)BAR_TEX_H / (float)BAR_TEX_W)
#define BAR_GAP_FRAC    0.035f
#define CORNER_FRAC     0.075f
// How far outside the screen's own corner the bracket sits, same standoff
// convention as BAR_GAP_FRAC - drawn dead-on the corner (no gap) put the
// icon half on top of the picture instead of clearly outside it.
#define CORNER_GAP_FRAC 0.035f
// Hover zones are bigger than the art, since aiming at a thin bar is fussy
#define HOVER_MARGIN 1.7f
// Widened so the resize corners are easier to grab without also having to
// reach exactly onto the screen's own edge
#define CORNER_HOVER 2.2f
// The bar is small on purpose, so its hover zone is proportionally wider
#define BAR_HOVER 2.0f

// Handle art, one small swapchain each so there is no atlas offset convention
// to get wrong
#define BAR_TEX_W 256
#define BAR_TEX_H 24
#define CORNER_TEX_W 64
#define CORNER_TEX_H 64

// Size of one cell in the top bar icon strip (see topBarSwapchain below) and
// of the slider drag-thumb's hover ring.
#define OUTLINE_TEX 128

// Nothing under the ray, but close enough to the screen to keep drawing it
#define HOVER_HALO      4
// How far past each edge that reaches, as a fraction of the screen
#define HALO_FRAC 0.5f
// How far the ray runs when it is aimed at nothing at all, in metres
#define FREE_BEAM_M 4.0f

// Return codes for waitBeginFrame
#define FRAME_EXIT   -1
#define FRAME_IDLE    0
#define FRAME_RENDER  1

// Synthetic depth patterns for the stereo test path
#define DEPTH_MODE_OFF   0
#define DEPTH_MODE_FLAT  1
#define DEPTH_MODE_RAMP  2
#define DEPTH_MODE_BLOB  3
// Tints each eye instead of warping, so eye routing can be checked by
// closing one eye rather than by judging depth
#define DEPTH_MODE_EYETEST 4
// Draws a synthetic bar through the warp and reads back where it landed in
// each eye, so the shift direction is measured rather than eyeballed
#define DEPTH_MODE_SHIFTTEST 5
// Real depth from the MiDaS model, run in Java on LiteRT
#define DEPTH_MODE_MODEL 6

#define DEPTH_TEX_SIZE 256
// Small enough that reading it back every frame is free. Averaged into
// GLOW_SAMPLE_COUNT regions (see GLOW_SAMPLE_* below), not as one
// whole-frame colour - 9 divides cleanly into a 3x3 grid of 3x3 blocks,
// eight of which (everything but the centre) feed the glow.
#define GLOW_TEX_SIZE 9
#define GLOW_SAMPLE_COUNT 8
#define GLOW_SAMPLE_TL 0
#define GLOW_SAMPLE_T  1
#define GLOW_SAMPLE_TR 2
#define GLOW_SAMPLE_L  3
#define GLOW_SAMPLE_R  4
#define GLOW_SAMPLE_BL 5
#define GLOW_SAMPLE_B  6
#define GLOW_SAMPLE_BR 7

// setprop this to any new value to dump one frame's worth of warp inputs and
// outputs, so shader changes can be tried on captured frames off device
#define CAPTURE_PROP "debug.moonlight.capture"
#define CAPTURE_POLL_FRAMES 30

// Tuning knobs, all live over setprop so a headset session can A/B them
// without a rebuild. Each is an integer percent of the real value.
#define PROP_DEPTH_ALPHA "debug.moonlight.depthalpha"
#define PROP_RANGE_ALPHA "debug.moonlight.rangealpha"
#define PROP_UPSAMPLE "debug.moonlight.upsample"
#define PROP_UPSAMPLE_SIGMA "debug.moonlight.upsamplesigma"
#define PROP_DEPTH_SHARP "debug.moonlight.depthsharp"
#define PROP_OVERLAY "debug.moonlight.overlay"
#define PROP_PASSTHROUGH "debug.moonlight.passthrough"

// Enough for a dozen lines of stats without being big enough to matter
#define OVERLAY_WIDTH 768
#define OVERLAY_HEIGHT 512
#define PROP_OCCLUSION "debug.moonlight.occlusion"
#define PROP_SEPARATION "debug.moonlight.separation"
#define PROP_DISTANCE "debug.moonlight.distance"
#define PROP_SCREEN "debug.moonlight.screen"
#define PROP_CONVERGENCE "debug.moonlight.convergence"
#define PROP_DEPTH_GLOBAL "debug.moonlight.depthglobal"
#define PROP_DEPTH_LOCAL "debug.moonlight.depthlocal"
#define PROP_POINTER_CUTOFF "debug.moonlight.pointercutoff"
#define PROP_POINTER_BETA "debug.moonlight.pointerbeta"
#define PROP_BEAM_WIDTH "debug.moonlight.beamwidth"
#define PROP_POINTER_WAKE "debug.moonlight.pointerwake"
#define PROP_POINTER_SLEEP "debug.moonlight.pointersleep"

// Bins for the percentile search over the model output
#define DEPTH_HIST_BINS 512

// Radius of the low pass that splits the depth map into an overall shape and
// the local detail on top of it. About a tenth of the frame.
#define DEPTH_LOWPASS_RADIUS 11

// Fraction of the shorter frame dimension used as the floating screen's
// corner radius - subtle by design ("slightly rounded"), scales with
// resolution instead of a fixed pixel count so it looks the same relative
// size from 360p up through ultra-wide 4K.
#define SCREEN_CORNER_RADIUS_FRAC 0.03f
// Width of the soft fade band at the screen's boundary (corners and straight
// edges alike), same shorter-dimension-relative scaling as the radius above
// so a hard cutoff becomes a gradual feather into passthrough instead.
// Widened from an initial 0.015 - that was too subtle to read as anything
// but a crisp edge once the glow halo picked up right where it left off;
// this is meant to visibly dissolve the video's own last few percent into
// the halo, not just antialias the corner.
#define SCREEN_EDGE_FEATHER_FRAC 0.05f

// Productivity mode, Phase 1: a fixed default arrangement for N flat
// screens. No rotate/arrange controls yet (that is Phase 2) - these are
// just sane constants so there is something to look at.
#define PRODUCTIVITY_SCREEN_COUNT 3
#define PRODUCTIVITY_DISTANCE_M 2.2f
#define PRODUCTIVITY_SCREEN_WIDTH_M 1.35f
#define PRODUCTIVITY_GAP_M 0.06f

// Top menu bar. One module, shared by Gaming and Productivity Mode alike -
// the only difference between them is which screen(s) it floats above
// (topBarPose() below branches on ctx->productivityMode for the anchor; the
// bar's own layout is identical either way). Modular by design: each item
// (exit, brightness now, more later) is one slot in a row laid out by
// topBarItemPose(index, count). Only the count and what each slot draws/does
// needs to change to add one.
#define PRODUCTIVITY_BAR_Y_OFFSET_M 0.50f
// Doubled from the original 0.10/0.045/0.28 - too small to grab reliably
#define TOPBAR_ITEM_SIZE_M 0.20f
#define TOPBAR_ITEM_GAP_M 0.03f
// Keyboard icon pulled for now (2026-09-17) - it correctly drives
// InputMethodManager.toggleSoftInput() end to end, confirmed via logcat all
// the way into Horizon OS's own KeyboardInputMethodService, but the OS's
// own keyboard-tracking subsystem fails to bring the panel up while
// hand-tracking is the active input mode (its own log: "FIXME: failed to
// enable keyboard tracking"). Platform-side gap, not an app bug - revisit
// if a future Horizon OS build fixes it. Re-adding is just restoring
// TOPBAR_ITEM_COUNT to 5, giving keyboard an index again, and putting its
// draw call and hit-test block back (see git history for this commit).
#define TOPBAR_ITEM_COUNT 5
#define TOPBAR_EXIT_INDEX 0
#define TOPBAR_BRIGHTNESS_INDEX 1
#define TOPBAR_CURVE_INDEX 2
// Screen radius range the curve slider maps to, both ends as a multiple of
// viewing distance - shared by updatePlacement()'s seed and
// applySliderValue()'s live update so they can't drift apart. Lowered the
// tight end from 1.0 (screen curling right up to the eye) to 0.6 for a
// noticeably tighter max wrap.
#define CURVE_RADIUS_MAX_MULT 4.0f
#define CURVE_RADIUS_MIN_MULT 0.6f
#define TOPBAR_DEPTH_INDEX 3
// Client-only wide-resolution toggle - see Game.java's
// onVrUltrawideToggleRequested() for why this one triggers a full reconnect
// instead of flipping a render-side flag like the other toggles here.
#define TOPBAR_ULTRAWIDE_INDEX 4
// ctx->openSlider when no slider is open - not a real item index
#define TOPBAR_NO_SLIDER (-1)
#define TOPBAR_WIDTH_M (TOPBAR_ITEM_COUNT * TOPBAR_ITEM_SIZE_M \
                        + (TOPBAR_ITEM_COUNT - 1) * TOPBAR_ITEM_GAP_M)
// Gaming's screen is user-resizable (unlike Productivity Mode's fixed-size
// screens), so its anchor needs a live standoff gap above the current screen
// height rather than a fixed one.
#define TOPBAR_GAP_FRAC 0.035f

// A soft, mostly-see-through pill drawn behind the icons (Java side, baked
// into the same texture) so they stay legible against a bright wall. Bleeds
// past the icon strip's own bounds so the feather has room to fall off
// before hitting the texture edge - the strip texture and the render quad
// are both sized bigger than the icons alone to match.
#define TOPBAR_TEX_MARGIN 40
#define TOPBAR_BG_MARGIN_M 0.031f
#define TOPBAR_TEX_W (OUTLINE_TEX * TOPBAR_ITEM_COUNT + TOPBAR_TEX_MARGIN * 2)
#define TOPBAR_TEX_H (OUTLINE_TEX + TOPBAR_TEX_MARGIN * 2)

// Shared slider chrome - one track/thumb, repositioned above whichever
// item opened it (ctx->openSlider). Brightness: right = full passthrough
// (default), left = full black. Curve: right = ~180 degree wrap, left = flat.
#define SLIDER_TRACK_WIDTH_M 0.56f
// Thinner than a plain doubling of the old 0.03 would give - the track
// itself should read as a slim line even though the grabbable thumb is
// much bigger now
#define SLIDER_TRACK_HEIGHT_M 0.022f
#define SLIDER_THUMB_SIZE_M 0.09f
#define SLIDER_GAP_M 0.03f
#define SLIDER_TRACK_TEX_W 256
#define SLIDER_TRACK_TEX_H 32
#define SLIDER_THUMB_TEX 64
// One dot per side, alpha-only (RGB unused) - the whole visible passthrough
// is dimmed by compositing a black sphere behind everything, so the texture
// itself never needs more than one colour.
#define DIM_TEX 4

// Whole-view fade to/from black: session start and the couple of seconds
// before exit. Independent of the dim sphere above - that one only ever
// covers the room, and this one has to cover the screen too, so it is its
// own sphere submitted dead last (see nativeEndFrame), on top of literally
// everything else the frame draws.
#define FADE_NONE 0
#define FADE_IN   1
#define FADE_OUT  2
// Was 2s - felt a bit long, especially on the way out
#define FADE_DURATION_NS 1000000000L

// Glow on/off, next to the brightness slider specifically (not curve's) -
// appears and disappears with it, since it only means anything while the
// room-darkening slider is open.
// Doubled to match TOPBAR_ITEM_SIZE_M's own 0.10->0.20 - this one was
// missed in that pass and stayed noticeably smaller than its neighbours.
#define GLOW_TOGGLE_SIZE_M 0.12f
#define GLOW_TOGGLE_GAP_M 0.025f

// The glow itself: a small quad hugging the screen's own edges rather than
// a tint on the full-surround dim sphere (that one stays flat black - see
// the dim layer below). Short throw on purpose - just the last little bit
// of the picture bleeding out before it fades, not a room-filling wash.
// Gaming mode only for now, same as the rest of the top bar's newer
// modules; Productivity's screens don't get a halo yet.
// Higher res than the plain dim/fade spheres - this one needs a genuinely
// smooth radial gradient, not just a flat alpha, so a coarse texture would
// read as a hard-edged block instead of a fade.
#define GLOW_HALO_TEX 128
// Shrunk from an initial 0.18 - still too big/overpowering at that size.
// Nudged back up slightly from 0.10 to give the now-wider
// SCREEN_EDGE_FEATHER_FRAC dissolve room to land in before the halo itself
// fades out, rather than the two racing to zero at the same distance.
#define GLOW_MARGIN_FRAC 0.13f

typedef struct { float x, y, z; } Vec3;

// One euro filter: a low pass whose cutoff rises with speed, so a resting
// hand is smoothed hard while a fast sweep is barely delayed
typedef struct {
    int valid;
    float x;
    float dx;
} EuroState;

typedef struct {
    JavaVM* vm;
    jobject activity;

    EGLDisplay eglDisplay;
    EGLConfig eglConfig;
    EGLContext eglContext;
    EGLSurface eglPbuffer;

    XrInstance instance;
    XrSystemId systemId;
    XrSession session;
    XrSpace localSpace;
    XrSpace viewSpace;

    XrSwapchain swapchain;
    uint32_t swapchainImageCount;
    XrSwapchainImageOpenGLESKHR* swapchainImages;
    int64_t swapchainFormat;

    // Stats overlay. The activity window is not on screen in an immersive
    // session, so the 2d TextView upstream uses is invisible here and the
    // numbers have to go into the scene as their own layer. Keeping it out of
    // the video swapchain means it is never warped, never doubled, and costs
    // no warp time.
    XrSwapchain overlaySwapchain;
    uint32_t overlayImageCount;
    XrSwapchainImageOpenGLESKHR* overlayImages;
    int overlayHasContent;
    int overlayVisible;

    int videoWidth;
    int videoHeight;

    // Stereo test path. When stereoMode is not OFF the swapchain is double
    // wide and each eye gets its own warped copy of the frame
    int stereoMode;
    int depthDebug;
    // Double buffered: the frame loop samples one while the depth thread
    // writes the other, so neither ever waits on the other
    GLuint depthTextures[2];
    volatile int depthReadIndex;

    // Second context in the same share group for the depth thread. Inference
    // takes longer than a display frame, so it cannot run on the frame loop
    EGLContext depthContext;
    EGLSurface depthPbuffer;

    // Depth model staging. The frame is downscaled to DEPTH_TEX_SIZE on the
    // GPU, read back, run through the model in Java, and the result goes
    // back up into the depth texture
    GLuint downscaleProgram;
    GLint downscaleTexMatrixUniform;
    GLuint downscaleTexture;
    GLuint downscaleFbo;
    unsigned char* readbackBuf;
    float* modelInput;
    float* modelOutput;
    unsigned char* depthUploadBuf;

    // Ambient "glow" colour (v1: a plain average of the current frame) that
    // tints the passthrough dim sphere instead of flat black, so a dark
    // room picks up ambient light coloured like whatever's on screen -
    // independent of the depth model, so it works with 3D effect off too.
    // Own program/target rather than reusing downscaleProgram, since that
    // one only exists when stereoMode == DEPTH_MODE_MODEL.
    GLuint glowProgram;
    GLint glowTexMatrixUniform;
    GLuint glowTexture;
    GLuint glowFbo;
    // One colour per edge/corner sample (see GLOW_SAMPLE_* below),
    // inverse-distance blended across the halo in updateGlowHalo() - v1
    // only ever had one averaged colour for the whole frame, which read
    // as a flat wash rather than actual bias lighting picking up what's
    // near each edge.
    float glowSampleR[GLOW_SAMPLE_COUNT];
    float glowSampleG[GLOW_SAMPLE_COUNT];
    float glowSampleB[GLOW_SAMPLE_COUNT];
    // User on/off for the effect above - separate from whether it's even
    // computed (computeGlowColor() is skipped entirely when this is off, so
    // disabling it also saves the per-frame downscale cost, not just the
    // visual). Icon appears next to the brightness slider specifically.
    int glowEnabled;
    XrSwapchain glowToggleSwapchain;
    uint32_t glowToggleImageCount;
    XrSwapchainImageOpenGLESKHR* glowToggleImages;
    int glowToggleReady;
    // The halo itself - alpha shape recomputed each frame alongside the
    // colour (GLOW_HALO_TEX is small enough that this costs nothing), see
    // updateGlowHalo().
    XrSwapchain glowHaloSwapchain;
    uint32_t glowHaloImageCount;
    XrSwapchainImageOpenGLESKHR* glowHaloImages;
    int glowHaloReady;

    // Temporal smoothing. The normalization range is smoothed separately from
    // the map itself: a single outlier pixel moving the min or max used to
    // shift the whole mapping, which pumps the entire image.
    float* depthEma;
    float* depthLow;
    float* depthScratch;
    float* depthColSums;
    float depthGlobal;
    float depthLocal;
    int depthEmaValid;
    float smoothLo;
    float smoothHi;
    int rangeValid;
    float depthAlpha;
    float rangeAlpha;

    // Edge aware upsample of the depth map, quarter of the video size
    GLuint upsampleProgram;
    GLint upsampleTexMatrixUniform;
    GLint upsampleSigmaUniform;
    GLint upsampleSharpUniform;
    float depthSharp;
    GLuint upsampleTexture;
    GLuint upsampleFbo;
    int upsampleWidth;
    int upsampleHeight;
    int upsampleEnabled;
    float upsampleSigmaR;

    // Occlusion aware offset map, both eyes packed into rg
    GLuint offsetProgram;
    GLint offsetDispUniform;
    GLint offsetConvUniform;
    GLuint offsetTexture;
    GLuint offsetFbo;
    int occlusionEnabled;
    float convergence;
    float separationOverride;
    float distanceOverride;
    float screenOverride;

    GLuint oesTexture;

    // Virtual Sunshine PMode: one independent OES texture per screen, fed by
    // its own background-process MediaCodec decoder (see
    // com.limelight.pmode.PModeScreenServiceBase) - replaces the old design
    // of one shared decoded frame column-cropped into N quads, which only
    // ever showed one real monitor no matter how many quads it was cut into.
    GLuint productivityOesTexture[PRODUCTIVITY_SCREEN_COUNT];
    float productivityTexMatrix[PRODUCTIVITY_SCREEN_COUNT][16];
    int productivityHasFrame[PRODUCTIVITY_SCREEN_COUNT];

    GLuint program;
    GLint texMatrixUniform;
    GLint disparityUniform;
    GLint tintUniform;
    GLint barTestUniform;
    GLint occlusionUniform;
    GLint eyeIndexUniform;
    GLint convergenceUniform;
    GLint dispTexelsUniform;
    GLint lowResWidthUniform;
    GLint frameWidthUniform;
    // Separate from frameWidthUniform above (which is the depth-reprojection
    // texel-stepping width and, in Productivity mode, deliberately the WHOLE
    // swapchain width, not one screen's share of it). This pair is the
    // actual per-draw-call frame size in pixels, used only for the rounded-
    // corner mask - in Productivity mode that's one column's width, not the
    // full swapchain.
    GLint roundFrameSizeUniform;
    GLint cornerRadiusUniform;
    GLint edgeFeatherUniform;
    GLuint fbo;
    int barTestFramesLogged;

    // Frame capture for offline shader work
    char captureDir[256];
    char captureTag[PROP_VALUE_MAX];
    char lastCaptureTag[PROP_VALUE_MAX];
    int captureRequested;
    long capturePollCounter;

    XrSessionState sessionState;
    int sessionRunning;
    int exitRequested;
    XrTime predictedDisplayTime;
    int shouldRender;
    int everRendered;

    int cylinderSupported;
    int equirectSupported;

    int srgbWriteControl;
    // Passthrough is just an environment blend mode: with alpha blend the
    // runtime shows the room wherever our layers do not cover. Both headsets
    // offer it, but Meta only turns the cameras on if the manifest asks.
    int alphaBlendSupported;
    int passthrough;
    // Live on/off for the whole depth/stereo warp - separate from
    // stereoMode below, which is fixed for the session (it decides the
    // swapchain's actual size). This just zeroes separation each frame when
    // off, so toggling never needs to resize anything. Top bar's 3D-effect
    // icon controls it; Java owns the persisted value.
    int depthEffectOn;
    // Hand tracking arrives as another interaction profile rather than as a
    // separate input path, so the pointer does not know the difference
    int handInteraction;
    int msftHandInteraction;
    XrPath handProfile;
    XrPath msftHandProfile;
    int handTracking;
    int handClickOk;
    // Looking at something instead of pointing at it. Lowest priority of the
    // three, so a controller or a hand always wins when one is aiming.
    int eyeGaze;
    int gazeEnabled;
    XrAction gazeAction;
    int lastSnapshot;
    // Reading the joints directly, because a pinch is not always offered as an
    // input. Thumb to fingertip is the whole of it.
    int jointTracking;
    XrHandTrackerEXT handTrackers[HAND_COUNT];
    int jointPinch[HAND_COUNT];
    Vec3 pinchPoint[HAND_COUNT];
    int pinchPointValid[HAND_COUNT];
    // A ray built out of the joints, for runtimes that track hands but do not
    // offer a pointer pose of their own - only actually used when
    // handTrackingAim is false, see jointPinching().
    XrPosef handRay[HAND_COUNT];
    int handRayValid[HAND_COUNT];
    // Meta-specific ready-made aim pose + pinch strength, preferred over
    // buildHandRay()'s shoulder-ray math and the raw thumb/index gap
    // distance below when available - see jointPinching().
    int handTrackingAim;
    PFN_xrCreateHandTrackerEXT pfnCreateHandTracker;
    PFN_xrDestroyHandTrackerEXT pfnDestroyHandTracker;
    PFN_xrLocateHandJointsEXT pfnLocateHandJoints;
    // Asks the runtime to keep CPU/GPU clocks up for the whole session
    // instead of whatever it would otherwise pick for an app it doesn't
    // recognise as latency sensitive - see handleSessionStateChange().
    int perfSettingsSupported;
    PFN_xrPerfSettingsSetPerformanceLevelEXT pfnSetPerfLevel;
    int usingHands[SRC_COUNT];
    // A pinch that woke the pointer is not also a click, so it is swallowed
    // until the hand opens again
    int pinchSwallowed[SRC_COUNT];

    PFN_xrGetOpenGLESGraphicsRequirementsKHR pfnGetGlesReqs;

    // Controller input. The aim ray is intersected with the screen and the hit
    // point drives the host mouse, so the PC sees an ordinary absolute mouse
    XrActionSet actionSet;
    XrAction aimAction;
    XrAction triggerAction;
    XrAction rightClickAction;
    XrAction middleClickAction;
    XrAction scrollAction;
    XrAction grabAction;
    XrAction toggleAction;
    XrAction hapticAction;
    XrSpace aimSpaces[SRC_COUNT];
    XrPath handPaths[HAND_COUNT];
    int inputReady;
    int picoInteraction;
    // Pointing is a per session toggle on top of the preference, since
    // absolute positions fight any game that does its own mouse look
    int pointerOn;
    int togglePrev;
    int triggerDown[SRC_COUNT];
    // Rising edges, so a button already held when the ray wanders onto a handle
    // does not grab it. Dragging a window on the host desktop past the edge of
    // the picture would otherwise turn into a resize.
    int triggerEdge[SRC_COUNT];
    // Indexed by the chosen source, and gaze has no grip, so it needs the
    // extra slot even though nothing ever writes to it
    int gripEdge[SRC_COUNT];
    int grabByTrigger;
    int buttonsDown;
    int pmodeButtonsDown;
    float scrollCarry;
    long lastInputNs;

    // One euro filter state for the hit point, per axis
    EuroState filterU;
    EuroState filterV;
    float pointerMinCutoff;
    float pointerBeta;
    long lastHitNs;
    int lastHand;

    // Movement gate. The pointer only appears after the controller has been
    // moved deliberately, and disappears once it has been still a while.
    int poseSeen[SRC_COUNT];
    XrPosef lastAim[SRC_COUNT];
    float movingFor;
    float stillFor;
    int pointerAwake;
    float pointerWake;
    float pointerSleep;

    // Laser. Two tiny quad layers rather than a projection layer: the whole
    // renderer draws nothing per frame for this, the compositor places it
    XrSwapchain pointerSwapchain;
    uint32_t pointerImageCount;
    XrSwapchainImageOpenGLESKHR* pointerImages;
    int pointerArtReady;
    int beamVisible;
    // Ray drawn with nothing under it, so there is no cursor to go with it
    int beamFree;
    // Aimed by the eyes, so there is a cursor but no ray
    int beamGaze;
    XrVector3f beamStart;
    XrVector3f beamEnd;
    XrVector3f headPos;
    XrQuaternionf screenOrientation;
    float beamWidth;

    // Productivity mode: N flat screens instead of the single stereo one
    // above. Phase 1 only - fixed default arrangement, no grab/rotate/
    // arrange controls yet (that's all still ctx->screenPose/grab* below,
    // untouched and unused while this is set).
    int productivityMode;

    // Where the screen actually is. Seeded from the distance and width
    // preferences and then owned by the grab, so moving it does not fight the
    // sliders. Touching either slider puts it back under their control.
    XrPosef screenPose;
    float screenWidth;
    float screenRadius;
    int placementValid;
    int sliderSeen;
    float lastDistance;
    float lastQuadWidth;

    int grabDown[SRC_COUNT];
    int grabMode;
    int grabHand;
    float grabU, grabV;
    XrPosef grabAim;
    XrPosef grabScreen;
    float grabWidth;
    float grabHeight;
    float grabRadius;
    // Resize works against the corner opposite the one being dragged, which
    // stays put, and along the diagonal it started on
    float grabOppX, grabOppY;
    float grabDiagX, grabDiagY;
    int poseDirty;

    // Passthrough brightness slider, opened from the top bar. 1.0 = full
    // passthrough (default), 0.0 = full black. GRAB_SLIDER reuses grabMode/
    // grabHand above rather than adding a parallel pair of fields.
    float passthroughLevel;
    int passthroughLevelDirty;
    // 0 = flat, 1 = ~180 degree wrap - drives ctx->screenRadius directly
    // (see updateTopBar()), independent of updatePlacement()'s own seed so
    // dragging it never resets screen position/pose.
    float curveAmount;
    int curveAmountDirty;
    // TOPBAR_NO_SLIDER, or the index of the item whose slider is open
    int openSlider;
    // Snapshot of openSlider taken when a drag starts, so the drag keeps
    // controlling the same value even if openSlider is somehow touched by
    // the other hand mid-drag
    int grabSliderTarget;

    // Hover state, read by the frame loop to decide which handle to draw
    int hoverKind;
    int hoverCorner;
    XrSwapchain barSwapchain;
    XrSwapchain cornerSwapchain;
    uint32_t barImageCount;
    uint32_t cornerImageCount;
    XrSwapchainImageOpenGLESKHR* barImages;
    XrSwapchainImageOpenGLESKHR* cornerImages;
    int handleArtReady;

    // Top bar icon strip (Exit, Brightness, ...), one texture wide enough for
    // TOPBAR_ITEM_COUNT cells, drawn in Java and uploaded whole - see
    // nativeUploadTopBarArt. Shared by both modes; see topBarPose().
    XrSwapchain topBarSwapchain;
    uint32_t topBarImageCount;
    XrSwapchainImageOpenGLESKHR* topBarImages;
    int topBarReady;

    // Brightness slider, opened from the top bar. Track is static art built
    // once like the move bar; the thumb is its own quad so dragging it is
    // just a pose update, never a texture re-upload.
    XrSwapchain sliderTrackSwapchain;
    XrSwapchain sliderThumbSwapchain;
    uint32_t sliderTrackImageCount;
    uint32_t sliderThumbImageCount;
    XrSwapchainImageOpenGLESKHR* sliderTrackImages;
    XrSwapchainImageOpenGLESKHR* sliderThumbImages;
    int sliderArtReady;

    // Full-surround dim layer (an equirect sphere, radius 0 = infinite) that
    // the passthrough brightness slider controls. Solid black; only its alpha
    // ever changes, re-uploaded solely when passthroughLevel actually moves.
    XrSwapchain dimSwapchain;
    uint32_t dimImageCount;
    XrSwapchainImageOpenGLESKHR* dimImages;
    int dimReady;
    float dimUploadedLevel;
    int dimUploadedValid;

    // Whole-view fade sphere - same recipe as the dim layer above (tiny
    // solid-black alpha-only equirect), but its own independent swapchain
    // and alpha, since it has to sit in front of the screen too. See
    // FADE_NONE/FADE_IN/FADE_OUT.
    XrSwapchain fadeSwapchain;
    uint32_t fadeImageCount;
    XrSwapchainImageOpenGLESKHR* fadeImages;
    int fadeReady;
    float fadeUploadedAlpha;
    int fadeUploadedValid;
    int fadeState;
    float fadeAlpha;       // 0 = clear, 1 = opaque black
    float fadeFromAlpha;   // ctx->fadeAlpha captured when the current phase started
    long fadeStartNs;
    // Set once a FADE_OUT run reaches full black; consumed (and cleared) by
    // nativeUpdateInput on the next frame, which is what actually raises
    // IN_EXIT_PRESSED - deferred this way so Java's finish() never lands on
    // a frame the user could still see, only on one that's already black.
    int fadeOutComplete;

    long statFrames;
    long statTotalNs;
    long statMaxNs;

    // Real GPU time for the warp passes. The wall clock around the draw calls
    // only ever measured how long submission took, since nothing waits on the
    // GPU, so it read about 0.1 ms no matter what the shaders did.
    int timerSupported;
    GLuint timerQueries[2];
    int timerSlot;
    int timerPending[2];
    // A query whose result never lands would wedge the pair forever, since
    // the slot only flips once the outstanding one is collected
    int timerPendingFrames[2];
    long gpuTotalNs;
    long gpuMaxNs;
    long gpuSamples;

    // Separate accumulator so reading the number for the overlay does not
    // disturb the logcat cadence
    long overlayGpuTotalNs;
    long overlayGpuSamples;
} XrCtx;

typedef void (*PFNGENQUERIESEXT)(GLsizei, GLuint*);
typedef void (*PFNBEGINQUERYEXT)(GLenum, GLuint);
typedef void (*PFNENDQUERYEXT)(GLenum);
typedef void (*PFNGETQUERYOBJECTUIVEXT)(GLuint, GLenum, GLuint*);
typedef void (*PFNGETQUERYOBJECTUI64VEXT)(GLuint, GLenum, GLuint64*);

static PFNGENQUERIESEXT pfnGenQueries;
static PFNBEGINQUERYEXT pfnBeginQuery;
static PFNENDQUERYEXT pfnEndQuery;
static PFNGETQUERYOBJECTUIVEXT pfnGetQueryObjectuiv;
static PFNGETQUERYOBJECTUI64VEXT pfnGetQueryObjectui64v;

#ifndef GL_TIME_ELAPSED_EXT
#define GL_TIME_ELAPSED_EXT 0x88BF
#endif
#ifndef GL_QUERY_RESULT_EXT
#define GL_QUERY_RESULT_EXT 0x8866
#endif
#ifndef GL_QUERY_RESULT_AVAILABLE_EXT
#define GL_QUERY_RESULT_AVAILABLE_EXT 0x8867
#endif

static const char* VERTEX_SRC =
    "#version 300 es\n"
    "in vec4 a_position;\n"
    "in vec4 a_texcoord;\n"
    "out vec2 v_plain;\n"
    "void main() {\n"
    "    gl_Position = a_position;\n"
    "    v_plain = a_texcoord.xy;\n"
    "}\n";

// Gather warp. Each output pixel samples the color frame shifted by a
// disparity derived from the depth map. u_disparity is signed per eye and
// zero in mono, which makes this exactly the old passthrough. The transform
// matrix is applied after the shift since the shift is defined in frame
// space, not in the video driver's transformed space.
static const char* FRAGMENT_SRC =
    "#version 300 es\n"
    "#extension GL_OES_EGL_image_external_essl3 : require\n"
    "precision highp float;\n"
    "in vec2 v_plain;\n"
    "uniform samplerExternalOES u_texture;\n"
    "uniform sampler2D u_depth;\n"
    "uniform sampler2D u_offsets;\n"
    "uniform mat4 u_texmatrix;\n"
    "uniform float u_disparity;\n"
    "uniform float u_showDepth;\n"
    "uniform float u_barTest;\n"
    "uniform vec3 u_tint;\n"
    "uniform float u_occlusion;\n"
    "uniform float u_eyeIndex;\n"
    "uniform float u_convergence;\n"
    "uniform float u_dispTexels;\n"
    "uniform float u_lowResWidth;\n"
    "uniform float u_frameWidth;\n"
    // Separate from u_frameWidth (that one is deliberately the full
    // swapchain width in Productivity mode - see roundFrameSizeUniform's
    // comment at its declaration). Radius of 0 disables the mask entirely.
    "uniform vec2 u_roundFrameSize;\n"
    "uniform float u_cornerRadiusPx;\n"
    // Half-width (pixels) of the soft fade band centred on the rounded-rect
    // boundary - 0 falls back to a bare ~1px antialiasing edge.
    "uniform float u_edgeFeatherPx;\n"
    "out vec4 fragColor;\n"
    "void main() {\n"
    "    float d = texture(u_depth, v_plain).a;\n"
    "    if (u_showDepth > 0.5) {\n"
    "        fragColor = vec4(d, d, d, 1.0);\n"
    "        return;\n"
    "    }\n"
    "    vec2 tc = v_plain;\n"
    "    if (u_occlusion > 0.5) {\n"
    // The offset map already picked the right surface. All that is left is
    // the exact position on it, which the low resolution search only knew to
    // within a texel, and that quantization stair steps along a diagonal
    // silhouette. Two Newton steps against the full resolution depth settle
    // it to well under a pixel.
    "        int reach = int(ceil(abs(u_dispTexels)\n"
    "                        * max(u_convergence, 1.0 - u_convergence))) + 2;\n"
    "        vec2 enc = texture(u_offsets, v_plain).rg;\n"
    "        float off = (u_eyeIndex < 0.5 ? enc.r : enc.g) - 0.5;\n"
    "        tc.x = v_plain.x + off * 2.0 * float(reach) / u_lowResWidth;\n"
    "        float h = 1.0 / u_frameWidth;\n"
    "        for (int i = 0; i < 2; i++) {\n"
    "            float d0 = texture(u_depth, vec2(tc.x, v_plain.y)).a;\n"
    "            float dm = texture(u_depth, vec2(tc.x - h, v_plain.y)).a;\n"
    "            float dp = texture(u_depth, vec2(tc.x + h, v_plain.y)).a;\n"
    "            float e = (tc.x - v_plain.x) + u_disparity * (d0 - u_convergence);\n"
    "            float slope = 1.0 + u_disparity * (dp - dm) / (2.0 * h);\n"
    "            if (abs(slope) < 0.25) {\n"
    "                slope = 0.25;\n"
    "            }\n"
    "            tc.x -= clamp(e / slope, -4.0 * h, 4.0 * h);\n"
    "        }\n"
    "    }\n"
    "    else {\n"
    "        tc.x -= u_disparity * (d - u_convergence);\n"
    "    }\n"
    "    if (u_barTest > 0.5) {\n"
    "        float b = 1.0 - step(0.004, abs(tc.x - 0.5));\n"
    "        fragColor = vec4(b, b, b, 1.0);\n"
    "        return;\n"
    "    }\n"
    "    fragColor = texture(u_texture, (u_texmatrix * vec4(tc, 0.0, 1.0)).xy);\n"
    "    fragColor.rgb *= u_tint;\n"
    // Standard rounded-box signed distance field (Inigo Quilez's formula),
    // worked in physical pixels via u_roundFrameSize so a corner stays a
    // true circle rather than an ellipse on a non-square (e.g. ultra-wide)
    // frame. The layer's blend mode expects premultiplied alpha, so both
    // rgb and a get scaled together - see the layerFlags comment at the
    // quad/cylinder submission site.
    "    if (u_cornerRadiusPx > 0.0) {\n"
    "        vec2 pos = v_plain * u_roundFrameSize;\n"
    "        vec2 halfSize = u_roundFrameSize * 0.5;\n"
    "        vec2 cornerRel = abs(pos - halfSize) - (halfSize - u_cornerRadiusPx);\n"
    "        float dist = length(max(cornerRel, 0.0)) - u_cornerRadiusPx;\n"
    "        float feather = max(u_edgeFeatherPx, 1.0);\n"
    "        float edgeAlpha = 1.0 - smoothstep(-feather, feather, dist);\n"
    "        fragColor.rgb *= edgeAlpha;\n"
    "        fragColor.a *= edgeAlpha;\n"
    "    }\n"
    "}\n";

// Joint bilateral upsample of the depth map. The model output is 256x256
// against a 4K frame, so one depth texel covers a 15x8 block and every depth
// boundary reaches the warp as a 15 pixel ramp. That ramp is the halo: it
// shears whatever colour happens to sit under it.
//
// Each output pixel weights the 5x5 low resolution depth neighbourhood by how
// closely each neighbour's colour matches the colour here, so the depth edge
// snaps to the colour edge instead of straddling it. Measured on a captured
// frame this takes the edge from 15 px to 5 px, which is the resolution limit
// of a 256x256 source rather than of this filter.
//
// The guide rides in the rgb of the depth texture, so it is by construction
// the same frame the depth was inferred from. u_sigmaR trades edge snapping
// against depth detail invented out of colour texture: grass and carpet will
// speckle if it is set too tight.
static const char* UPSAMPLE_FRAGMENT_SRC =
    "#version 300 es\n"
    "#extension GL_OES_EGL_image_external_essl3 : require\n"
    "precision highp float;\n"
    "in vec2 v_plain;\n"
    "uniform samplerExternalOES u_texture;\n"
    "uniform sampler2D u_depth;\n"
    "uniform mat4 u_texmatrix;\n"
    "uniform float u_sigmaR;\n"
    "uniform float u_sharp;\n"
    "out vec4 fragColor;\n"
    "const float N = 256.0;\n"
    "const float SIGMA_S = 1.5;\n"
    "const float FLAT = 0.05;\n"
    "void main() {\n"
    "    vec3 hi = texture(u_texture, (u_texmatrix * vec4(v_plain, 0.0, 1.0)).xy).rgb;\n"
    "    vec2 lp = v_plain * N - 0.5;\n"
    "    ivec2 base = ivec2(floor(lp));\n"
    "    float num = 0.0;\n"
    "    float den = 0.0;\n"
    "    float dlo = 1.0;\n"
    "    float dhi = 0.0;\n"
    "    for (int dy = -2; dy <= 2; dy++) {\n"
    "        for (int dx = -2; dx <= 2; dx++) {\n"
    "            ivec2 q = clamp(base + ivec2(dx, dy), ivec2(0), ivec2(int(N) - 1));\n"
    "            vec4 s = texelFetch(u_depth, q, 0);\n"
    "            vec2 off = vec2(q) - lp;\n"
    "            float ws = exp(-dot(off, off) / (2.0 * SIGMA_S * SIGMA_S));\n"
    "            vec3 cd = hi - s.rgb;\n"
    "            float wr = exp(-dot(cd, cd) / (2.0 * u_sigmaR * u_sigmaR));\n"
    "            float w = ws * wr;\n"
    "            num += w * s.a;\n"
    "            den += w;\n"
    "            dlo = min(dlo, s.a);\n"
    "            dhi = max(dhi, s.a);\n"
    "        }\n"
    "    }\n"
    "    float d = num / max(den, 1e-6);\n"
    // A soft depth ramp across a silhouette spreads the disocclusion over the
    // width of the ramp, and that band is the smear. Pushing each texel to
    // whichever side of the local range it is nearer turns the ramp back into
    // a step, using the min and max of taps already read. Flat neighbourhoods
    // are left alone, so only boundaries move.
    "    float span = dhi - dlo;\n"
    "    if (u_sharp > 0.0 && span >= FLAT) {\n"
    "        float u = clamp((d - dlo) / max(span, 1e-6), 0.0, 1.0);\n"
    "        float snapped = dlo + span / (1.0 + exp(-24.0 * (u - 0.5)));\n"
    "        d = mix(d, snapped, u_sharp);\n"
    "    }\n"
    "    fragColor = vec4(d);\n"
    "}\n";

// Inverts the warp properly, once per frame for both eyes, at the same
// quarter resolution as the depth map.
//
// A source pixel at offset t from this one lands here with error
//     e(t) = t + disp * (d(here + t) - convergence)
// so every zero crossing of e is a source that genuinely lands on this pixel.
// Sampling depth at the destination, which is what the warp did before, is
// only right where depth is flat; at a depth step it is wrong by most of the
// disparity range, which is 57 px at 4K, and that is the smearing. More than
// one crossing means two surfaces compete for this pixel, and the nearest one
// wins, which is what occlusion means.
//
// The whole search span is only about nine texels at this resolution, so the
// exhaustive version is affordable. Both eyes share the depth reads.
static const char* OFFSET_FRAGMENT_SRC =
    "#version 300 es\n"
    "precision highp float;\n"
    "in vec2 v_plain;\n"
    "uniform sampler2D u_depth;\n"
    "uniform float u_dispTexels;\n"
    "uniform float u_convergence;\n"
    "out vec4 fragColor;\n"
    "void main() {\n"
    "    ivec2 sz = textureSize(u_depth, 0);\n"
    "    int x = int(gl_FragCoord.x);\n"
    "    int y = int(gl_FragCoord.y);\n"
    "    int reach = int(ceil(abs(u_dispTexels)\n"
    "                    * max(u_convergence, 1.0 - u_convergence))) + 2;\n"
    "    vec2 result = vec2(0.0);\n"
    "    for (int eye = 0; eye < 2; eye++) {\n"
    "        float disp = (eye == 0) ? u_dispTexels : -u_dispTexels;\n"
    "        float here = texelFetch(u_depth, ivec2(x, y), 0).a;\n"
    "        float bestD = -1.0;\n"
    "        float bestOff = -disp * (here - u_convergence);\n"
    "        float pd = texelFetch(u_depth,\n"
    "                ivec2(clamp(x - reach, 0, sz.x - 1), y), 0).a;\n"
    "        float pe = float(-reach) + disp * (pd - u_convergence);\n"
    "        for (int t = -reach + 1; t <= reach; t++) {\n"
    "            float cd = texelFetch(u_depth,\n"
    "                    ivec2(clamp(x + t, 0, sz.x - 1), y), 0).a;\n"
    "            float ce = float(t) + disp * (cd - u_convergence);\n"
    "            float span = ce - pe;\n"
    "            if (pe * ce <= 0.0 && abs(span) > 1e-6) {\n"
    "                float f = clamp(-pe / span, 0.0, 1.0);\n"
    "                float rd = pd + f * (cd - pd);\n"
    "                if (rd > bestD) {\n"
    "                    bestD = rd;\n"
    "                    bestOff = float(t - 1) + f;\n"
    "                }\n"
    "            }\n"
    "            pd = cd;\n"
    "            pe = ce;\n"
    "        }\n"
    "        result[eye] = bestOff;\n"
    "    }\n"
    "    fragColor = vec4(result / (2.0 * float(reach)) + 0.5, 0.0, 1.0);\n"
    "}\n";

// Feeds the depth model. The video is far larger than 256x256, so a single
// bilinear tap per output pixel aliases badly and the depth map crawls with
// it. A 4x4 box over each destination pixel is still nothing on this GPU.
static const char* DOWNSCALE_FRAGMENT_SRC =
    "#version 300 es\n"
    "#extension GL_OES_EGL_image_external_essl3 : require\n"
    "precision highp float;\n"
    "in vec2 v_plain;\n"
    "uniform samplerExternalOES u_texture;\n"
    "uniform mat4 u_texmatrix;\n"
    "out vec4 fragColor;\n"
    "void main() {\n"
    "    vec3 sum = vec3(0.0);\n"
    "    for (int y = 0; y < 4; y++) {\n"
    "        for (int x = 0; x < 4; x++) {\n"
    "            vec2 off = (vec2(float(x), float(y)) - 1.5) * (0.25 / 256.0);\n"
    "            vec2 tc = v_plain + off;\n"
    "            sum += texture(u_texture, (u_texmatrix * vec4(tc, 0.0, 1.0)).xy).rgb;\n"
    "        }\n"
    "    }\n"
    "    fragColor = vec4(sum * (1.0 / 16.0), 1.0);\n"
    "}\n";

// Same fullscreen strip as the 2d GL path, x y u v
static const float VERTEX_DATA[] = {
    -1.0f, -1.0f, 0.0f, 0.0f,
     1.0f, -1.0f, 1.0f, 0.0f,
    -1.0f,  1.0f, 0.0f, 1.0f,
     1.0f,  1.0f, 1.0f, 1.0f,
};

static long nowNs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000000L + ts.tv_nsec;
}

static int checkXr(XrResult res, const char* what) {
    if (XR_FAILED(res)) {
        LOGE("%s failed: %d", what, res);
        return 0;
    }
    return 1;
}

static GLuint compileShader(GLenum type, const char* src) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &src, NULL);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(shader, sizeof(log), NULL, log);
        LOGE("shader compile failed: %s", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static int initEgl(XrCtx* ctx) {
    ctx->eglDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (ctx->eglDisplay == EGL_NO_DISPLAY) {
        LOGE("eglGetDisplay failed");
        return 0;
    }
    if (!eglInitialize(ctx->eglDisplay, NULL, NULL)) {
        LOGE("eglInitialize failed");
        return 0;
    }

    const EGLint configAttribs[] = {
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_NONE
    };
    EGLint numConfigs = 0;
    if (!eglChooseConfig(ctx->eglDisplay, configAttribs, &ctx->eglConfig, 1, &numConfigs) ||
            numConfigs < 1) {
        LOGE("eglChooseConfig failed");
        return 0;
    }

    const EGLint contextAttribs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    ctx->eglContext = eglCreateContext(ctx->eglDisplay, ctx->eglConfig, EGL_NO_CONTEXT, contextAttribs);
    if (ctx->eglContext == EGL_NO_CONTEXT) {
        LOGE("eglCreateContext failed: %d", eglGetError());
        return 0;
    }

    // The context needs a surface current but everything renders to FBOs
    const EGLint pbufferAttribs[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
    ctx->eglPbuffer = eglCreatePbufferSurface(ctx->eglDisplay, ctx->eglConfig, pbufferAttribs);
    if (ctx->eglPbuffer == EGL_NO_SURFACE) {
        LOGE("eglCreatePbufferSurface failed: %d", eglGetError());
        return 0;
    }

    if (!eglMakeCurrent(ctx->eglDisplay, ctx->eglPbuffer, ctx->eglPbuffer, ctx->eglContext)) {
        LOGE("eglMakeCurrent failed: %d", eglGetError());
        return 0;
    }

    return 1;
}

static int initXrInstance(XrCtx* ctx) {
    PFN_xrInitializeLoaderKHR initLoader = NULL;
    xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR",
                          (PFN_xrVoidFunction*)&initLoader);
    if (initLoader != NULL) {
        XrLoaderInitInfoAndroidKHR loaderInfo = { XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR };
        loaderInfo.applicationVM = ctx->vm;
        loaderInfo.applicationContext = ctx->activity;
        initLoader((XrLoaderInitInfoBaseHeaderKHR*)&loaderInfo);
    }

    uint32_t extCount = 0;
    xrEnumerateInstanceExtensionProperties(NULL, 0, &extCount, NULL);
    XrExtensionProperties* exts = calloc(extCount, sizeof(XrExtensionProperties));
    for (uint32_t i = 0; i < extCount; i++) {
        exts[i].type = XR_TYPE_EXTENSION_PROPERTIES;
    }
    xrEnumerateInstanceExtensionProperties(NULL, extCount, &extCount, exts);

    int haveGles = 0, haveAndroidCreate = 0;
    LOGI("runtime offers %u OpenXR extensions", extCount);
    for (uint32_t i = 0; i < extCount; i++) {
        LOGI("  extension %s", exts[i].extensionName);
        if (!strcmp(exts[i].extensionName, XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME)) haveGles = 1;
        if (!strcmp(exts[i].extensionName, XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME)) haveAndroidCreate = 1;
        if (!strcmp(exts[i].extensionName, XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME)) ctx->cylinderSupported = 1;
        if (!strcmp(exts[i].extensionName, XR_BD_CONTROLLER_INTERACTION_EXTENSION_NAME)) ctx->picoInteraction = 1;
        if (!strcmp(exts[i].extensionName, XR_KHR_COMPOSITION_LAYER_EQUIRECT2_EXTENSION_NAME)) ctx->equirectSupported = 1;
        if (!strcmp(exts[i].extensionName, XR_EXT_HAND_INTERACTION_EXTENSION_NAME)) ctx->handInteraction = 1;
        if (!strcmp(exts[i].extensionName, XR_MSFT_HAND_INTERACTION_EXTENSION_NAME)) ctx->msftHandInteraction = 1;
        if (!strcmp(exts[i].extensionName, XR_EXT_HAND_TRACKING_EXTENSION_NAME)) ctx->handTracking = 1;
        if (!strcmp(exts[i].extensionName, XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME)) ctx->eyeGaze = 1;
        if (!strcmp(exts[i].extensionName, XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME)) ctx->perfSettingsSupported = 1;
        if (!strcmp(exts[i].extensionName, XR_FB_HAND_TRACKING_AIM_EXTENSION_NAME)) ctx->handTrackingAim = 1;
    }
    free(exts);

    if (!haveGles || !haveAndroidCreate) {
        LOGE("required OpenXR extensions missing (gles=%d androidCreate=%d)", haveGles, haveAndroidCreate);
        return 0;
    }

    const char* enabledExts[11];
    uint32_t enabledCount = 0;
    enabledExts[enabledCount++] = XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME;
    enabledExts[enabledCount++] = XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME;
    if (ctx->cylinderSupported) {
        enabledExts[enabledCount++] = XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME;
    }
    if (ctx->picoInteraction) {
        enabledExts[enabledCount++] = XR_BD_CONTROLLER_INTERACTION_EXTENSION_NAME;
    }
    if (ctx->equirectSupported) {
        enabledExts[enabledCount++] = XR_KHR_COMPOSITION_LAYER_EQUIRECT2_EXTENSION_NAME;
    }
    if (ctx->handInteraction) {
        enabledExts[enabledCount++] = XR_EXT_HAND_INTERACTION_EXTENSION_NAME;
    }
    // Some runtimes will not honour the hand interaction profile unless the
    // tracking extension is enabled next to it
    if (ctx->handTracking) {
        enabledExts[enabledCount++] = XR_EXT_HAND_TRACKING_EXTENSION_NAME;
    }
    if (ctx->eyeGaze) {
        enabledExts[enabledCount++] = XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME;
    }
    if (ctx->msftHandInteraction) {
        enabledExts[enabledCount++] = XR_MSFT_HAND_INTERACTION_EXTENSION_NAME;
    }
    if (ctx->perfSettingsSupported) {
        enabledExts[enabledCount++] = XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME;
    }
    // Meta-specific: a ready-made aim pose + per-finger pinch strength
    // chained onto the same xrLocateHandJointsEXT call jointPinching()
    // already makes, instead of hand-rolling both from raw joint math -
    // see buildHandRay()/jointPinching() for the fallback used when this
    // isn't available (other runtimes, e.g. Pico, don't have it).
    if (ctx->handTrackingAim) {
        enabledExts[enabledCount++] = XR_FB_HAND_TRACKING_AIM_EXTENSION_NAME;
    }

    XrInstanceCreateInfoAndroidKHR androidInfo = { XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR };
    androidInfo.applicationVM = ctx->vm;
    androidInfo.applicationActivity = ctx->activity;

    XrInstanceCreateInfo createInfo = { XR_TYPE_INSTANCE_CREATE_INFO };
    createInfo.next = &androidInfo;
    strncpy(createInfo.applicationInfo.applicationName, "Moonlight", XR_MAX_APPLICATION_NAME_SIZE - 1);
    createInfo.applicationInfo.applicationVersion = 1;
    strncpy(createInfo.applicationInfo.engineName, "Moonlight", XR_MAX_ENGINE_NAME_SIZE - 1);
    createInfo.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    createInfo.enabledExtensionCount = enabledCount;
    createInfo.enabledExtensionNames = enabledExts;

    if (!checkXr(xrCreateInstance(&createInfo, &ctx->instance), "xrCreateInstance")) {
        return 0;
    }

    if (ctx->perfSettingsSupported) {
        if (XR_FAILED(xrGetInstanceProcAddr(ctx->instance, "xrPerfSettingsSetPerformanceLevelEXT",
                                            (PFN_xrVoidFunction*)&ctx->pfnSetPerfLevel))) {
            ctx->pfnSetPerfLevel = NULL;
            ctx->perfSettingsSupported = 0;
        }
    }

    XrSystemGetInfo systemInfo = { XR_TYPE_SYSTEM_GET_INFO };
    systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (!checkXr(xrGetSystem(ctx->instance, &systemInfo, &ctx->systemId), "xrGetSystem")) {
        return 0;
    }

    uint32_t blendModeCount = 0;
    xrEnumerateEnvironmentBlendModes(ctx->instance, ctx->systemId,
                                     XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                     0, &blendModeCount, NULL);
    if (blendModeCount > 0) {
        XrEnvironmentBlendMode* modes = calloc(blendModeCount, sizeof(XrEnvironmentBlendMode));
        xrEnumerateEnvironmentBlendModes(ctx->instance, ctx->systemId,
                                         XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                         blendModeCount, &blendModeCount, modes);
        for (uint32_t i = 0; i < blendModeCount; i++) {
            LOGI("environment blend mode %u available", modes[i]);
            if (modes[i] == XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND) {
                ctx->alphaBlendSupported = 1;
            }
        }
        free(modes);
    }
    LOGI("passthrough %s", ctx->alphaBlendSupported ? "available" : "not offered by this runtime");

    // Offering the extension is not the same as having the hardware, so the
    // system is asked directly before anything is bound to a gaze
    if (ctx->eyeGaze) {
        XrSystemEyeGazeInteractionPropertiesEXT gazeProps = {
            XR_TYPE_SYSTEM_EYE_GAZE_INTERACTION_PROPERTIES_EXT
        };
        XrSystemProperties props = { XR_TYPE_SYSTEM_PROPERTIES };
        props.next = &gazeProps;
        if (XR_FAILED(xrGetSystemProperties(ctx->instance, ctx->systemId, &props))
                || !gazeProps.supportsEyeGazeInteraction) {
            ctx->eyeGaze = 0;
        }
        LOGI("eye gaze %s", ctx->eyeGaze ? "available" : "offered but not supported by this system");
    }

    if (ctx->handTracking) {
        XrSystemHandTrackingPropertiesEXT handProps = {
            XR_TYPE_SYSTEM_HAND_TRACKING_PROPERTIES_EXT
        };
        XrSystemProperties props = { XR_TYPE_SYSTEM_PROPERTIES };
        props.next = &handProps;
        if (XR_FAILED(xrGetSystemProperties(ctx->instance, ctx->systemId, &props))
                || !handProps.supportsHandTracking) {
            ctx->handTracking = 0;
        }
        LOGI("hand joints %s", ctx->handTracking ? "available" : "not supported by this system");
    }

    xrGetInstanceProcAddr(ctx->instance, "xrGetOpenGLESGraphicsRequirementsKHR",
                          (PFN_xrVoidFunction*)&ctx->pfnGetGlesReqs);
    if (ctx->pfnGetGlesReqs == NULL) {
        LOGE("xrGetOpenGLESGraphicsRequirementsKHR not found");
        return 0;
    }

    return 1;
}

static int initXrSession(XrCtx* ctx) {
    // Spec requires this call before session creation
    XrGraphicsRequirementsOpenGLESKHR reqs = { XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR };
    if (!checkXr(ctx->pfnGetGlesReqs(ctx->instance, ctx->systemId, &reqs), "get gles requirements")) {
        return 0;
    }

    XrGraphicsBindingOpenGLESAndroidKHR binding = { XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR };
    binding.display = ctx->eglDisplay;
    binding.config = ctx->eglConfig;
    binding.context = ctx->eglContext;

    XrSessionCreateInfo sessionInfo = { XR_TYPE_SESSION_CREATE_INFO };
    sessionInfo.next = &binding;
    sessionInfo.systemId = ctx->systemId;
    if (!checkXr(xrCreateSession(ctx->instance, &sessionInfo, &ctx->session), "xrCreateSession")) {
        return 0;
    }

    XrReferenceSpaceCreateInfo spaceInfo = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    spaceInfo.poseInReferenceSpace.orientation.w = 1.0f;
    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    if (!checkXr(xrCreateReferenceSpace(ctx->session, &spaceInfo, &ctx->localSpace), "create local space")) {
        return 0;
    }
    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    if (!checkXr(xrCreateReferenceSpace(ctx->session, &spaceInfo, &ctx->viewSpace), "create view space")) {
        return 0;
    }

    return 1;
}

static int initSwapchain(XrCtx* ctx) {
    uint32_t formatCount = 0;
    xrEnumerateSwapchainFormats(ctx->session, 0, &formatCount, NULL);
    int64_t* formats = calloc(formatCount, sizeof(int64_t));
    xrEnumerateSwapchainFormats(ctx->session, formatCount, &formatCount, formats);

    ctx->swapchainFormat = 0;
    for (uint32_t i = 0; i < formatCount; i++) {
        if (formats[i] == GL_SRGB8_ALPHA8) {
            ctx->swapchainFormat = GL_SRGB8_ALPHA8;
            break;
        }
    }
    if (ctx->swapchainFormat == 0 && formatCount > 0) {
        ctx->swapchainFormat = formats[0];
        LOGW("no SRGB8_ALPHA8 swapchain format, using %lld", (long long)ctx->swapchainFormat);
    }
    free(formats);

    // Stereo renders left and right eye views side by side in one swapchain
    int chainWidth = ctx->stereoMode != DEPTH_MODE_OFF ? ctx->videoWidth * 2 : ctx->videoWidth;

    XrSwapchainCreateInfo swapInfo = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
    swapInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    swapInfo.format = ctx->swapchainFormat;
    swapInfo.sampleCount = 1;
    swapInfo.width = chainWidth;
    swapInfo.height = ctx->videoHeight;
    swapInfo.faceCount = 1;
    swapInfo.arraySize = 1;
    swapInfo.mipCount = 1;
    if (!checkXr(xrCreateSwapchain(ctx->session, &swapInfo, &ctx->swapchain), "xrCreateSwapchain")) {
        return 0;
    }

    xrEnumerateSwapchainImages(ctx->swapchain, 0, &ctx->swapchainImageCount, NULL);
    ctx->swapchainImages = calloc(ctx->swapchainImageCount, sizeof(XrSwapchainImageOpenGLESKHR));
    for (uint32_t i = 0; i < ctx->swapchainImageCount; i++) {
        ctx->swapchainImages[i].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR;
    }
    if (!checkXr(xrEnumerateSwapchainImages(ctx->swapchain, ctx->swapchainImageCount,
            &ctx->swapchainImageCount, (XrSwapchainImageBaseHeader*)ctx->swapchainImages),
            "enumerate swapchain images")) {
        return 0;
    }

    LOGI("swapchain %dx%d format %lld, %u images (stereo mode %d)", chainWidth, ctx->videoHeight,
         (long long)ctx->swapchainFormat, ctx->swapchainImageCount, ctx->stereoMode);

    XrSwapchainCreateInfo overlayInfo = swapInfo;
    overlayInfo.width = OVERLAY_WIDTH;
    overlayInfo.height = OVERLAY_HEIGHT;
    if (checkXr(xrCreateSwapchain(ctx->session, &overlayInfo, &ctx->overlaySwapchain),
                "create overlay swapchain")) {
        xrEnumerateSwapchainImages(ctx->overlaySwapchain, 0, &ctx->overlayImageCount, NULL);
        ctx->overlayImages = calloc(ctx->overlayImageCount, sizeof(XrSwapchainImageOpenGLESKHR));
        for (uint32_t i = 0; i < ctx->overlayImageCount; i++) {
            ctx->overlayImages[i].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR;
        }
        xrEnumerateSwapchainImages(ctx->overlaySwapchain, ctx->overlayImageCount,
                                   &ctx->overlayImageCount,
                                   (XrSwapchainImageBaseHeader*)ctx->overlayImages);
    }
    else {
        // The stream is worth more than the stats, so carry on without it
        ctx->overlaySwapchain = XR_NULL_HANDLE;
    }

    return 1;
}

// Builds the hardcoded depth map for the stereo test path. Depth convention:
// 0 far, 1 near, 0.5 sits exactly on the screen plane (zero disparity)
static void fillSyntheticDepth(XrCtx* ctx) {
    const int n = DEPTH_TEX_SIZE;
    // RGBA throughout: depth in alpha, guide colour in rgb. The synthetic
    // patterns have no guide, so it stays neutral and the upsample falls back
    // to a plain blur on them.
    unsigned char* buf = malloc((size_t)n * n * 4);

    for (int y = 0; y < n; y++) {
        for (int x = 0; x < n; x++) {
            float fx = x / (float)(n - 1);
            float fy = y / (float)(n - 1);
            float d;
            switch (ctx->stereoMode) {
                case DEPTH_MODE_RAMP:
                    d = fx;
                    break;
                case DEPTH_MODE_BLOB: {
                    float dx = fx - 0.5f;
                    float dy = fy - 0.5f;
                    float sigma = 0.15f;
                    d = 0.35f + 0.5f * expf(-(dx * dx + dy * dy) / (2.0f * sigma * sigma));
                    break;
                }
                case DEPTH_MODE_SHIFTTEST:
                    // Constant near depth so the whole bar shifts uniformly
                    d = 0.85f;
                    break;
                case DEPTH_MODE_FLAT:
                default:
                    d = 0.5f;
                    break;
            }
            if (d < 0.0f) d = 0.0f;
            if (d > 1.0f) d = 1.0f;
            unsigned char* px = buf + ((size_t)y * n + x) * 4;
            px[0] = px[1] = px[2] = 128;
            px[3] = (unsigned char)(d * 255.0f + 0.5f);
        }
    }

    for (int i = 0; i < 2; i++) {
        glBindTexture(GL_TEXTURE_2D, ctx->depthTextures[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, n, n, 0, GL_RGBA, GL_UNSIGNED_BYTE, buf);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    free(buf);
}

static int linkProgram(GLuint* out, const char* fragmentSrc, const char* what) {
    GLuint vs = compileShader(GL_VERTEX_SHADER, VERTEX_SRC);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, fragmentSrc);
    if (vs == 0 || fs == 0) {
        return 0;
    }
    GLuint program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glBindAttribLocation(program, 0, "a_position");
    glBindAttribLocation(program, 1, "a_texcoord");
    glLinkProgram(program);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint linked = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[512];
        glGetProgramInfoLog(program, sizeof(log), NULL, log);
        LOGE("%s program link failed: %s", what, log);
        return 0;
    }
    *out = program;
    return 1;
}

// Quarter resolution is enough: at 1920x1080 the measured edge width was the
// same 5 px, so the extra four times the pixels bought nothing.
static int initUpsample(XrCtx* ctx) {
    ctx->upsampleWidth = ctx->videoWidth / 4;
    ctx->upsampleHeight = ctx->videoHeight / 4;

    if (!linkProgram(&ctx->upsampleProgram, UPSAMPLE_FRAGMENT_SRC, "upsample")) {
        return 0;
    }
    ctx->upsampleTexMatrixUniform = glGetUniformLocation(ctx->upsampleProgram, "u_texmatrix");
    ctx->upsampleSigmaUniform = glGetUniformLocation(ctx->upsampleProgram, "u_sigmaR");
    ctx->upsampleSharpUniform = glGetUniformLocation(ctx->upsampleProgram, "u_sharp");
    glUseProgram(ctx->upsampleProgram);
    glUniform1i(glGetUniformLocation(ctx->upsampleProgram, "u_texture"), 0);
    glUniform1i(glGetUniformLocation(ctx->upsampleProgram, "u_depth"), 1);

    glGenTextures(1, &ctx->upsampleTexture);
    glBindTexture(GL_TEXTURE_2D, ctx->upsampleTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, ctx->upsampleWidth, ctx->upsampleHeight, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenFramebuffers(1, &ctx->upsampleFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, ctx->upsampleFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           ctx->upsampleTexture, 0);
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        LOGE("upsample framebuffer incomplete: 0x%x", status);
        return 0;
    }

    if (!linkProgram(&ctx->offsetProgram, OFFSET_FRAGMENT_SRC, "offset")) {
        return 0;
    }
    ctx->offsetDispUniform = glGetUniformLocation(ctx->offsetProgram, "u_dispTexels");
    ctx->offsetConvUniform = glGetUniformLocation(ctx->offsetProgram, "u_convergence");
    glUseProgram(ctx->offsetProgram);
    glUniform1i(glGetUniformLocation(ctx->offsetProgram, "u_depth"), 1);

    glGenTextures(1, &ctx->offsetTexture);
    glBindTexture(GL_TEXTURE_2D, ctx->offsetTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, ctx->upsampleWidth, ctx->upsampleHeight, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenFramebuffers(1, &ctx->offsetFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, ctx->offsetFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           ctx->offsetTexture, 0);
    status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        LOGE("offset framebuffer incomplete: 0x%x", status);
        return 0;
    }

    LOGI("depth upsample and offset search ready at %dx%d",
         ctx->upsampleWidth, ctx->upsampleHeight);
    return 1;
}

// GL side of the depth model path: the downscale target the frame is
// rendered into, and the staging buffers it is read back through
static int initDepthModel(XrCtx* ctx) {
    const int n = DEPTH_TEX_SIZE;

    if (!linkProgram(&ctx->downscaleProgram, DOWNSCALE_FRAGMENT_SRC, "downscale")) {
        return 0;
    }
    ctx->downscaleTexMatrixUniform = glGetUniformLocation(ctx->downscaleProgram, "u_texmatrix");
    glUseProgram(ctx->downscaleProgram);
    glUniform1i(glGetUniformLocation(ctx->downscaleProgram, "u_texture"), 0);

    glGenTextures(1, &ctx->downscaleTexture);
    glBindTexture(GL_TEXTURE_2D, ctx->downscaleTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, n, n, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    glGenFramebuffers(1, &ctx->downscaleFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, ctx->downscaleFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           ctx->downscaleTexture, 0);
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        LOGE("downscale framebuffer incomplete: 0x%x", status);
        return 0;
    }

    // The depth thread gets its own context in the same share group, so it
    // can upload into the back depth texture while the frame loop draws
    const EGLint contextAttribs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    ctx->depthContext = eglCreateContext(ctx->eglDisplay, ctx->eglConfig, ctx->eglContext,
                                         contextAttribs);
    if (ctx->depthContext == EGL_NO_CONTEXT) {
        LOGE("depth thread context creation failed: %d", eglGetError());
        return 0;
    }
    const EGLint pbufferAttribs[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
    ctx->depthPbuffer = eglCreatePbufferSurface(ctx->eglDisplay, ctx->eglConfig, pbufferAttribs);
    if (ctx->depthPbuffer == EGL_NO_SURFACE) {
        LOGE("depth thread pbuffer creation failed: %d", eglGetError());
        return 0;
    }

    ctx->readbackBuf = malloc((size_t)n * n * 4);
    ctx->modelInput = malloc((size_t)n * n * 3 * sizeof(float));
    ctx->modelOutput = malloc((size_t)n * n * sizeof(float));
    ctx->depthUploadBuf = malloc((size_t)n * n * 4);
    ctx->depthEma = malloc((size_t)n * n * sizeof(float));
    ctx->depthLow = malloc((size_t)n * n * sizeof(float));
    ctx->depthScratch = malloc((size_t)n * n * sizeof(float));
    ctx->depthColSums = malloc((size_t)n * sizeof(float));
    if (ctx->readbackBuf == NULL || ctx->modelInput == NULL || ctx->modelOutput == NULL ||
            ctx->depthUploadBuf == NULL || ctx->depthEma == NULL ||
            ctx->depthLow == NULL || ctx->depthScratch == NULL ||
            ctx->depthColSums == NULL) {
        LOGE("depth staging buffer allocation failed");
        return 0;
    }

    LOGI("depth model staging ready at %dx%d", n, n);
    return 1;
}

// Reuses the same box-filter downscale shader as the depth model
// (DOWNSCALE_FRAGMENT_SRC already spans the whole frame via the vertex
// stage's interpolated coordinate, so a smaller target here is still a
// valid whole-frame sample, just coarser) - but its own program/target,
// since downscaleProgram only exists in DEPTH_MODE_MODEL sessions and
// glow needs to work regardless of the 3D-effect toggle.
static int initGlow(XrCtx* ctx) {
    if (!linkProgram(&ctx->glowProgram, DOWNSCALE_FRAGMENT_SRC, "glow")) {
        return 0;
    }
    ctx->glowTexMatrixUniform = glGetUniformLocation(ctx->glowProgram, "u_texmatrix");
    glUseProgram(ctx->glowProgram);
    glUniform1i(glGetUniformLocation(ctx->glowProgram, "u_texture"), 0);

    glGenTextures(1, &ctx->glowTexture);
    glBindTexture(GL_TEXTURE_2D, ctx->glowTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, GLOW_TEX_SIZE, GLOW_TEX_SIZE, 0, GL_RGBA,
                GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    glGenFramebuffers(1, &ctx->glowFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, ctx->glowFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           ctx->glowTexture, 0);
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        LOGE("glow framebuffer incomplete: 0x%x", status);
        return 0;
    }

    // Full passthrough (the default) never shows the dim sphere at all, so
    // starting white is harmless - it only ever multiplies visible alpha
    // once the room actually darkens, by which point a real frame has run.
    for (int c = 0; c < GLOW_SAMPLE_COUNT; c++) {
        ctx->glowSampleR[c] = ctx->glowSampleG[c] = ctx->glowSampleB[c] = 1.0f;
    }
    return 1;
}

static int initGl(XrCtx* ctx) {
    GLuint vs = compileShader(GL_VERTEX_SHADER, VERTEX_SRC);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, FRAGMENT_SRC);
    if (vs == 0 || fs == 0) {
        return 0;
    }

    ctx->program = glCreateProgram();
    glAttachShader(ctx->program, vs);
    glAttachShader(ctx->program, fs);
    glBindAttribLocation(ctx->program, 0, "a_position");
    glBindAttribLocation(ctx->program, 1, "a_texcoord");
    glLinkProgram(ctx->program);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint linked = 0;
    glGetProgramiv(ctx->program, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[512];
        glGetProgramInfoLog(ctx->program, sizeof(log), NULL, log);
        LOGE("program link failed: %s", log);
        return 0;
    }
    ctx->texMatrixUniform = glGetUniformLocation(ctx->program, "u_texmatrix");
    ctx->disparityUniform = glGetUniformLocation(ctx->program, "u_disparity");
    ctx->tintUniform = glGetUniformLocation(ctx->program, "u_tint");
    ctx->barTestUniform = glGetUniformLocation(ctx->program, "u_barTest");
    ctx->occlusionUniform = glGetUniformLocation(ctx->program, "u_occlusion");
    ctx->eyeIndexUniform = glGetUniformLocation(ctx->program, "u_eyeIndex");
    ctx->convergenceUniform = glGetUniformLocation(ctx->program, "u_convergence");
    ctx->dispTexelsUniform = glGetUniformLocation(ctx->program, "u_dispTexels");
    ctx->lowResWidthUniform = glGetUniformLocation(ctx->program, "u_lowResWidth");
    ctx->frameWidthUniform = glGetUniformLocation(ctx->program, "u_frameWidth");
    ctx->roundFrameSizeUniform = glGetUniformLocation(ctx->program, "u_roundFrameSize");
    ctx->cornerRadiusUniform = glGetUniformLocation(ctx->program, "u_cornerRadiusPx");
    ctx->edgeFeatherUniform = glGetUniformLocation(ctx->program, "u_edgeFeatherPx");

    // Sampler units are fixed: color on 0, depth on 1
    glUseProgram(ctx->program);
    glUniform1i(glGetUniformLocation(ctx->program, "u_texture"), 0);
    glUniform1i(glGetUniformLocation(ctx->program, "u_depth"), 1);
    glUniform1i(glGetUniformLocation(ctx->program, "u_offsets"), 2);
    glUniform1f(glGetUniformLocation(ctx->program, "u_showDepth"),
                ctx->depthDebug ? 1.0f : 0.0f);

    glGenTextures(2, ctx->depthTextures);
    fillSyntheticDepth(ctx);

    glGenTextures(1, &ctx->oesTexture);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, ctx->oesTexture);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // One independent OES texture per PMode screen. Harmless to always
    // create these even outside productivity mode - a handful of unused
    // texture names cost nothing until something binds a SurfaceTexture to
    // them, which only happens when XrRenderer actually enters PMode.
    glGenTextures(PRODUCTIVITY_SCREEN_COUNT, ctx->productivityOesTexture);
    for (int i = 0; i < PRODUCTIVITY_SCREEN_COUNT; i++) {
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, ctx->productivityOesTexture[i]);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }

    glGenFramebuffers(1, &ctx->fbo);

    const char* glExts = (const char*)glGetString(GL_EXTENSIONS);
    ctx->srgbWriteControl = glExts != NULL && strstr(glExts, "GL_EXT_sRGB_write_control") != NULL;

    if (glExts != NULL && strstr(glExts, "GL_EXT_disjoint_timer_query") != NULL) {
        pfnGenQueries = (PFNGENQUERIESEXT)eglGetProcAddress("glGenQueriesEXT");
        pfnBeginQuery = (PFNBEGINQUERYEXT)eglGetProcAddress("glBeginQueryEXT");
        pfnEndQuery = (PFNENDQUERYEXT)eglGetProcAddress("glEndQueryEXT");
        pfnGetQueryObjectuiv = (PFNGETQUERYOBJECTUIVEXT)eglGetProcAddress("glGetQueryObjectuivEXT");
        pfnGetQueryObjectui64v =
                (PFNGETQUERYOBJECTUI64VEXT)eglGetProcAddress("glGetQueryObjectui64vEXT");
        if (pfnGenQueries != NULL && pfnBeginQuery != NULL && pfnEndQuery != NULL &&
                pfnGetQueryObjectuiv != NULL && pfnGetQueryObjectui64v != NULL) {
            pfnGenQueries(2, ctx->timerQueries);
            ctx->timerSupported = 1;
        }
    }
    if (!ctx->timerSupported) {
        LOGW("GL_EXT_disjoint_timer_query missing, GPU times unavailable");
    }

    // The video frames are already gamma encoded. With an sRGB swapchain the
    // GPU would encode again on write, so turn that off. Without the
    // extension colors will look washed out and we would need a shader fix.
    if (ctx->swapchainFormat == GL_SRGB8_ALPHA8 && !ctx->srgbWriteControl) {
        LOGW("GL_EXT_sRGB_write_control not available, expect wrong gamma");
    }

    if (ctx->stereoMode == DEPTH_MODE_MODEL) {
        if (!initDepthModel(ctx) || !initUpsample(ctx)) {
            return 0;
        }
    }

    if (!initGlow(ctx)) {
        return 0;
    }

    return 1;
}

static void handleSessionStateChange(XrCtx* ctx, XrSessionState newState) {
    LOGI("session state %d -> %d", ctx->sessionState, newState);
    ctx->sessionState = newState;

    switch (newState) {
        case XR_SESSION_STATE_READY: {
            XrSessionBeginInfo beginInfo = { XR_TYPE_SESSION_BEGIN_INFO };
            beginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            if (checkXr(xrBeginSession(ctx->session, &beginInfo), "xrBeginSession")) {
                ctx->sessionRunning = 1;
                // Start fully black and ease in, rather than snapping straight
                // to the room/screen the instant the compositor takes over.
                ctx->fadeState = FADE_IN;
                ctx->fadeFromAlpha = 1.0f;
                ctx->fadeAlpha = 1.0f;
                ctx->fadeStartNs = nowNs();
                // Immersive VR sessions already get scheduling priority from
                // the com.oculus.intent.category.VR intent filter, but this
                // asks explicitly rather than hoping the runtime's default
                // pick is generous - sustained (not boost, which the spec
                // frames as a short burst allowance) since a stream runs for
                // the whole session, not a one-off load.
                if (ctx->perfSettingsSupported && ctx->pfnSetPerfLevel != NULL) {
                    ctx->pfnSetPerfLevel(ctx->session, XR_PERF_SETTINGS_DOMAIN_CPU_EXT,
                                         XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT);
                    ctx->pfnSetPerfLevel(ctx->session, XR_PERF_SETTINGS_DOMAIN_GPU_EXT,
                                         XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT);
                }
            }
            break;
        }
        case XR_SESSION_STATE_STOPPING:
            xrEndSession(ctx->session);
            ctx->sessionRunning = 0;
            break;
        case XR_SESSION_STATE_EXITING:
        case XR_SESSION_STATE_LOSS_PENDING:
            ctx->sessionRunning = 0;
            ctx->exitRequested = 1;
            break;
        default:
            break;
    }
}

// A pinch is how these headsets click, but it is not always offered as an
// input to bind to. The joints always are, so it is measured here instead:
// thumb tip to index tip, with a gap between the closing and opening distances
// so a hand held near the threshold does not chatter.
#define PINCH_ON_M  0.020f
#define PINCH_OFF_M 0.032f
// Same hysteresis idea, but for XR_FB_hand_tracking_aim's continuous
// 0..1 pinchStrengthIndex instead of a raw joint gap - see jointPinching().
#define PINCH_STRENGTH_ON  0.80f
#define PINCH_STRENGTH_OFF 0.55f

static void initJointTracking(XrCtx* ctx) {
    if (!ctx->handTracking) {
        return;
    }
    if (XR_FAILED(xrGetInstanceProcAddr(ctx->instance, "xrCreateHandTrackerEXT",
                                        (PFN_xrVoidFunction*)&ctx->pfnCreateHandTracker))
            || XR_FAILED(xrGetInstanceProcAddr(ctx->instance, "xrDestroyHandTrackerEXT",
                                               (PFN_xrVoidFunction*)&ctx->pfnDestroyHandTracker))
            || XR_FAILED(xrGetInstanceProcAddr(ctx->instance, "xrLocateHandJointsEXT",
                                               (PFN_xrVoidFunction*)&ctx->pfnLocateHandJoints))
            || ctx->pfnCreateHandTracker == NULL || ctx->pfnLocateHandJoints == NULL) {
        LOGW("hand joint entry points missing");
        ctx->jointTracking = 0;
        return;
    }

    for (int h = 0; h < HAND_COUNT; h++) {
        XrHandTrackerCreateInfoEXT info = { XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT };
        info.hand = h == HAND_LEFT ? XR_HAND_LEFT_EXT : XR_HAND_RIGHT_EXT;
        info.handJointSet = XR_HAND_JOINT_SET_DEFAULT_EXT;
        if (!checkXr(ctx->pfnCreateHandTracker(ctx->session, &info, &ctx->handTrackers[h]),
                     "create hand tracker")) {
            ctx->handTrackers[h] = XR_NULL_HANDLE;
            return;
        }
    }
    ctx->jointTracking = 1;
    LOGI("reading hand joints for pinch");
}

// Which kind of thing is driving each hand. Hands are never still enough for
// the movement gate to mean anything, so they wake the pointer a different way
// and need to be told apart from controllers.
static void refreshInputSource(XrCtx* ctx) {
    if (ctx->session == XR_NULL_HANDLE || !ctx->inputReady) {
        return;
    }
    for (int h = 0; h < HAND_COUNT; h++) {
        XrInteractionProfileState state = { XR_TYPE_INTERACTION_PROFILE_STATE };
        if (XR_FAILED(xrGetCurrentInteractionProfile(ctx->session, ctx->handPaths[h], &state))) {
            continue;
        }
        // Without a pinch bound there is nothing to wake the pointer with, so
        // those hands stay on the movement gate rather than becoming unusable
        int hands = ctx->handClickOk && state.interactionProfile != XR_NULL_PATH
                && (state.interactionProfile == ctx->handProfile
                    || state.interactionProfile == ctx->msftHandProfile);
        if (hands != ctx->usingHands[h]) {
            LOGI("hand %d is now driven by %s", h, hands ? "hand tracking" : "a controller");
        }
        ctx->usingHands[h] = hands;
    }
}

static void pollEvents(XrCtx* ctx) {
    XrEventDataBuffer event;
    for (;;) {
        event.type = XR_TYPE_EVENT_DATA_BUFFER;
        event.next = NULL;
        XrResult res = xrPollEvent(ctx->instance, &event);
        if (res != XR_SUCCESS) {
            break;
        }
        switch (event.type) {
            case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
                XrEventDataSessionStateChanged* sc = (XrEventDataSessionStateChanged*)&event;
                handleSessionStateChange(ctx, sc->state);
                break;
            }
            case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING: {
                XrEventDataReferenceSpaceChangePending* change =
                        (XrEventDataReferenceSpaceChangePending*)&event;
                if (change->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL) {
                    // Recentring is the user saying where forward is, so the
                    // screen goes back to the placement a fresh install has
                    // rather than keeping an offset from the old origin
                    ctx->placementValid = 0;
                    ctx->grabMode = GRAB_NONE;
                    LOGI("recentred, screen placement reset");
                }
                break;
            }
            case XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED:
                // Picking a controller up or putting it down swaps the profile
                // on that hand, and the pointer wakes differently for each
                refreshInputSource(ctx);
                break;
            case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
                ctx->exitRequested = 1;
                break;
            default:
                break;
        }
    }
}

static Vec3 vecSub(Vec3 a, Vec3 b) {
    Vec3 r = { a.x - b.x, a.y - b.y, a.z - b.z };
    return r;
}

static XrQuaternionf quatConj(XrQuaternionf q) {
    XrQuaternionf r = { -q.x, -q.y, -q.z, q.w };
    return r;
}

static XrQuaternionf quatMul(XrQuaternionf a, XrQuaternionf b) {
    XrQuaternionf r;
    r.w = a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z;
    r.x = a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y;
    r.y = a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x;
    r.z = a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w;
    return r;
}

// Repeated products drift off the unit sphere and the compositor is entitled
// to reject that
static XrQuaternionf quatNorm(XrQuaternionf q) {
    float len = sqrtf(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (len < 1e-6f) {
        XrQuaternionf id = { 0.0f, 0.0f, 0.0f, 1.0f };
        return id;
    }
    q.x /= len;
    q.y /= len;
    q.z /= len;
    q.w /= len;
    return q;
}

static Vec3 quatRotate(XrQuaternionf q, Vec3 v) {
    // v + w * (2 * cross(q.xyz, v)) + cross(q.xyz, 2 * cross(q.xyz, v))
    Vec3 u = { q.x, q.y, q.z };
    Vec3 t = { 2.0f * (u.y * v.z - u.z * v.y),
               2.0f * (u.z * v.x - u.x * v.z),
               2.0f * (u.x * v.y - u.y * v.x) };
    Vec3 r = { v.x + q.w * t.x + (u.y * t.z - u.z * t.y),
               v.y + q.w * t.y + (u.z * t.x - u.x * t.z),
               v.z + q.w * t.z + (u.x * t.y - u.y * t.x) };
    return r;
}

static float euroAlpha(float cutoff, float dt) {
    float tau = 1.0f / (2.0f * (float)M_PI * cutoff);
    return 1.0f / (1.0f + tau / dt);
}

static float euroFilter(EuroState* s, float x, float dt, float minCutoff, float beta) {
    if (!s->valid || dt <= 0.0f) {
        s->valid = 1;
        s->x = x;
        s->dx = 0.0f;
        return x;
    }
    float dx = (x - s->x) / dt;
    s->dx += euroAlpha(POINTER_D_CUTOFF, dt) * (dx - s->dx);
    float cutoff = minCutoff + beta * fabsf(s->dx);
    s->x += euroAlpha(cutoff, dt) * (x - s->x);
    return s->x;
}

// Rotation whose local axes are the three given unit vectors. Used to stand a
// quad layer up along the beam while keeping its face toward the viewer.
static XrQuaternionf quatFromBasis(Vec3 x, Vec3 y, Vec3 z) {
    float m[3][3] = {
        { x.x, y.x, z.x },
        { x.y, y.y, z.y },
        { x.z, y.z, z.z },
    };
    float trace = m[0][0] + m[1][1] + m[2][2];
    XrQuaternionf q;
    if (trace > 0.0f) {
        float s = sqrtf(trace + 1.0f) * 2.0f;
        q.w = 0.25f * s;
        q.x = (m[2][1] - m[1][2]) / s;
        q.y = (m[0][2] - m[2][0]) / s;
        q.z = (m[1][0] - m[0][1]) / s;
    }
    else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
        float s = sqrtf(1.0f + m[0][0] - m[1][1] - m[2][2]) * 2.0f;
        q.w = (m[2][1] - m[1][2]) / s;
        q.x = 0.25f * s;
        q.y = (m[0][1] + m[1][0]) / s;
        q.z = (m[0][2] + m[2][0]) / s;
    }
    else if (m[1][1] > m[2][2]) {
        float s = sqrtf(1.0f + m[1][1] - m[0][0] - m[2][2]) * 2.0f;
        q.w = (m[0][2] - m[2][0]) / s;
        q.x = (m[0][1] + m[1][0]) / s;
        q.y = 0.25f * s;
        q.z = (m[1][2] + m[2][1]) / s;
    }
    else {
        float s = sqrtf(1.0f + m[2][2] - m[0][0] - m[1][1]) * 2.0f;
        q.w = (m[1][0] - m[0][1]) / s;
        q.x = (m[0][2] + m[2][0]) / s;
        q.y = (m[1][2] + m[2][1]) / s;
        q.z = 0.25f * s;
    }
    return q;
}

static Vec3 vecNorm(Vec3 v) {
    float len = sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
    if (len < 1e-6f) {
        Vec3 zero = { 0.0f, 0.0f, 0.0f };
        return zero;
    }
    Vec3 r = { v.x / len, v.y / len, v.z / len };
    return r;
}

static Vec3 vecCross(Vec3 a, Vec3 b) {
    Vec3 r = { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
    return r;
}

static XrPath toPath(XrCtx* ctx, const char* str) {
    XrPath path = XR_NULL_PATH;
    xrStringToPath(ctx->instance, str, &path);
    return path;
}

static XrAction makeAction(XrCtx* ctx, XrActionType type, const char* name, const char* label) {
    XrActionCreateInfo info = { XR_TYPE_ACTION_CREATE_INFO };
    info.actionType = type;
    strncpy(info.actionName, name, XR_MAX_ACTION_NAME_SIZE - 1);
    strncpy(info.localizedActionName, label, XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
    info.countSubactionPaths = HAND_COUNT;
    info.subactionPaths = ctx->handPaths;

    XrAction action = XR_NULL_HANDLE;
    if (!checkXr(xrCreateAction(ctx->actionSet, &info, &action), name)) {
        return XR_NULL_HANDLE;
    }
    return action;
}

// One unsupported path rejects a whole profile, so the full set is offered
// first and a runtime that does not recognise this controller falls back to
// aim and trigger, which every profile has.
static void suggestBindings(XrCtx* ctx, const char* profile, int full) {
    XrActionSuggestedBinding b[16];
    uint32_t n = 0;
    static const char* hands[HAND_COUNT] = { "/user/hand/left", "/user/hand/right" };
    // x and y on the left controller, a and b on the right
    static const char* rightClick[HAND_COUNT] = { "input/x/click", "input/a/click" };
    static const char* middleClick[HAND_COUNT] = { "input/y/click", "input/b/click" };
    int simple = strstr(profile, "/khr/") != NULL;

    for (int h = 0; h < HAND_COUNT; h++) {
        char path[XR_MAX_PATH_LENGTH];

        snprintf(path, sizeof(path), "%s/input/aim/pose", hands[h]);
        b[n].action = ctx->aimAction;
        b[n++].binding = toPath(ctx, path);

        snprintf(path, sizeof(path), "%s/%s", hands[h],
                 simple ? "input/select/click" : "input/trigger/value");
        b[n].action = ctx->triggerAction;
        b[n++].binding = toPath(ctx, path);

        // Every profile with a trigger also has a haptic motor, so this isn't
        // gated behind full/simple like the rest below
        snprintf(path, sizeof(path), "%s/output/haptic", hands[h]);
        b[n].action = ctx->hapticAction;
        b[n++].binding = toPath(ctx, path);

        if (!full || simple) {
            continue;
        }

        snprintf(path, sizeof(path), "%s/%s", hands[h], rightClick[h]);
        b[n].action = ctx->rightClickAction;
        b[n++].binding = toPath(ctx, path);

        snprintf(path, sizeof(path), "%s/%s", hands[h], middleClick[h]);
        b[n].action = ctx->middleClickAction;
        b[n++].binding = toPath(ctx, path);

        snprintf(path, sizeof(path), "%s/input/thumbstick", hands[h]);
        b[n].action = ctx->scrollAction;
        b[n++].binding = toPath(ctx, path);

        snprintf(path, sizeof(path), "%s/input/thumbstick/click", hands[h]);
        b[n].action = ctx->toggleAction;
        b[n++].binding = toPath(ctx, path);

        snprintf(path, sizeof(path), "%s/input/squeeze/value", hands[h]);
        b[n].action = ctx->grabAction;
        b[n++].binding = toPath(ctx, path);
    }

    XrInteractionProfileSuggestedBinding suggest = { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
    suggest.interactionProfile = toPath(ctx, profile);
    suggest.countSuggestedBindings = n;
    suggest.suggestedBindings = b;

    XrResult res = xrSuggestInteractionProfileBindings(ctx->instance, &suggest);
    if (XR_SUCCEEDED(res)) {
        LOGI("bindings accepted for %s (%s)", profile, full ? "full" : "reduced");
    }
    else if (full) {
        LOGW("full bindings rejected for %s (%d), trying aim and trigger only", profile, res);
        suggestBindings(ctx, profile, 0);
    }
    else {
        LOGW("bindings rejected for %s (%d)", profile, res);
    }
}

// Hands come in through the same actions the controllers use, so everything
// downstream of here treats them identically: same ray, same handles, same
// picker. Only the paths differ, which is why this is its own function rather
// than another flag on the one above.
static XrResult trySuggestHands(XrCtx* ctx, const char* profile, const char* aim,
                                const char* click, const char* grasp) {
    XrActionSuggestedBinding b[6];
    uint32_t n = 0;
    static const char* hands[HAND_COUNT] = { "/user/hand/left", "/user/hand/right" };

    for (int h = 0; h < HAND_COUNT; h++) {
        char path[XR_MAX_PATH_LENGTH];

        snprintf(path, sizeof(path), "%s/%s", hands[h], aim);
        b[n].action = ctx->aimAction;
        b[n++].binding = toPath(ctx, path);

        if (click != NULL) {
            snprintf(path, sizeof(path), "%s/%s", hands[h], click);
            b[n].action = ctx->triggerAction;
            b[n++].binding = toPath(ctx, path);
        }

        if (grasp != NULL) {
            snprintf(path, sizeof(path), "%s/%s", hands[h], grasp);
            b[n].action = ctx->grabAction;
            b[n++].binding = toPath(ctx, path);
        }
    }

    XrInteractionProfileSuggestedBinding suggest = { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
    suggest.interactionProfile = toPath(ctx, profile);
    suggest.countSuggestedBindings = n;
    suggest.suggestedBindings = b;

    return xrSuggestInteractionProfileBindings(ctx->instance, &suggest);
}

// Runtimes that offer the hand profile do not all implement every input in it,
// and one unsupported path throws out the whole suggestion. So the inputs are
// offered up in falling order of usefulness until a set is accepted. Returns
// whether a pinch ended up bound, since without one the hands cannot wake the
// pointer and are better left to the movement gate.
static int suggestHandBindings(XrCtx* ctx, const char* profile, const char* aim,
                               const char* const* clicks, int clickCount,
                               const char* grasp) {
    XrResult res = XR_SUCCESS;
    for (int c = 0; c < clickCount; c++) {
        if (grasp != NULL) {
            res = trySuggestHands(ctx, profile, aim, clicks[c], grasp);
            if (XR_SUCCEEDED(res)) {
                LOGI("hand bindings accepted for %s (%s and grasp)", profile, clicks[c]);
                return 1;
            }
        }
        res = trySuggestHands(ctx, profile, aim, clicks[c], NULL);
        if (XR_SUCCEEDED(res)) {
            LOGI("hand bindings accepted for %s (%s)", profile, clicks[c]);
            return 1;
        }
    }
    res = trySuggestHands(ctx, profile, aim, NULL, NULL);
    if (XR_SUCCEEDED(res)) {
        LOGW("only the aim pose bound for %s, so hands cannot click", profile);
        return 0;
    }
    LOGW("hand bindings rejected for %s, even the aim pose alone (%d)", profile, res);
    return 0;
}

static int initXrInput(XrCtx* ctx) {
    XrActionSetCreateInfo setInfo = { XR_TYPE_ACTION_SET_CREATE_INFO };
    strncpy(setInfo.actionSetName, "moonlight", XR_MAX_ACTION_SET_NAME_SIZE - 1);
    strncpy(setInfo.localizedActionSetName, "Moonlight", XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE - 1);
    if (!checkXr(xrCreateActionSet(ctx->instance, &setInfo, &ctx->actionSet), "create action set")) {
        return 0;
    }

    ctx->handPaths[HAND_LEFT] = toPath(ctx, "/user/hand/left");
    ctx->handPaths[HAND_RIGHT] = toPath(ctx, "/user/hand/right");

    ctx->aimAction = makeAction(ctx, XR_ACTION_TYPE_POSE_INPUT, "aim", "Pointer");
    ctx->triggerAction = makeAction(ctx, XR_ACTION_TYPE_FLOAT_INPUT, "trigger", "Left click");
    ctx->rightClickAction = makeAction(ctx, XR_ACTION_TYPE_BOOLEAN_INPUT, "rightclick", "Right click");
    ctx->middleClickAction = makeAction(ctx, XR_ACTION_TYPE_BOOLEAN_INPUT, "middleclick", "Middle click");
    ctx->scrollAction = makeAction(ctx, XR_ACTION_TYPE_VECTOR2F_INPUT, "scroll", "Scroll");
    ctx->grabAction = makeAction(ctx, XR_ACTION_TYPE_FLOAT_INPUT, "grab", "Move the screen");
    ctx->toggleAction = makeAction(ctx, XR_ACTION_TYPE_BOOLEAN_INPUT, "pointertoggle", "Pointer on or off");
    ctx->hapticAction = makeAction(ctx, XR_ACTION_TYPE_VIBRATION_OUTPUT, "haptic", "Haptic feedback");

    if (ctx->aimAction == XR_NULL_HANDLE || ctx->triggerAction == XR_NULL_HANDLE) {
        return 0;
    }

    suggestBindings(ctx, "/interaction_profiles/khr/simple_controller", 1);
    suggestBindings(ctx, "/interaction_profiles/oculus/touch_controller", 1);
    if (ctx->picoInteraction) {
        suggestBindings(ctx, "/interaction_profiles/bytedance/pico4_controller", 1);
    }

    // Hands. aim_activate is the spec's own name for pointing at something out
    // of reach and pinching to act on it, which is exactly what the ray does.
    // Gaze is its own top level path rather than a hand, so it needs an action
    // of its own. There is no click on it: whatever the runtime reports as a
    // trigger, usually a pinch, does the clicking.
    if (ctx->eyeGaze) {
        XrActionCreateInfo info = { XR_TYPE_ACTION_CREATE_INFO };
        info.actionType = XR_ACTION_TYPE_POSE_INPUT;
        strncpy(info.actionName, "gaze", XR_MAX_ACTION_NAME_SIZE - 1);
        strncpy(info.localizedActionName, "Gaze pointer",
                XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
        if (checkXr(xrCreateAction(ctx->actionSet, &info, &ctx->gazeAction), "gaze action")) {
            XrActionSuggestedBinding b;
            b.action = ctx->gazeAction;
            b.binding = toPath(ctx, "/user/eyes_ext/input/gaze_ext/pose");

            XrInteractionProfileSuggestedBinding suggest = {
                XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING
            };
            suggest.interactionProfile = toPath(ctx,
                    "/interaction_profiles/ext/eye_gaze_interaction");
            suggest.countSuggestedBindings = 1;
            suggest.suggestedBindings = &b;
            if (XR_FAILED(xrSuggestInteractionProfileBindings(ctx->instance, &suggest))) {
                LOGW("gaze bindings rejected");
                ctx->gazeAction = XR_NULL_HANDLE;
                ctx->eyeGaze = 0;
            }
        }
        else {
            ctx->gazeAction = XR_NULL_HANDLE;
            ctx->eyeGaze = 0;
        }
    }

    if (ctx->handInteraction) {
        // aim_activate is the spec's own name for the far pointer pinch, and
        // pinch is the plain one. Runtimes vary in which they implement.
        static const char* const clicks[] = {
            "input/aim_activate_ext/value", "input/pinch_ext/value"
        };
        const char* profile = "/interaction_profiles/ext/hand_interaction_ext";
        ctx->handClickOk |= suggestHandBindings(ctx, profile, "input/aim_ext/pose",
                                                clicks, 2, "input/grasp_ext/value");
        ctx->handProfile = toPath(ctx, profile);
    }
    // Older runtimes that predate the EXT profile. Same idea, fewer inputs.
    if (ctx->msftHandInteraction) {
        static const char* const clicks[] = { "input/select/value" };
        const char* profile = "/interaction_profiles/microsoft/hand_interaction";
        ctx->handClickOk |= suggestHandBindings(ctx, profile, "input/aim/pose",
                                                clicks, 1, "input/squeeze/value");
        ctx->msftHandProfile = toPath(ctx, profile);
    }

    XrSessionActionSetsAttachInfo attach = { XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
    attach.countActionSets = 1;
    attach.actionSets = &ctx->actionSet;
    if (!checkXr(xrAttachSessionActionSets(ctx->session, &attach), "attach action sets")) {
        return 0;
    }

    for (int h = 0; h < HAND_COUNT; h++) {
        XrActionSpaceCreateInfo spaceInfo = { XR_TYPE_ACTION_SPACE_CREATE_INFO };
        spaceInfo.action = ctx->aimAction;
        spaceInfo.subactionPath = ctx->handPaths[h];
        spaceInfo.poseInActionSpace.orientation.w = 1.0f;
        if (!checkXr(xrCreateActionSpace(ctx->session, &spaceInfo, &ctx->aimSpaces[h]),
                     "create aim space")) {
            return 0;
        }
    }

    if (ctx->gazeAction != XR_NULL_HANDLE) {
        XrActionSpaceCreateInfo spaceInfo = { XR_TYPE_ACTION_SPACE_CREATE_INFO };
        spaceInfo.action = ctx->gazeAction;
        spaceInfo.subactionPath = XR_NULL_PATH;
        spaceInfo.poseInActionSpace.orientation.w = 1.0f;
        if (!checkXr(xrCreateActionSpace(ctx->session, &spaceInfo, &ctx->aimSpaces[SRC_GAZE]),
                     "create gaze space")) {
            ctx->aimSpaces[SRC_GAZE] = XR_NULL_HANDLE;
        }
    }

    ctx->inputReady = 1;
    ctx->pointerOn = 1;
    initJointTracking(ctx);
    refreshInputSource(ctx);
    LOGI("controller input ready (pico bindings %s, hand pinch %s)",
         ctx->picoInteraction ? "offered" : "not offered by this runtime",
         ctx->handClickOk ? "bound" : (ctx->jointTracking ? "from joints" : "unavailable"));
    return 1;
}

static float actionFloat(XrCtx* ctx, XrAction action, int hand) {
    if (action == XR_NULL_HANDLE) {
        return 0.0f;
    }
    XrActionStateGetInfo get = { XR_TYPE_ACTION_STATE_GET_INFO };
    get.action = action;
    get.subactionPath = hand < 0 ? XR_NULL_PATH : ctx->handPaths[hand];

    XrActionStateFloat state = { XR_TYPE_ACTION_STATE_FLOAT };
    if (XR_FAILED(xrGetActionStateFloat(ctx->session, &get, &state)) || !state.isActive) {
        return 0.0f;
    }
    return state.currentState;
}

static int actionBool(XrCtx* ctx, XrAction action, int hand) {
    if (action == XR_NULL_HANDLE) {
        return 0;
    }
    XrActionStateGetInfo get = { XR_TYPE_ACTION_STATE_GET_INFO };
    get.action = action;
    get.subactionPath = hand < 0 ? XR_NULL_PATH : ctx->handPaths[hand];

    XrActionStateBoolean state = { XR_TYPE_ACTION_STATE_BOOLEAN };
    if (XR_FAILED(xrGetActionStateBoolean(ctx->session, &get, &state)) || !state.isActive) {
        return 0;
    }
    return state.currentState != 0;
}

static XrVector2f actionVec2(XrCtx* ctx, XrAction action, int hand) {
    XrVector2f zero = { 0.0f, 0.0f };
    if (action == XR_NULL_HANDLE) {
        return zero;
    }
    XrActionStateGetInfo get = { XR_TYPE_ACTION_STATE_GET_INFO };
    get.action = action;
    get.subactionPath = hand < 0 ? XR_NULL_PATH : ctx->handPaths[hand];

    XrActionStateVector2f state = { XR_TYPE_ACTION_STATE_VECTOR2F };
    if (XR_FAILED(xrGetActionStateVector2f(ctx->session, &get, &state)) || !state.isActive) {
        return zero;
    }
    return state.currentState;
}

// Where the aim ray lands on the screen, in 0..1 texture coordinates with v
// running down the picture. Handles the cylinder as well, since the surface
// bulges toward the viewer and a flat approximation is wrong at the edges by
// the sagitta, which is a fifth of a metre on a wrapped 3 m screen.
static int screenProject(XrPosef aim, XrPosef screen, float width, float height,
                         float radius, int curved, float* outU, float* outV) {
    XrQuaternionf inv = quatConj(screen.orientation);
    Vec3 aimPos = { aim.position.x, aim.position.y, aim.position.z };
    Vec3 screenPos = { screen.position.x, screen.position.y, screen.position.z };
    Vec3 forward = { 0.0f, 0.0f, -1.0f };

    // Both into the screen's own frame, where the surface sits in the xy plane
    Vec3 o = quatRotate(inv, vecSub(aimPos, screenPos));
    Vec3 d = quatRotate(inv, quatRotate(aim.orientation, forward));

    float hx, hy;
    if (curved) {
        // Axis is vertical through the cylinder centre, which sits behind the
        // surface by the radius. The viewer is inside, so there is one root.
        float cz = radius;
        float ox = o.x, oz = o.z - cz;
        float a = d.x * d.x + d.z * d.z;
        float b = 2.0f * (ox * d.x + oz * d.z);
        float c = ox * ox + oz * oz - radius * radius;
        if (a < 1e-6f) {
            return 0;
        }
        float disc = b * b - 4.0f * a * c;
        if (disc < 0.0f) {
            return 0;
        }
        float t = (-b + sqrtf(disc)) / (2.0f * a);
        if (t <= 0.0f) {
            return 0;
        }
        float px = o.x + t * d.x;
        float py = o.y + t * d.y;
        float pz = o.z + t * d.z;
        // Angle off the centre of the arc, which faces -z from the axis
        float angle = atan2f(px, cz - pz);
        float centralAngle = width / radius;
        hx = angle / centralAngle;
        hy = py / height;
    }
    else {
        // The quad faces +z in its own frame, so the viewer has to be in front
        // of it and pointing back at it
        if (o.z <= 0.0f || d.z >= -1e-6f) {
            return 0;
        }
        float t = -o.z / d.z;
        hx = (o.x + t * d.x) / width;
        hy = (o.y + t * d.y) / height;
    }

    *outU = hx + 0.5f;
    // Texture rows run down the picture, world y runs up it
    *outV = 0.5f - hy;
    return 1;
}
// A pointer ray from the joints, for runtimes that track hands but never offer
// a pointer pose. Cast from a shoulder rather than from the hand itself: a ray
// along the finger swings wildly with small movements of the wrist, while one
// through the hand from the shoulder is what the arm is actually aiming and is
// steady enough to hold on a target.
static void buildHandRay(XrCtx* ctx, int hand, const XrPosef* head,
                         const XrHandJointLocationEXT* joints) {
    const XrHandJointLocationEXT* knuckle = &joints[XR_HAND_JOINT_INDEX_PROXIMAL_EXT];
    if (!(knuckle->locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)) {
        ctx->handRayValid[hand] = 0;
        return;
    }

    Vec3 offset = { hand == HAND_RIGHT ? 0.17f : -0.17f, -0.20f, 0.05f };
    Vec3 shoulder = quatRotate(head->orientation, offset);
    shoulder.x += head->position.x;
    shoulder.y += head->position.y;
    shoulder.z += head->position.z;

    Vec3 origin = { knuckle->pose.position.x, knuckle->pose.position.y,
                    knuckle->pose.position.z };
    Vec3 dir = vecSub(origin, shoulder);
    float len = sqrtf(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
    if (len < 0.05f) {
        ctx->handRayValid[hand] = 0;
        return;
    }
    dir = vecNorm(dir);

    // A pose points down its own -Z, so the basis is built around that
    Vec3 worldUp = { 0.0f, 1.0f, 0.0f };
    Vec3 rayZ = { -dir.x, -dir.y, -dir.z };
    Vec3 rayX = vecCross(worldUp, rayZ);
    float side = sqrtf(rayX.x * rayX.x + rayX.y * rayX.y + rayX.z * rayX.z);
    if (side < 0.01f) {
        Vec3 fallback = { 1.0f, 0.0f, 0.0f };
        rayX = vecCross(fallback, rayZ);
    }
    rayX = vecNorm(rayX);
    Vec3 rayY = vecCross(rayZ, rayX);

    ctx->handRay[hand].orientation = quatFromBasis(rayX, rayY, rayZ);
    ctx->handRay[hand].position = knuckle->pose.position;
    ctx->handRayValid[hand] = 1;
}

static int jointPinching(XrCtx* ctx, int hand, XrSpace space, const XrPosef* head,
                         int headValid) {
    if (!ctx->jointTracking || ctx->handTrackers[hand] == XR_NULL_HANDLE) {
        ctx->handRayValid[hand] = 0;
        return 0;
    }

    XrHandJointLocationEXT joints[XR_HAND_JOINT_COUNT_EXT];
    XrHandJointLocationsEXT locations = { XR_TYPE_HAND_JOINT_LOCATIONS_EXT };
    locations.jointCount = XR_HAND_JOINT_COUNT_EXT;
    locations.jointLocations = joints;

    // Meta-specific: chained onto the same locate call, filled by the
    // runtime alongside the joints themselves - no extra API call needed.
    XrHandTrackingAimStateFB aimState = { XR_TYPE_HAND_TRACKING_AIM_STATE_FB };
    if (ctx->handTrackingAim) {
        locations.next = &aimState;
    }

    XrHandJointsLocateInfoEXT locate = { XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT };
    locate.baseSpace = space;
    locate.time = ctx->predictedDisplayTime;
    if (XR_FAILED(ctx->pfnLocateHandJoints(ctx->handTrackers[hand], &locate, &locations))
            || !locations.isActive) {
        ctx->jointPinch[hand] = 0;
        ctx->pinchPointValid[hand] = 0;
        ctx->handRayValid[hand] = 0;
        return 0;
    }

    // Meta's own aim pose is steadier than buildHandRay()'s hand-rolled
    // shoulder-ray, and doesn't need a valid head pose to construct -
    // preferred whenever the runtime actually computed one this frame.
    int haveAim = ctx->handTrackingAim
            && (aimState.status & XR_HAND_TRACKING_AIM_VALID_BIT_FB) != 0;
    if (haveAim) {
        ctx->handRay[hand] = aimState.aimPose;
        ctx->handRayValid[hand] = 1;
    }
    else if (headValid) {
        buildHandRay(ctx, hand, head, joints);
    }

    const XrHandJointLocationEXT* thumb = &joints[XR_HAND_JOINT_THUMB_TIP_EXT];
    const XrHandJointLocationEXT* index = &joints[XR_HAND_JOINT_INDEX_TIP_EXT];
    if (!(thumb->locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)
            || !(index->locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)) {
        ctx->jointPinch[hand] = 0;
        ctx->pinchPointValid[hand] = 0;
        return 0;
    }

    // Where the pinch happened, which is what a drag follows - always the
    // real fingertip joints regardless of which pinch signal is used below,
    // since the aim extension has no equivalent of its own.
    ctx->pinchPoint[hand].x = (thumb->pose.position.x + index->pose.position.x) * 0.5f;
    ctx->pinchPoint[hand].y = (thumb->pose.position.y + index->pose.position.y) * 0.5f;
    ctx->pinchPoint[hand].z = (thumb->pose.position.z + index->pose.position.z) * 0.5f;
    ctx->pinchPointValid[hand] = 1;

    // Meta's continuous per-finger pinch strength is preferred over the
    // raw thumb/index gap distance whenever this frame's locate actually
    // computed one - COMPUTED can be set without VALID (e.g. a hand at the
    // edge of tracking), so this is checked independently of haveAim above.
    if (ctx->handTrackingAim && (aimState.status & XR_HAND_TRACKING_AIM_COMPUTED_BIT_FB) != 0) {
        float threshold = ctx->jointPinch[hand] ? PINCH_STRENGTH_OFF : PINCH_STRENGTH_ON;
        ctx->jointPinch[hand] = aimState.pinchStrengthIndex >= threshold;
    }
    else {
        float dx = thumb->pose.position.x - index->pose.position.x;
        float dy = thumb->pose.position.y - index->pose.position.y;
        float dz = thumb->pose.position.z - index->pose.position.z;
        float gap = sqrtf(dx * dx + dy * dy + dz * dz);
        ctx->jointPinch[hand] = gap < (ctx->jointPinch[hand] ? PINCH_OFF_M : PINCH_ON_M);
    }
    return ctx->jointPinch[hand];
}



// Which affordance the ray is over. Corners are numbered 0 top left, 1 top
// right, 2 bottom left, 3 bottom right.
static int hoverTest(float u, float v, float width, float height, int* corner) {
    // Centred on the corner, reaching as far outside the picture as inside,
    // because that is where the bracket is drawn
    float reachM = CORNER_FRAC * width * CORNER_HOVER * 0.5f;
    float cu = reachM / width;
    float cv = reachM / height;

    int left = fabsf(u) < cu;
    int right = fabsf(u - 1.0f) < cu;
    int top = fabsf(v) < cv;
    int bottom = fabsf(v - 1.0f) < cv;
    if ((left || right) && (top || bottom)) {
        *corner = (top ? 0 : 2) + (right ? 1 : 0);
        return HOVER_CORNER;
    }

    if (u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f) {
        return HOVER_SCREEN;
    }

    // The move bar sits under the bottom edge, so v runs past 1 here
    float barU = BAR_WIDTH_FRAC * BAR_HOVER * 0.5f;
    float reach = (BAR_GAP_FRAC + BAR_HEIGHT_FRAC * 3.0f) * width / height;
    if (v > 1.0f && v < 1.0f + reach && fabsf(u - 0.5f) < barU) {
        return HOVER_BAR;
    }

    // Beyond the picture the ray still draws out to a margin, so it does not
    // blink out on the way to the handles underneath
    if (u > -HALO_FRAC && u < 1.0f + HALO_FRAC && v > -HALO_FRAC && v < 1.0f + HALO_FRAC) {
        return HOVER_HALO;
    }

    return HOVER_NONE;
}

// The inverse of screenHit: where a texture coordinate sits in space. The beam
// is drawn to the filtered point rather than the raw one, so the ray and the
// cursor agree instead of the ray shaking around a steady cursor.
static Vec3 screenPoint(float u, float v, XrPosef screen, float width, float height,
                        float radius, int curved) {
    Vec3 local;
    local.y = (0.5f - v) * height;
    if (curved) {
        float angle = (u - 0.5f) * (width / radius);
        local.x = radius * sinf(angle);
        local.z = radius - radius * cosf(angle);
    }
    else {
        local.x = (u - 0.5f) * width;
        local.z = 0.0f;
    }

    Vec3 rotated = quatRotate(screen.orientation, local);
    Vec3 world = { screen.position.x + rotated.x,
                   screen.position.y + rotated.y,
                   screen.position.z + rotated.z };
    return world;
}

static int createPointerSwapchain(XrCtx* ctx) {
    XrSwapchainCreateInfo info = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
    info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    info.format = ctx->swapchainFormat;
    info.sampleCount = 1;
    info.width = PTR_TEX_W;
    info.height = PTR_TEX_H;
    info.faceCount = 1;
    info.arraySize = 1;
    info.mipCount = 1;
    if (!checkXr(xrCreateSwapchain(ctx->session, &info, &ctx->pointerSwapchain),
                 "create pointer swapchain")) {
        ctx->pointerSwapchain = XR_NULL_HANDLE;
        return 0;
    }

    xrEnumerateSwapchainImages(ctx->pointerSwapchain, 0, &ctx->pointerImageCount, NULL);
    ctx->pointerImages = calloc(ctx->pointerImageCount, sizeof(XrSwapchainImageOpenGLESKHR));
    for (uint32_t i = 0; i < ctx->pointerImageCount; i++) {
        ctx->pointerImages[i].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR;
    }
    xrEnumerateSwapchainImages(ctx->pointerSwapchain, ctx->pointerImageCount,
                               &ctx->pointerImageCount,
                               (XrSwapchainImageBaseHeader*)ctx->pointerImages);

    // Handles get a swapchain each rather than a corner of the atlas, so there
    // is no image rect origin convention to guess at
    info.width = BAR_TEX_W;
    info.height = BAR_TEX_H;
    if (checkXr(xrCreateSwapchain(ctx->session, &info, &ctx->barSwapchain), "create bar swapchain")) {
        xrEnumerateSwapchainImages(ctx->barSwapchain, 0, &ctx->barImageCount, NULL);
        ctx->barImages = calloc(ctx->barImageCount, sizeof(XrSwapchainImageOpenGLESKHR));
        for (uint32_t i = 0; i < ctx->barImageCount; i++) {
            ctx->barImages[i].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR;
        }
        xrEnumerateSwapchainImages(ctx->barSwapchain, ctx->barImageCount, &ctx->barImageCount,
                                   (XrSwapchainImageBaseHeader*)ctx->barImages);
    }
    else {
        ctx->barSwapchain = XR_NULL_HANDLE;
    }

    // Top bar icon strip - one texture, TOPBAR_ITEM_COUNT cells wide plus a
    // bleed margin for the background pill's feather, shared by both modes
    // (see topBarPose()).
    info.width = TOPBAR_TEX_W;
    info.height = TOPBAR_TEX_H;
    if (checkXr(xrCreateSwapchain(ctx->session, &info, &ctx->topBarSwapchain),
                "create top bar swapchain")) {
        xrEnumerateSwapchainImages(ctx->topBarSwapchain, 0, &ctx->topBarImageCount, NULL);
        ctx->topBarImages = calloc(ctx->topBarImageCount,
                                   sizeof(XrSwapchainImageOpenGLESKHR));
        for (uint32_t i = 0; i < ctx->topBarImageCount; i++) {
            ctx->topBarImages[i].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR;
        }
        xrEnumerateSwapchainImages(ctx->topBarSwapchain, ctx->topBarImageCount,
                                   &ctx->topBarImageCount,
                                   (XrSwapchainImageBaseHeader*)ctx->topBarImages);
    }
    else {
        ctx->topBarSwapchain = XR_NULL_HANDLE;
    }

    info.width = SLIDER_TRACK_TEX_W;
    info.height = SLIDER_TRACK_TEX_H;
    if (checkXr(xrCreateSwapchain(ctx->session, &info, &ctx->sliderTrackSwapchain),
                "create slider track swapchain")) {
        xrEnumerateSwapchainImages(ctx->sliderTrackSwapchain, 0, &ctx->sliderTrackImageCount, NULL);
        ctx->sliderTrackImages = calloc(ctx->sliderTrackImageCount,
                                        sizeof(XrSwapchainImageOpenGLESKHR));
        for (uint32_t i = 0; i < ctx->sliderTrackImageCount; i++) {
            ctx->sliderTrackImages[i].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR;
        }
        xrEnumerateSwapchainImages(ctx->sliderTrackSwapchain, ctx->sliderTrackImageCount,
                                   &ctx->sliderTrackImageCount,
                                   (XrSwapchainImageBaseHeader*)ctx->sliderTrackImages);
    }
    else {
        ctx->sliderTrackSwapchain = XR_NULL_HANDLE;
    }

    info.width = SLIDER_THUMB_TEX;
    info.height = SLIDER_THUMB_TEX;
    if (checkXr(xrCreateSwapchain(ctx->session, &info, &ctx->sliderThumbSwapchain),
                "create slider thumb swapchain")) {
        xrEnumerateSwapchainImages(ctx->sliderThumbSwapchain, 0, &ctx->sliderThumbImageCount, NULL);
        ctx->sliderThumbImages = calloc(ctx->sliderThumbImageCount,
                                        sizeof(XrSwapchainImageOpenGLESKHR));
        for (uint32_t i = 0; i < ctx->sliderThumbImageCount; i++) {
            ctx->sliderThumbImages[i].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR;
        }
        xrEnumerateSwapchainImages(ctx->sliderThumbSwapchain, ctx->sliderThumbImageCount,
                                   &ctx->sliderThumbImageCount,
                                   (XrSwapchainImageBaseHeader*)ctx->sliderThumbImages);
    }
    else {
        ctx->sliderThumbSwapchain = XR_NULL_HANDLE;
    }

    // Glow on/off icon, next to the brightness slider
    info.width = OUTLINE_TEX;
    info.height = OUTLINE_TEX;
    if (checkXr(xrCreateSwapchain(ctx->session, &info, &ctx->glowToggleSwapchain),
                "create glow toggle swapchain")) {
        xrEnumerateSwapchainImages(ctx->glowToggleSwapchain, 0, &ctx->glowToggleImageCount, NULL);
        ctx->glowToggleImages = calloc(ctx->glowToggleImageCount,
                                       sizeof(XrSwapchainImageOpenGLESKHR));
        for (uint32_t i = 0; i < ctx->glowToggleImageCount; i++) {
            ctx->glowToggleImages[i].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR;
        }
        xrEnumerateSwapchainImages(ctx->glowToggleSwapchain, ctx->glowToggleImageCount,
                                   &ctx->glowToggleImageCount,
                                   (XrSwapchainImageBaseHeader*)ctx->glowToggleImages);
    }
    else {
        ctx->glowToggleSwapchain = XR_NULL_HANDLE;
    }

    // The glow halo itself
    info.width = GLOW_HALO_TEX;
    info.height = GLOW_HALO_TEX;
    if (checkXr(xrCreateSwapchain(ctx->session, &info, &ctx->glowHaloSwapchain),
                "create glow halo swapchain")) {
        xrEnumerateSwapchainImages(ctx->glowHaloSwapchain, 0, &ctx->glowHaloImageCount, NULL);
        ctx->glowHaloImages = calloc(ctx->glowHaloImageCount,
                                     sizeof(XrSwapchainImageOpenGLESKHR));
        for (uint32_t i = 0; i < ctx->glowHaloImageCount; i++) {
            ctx->glowHaloImages[i].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR;
        }
        xrEnumerateSwapchainImages(ctx->glowHaloSwapchain, ctx->glowHaloImageCount,
                                   &ctx->glowHaloImageCount,
                                   (XrSwapchainImageBaseHeader*)ctx->glowHaloImages);
    }
    else {
        ctx->glowHaloSwapchain = XR_NULL_HANDLE;
    }

    // Tiny - alpha is the only thing about this texture that ever matters
    info.width = DIM_TEX;
    info.height = DIM_TEX;
    if (checkXr(xrCreateSwapchain(ctx->session, &info, &ctx->dimSwapchain),
                "create dim swapchain")) {
        xrEnumerateSwapchainImages(ctx->dimSwapchain, 0, &ctx->dimImageCount, NULL);
        ctx->dimImages = calloc(ctx->dimImageCount, sizeof(XrSwapchainImageOpenGLESKHR));
        for (uint32_t i = 0; i < ctx->dimImageCount; i++) {
            ctx->dimImages[i].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR;
        }
        xrEnumerateSwapchainImages(ctx->dimSwapchain, ctx->dimImageCount, &ctx->dimImageCount,
                                   (XrSwapchainImageBaseHeader*)ctx->dimImages);
    }
    else {
        ctx->dimSwapchain = XR_NULL_HANDLE;
    }

    // Same tiny alpha-only recipe as the dim swapchain above
    if (checkXr(xrCreateSwapchain(ctx->session, &info, &ctx->fadeSwapchain),
                "create fade swapchain")) {
        xrEnumerateSwapchainImages(ctx->fadeSwapchain, 0, &ctx->fadeImageCount, NULL);
        ctx->fadeImages = calloc(ctx->fadeImageCount, sizeof(XrSwapchainImageOpenGLESKHR));
        for (uint32_t i = 0; i < ctx->fadeImageCount; i++) {
            ctx->fadeImages[i].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR;
        }
        xrEnumerateSwapchainImages(ctx->fadeSwapchain, ctx->fadeImageCount, &ctx->fadeImageCount,
                                   (XrSwapchainImageBaseHeader*)ctx->fadeImages);
    }
    else {
        ctx->fadeSwapchain = XR_NULL_HANDLE;
    }

    info.width = CORNER_TEX_W;
    info.height = CORNER_TEX_H;
    if (checkXr(xrCreateSwapchain(ctx->session, &info, &ctx->cornerSwapchain),
                "create corner swapchain")) {
        xrEnumerateSwapchainImages(ctx->cornerSwapchain, 0, &ctx->cornerImageCount, NULL);
        ctx->cornerImages = calloc(ctx->cornerImageCount, sizeof(XrSwapchainImageOpenGLESKHR));
        for (uint32_t i = 0; i < ctx->cornerImageCount; i++) {
            ctx->cornerImages[i].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR;
        }
        xrEnumerateSwapchainImages(ctx->cornerSwapchain, ctx->cornerImageCount,
                                   &ctx->cornerImageCount,
                                   (XrSwapchainImageBaseHeader*)ctx->cornerImages);
    }
    else {
        ctx->cornerSwapchain = XR_NULL_HANDLE;
    }

    return 1;
}

// Uploads one CPU buffer into a swapchain and hands the image straight back
static int uploadArt(XrCtx* ctx, XrSwapchain chain, XrSwapchainImageOpenGLESKHR* images,
                     const unsigned char* px, int width, int height) {
    if (chain == XR_NULL_HANDLE) {
        return 0;
    }

    uint32_t index = 0;
    XrSwapchainImageAcquireInfo acquire = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    if (!checkXr(xrAcquireSwapchainImage(chain, &acquire, &index), "acquire art image")) {
        return 0;
    }
    XrSwapchainImageWaitInfo wait = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    wait.timeout = XR_INFINITE_DURATION;
    xrWaitSwapchainImage(chain, &wait);

    glBindTexture(GL_TEXTURE_2D, images[index].image);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glBindTexture(GL_TEXTURE_2D, 0);

    XrSwapchainImageReleaseInfo release = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    xrReleaseSwapchainImage(chain, &release);
    return 1;
}

// Rows arrive bottom up, so a photo uploaded as it comes would put the sky
// underfoot
static int uploadFlipped(XrCtx* ctx, XrSwapchain chain, XrSwapchainImageOpenGLESKHR* images,
                         const unsigned char* px, int width, int height) {
    size_t stride = (size_t)width * 4;
    unsigned char* flipped = malloc(stride * height);
    if (flipped == NULL) {
        return 0;
    }
    for (int y = 0; y < height; y++) {
        memcpy(flipped + stride * y, px + stride * (height - 1 - y), stride);
    }
    int ok = uploadArt(ctx, chain, images, flipped, width, height);
    free(flipped);
    return ok;
}

// Soft edged coverage for a distance from a shape, in pixels
static float edgeAlpha(float distance, float halfStroke) {
    float a = (halfStroke - distance) / 1.5f + 0.5f;
    if (a < 0.0f) return 0.0f;
    if (a > 1.0f) return 1.0f;
    return a;
}

static void buildHandleArt(XrCtx* ctx) {
    unsigned char* bar = calloc(BAR_TEX_W * BAR_TEX_H * 4, 1);
    unsigned char* corner = calloc(CORNER_TEX_W * CORNER_TEX_H * 4, 1);
    if (bar == NULL || corner == NULL) {
        free(bar);
        free(corner);
        return;
    }

    // A rounded bar, symmetric, so the row order does not matter here
    float barR = BAR_TEX_H * 0.5f;
    for (int y = 0; y < BAR_TEX_H; y++) {
        for (int x = 0; x < BAR_TEX_W; x++) {
            float px = x + 0.5f, py = y + 0.5f;
            float cx = px;
            if (cx < barR) cx = barR;
            if (cx > BAR_TEX_W - barR) cx = BAR_TEX_W - barR;
            float dx = px - cx, dy = py - barR;
            float d = sqrtf(dx * dx + dy * dy);
            unsigned char* p = bar + ((y * BAR_TEX_W) + x) * 4;
            unsigned char a = (unsigned char)(edgeAlpha(d, barR - 1.0f) * 235.0f);
            p[0] = p[1] = p[2] = a;
            p[3] = a;
        }
    }

    // A rounded bracket whose outer corner sits at the middle of the tile, with
    // the two runs going right and down from it, so centring the quad on a
    // corner of the screen wraps that corner. Rows are written bottom up: a
    // buffer uploaded the normal way arrives vertically flipped.
    const float mid = CORNER_TEX_W * 0.5f;
    const float arcR = 10.0f;
    const float stroke = 3.0f;
    for (int y = 0; y < CORNER_TEX_H; y++) {
        for (int x = 0; x < CORNER_TEX_W; x++) {
            float px = x + 0.5f, py = y + 0.5f;
            float d;
            if (px < mid + arcR && py < mid + arcR) {
                float ax = px - (mid + arcR), ay = py - (mid + arcR);
                d = fabsf(sqrtf(ax * ax + ay * ay) - arcR);
            }
            else if (px >= mid + arcR) {
                d = fabsf(py - mid);
            }
            else {
                d = fabsf(px - mid);
            }
            unsigned char* p = corner + (((CORNER_TEX_H - 1 - y) * CORNER_TEX_W) + x) * 4;
            unsigned char a = (unsigned char)(edgeAlpha(d, stroke) * 235.0f);
            p[0] = p[1] = p[2] = a;
            p[3] = a;
        }
    }

    int ok = uploadArt(ctx, ctx->barSwapchain, ctx->barImages, bar, BAR_TEX_W, BAR_TEX_H);
    ok &= uploadArt(ctx, ctx->cornerSwapchain, ctx->cornerImages, corner,
                    CORNER_TEX_W, CORNER_TEX_H);
    ctx->handleArtReady = ok;

    free(bar);
    free(corner);
}

// Slider track (a pill, same technique as the move bar above) and thumb (a
// filled soft-edged disk). Built once; the thumb's position along the track
// is a per-frame pose update in nativeEndFrame, never a texture change.
static void buildSliderArt(XrCtx* ctx) {
    unsigned char* track = calloc(SLIDER_TRACK_TEX_W * SLIDER_TRACK_TEX_H * 4, 1);
    unsigned char* thumb = calloc(SLIDER_THUMB_TEX * SLIDER_THUMB_TEX * 4, 1);
    if (track == NULL || thumb == NULL) {
        free(track);
        free(thumb);
        return;
    }

    float trackR = SLIDER_TRACK_TEX_H * 0.5f;
    for (int y = 0; y < SLIDER_TRACK_TEX_H; y++) {
        for (int x = 0; x < SLIDER_TRACK_TEX_W; x++) {
            float px = x + 0.5f, py = y + 0.5f;
            float cx = px;
            if (cx < trackR) cx = trackR;
            if (cx > SLIDER_TRACK_TEX_W - trackR) cx = SLIDER_TRACK_TEX_W - trackR;
            float dx = px - cx, dy = py - trackR;
            float d = sqrtf(dx * dx + dy * dy);
            unsigned char* p = track + ((y * SLIDER_TRACK_TEX_W) + x) * 4;
            unsigned char a = (unsigned char)(edgeAlpha(d, trackR - 1.0f) * 200.0f);
            p[0] = p[1] = p[2] = a;
            p[3] = a;
        }
    }

    float thumbR = SLIDER_THUMB_TEX * 0.5f;
    for (int y = 0; y < SLIDER_THUMB_TEX; y++) {
        for (int x = 0; x < SLIDER_THUMB_TEX; x++) {
            float dx = (x + 0.5f) - thumbR, dy = (y + 0.5f) - thumbR;
            float d = sqrtf(dx * dx + dy * dy);
            unsigned char* p = thumb + ((y * SLIDER_THUMB_TEX) + x) * 4;
            unsigned char a = (unsigned char)(edgeAlpha(d, thumbR - 1.5f) * 255.0f);
            // Premultiplied, like the track above - XR_COMPOSITION_LAYER_
            // BLEND_TEXTURE_SOURCE_ALPHA_BIT's blend equation is Color_dst =
            // Color_src + Color_dst*(1-Alpha_src), which assumes Color_src
            // already carries its own alpha. A flat 255 here (not scaled by
            // a) still adds full white outside the circle even at alpha 0,
            // which is exactly why this rendered as a hard square instead
            // of fading to nothing.
            p[0] = p[1] = p[2] = a;
            p[3] = a;
        }
    }

    int ok = uploadArt(ctx, ctx->sliderTrackSwapchain, ctx->sliderTrackImages, track,
                       SLIDER_TRACK_TEX_W, SLIDER_TRACK_TEX_H);
    ok &= uploadArt(ctx, ctx->sliderThumbSwapchain, ctx->sliderThumbImages, thumb,
                    SLIDER_THUMB_TEX, SLIDER_THUMB_TEX);
    ctx->sliderArtReady = ok;

    free(track);
    free(thumb);
}

// Has to run on the frame loop with the session going. Waiting on a swapchain
// image at init time blocks until the runtime is ready to hand one over, which
// on a session that has not begun is never, and the whole session hangs behind
// it with the shell stuck on its loading screen.
static int uploadPointerArt(XrCtx* ctx) {
    unsigned char* px = calloc(PTR_TEX_W * PTR_TEX_H * 4, 1);
    if (px == NULL) {
        return 0;
    }

    const float half = PTR_TEX_W * 0.5f;
    for (int y = 0; y < PTR_BEAM_H; y++) {
        // Fades at both ends. Which end of the texture meets the hand depends
        // on how the runtime orients the image, and symmetric art does not care
        float along = (y + 0.5f) / PTR_BEAM_H;
        float edge = along < 0.5f ? along : 1.0f - along;
        float lengthFade = edge < 0.12f ? edge / 0.12f : 1.0f;
        for (int x = 0; x < PTR_TEX_W; x++) {
            float r = fabsf((x + 0.5f) - half) / half;
            float t = r * 3.2f;
            float a = expf(-t * t) * lengthFade;
            unsigned char* p = px + ((y * PTR_TEX_W) + x) * 4;
            unsigned char lit = (unsigned char)(a * 255.0f + 0.5f);
            p[0] = lit;
            p[1] = lit;
            p[2] = lit;
            p[3] = lit;
        }
    }

    for (int y = 0; y < PTR_DOT_H; y++) {
        for (int x = 0; x < PTR_TEX_W; x++) {
            float dx = ((x + 0.5f) - half) / half;
            float dy = ((y + 0.5f) - PTR_DOT_H * 0.5f) / (PTR_DOT_H * 0.5f);
            float r = sqrtf(dx * dx + dy * dy);
            // Solid core with a soft edge, and a darker rim so it stays
            // visible against a bright picture
            float a = r < 0.45f ? 1.0f : (r < 0.75f ? (0.75f - r) / 0.30f : 0.0f);
            float shade = r < 0.35f ? 1.0f : 0.25f;
            unsigned char* p = px + (((PTR_BEAM_H + y) * PTR_TEX_W) + x) * 4;
            unsigned char lit = (unsigned char)(a * 255.0f * shade + 0.5f);
            p[0] = lit;
            p[1] = lit;
            p[2] = lit;
            p[3] = (unsigned char)(a * 255.0f + 0.5f);
        }
    }

    uint32_t index = 0;
    XrSwapchainImageAcquireInfo acquire = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    if (checkXr(xrAcquireSwapchainImage(ctx->pointerSwapchain, &acquire, &index),
                "acquire pointer image")) {
        XrSwapchainImageWaitInfo wait = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
        wait.timeout = XR_INFINITE_DURATION;
        xrWaitSwapchainImage(ctx->pointerSwapchain, &wait);

        glBindTexture(GL_TEXTURE_2D, ctx->pointerImages[index].image);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, PTR_TEX_W, PTR_TEX_H,
                        GL_RGBA, GL_UNSIGNED_BYTE, px);
        glBindTexture(GL_TEXTURE_2D, 0);

        XrSwapchainImageReleaseInfo release = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
        xrReleaseSwapchainImage(ctx->pointerSwapchain, &release);
        // Drawn once and submitted from then on, the art never changes
        ctx->pointerArtReady = 1;
    }

    free(px);
    if (ctx->pointerArtReady) {
        buildHandleArt(ctx);
        buildSliderArt(ctx);
    }
    return ctx->pointerArtReady;
}

// The sliders place the screen, the grab moves it from there. Moving either
// slider is taken as the user asking for the placement back.
static void updatePlacement(XrCtx* ctx, float distance, float quadWidth) {
    int sliderMoved = ctx->sliderSeen
            && (fabsf(distance - ctx->lastDistance) > 1e-4f
                || fabsf(quadWidth - ctx->lastQuadWidth) > 1e-4f);

    if (!ctx->placementValid || sliderMoved) {
        memset(&ctx->screenPose, 0, sizeof(ctx->screenPose));
        ctx->screenPose.orientation.w = 1.0f;
        ctx->screenPose.position.z = -distance;
        ctx->screenWidth = quadWidth;
        // Radius runs from CURVE_RADIUS_MAX_MULT x distance (slightly
        // curved) down to CURVE_RADIUS_MIN_MULT x distance (a tight wrap)
        // as curveAmount rises. Only the seed here - live changes from the
        // top bar's curve slider go straight to ctx->screenRadius instead
        // (see applySliderValue()), since this whole branch also resets
        // screen position/pose.
        ctx->screenRadius = distance * (CURVE_RADIUS_MIN_MULT
                + (CURVE_RADIUS_MAX_MULT - CURVE_RADIUS_MIN_MULT) * (1.0f - ctx->curveAmount));
        ctx->placementValid = 1;
        ctx->grabMode = GRAB_NONE;
        ctx->poseDirty = 1;
    }

    ctx->lastDistance = distance;
    ctx->lastQuadWidth = quadWidth;
    ctx->sliderSeen = 1;
}

// Phase 1 fixed layout: N screens spaced evenly left-to-right on a shallow
// arc, all facing back toward the origin. No curve/distance/height/spacing
// controls yet (that's Phase 2). The small-angle approximation
// (arc length ~= chord length) is fine here since this is just a default,
// not a value a slider needs to be precise about.
static XrPosef productivityScreenPose(int index) {
    float step = (PRODUCTIVITY_SCREEN_WIDTH_M + PRODUCTIVITY_GAP_M) / PRODUCTIVITY_DISTANCE_M;
    float angle = (index - (PRODUCTIVITY_SCREEN_COUNT - 1) * 0.5f) * step;

    XrPosef pose;
    memset(&pose, 0, sizeof(pose));
    pose.orientation.y = sinf(angle * 0.5f);
    pose.orientation.w = cosf(angle * 0.5f);
    pose.position.x = PRODUCTIVITY_DISTANCE_M * sinf(angle);
    pose.position.z = -PRODUCTIVITY_DISTANCE_M * cosf(angle);
    return pose;
}

// Anchor for the whole top menu bar: straight above the screen(s), facing
// the same way they do. The bar itself is one module shared by both modes -
// this is the only place that actually differs between them, since Gaming
// has one resizable screen and Productivity Mode has three fixed ones.
static XrPosef topBarPose(XrCtx* ctx) {
    if (ctx->productivityMode) {
        XrPosef pose = productivityScreenPose((PRODUCTIVITY_SCREEN_COUNT - 1) / 2);
        Vec3 local = { 0.0f, PRODUCTIVITY_BAR_Y_OFFSET_M, 0.02f };
        Vec3 up = quatRotate(pose.orientation, local);
        pose.position.x += up.x;
        pose.position.y += up.y;
        pose.position.z += up.z;
        return pose;
    }

    XrPosef pose = ctx->screenPose;
    float screenHeight = ctx->screenWidth * (float)ctx->videoHeight / (float)ctx->videoWidth;
    Vec3 local = { 0.0f, screenHeight * 0.5f + ctx->screenWidth * TOPBAR_GAP_FRAC
                         + TOPBAR_ITEM_SIZE_M * 0.5f, 0.02f };
    Vec3 up = quatRotate(pose.orientation, local);
    pose.position.x += up.x;
    pose.position.y += up.y;
    pose.position.z += up.z;
    return pose;
}

// One slot along the bar. Modules are laid out left to right in index order;
// adding one is just raising TOPBAR_ITEM_COUNT and giving the new index a
// place to draw/hit-test, same as TOPBAR_EXIT_INDEX/TOPBAR_BRIGHTNESS_INDEX.
static XrPosef topBarItemPose(XrCtx* ctx, int index, int count) {
    XrPosef pose = topBarPose(ctx);
    float step = TOPBAR_ITEM_SIZE_M + TOPBAR_ITEM_GAP_M;
    float x = (index - (count - 1) * 0.5f) * step;
    Vec3 offset = quatRotate(pose.orientation, (Vec3){ x, 0.0f, 0.0f });
    pose.position.x += offset.x;
    pose.position.y += offset.y;
    pose.position.z += offset.z;
    return pose;
}

// Above the brightness icon specifically, not the bar centre - it should
// read as belonging to the icon that opened it.
static XrPosef topBarSliderPose(XrCtx* ctx, int item) {
    XrPosef pose = topBarItemPose(ctx, item, TOPBAR_ITEM_COUNT);
    Vec3 local = { 0.0f, TOPBAR_ITEM_SIZE_M * 0.5f + SLIDER_GAP_M + SLIDER_TRACK_HEIGHT_M * 0.5f,
                   -0.005f };
    Vec3 up = quatRotate(pose.orientation, local);
    pose.position.x += up.x;
    pose.position.y += up.y;
    pose.position.z += up.z;
    return pose;
}

// To the right of the brightness slider's track - only ever positioned
// relative to TOPBAR_BRIGHTNESS_INDEX's slider, since glow has no meaning
// for the curve slider.
static XrPosef topBarGlowTogglePose(XrCtx* ctx) {
    XrPosef pose = topBarSliderPose(ctx, TOPBAR_BRIGHTNESS_INDEX);
    Vec3 local = { SLIDER_TRACK_WIDTH_M * 0.5f + GLOW_TOGGLE_GAP_M + GLOW_TOGGLE_SIZE_M * 0.5f,
                   0.0f, 0.0f };
    Vec3 offset = quatRotate(pose.orientation, local);
    pose.position.x += offset.x;
    pose.position.y += offset.y;
    pose.position.z += offset.z;
    return pose;
}

// Handed back only when a grab ends, so preferences are written once per move
// rather than every frame of it
// Flushes anything pending back to Java once it settles, rather than on
// every frame of a drag - screen pose (Gaming only) and passthrough level
// (both modes) are independent, so each has its own dirty flag.
static void writeInputPose(XrCtx* ctx, float* out) {
    if (ctx->poseDirty) {
        ctx->poseDirty = 0;
        out[IN_POSE_DIRTY] = 1.0f;
        out[IN_POSE + 0] = ctx->screenPose.position.x;
        out[IN_POSE + 1] = ctx->screenPose.position.y;
        out[IN_POSE + 2] = ctx->screenPose.position.z;
        out[IN_POSE + 3] = ctx->screenPose.orientation.x;
        out[IN_POSE + 4] = ctx->screenPose.orientation.y;
        out[IN_POSE + 5] = ctx->screenPose.orientation.z;
        out[IN_POSE + 6] = ctx->screenPose.orientation.w;
        out[IN_POSE + 7] = ctx->screenWidth;
        out[IN_POSE + 8] = ctx->screenRadius;
    }
    if (ctx->passthroughLevelDirty) {
        ctx->passthroughLevelDirty = 0;
        out[IN_PASSTHROUGH_DIRTY] = 1.0f;
        out[IN_PASSTHROUGH_LEVEL] = ctx->passthroughLevel;
    }
    if (ctx->curveAmountDirty) {
        ctx->curveAmountDirty = 0;
        out[IN_CURVE_DIRTY] = 1.0f;
        out[IN_CURVE_LEVEL] = ctx->curveAmount;
    }
}

// A short, light click rather than a buzz - this fires on every button
// press/grab in both modes, so it needs to read as a tap, not an event you
// have to wait out.
static void fireHaptic(XrCtx* ctx, int hand) {
    if (ctx->hapticAction == XR_NULL_HANDLE) {
        return;
    }
    XrHapticVibration vibration = { XR_TYPE_HAPTIC_VIBRATION };
    vibration.amplitude = 0.6f;
    vibration.duration = 60000000; // 60ms, in nanoseconds
    vibration.frequency = XR_FREQUENCY_UNSPECIFIED;

    XrHapticActionInfo info = { XR_TYPE_HAPTIC_ACTION_INFO };
    info.action = ctx->hapticAction;
    info.subactionPath = ctx->handPaths[hand];
    xrApplyHapticFeedback(ctx->session, &info, (const XrHapticBaseHeader*)&vibration);
}

// Move and resize both work off the handle the ray was over when the grip
// closed. Gripping the picture itself does nothing, which keeps the panel from
// being dragged by accident while pointing at something.
static void applyGrab(XrCtx* ctx, XrPosef* aims, const int* valid, int hand,
                      int hover, int corner, int offPicture, float height, int curved) {
    for (int h = 0; h < HAND_COUNT; h++) {
        int wasDown = ctx->grabDown[h];
        float value = actionFloat(ctx, ctx->grabAction, h);
        ctx->grabDown[h] = value > (wasDown ? PRESS_OFF : PRESS_ON);
        ctx->gripEdge[h] = ctx->grabDown[h] && !wasDown;
    }

    if (ctx->grabMode != GRAB_NONE) {
        int stillHeld = ctx->grabByTrigger ? ctx->triggerDown[ctx->grabHand]
                                           : ctx->grabDown[ctx->grabHand];
        if (!stillHeld || !valid[ctx->grabHand]) {
            // Persist where it ended up, not every frame of the drag
            ctx->grabMode = GRAB_NONE;
            ctx->poseDirty = 1;
            return;
        }
    }

    if (ctx->grabMode == GRAB_NONE) {
        if (hand < 0 || (hover != HOVER_BAR && hover != HOVER_CORNER)) {
            return;
        }

        // Apps disagree about which button grabs, so both do. The trigger only
        // counts where the handle hangs outside the picture, since inside it is
        // a left click and the bottom corners of a desktop are worth clicking.
        int byGrip = ctx->gripEdge[hand];
        int byTrigger = ctx->triggerEdge[hand] && offPicture;
        if (!byGrip && !byTrigger) {
            return;
        }
        ctx->grabByTrigger = !byGrip;

        fireHaptic(ctx, hand);
        ctx->grabHand = hand;
        ctx->grabAim = aims[hand];
        ctx->grabScreen = ctx->screenPose;
        ctx->grabWidth = ctx->screenWidth;
        ctx->grabHeight = height;
        ctx->grabRadius = ctx->screenRadius;

        if (hover == HOVER_BAR) {
            ctx->grabMode = GRAB_MOVE;
            return;
        }

        float u, v;
        if (!screenProject(aims[hand], ctx->grabScreen, ctx->screenWidth, height,
                           ctx->screenRadius, curved, &u, &v)) {
            return;
        }

        // The corner across the diagonal is the anchor, and the drag is
        // measured along the diagonal it started on
        int right = (corner == 1 || corner == 3);
        int bottom = (corner >= 2);
        ctx->grabOppX = (right ? -0.5f : 0.5f) * ctx->grabWidth;
        ctx->grabOppY = (bottom ? 0.5f : -0.5f) * ctx->grabHeight;
        ctx->grabDiagX = (u - 0.5f) * ctx->grabWidth - ctx->grabOppX;
        ctx->grabDiagY = (0.5f - v) * ctx->grabHeight - ctx->grabOppY;
        if (fabsf(ctx->grabDiagX) < 1e-3f && fabsf(ctx->grabDiagY) < 1e-3f) {
            return;
        }
        ctx->grabMode = GRAB_RESIZE;
        return;
    }

    int h = ctx->grabHand;
    if (ctx->grabMode == GRAB_MOVE) {
        // Rigid attach: the screen keeps its offset and rotation relative to
        // the hand, so it swings around naturally instead of sliding flat
        XrQuaternionf turn = quatMul(aims[h].orientation, quatConj(ctx->grabAim.orientation));
        Vec3 offset = { ctx->grabScreen.position.x - ctx->grabAim.position.x,
                        ctx->grabScreen.position.y - ctx->grabAim.position.y,
                        ctx->grabScreen.position.z - ctx->grabAim.position.z };
        Vec3 moved = quatRotate(turn, offset);

        ctx->screenPose.orientation = quatNorm(quatMul(turn, ctx->grabScreen.orientation));
        ctx->screenPose.position.x = aims[h].position.x + moved.x;
        ctx->screenPose.position.y = aims[h].position.y + moved.y;
        ctx->screenPose.position.z = aims[h].position.z + moved.z;
        return;
    }

    // Resize. Everything is measured against the pose the grab started from,
    // so growing the screen cannot feed back into where the ray lands on it.
    float u, v;
    if (!screenProject(aims[h], ctx->grabScreen, ctx->grabWidth, ctx->grabHeight,
                       ctx->grabRadius, curved, &u, &v)) {
        return;
    }

    float dx = (u - 0.5f) * ctx->grabWidth - ctx->grabOppX;
    float dy = (0.5f - v) * ctx->grabHeight - ctx->grabOppY;
    float diagLen = ctx->grabDiagX * ctx->grabDiagX + ctx->grabDiagY * ctx->grabDiagY;
    float scale = (dx * ctx->grabDiagX + dy * ctx->grabDiagY) / diagLen;
    if (scale < 0.05f) {
        scale = 0.05f;
    }

    float width = ctx->grabWidth * scale;
    if (width < SCREEN_MIN_WIDTH) width = SCREEN_MIN_WIDTH;
    if (width > SCREEN_MAX_WIDTH) width = SCREEN_MAX_WIDTH;
    float newHeight = ctx->grabHeight * (width / ctx->grabWidth);

    // Keeping the arc the same shape rather than flattening as it grows
    ctx->screenRadius = ctx->grabRadius * (width / ctx->grabWidth);
    ctx->screenWidth = width;

    // The anchor corner stays where it was, so the screen grows away from it
    Vec3 centreLocal;
    centreLocal.x = ctx->grabOppX + (ctx->grabOppX > 0.0f ? -0.5f : 0.5f) * width;
    centreLocal.y = ctx->grabOppY + (ctx->grabOppY > 0.0f ? -0.5f : 0.5f) * newHeight;
    centreLocal.z = 0.0f;

    Vec3 centre = quatRotate(ctx->grabScreen.orientation, centreLocal);
    ctx->screenPose.orientation = ctx->grabScreen.orientation;
    ctx->screenPose.position.x = ctx->grabScreen.position.x + centre.x;
    ctx->screenPose.position.y = ctx->grabScreen.position.y + centre.y;
    ctx->screenPose.position.z = ctx->grabScreen.position.z + centre.z;
}

// Where the ray lands on furniture rather than on the picture. Used to be two
// cases (the picker grid had a plane of its own, off the screen); now
// everything sits on the screen, so this is a plain pass-through kept for the
// call sites' sake.
static Vec3 furniturePoint(XrCtx* ctx, int hover, float u, float v, XrPosef screenPose,
                           float height, float radius, int curved) {
    (void)hover;
    return screenPoint(u, v, screenPose, ctx->screenWidth, height, radius, curved);
}

static void destroyXrInput(XrCtx* ctx) {
    for (int h = 0; h < HAND_COUNT; h++) {
        if (ctx->handTrackers[h] != XR_NULL_HANDLE && ctx->pfnDestroyHandTracker != NULL) {
            ctx->pfnDestroyHandTracker(ctx->handTrackers[h]);
            ctx->handTrackers[h] = XR_NULL_HANDLE;
        }
    }
    for (int h = 0; h < SRC_COUNT; h++) {
        if (ctx->aimSpaces[h] != XR_NULL_HANDLE) {
            xrDestroySpace(ctx->aimSpaces[h]);
            ctx->aimSpaces[h] = XR_NULL_HANDLE;
        }
    }
    if (ctx->actionSet != XR_NULL_HANDLE) {
        // Takes its actions with it
        xrDestroyActionSet(ctx->actionSet);
        ctx->actionSet = XR_NULL_HANDLE;
    }
    ctx->inputReady = 0;
}

static void destroyCtx(JNIEnv* env, XrCtx* ctx) {
    destroyXrInput(ctx);

    free(ctx->readbackBuf);
    free(ctx->modelInput);
    free(ctx->modelOutput);
    free(ctx->depthUploadBuf);
    free(ctx->depthEma);
    free(ctx->depthLow);
    free(ctx->depthScratch);
    free(ctx->depthColSums);

    if (ctx->swapchain != XR_NULL_HANDLE) {
        xrDestroySwapchain(ctx->swapchain);
    }
    free(ctx->swapchainImages);
    if (ctx->overlaySwapchain != XR_NULL_HANDLE) {
        xrDestroySwapchain(ctx->overlaySwapchain);
    }
    free(ctx->overlayImages);
    if (ctx->pointerSwapchain != XR_NULL_HANDLE) {
        xrDestroySwapchain(ctx->pointerSwapchain);
    }
    free(ctx->pointerImages);
    if (ctx->barSwapchain != XR_NULL_HANDLE) {
        xrDestroySwapchain(ctx->barSwapchain);
    }
    free(ctx->barImages);
    if (ctx->cornerSwapchain != XR_NULL_HANDLE) {
        xrDestroySwapchain(ctx->cornerSwapchain);
    }
    free(ctx->cornerImages);
    if (ctx->topBarSwapchain != XR_NULL_HANDLE) {
        xrDestroySwapchain(ctx->topBarSwapchain);
    }
    free(ctx->topBarImages);
    if (ctx->sliderTrackSwapchain != XR_NULL_HANDLE) {
        xrDestroySwapchain(ctx->sliderTrackSwapchain);
    }
    free(ctx->sliderTrackImages);
    if (ctx->sliderThumbSwapchain != XR_NULL_HANDLE) {
        xrDestroySwapchain(ctx->sliderThumbSwapchain);
    }
    free(ctx->sliderThumbImages);
    if (ctx->glowToggleSwapchain != XR_NULL_HANDLE) {
        xrDestroySwapchain(ctx->glowToggleSwapchain);
    }
    free(ctx->glowToggleImages);
    if (ctx->glowHaloSwapchain != XR_NULL_HANDLE) {
        xrDestroySwapchain(ctx->glowHaloSwapchain);
    }
    free(ctx->glowHaloImages);
    if (ctx->dimSwapchain != XR_NULL_HANDLE) {
        xrDestroySwapchain(ctx->dimSwapchain);
    }
    free(ctx->dimImages);
    if (ctx->fadeSwapchain != XR_NULL_HANDLE) {
        xrDestroySwapchain(ctx->fadeSwapchain);
    }
    free(ctx->fadeImages);
    if (ctx->localSpace != XR_NULL_HANDLE) {
        xrDestroySpace(ctx->localSpace);
    }
    if (ctx->viewSpace != XR_NULL_HANDLE) {
        xrDestroySpace(ctx->viewSpace);
    }
    if (ctx->session != XR_NULL_HANDLE) {
        xrDestroySession(ctx->session);
    }
    if (ctx->instance != XR_NULL_HANDLE) {
        xrDestroyInstance(ctx->instance);
    }

    if (ctx->eglDisplay != EGL_NO_DISPLAY) {
        eglMakeCurrent(ctx->eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (ctx->eglPbuffer != EGL_NO_SURFACE) {
            eglDestroySurface(ctx->eglDisplay, ctx->eglPbuffer);
        }
        if (ctx->eglContext != EGL_NO_CONTEXT) {
            eglDestroyContext(ctx->eglDisplay, ctx->eglContext);
        }
        eglReleaseThread();
    }

    if (ctx->activity != NULL) {
        (*env)->DeleteGlobalRef(env, ctx->activity);
    }
    free(ctx);
}

JNIEXPORT jlong JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeInit(JNIEnv* env, jobject thiz,
                                                       jobject activity, jint width, jint height,
                                                       jint stereoMode, jboolean depthDebug,
                                                       jint convergence, jint depthScale,
                                                       jboolean productivityMode) {
    XrCtx* ctx = calloc(1, sizeof(XrCtx));
    ctx->videoWidth = width;
    ctx->videoHeight = height;
    ctx->stereoMode = stereoMode;
    ctx->depthDebug = depthDebug;
    ctx->productivityMode = productivityMode;
    ctx->sessionState = XR_SESSION_STATE_UNKNOWN;
    // Depth arrives at about 20 Hz, so 0.6 settles in roughly two updates.
    // The range moves much more slowly on purpose, it should track the scene
    // rather than the frame.
    ctx->depthAlpha = 0.60f;
    ctx->rangeAlpha = 0.15f;
    // 0.25 measured best on a captured frame: same 5 px edge as tighter
    // values with a tenth of the speckle
    ctx->upsampleSigmaR = 0.25f;
    ctx->upsampleEnabled = 1;
    ctx->occlusionEnabled = 1;
    // Off until it earns its place in a blind comparison on device
    ctx->depthSharp = 0.0f;
    // Shown whenever there is text to show, the preference is the real gate
    ctx->overlayVisible = 1;
    ctx->separationOverride = -1.0f;
    ctx->distanceOverride = -1.0f;
    ctx->screenOverride = -1.0f;
    ctx->pointerMinCutoff = POINTER_MIN_CUTOFF;
    ctx->pointerBeta = POINTER_BETA;
    ctx->pointerWake = POINTER_WAKE_SEC;
    ctx->pointerSleep = POINTER_SLEEP_SEC;
    // Matches Meta's own documented convention for a system-style laser
    // (0.003-0.005m) instead of the previous 1cm, which read noticeably
    // thicker than Quest's own pointer for 2D panels - the goal is for
    // this to feel like the same visual language, not a custom one.
    ctx->beamWidth = 0.004f;
    // Comfort comes from absolute disparity and depth comes from the steps
    // between objects, so the overall shape is pulled toward the screen plane
    // while the local detail is boosted. Measured on captured frames this is
    // about 40 percent more depth at the object boundaries for slightly less
    // clipping than leaving it alone, where the best plain tone curve managed
    // 16 percent.
    ctx->depthGlobal = 1.0f;
    ctx->convergence = convergence / 100.0f;
    ctx->depthLocal = depthScale / 100.0f;
    // Full passthrough until nativeSetPassthroughLevel restores the saved
    // value - a struct fresh out of calloc would otherwise read as 0.0 (full
    // black) for the handful of frames before Java's restore call lands.
    ctx->passthroughLevel = 1.0f;
    // 0 (TOPBAR_EXIT_INDEX) is a real item, so this can't rely on calloc's
    // zero-init like most flags here
    ctx->openSlider = TOPBAR_NO_SLIDER;
    // Overwritten immediately by nativeSetGlowEnabled() once Java restores
    // the saved value - true here only covers the frames before that lands.
    ctx->glowEnabled = 1;
    // Overwritten immediately by nativeSetDepthEffect() once Java restores
    // the real saved value - true here only covers the handful of frames
    // before that lands.
    ctx->depthEffectOn = 1;
    (*env)->GetJavaVM(env, &ctx->vm);
    ctx->activity = (*env)->NewGlobalRef(env, activity);

    if (!initXrInstance(ctx) || !initEgl(ctx) || !initXrSession(ctx) ||
            !initSwapchain(ctx) || !initGl(ctx)) {
        destroyCtx(env, ctx);
        return 0;
    }

    // Optional: a runtime with no controllers, or one that rejects every
    // binding we know, still streams. It just has no pointer.
    if (!initXrInput(ctx)) {
        LOGW("controller input unavailable, pointer off");
        destroyXrInput(ctx);
    }
    else if (!createPointerSwapchain(ctx)) {
        LOGW("pointer swapchain unavailable, the ray will not be drawn");
    }

    LOGI("OpenXR init complete (cylinder=%d equirect=%d srgbWriteControl=%d)",
         ctx->cylinderSupported, ctx->equirectSupported, ctx->srgbWriteControl);
    return (jlong)(intptr_t)ctx;
}

JNIEXPORT void JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeSetCaptureDir(JNIEnv* env, jobject thiz,
                                                                jlong handle, jstring dir) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx == NULL || dir == NULL) {
        return;
    }
    const char* chars = (*env)->GetStringUTFChars(env, dir, NULL);
    if (chars != NULL) {
        strncpy(ctx->captureDir, chars, sizeof(ctx->captureDir) - 1);
        (*env)->ReleaseStringUTFChars(env, dir, chars);
        LOGI("capture dir %s, setprop %s to dump a frame", ctx->captureDir, CAPTURE_PROP);
    }
}

JNIEXPORT jint JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeGetTexId(JNIEnv* env, jobject thiz, jlong handle) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    return (jint)ctx->oesTexture;
}

// One OES texture per PMode screen (see PModeScreenServiceBase for why each
// screen needs its own independent texture rather than one shared frame).
JNIEXPORT jint JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeGetProductivityTexId(JNIEnv* env, jobject thiz,
                                                                       jlong handle, jint screenIndex) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx == NULL || screenIndex < 0 || screenIndex >= PRODUCTIVITY_SCREEN_COUNT) {
        return 0;
    }
    return (jint)ctx->productivityOesTexture[screenIndex];
}

// Called once per screen per frame, only when that screen's SurfaceTexture
// actually has a new frame (Java already called updateTexImage() and
// getTransformMatrix() before this - SurfaceTexture itself has no native
// equivalent, so the transform has to cross the JNI boundary as a plain
// float array, same as the single-screen texMatrix already does).
JNIEXPORT void JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeUpdateProductivityTexture(JNIEnv* env, jobject thiz,
                                                                            jlong handle, jint screenIndex,
                                                                            jfloatArray texMatrix) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx == NULL || screenIndex < 0 || screenIndex >= PRODUCTIVITY_SCREEN_COUNT) {
        return;
    }
    (*env)->GetFloatArrayRegion(env, texMatrix, 0, 16, ctx->productivityTexMatrix[screenIndex]);
    ctx->productivityHasFrame[screenIndex] = 1;
}

JNIEXPORT jobject JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeGetModelInput(JNIEnv* env, jobject thiz, jlong handle) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx->modelInput == NULL) {
        return NULL;
    }
    return (*env)->NewDirectByteBuffer(env, ctx->modelInput,
                                       (jlong)DEPTH_TEX_SIZE * DEPTH_TEX_SIZE * 3 * sizeof(float));
}

JNIEXPORT jobject JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeGetModelOutput(JNIEnv* env, jobject thiz, jlong handle) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx->modelOutput == NULL) {
        return NULL;
    }
    return (*env)->NewDirectByteBuffer(env, ctx->modelOutput,
                                       (jlong)DEPTH_TEX_SIZE * DEPTH_TEX_SIZE * sizeof(float));
}

// Draws the current frame into the downscale target and reads it back into
// the model input buffer. Rows are flipped on the way: GL hands back the
// bottom row first and the model wants the image the right way up, since
// monocular depth leans heavily on which way is down.
JNIEXPORT jlong JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeCaptureDepthInput(JNIEnv* env, jobject thiz,
                                                                    jlong handle,
                                                                    jfloatArray texMatrixArr) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    const int n = DEPTH_TEX_SIZE;
    long startNs = nowNs();

    float texMatrix[16];
    (*env)->GetFloatArrayRegion(env, texMatrixArr, 0, 16, texMatrix);

    glBindFramebuffer(GL_FRAMEBUFFER, ctx->downscaleFbo);
    glViewport(0, 0, n, n);
    if (ctx->srgbWriteControl) {
        glDisable(GL_FRAMEBUFFER_SRGB_EXT);
    }

    glUseProgram(ctx->downscaleProgram);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, ctx->oesTexture);
    glUniformMatrix4fv(ctx->downscaleTexMatrixUniform, 1, GL_FALSE, texMatrix);

    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, VERTEX_DATA);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, VERTEX_DATA + 2);
    glEnableVertexAttribArray(1);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    glReadPixels(0, 0, n, n, GL_RGBA, GL_UNSIGNED_BYTE, ctx->readbackBuf);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    for (int y = 0; y < n; y++) {
        const unsigned char* src = ctx->readbackBuf + (size_t)(n - 1 - y) * n * 4;
        float* dst = ctx->modelInput + (size_t)y * n * 3;
        for (int x = 0; x < n; x++) {
            dst[x * 3 + 0] = src[x * 4 + 0] * (1.0f / 255.0f);
            dst[x * 3 + 1] = src[x * 4 + 1] * (1.0f / 255.0f);
            dst[x * 3 + 2] = src[x * 4 + 2] * (1.0f / 255.0f);
        }
    }

    return nowNs() - startNs;
}

// Binds the depth thread's context. Called once from that thread before it
// touches GL or creates the delegate.
JNIEXPORT jboolean JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeBindDepthContext(JNIEnv* env, jobject thiz, jlong handle) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (!eglMakeCurrent(ctx->eglDisplay, ctx->depthPbuffer, ctx->depthPbuffer, ctx->depthContext)) {
        LOGE("depth thread eglMakeCurrent failed: %d", eglGetError());
        return JNI_FALSE;
    }
    return JNI_TRUE;
}

JNIEXPORT void JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeUnbindDepthContext(JNIEnv* env, jobject thiz, jlong handle) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    eglMakeCurrent(ctx->eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (ctx->depthPbuffer != EGL_NO_SURFACE) {
        eglDestroySurface(ctx->eglDisplay, ctx->depthPbuffer);
        ctx->depthPbuffer = EGL_NO_SURFACE;
    }
    if (ctx->depthContext != EGL_NO_CONTEXT) {
        eglDestroyContext(ctx->eglDisplay, ctx->depthContext);
        ctx->depthContext = EGL_NO_CONTEXT;
    }
    eglReleaseThread();
}

// 2nd and 98th percentile of the model output, via a histogram. Using the
// raw min and max lets one stray pixel own the whole mapping: on a measured
// frame the 2..98 span was 638 of an 805 wide min/max range, so a fifth of
// the output range was being spent on a handful of pixels.
static void robustRange(const float* v, int count, float* outLo, float* outHi) {
    float lo = v[0], hi = v[0];
    for (int i = 1; i < count; i++) {
        if (v[i] < lo) lo = v[i];
        if (v[i] > hi) hi = v[i];
    }
    if (hi <= lo) {
        *outLo = lo;
        *outHi = lo + 1.0f;
        return;
    }

    int hist[DEPTH_HIST_BINS];
    memset(hist, 0, sizeof(hist));
    float scale = DEPTH_HIST_BINS / (hi - lo);
    for (int i = 0; i < count; i++) {
        int b = (int)((v[i] - lo) * scale);
        if (b < 0) b = 0;
        if (b >= DEPTH_HIST_BINS) b = DEPTH_HIST_BINS - 1;
        hist[b]++;
    }

    int loTarget = (int)(count * 0.02f);
    int hiTarget = (int)(count * 0.98f);
    int acc = 0, loBin = 0, hiBin = DEPTH_HIST_BINS - 1;
    for (int b = 0; b < DEPTH_HIST_BINS; b++) {
        acc += hist[b];
        if (acc >= loTarget) {
            loBin = b;
            break;
        }
    }
    acc = 0;
    for (int b = 0; b < DEPTH_HIST_BINS; b++) {
        acc += hist[b];
        if (acc >= hiTarget) {
            hiBin = b;
            break;
        }
    }

    *outLo = lo + loBin / scale;
    *outHi = lo + (hiBin + 1) / scale;
    if (*outHi <= *outLo) {
        *outHi = *outLo + 1e-6f;
    }
}

static void boxBlurH(const float* src, float* dst, int n, int r) {
    float inv = 1.0f / (float)(2 * r + 1);
    for (int y = 0; y < n; y++) {
        const float* s = src + (size_t)y * n;
        float* d = dst + (size_t)y * n;
        float sum = 0.0f;
        for (int i = -r; i <= r; i++) {
            int x = i < 0 ? 0 : (i >= n ? n - 1 : i);
            sum += s[x];
        }
        for (int x = 0; x < n; x++) {
            d[x] = sum * inv;
            int add = x + r + 1;
            int sub = x - r;
            sum += s[add >= n ? n - 1 : add] - s[sub < 0 ? 0 : sub];
        }
    }
}

// Column sums carried a row at a time. The obvious version, one column at a
// time, strides a whole row between reads and misses cache on every access,
// which cost 15 ms here rather than 1.
static void boxBlurV(const float* src, float* dst, int n, int r, float* colSums) {
    float inv = 1.0f / (float)(2 * r + 1);
    memset(colSums, 0, (size_t)n * sizeof(float));
    for (int i = -r; i <= r; i++) {
        int y = i < 0 ? 0 : (i >= n ? n - 1 : i);
        const float* s = src + (size_t)y * n;
        for (int x = 0; x < n; x++) {
            colSums[x] += s[x];
        }
    }
    for (int y = 0; y < n; y++) {
        float* d = dst + (size_t)y * n;
        for (int x = 0; x < n; x++) {
            d[x] = colSums[x] * inv;
        }
        int add = y + r + 1;
        int sub = y - r;
        const float* a = src + (size_t)(add >= n ? n - 1 : add) * n;
        const float* b = src + (size_t)(sub < 0 ? 0 : sub) * n;
        for (int x = 0; x < n; x++) {
            colSums[x] += a[x] - b[x];
        }
    }
}

// Three box passes is close enough to a gaussian
static void lowPass(const float* src, float* dst, float* scratch, float* colSums, int n, int r) {
    boxBlurH(src, scratch, n, r);
    boxBlurV(scratch, dst, n, r, colSums);
    boxBlurH(dst, scratch, n, r);
    boxBlurV(scratch, dst, n, r, colSums);
    boxBlurH(dst, scratch, n, r);
    boxBlurV(scratch, dst, n, r, colSums);
}

// Normalizes the model output to 0..1 and uploads it as the depth map the
// warp samples. MiDaS emits relative inverse depth on an arbitrary scale, so
// the range has to be found per frame. Rows flip back here.
//
// Two separate temporal filters. The range is smoothed so the mapping does
// not jump when the scene changes, and the map itself is smoothed per texel
// so raw model flicker does not reach the eyes. The guide colour rides along
// in RGB so the upsampling pass gets the exact frame the depth came from.
//
// Runs on the depth thread, writing whichever texture the frame loop is not
// sampling, then publishing it. The finish is what makes the upload visible
// to the other context, and costs nothing here since this thread has no
// deadline.
JNIEXPORT jlong JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeUploadDepth(JNIEnv* env, jobject thiz, jlong handle) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    const int n = DEPTH_TEX_SIZE;
    long startNs = nowNs();

    float lo, hi;
    robustRange(ctx->modelOutput, n * n, &lo, &hi);
    if (!ctx->rangeValid) {
        ctx->smoothLo = lo;
        ctx->smoothHi = hi;
        ctx->rangeValid = 1;
    }
    else {
        ctx->smoothLo += ctx->rangeAlpha * (lo - ctx->smoothLo);
        ctx->smoothHi += ctx->rangeAlpha * (hi - ctx->smoothHi);
    }
    float scale = 1.0f / (ctx->smoothHi - ctx->smoothLo);
    float alpha = ctx->depthAlpha;
    int seed = !ctx->depthEmaValid;

    for (int y = 0; y < n; y++) {
        const float* src = ctx->modelOutput + (size_t)(n - 1 - y) * n;
        float* ema = ctx->depthEma + (size_t)y * n;
        for (int x = 0; x < n; x++) {
            float v = (src[x] - ctx->smoothLo) * scale;
            if (v < 0.0f) v = 0.0f;
            if (v > 1.0f) v = 1.0f;
            ema[x] = seed ? v : ema[x] + alpha * (v - ema[x]);
        }
    }
    ctx->depthEmaValid = 1;

    float kg = ctx->depthGlobal;
    float kl = ctx->depthLocal;
    float conv = ctx->convergence;

    // The low pass is only needed to split the map into overall shape and
    // local detail, so skip it when the remap is doing nothing. It costs
    // about 10 ms on this thread, which is latency the depth map cannot
    // afford for an effect measured to be invisible.
    int remapping = kg < 0.995f || kg > 1.005f || kl < 0.995f || kl > 1.005f;
    if (remapping) {
        lowPass(ctx->depthEma, ctx->depthLow, ctx->depthScratch, ctx->depthColSums, n,
                DEPTH_LOWPASS_RADIUS);
    }

    for (int y = 0; y < n; y++) {
        const float* guide = ctx->modelInput + (size_t)(n - 1 - y) * n * 3;
        const float* ema = ctx->depthEma + (size_t)y * n;
        const float* low = ctx->depthLow + (size_t)y * n;
        unsigned char* dst = ctx->depthUploadBuf + (size_t)y * n * 4;
        for (int x = 0; x < n; x++) {
            float v = remapping ? conv + kg * (low[x] - conv) + kl * (ema[x] - low[x])
                                : ema[x];
            if (v < 0.0f) v = 0.0f;
            if (v > 1.0f) v = 1.0f;

            dst[x * 4 + 0] = (unsigned char)(guide[x * 3 + 0] * 255.0f + 0.5f);
            dst[x * 4 + 1] = (unsigned char)(guide[x * 3 + 1] * 255.0f + 0.5f);
            dst[x * 4 + 2] = (unsigned char)(guide[x * 3 + 2] * 255.0f + 0.5f);
            dst[x * 4 + 3] = (unsigned char)(v * 255.0f + 0.5f);
        }
    }

    int writeIndex = 1 - ctx->depthReadIndex;
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, ctx->depthTextures[writeIndex]);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, n, n, GL_RGBA, GL_UNSIGNED_BYTE, ctx->depthUploadBuf);
    glFinish();
    ctx->depthReadIndex = writeIndex;

    return nowNs() - startNs;
}

JNIEXPORT jint JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeWaitBeginFrame(JNIEnv* env, jobject thiz, jlong handle) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;

    pollEvents(ctx);

    if (ctx->exitRequested) {
        return FRAME_EXIT;
    }

    if (!ctx->sessionRunning) {
        usleep(10000);
        return FRAME_IDLE;
    }

    XrFrameState frameState = { XR_TYPE_FRAME_STATE };
    if (!checkXr(xrWaitFrame(ctx->session, NULL, &frameState), "xrWaitFrame")) {
        return FRAME_EXIT;
    }
    if (!checkXr(xrBeginFrame(ctx->session, NULL), "xrBeginFrame")) {
        return FRAME_EXIT;
    }

    ctx->predictedDisplayTime = frameState.predictedDisplayTime;
    ctx->shouldRender = frameState.shouldRender;
    return FRAME_RENDER;
}

// Where the game/desktop audio should feel like it's coming from: pan toward
// whichever side the screen is on relative to where the head is actually
// facing, quieter the further back the user leans. Shared by both modes -
// Gaming passes its one movable screenPose, Productivity passes the fixed
// centre screen, since there's only one audio stream for the whole desktop
// (Sunshine mixes it before it ever reaches us) so the centre screen is the
// honest choice of anchor rather than attempting per-window audio.
static void computeSpatialAudio(XrCtx* ctx, XrPosef screenPose, float referenceDistance,
                                float* out) {
    XrSpaceLocation headLoc = { XR_TYPE_SPACE_LOCATION };
    const XrSpaceLocationFlags needed = XR_SPACE_LOCATION_POSITION_VALID_BIT
            | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    if (!(XR_SUCCEEDED(xrLocateSpace(ctx->viewSpace, ctx->localSpace,
                                     ctx->predictedDisplayTime, &headLoc))
            && (headLoc.locationFlags & needed) == needed)) {
        return;
    }

    Vec3 anchorPos = { screenPose.position.x, screenPose.position.y, screenPose.position.z };
    Vec3 headPos = { headLoc.pose.position.x, headLoc.pose.position.y, headLoc.pose.position.z };
    Vec3 toAnchor = vecSub(anchorPos, headPos);

    // Into head-local space: local.x is lateral (right positive), matching
    // what a stereo balance control needs directly.
    Vec3 local = quatRotate(quatConj(headLoc.pose.orientation), toAnchor);
    float distance = sqrtf(local.x * local.x + local.y * local.y + local.z * local.z);
    if (distance < 0.05f) {
        return;
    }

    float pan = local.x / distance;
    if (pan < -1.0f) pan = -1.0f;
    if (pan > 1.0f) pan = 1.0f;

    // 1.0 at the reference distance (each mode's current screen distance),
    // falling off, not muting, further back.
    float gain = referenceDistance / distance;
    if (gain > 1.0f) gain = 1.0f;
    if (gain < 0.2f) gain = 0.2f;

    out[IN_AUDIO_PAN] = pan;
    out[IN_AUDIO_GAIN] = gain;
}

// Phase 1 was just the exit button. Phase 2 adds pointer/left-click on the
// 3 screens themselves - deliberately not the single-screen hover-state
// machine below (no gaze, no grab/resize, no filtering): one ray, whichever
// of the 3 screens (or the exit button) it lands on first, one mouse button.
// That machine can come later if this turns out not to be enough.
// Exit + brightness icons, and the brightness slider once opened. Shared by
// both modes - only topBarPose()'s anchor differs (see there). Runs once a
// frame given both hands' current aim pose/validity. A drag in progress
// needs the whole array rather than just whichever hand is hovering right
// now, so a release is still caught even if that hand's pose lookup fails
// this frame - same reasoning as applyGrab().
// Applies a slider's dragged position to whichever value it controls. Also
// used to seed the value the instant a drag starts (see the two call sites
// below), not just on later frames of the drag.
static void applySliderValue(XrCtx* ctx, int item, float value) {
    if (item == TOPBAR_BRIGHTNESS_INDEX) {
        ctx->passthroughLevel = value;
    }
    else {
        ctx->curveAmount = value;
        // Independent of updatePlacement()'s own seed, on purpose - that
        // path also resets screen position, which a curve adjustment must
        // not do. See the field comment on ctx->curveAmount.
        ctx->screenRadius = ctx->lastDistance * (CURVE_RADIUS_MIN_MULT
                + (CURVE_RADIUS_MAX_MULT - CURVE_RADIUS_MIN_MULT) * (1.0f - value));
    }
}

static void markSliderDirty(XrCtx* ctx, int item) {
    if (item == TOPBAR_BRIGHTNESS_INDEX) {
        ctx->passthroughLevelDirty = 1;
    }
    else {
        ctx->curveAmountDirty = 1;
    }
}

static int updateTopBar(XrCtx* ctx, XrPosef* aims, const int* valid, float* out) {
    if (ctx->grabMode == GRAB_SLIDER) {
        int h = ctx->grabHand;
        if (!valid[h] || !ctx->triggerDown[h]) {
            ctx->grabMode = GRAB_NONE;
            markSliderDirty(ctx, ctx->grabSliderTarget);
            return 0;
        }
        float u, v;
        if (screenProject(aims[h], topBarSliderPose(ctx, ctx->grabSliderTarget),
                          SLIDER_TRACK_WIDTH_M, SLIDER_TRACK_HEIGHT_M, 0.0f, 0, &u, &v)) {
            applySliderValue(ctx, ctx->grabSliderTarget, u < 0.0f ? 0.0f : (u > 1.0f ? 1.0f : u));
        }
        return 1;
    }

    for (int h = 0; h < HAND_COUNT; h++) {
        if (!valid[h]) {
            continue;
        }
        float u, v;
        if (screenProject(aims[h], topBarItemPose(ctx, TOPBAR_EXIT_INDEX, TOPBAR_ITEM_COUNT),
                          TOPBAR_ITEM_SIZE_M, TOPBAR_ITEM_SIZE_M, 0.0f, 0, &u, &v)
                && u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f) {
            // Don't raise IN_EXIT_PRESSED yet - fade to black first (see
            // FADE_OUT in nativeEndFrame/nativeUpdateInput) so Java's
            // finish() never lands on a frame the user can still see.
            // Guarded so a second tap mid-fade can't reset the clock.
            if (ctx->triggerEdge[h] && ctx->fadeState != FADE_OUT) {
                ctx->fadeState = FADE_OUT;
                ctx->fadeFromAlpha = ctx->fadeAlpha;
                ctx->fadeStartNs = nowNs();
                fireHaptic(ctx, h);
            }
            return 1;
        }

        if (screenProject(aims[h], topBarItemPose(ctx, TOPBAR_DEPTH_INDEX, TOPBAR_ITEM_COUNT),
                          TOPBAR_ITEM_SIZE_M, TOPBAR_ITEM_SIZE_M, 0.0f, 0, &u, &v)
                && u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f) {
            if (ctx->triggerEdge[h]) {
                out[IN_DEPTH_TOGGLE] = 1.0f;
                fireHaptic(ctx, h);
            }
            return 1;
        }

        if (screenProject(aims[h], topBarItemPose(ctx, TOPBAR_ULTRAWIDE_INDEX, TOPBAR_ITEM_COUNT),
                          TOPBAR_ITEM_SIZE_M, TOPBAR_ITEM_SIZE_M, 0.0f, 0, &u, &v)
                && u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f) {
            if (ctx->triggerEdge[h]) {
                out[IN_ULTRAWIDE_TOGGLE] = 1.0f;
                fireHaptic(ctx, h);
            }
            return 1;
        }

        if (ctx->openSlider == TOPBAR_BRIGHTNESS_INDEX
                && screenProject(aims[h], topBarGlowTogglePose(ctx),
                                 GLOW_TOGGLE_SIZE_M, GLOW_TOGGLE_SIZE_M, 0.0f, 0, &u, &v)
                && u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f) {
            if (ctx->triggerEdge[h]) {
                out[IN_GLOW_TOGGLE] = 1.0f;
                fireHaptic(ctx, h);
            }
            return 1;
        }

        int sliderItems[2] = { TOPBAR_BRIGHTNESS_INDEX, TOPBAR_CURVE_INDEX };
        int hitItem = -1;
        for (int i = 0; i < 2; i++) {
            if (screenProject(aims[h], topBarItemPose(ctx, sliderItems[i], TOPBAR_ITEM_COUNT),
                              TOPBAR_ITEM_SIZE_M, TOPBAR_ITEM_SIZE_M, 0.0f, 0, &u, &v)
                    && u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f) {
                hitItem = sliderItems[i];
                break;
            }
        }
        if (hitItem >= 0) {
            if (ctx->triggerEdge[h]) {
                ctx->openSlider = (ctx->openSlider == hitItem) ? TOPBAR_NO_SLIDER : hitItem;
                fireHaptic(ctx, h);
            }
            return 1;
        }

        if (ctx->openSlider != TOPBAR_NO_SLIDER && ctx->triggerEdge[h]
                && screenProject(aims[h], topBarSliderPose(ctx, ctx->openSlider),
                                 SLIDER_TRACK_WIDTH_M, SLIDER_TRACK_HEIGHT_M, 0.0f, 0, &u, &v)
                && u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f) {
            ctx->grabMode = GRAB_SLIDER;
            ctx->grabHand = h;
            ctx->grabSliderTarget = ctx->openSlider;
            applySliderValue(ctx, ctx->openSlider, u);
            fireHaptic(ctx, h);
            return 1;
        }
    }
    return 0;
}

static void updateProductivityInput(XrCtx* ctx, jboolean pointerEnabled, float* out) {
    out[IN_PMODE_SCREEN] = -1.0f;

    if (!ctx->inputReady || !pointerEnabled || ctx->sessionState != XR_SESSION_STATE_FOCUSED) {
        ctx->pmodeButtonsDown = 0;
        return;
    }

    XrActiveActionSet active;
    active.actionSet = ctx->actionSet;
    active.subactionPath = XR_NULL_PATH;
    XrActionsSyncInfo sync = { XR_TYPE_ACTIONS_SYNC_INFO };
    sync.countActiveActionSets = 1;
    sync.activeActionSets = &active;
    if (XR_FAILED(xrSyncActions(ctx->session, &sync))) {
        ctx->pmodeButtonsDown = 0;
        return;
    }

    // Same quad height the render path computes for these screens
    // (renderVideoFrame's productivity branch) - hit-testing has to agree
    // with what's actually drawn, or the ray and the picture disagree about
    // where the screen's edges are.
    float colWidth = (float)ctx->videoWidth / PRODUCTIVITY_SCREEN_COUNT;
    float colHeight = (float)ctx->videoHeight;
    float quadHeight = PRODUCTIVITY_SCREEN_WIDTH_M * (colHeight / colWidth);

    int screenHit = -1;
    int hitHand = -1;
    float hitU = 0.0f, hitV = 0.0f;
    XrPosef aims[HAND_COUNT];
    int aimValid[HAND_COUNT] = { 0, 0 };

    for (int h = 0; h < HAND_COUNT; h++) {
        int wasDown = ctx->triggerDown[h];
        float value = actionFloat(ctx, ctx->triggerAction, h);
        ctx->triggerDown[h] = value > (wasDown ? PRESS_OFF : PRESS_ON);
        ctx->triggerEdge[h] = ctx->triggerDown[h] && !wasDown;

        if (ctx->aimSpaces[h] == XR_NULL_HANDLE) {
            continue;
        }
        XrSpaceLocation loc = { XR_TYPE_SPACE_LOCATION };
        const XrSpaceLocationFlags needed = XR_SPACE_LOCATION_POSITION_VALID_BIT
                | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
        if (!(XR_SUCCEEDED(xrLocateSpace(ctx->aimSpaces[h], ctx->localSpace,
                                         ctx->predictedDisplayTime, &loc))
                && (loc.locationFlags & needed) == needed)) {
            continue;
        }
        aims[h] = loc.pose;
        aimValid[h] = 1;

        if (screenHit < 0) {
            for (int i = 0; i < PRODUCTIVITY_SCREEN_COUNT; i++) {
                float su, sv;
                if (screenProject(loc.pose, productivityScreenPose(i), PRODUCTIVITY_SCREEN_WIDTH_M,
                                  quadHeight, 0.0f, 0, &su, &sv)
                        && su >= 0.0f && su <= 1.0f && sv >= 0.0f && sv <= 1.0f) {
                    screenHit = i;
                    hitHand = h;
                    hitU = su;
                    hitV = sv;
                    break;
                }
            }
        }
    }

    updateTopBar(ctx, aims, aimValid, out);

    int mask = (screenHit >= 0 && ctx->triggerDown[hitHand]) ? VR_BUTTON_LEFT : 0;
    int prevMask = ctx->pmodeButtonsDown;
    ctx->pmodeButtonsDown = mask;

    if (screenHit >= 0) {
        out[IN_PMODE_HIT] = 1.0f;
        out[IN_PMODE_SCREEN] = (float)screenHit;
        out[IN_PMODE_U] = hitU;
        out[IN_PMODE_V] = hitV;
        out[IN_PMODE_BUTTONS] = (float)mask;
        if ((~prevMask & mask) != 0) {
            fireHaptic(ctx, hitHand);
        }
    }

    writeInputPose(ctx, out);
}

// Reads the controllers and works out where they are pointing on the screen.
// Java turns the result into host mouse events, so nothing here knows about
// the connection.
JNIEXPORT void JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeUpdateInput(JNIEnv* env, jobject thiz,
                                                              jlong handle, jfloat distance,
                                                              jfloat quadWidth,
                                                              jboolean headLocked,
                                                              jboolean pointerEnabled,
                                                              jboolean gazeEnabled,
                                                              jfloatArray outArr) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    float out[IN_SLOTS];
    memset(out, 0, sizeof(out));
    if (ctx != NULL) {
        ctx->gazeEnabled = gazeEnabled;
    }
    // The fade-to-black before exit finished rendering last frame - only now
    // is it safe to actually tell Java to finish(), since this is the first
    // frame Java will act on that the user can no longer see anything of.
    // Checked ahead of both modes' own input handling and any early-return
    // below, since exit must go through regardless of focus/placement state.
    if (ctx != NULL && ctx->fadeOutComplete) {
        ctx->fadeOutComplete = 0;
        out[IN_EXIT_PRESSED] = 1.0f;
        (*env)->SetFloatArrayRegion(env, outArr, 0, IN_SLOTS, out);
        return;
    }
    // Neutral balance/gain by default - Gaming mode never reaches the code
    // that would change these, so its audio is always exactly this, i.e.
    // untouched.
    out[IN_AUDIO_PAN] = 0.0f;
    out[IN_AUDIO_GAIN] = 1.0f;

    if (ctx != NULL && ctx->productivityMode) {
        computeSpatialAudio(ctx, productivityScreenPose((PRODUCTIVITY_SCREEN_COUNT - 1) / 2),
                           PRODUCTIVITY_DISTANCE_M, out);
        updateProductivityInput(ctx, pointerEnabled, out);
        (*env)->SetFloatArrayRegion(env, outArr, 0, IN_SLOTS, out);
        return;
    }

    // Anything held has to come back up when pointing stops, or the host is
    // left with a stuck button
    if (ctx == NULL || !ctx->inputReady || !pointerEnabled || !ctx->placementValid
            || ctx->sessionState != XR_SESSION_STATE_FOCUSED) {
        if (ctx != NULL) {
            ctx->buttonsDown = 0;
            ctx->beamVisible = 0;
            if (ctx->grabMode != 0) {
                // Dropping focus mid grab has to count as letting go, or the
                // anchor is stale when focus comes back and the screen jumps
                ctx->grabMode = 0;
                ctx->poseDirty = 1;
            }
        }
        (*env)->SetFloatArrayRegion(env, outArr, 0, IN_SLOTS, out);
        return;
    }

    XrActiveActionSet active;
    active.actionSet = ctx->actionSet;
    active.subactionPath = XR_NULL_PATH;

    XrActionsSyncInfo sync = { XR_TYPE_ACTIONS_SYNC_INFO };
    sync.countActiveActionSets = 1;
    sync.activeActionSets = &active;
    if (XR_FAILED(xrSyncActions(ctx->session, &sync))) {
        ctx->buttonsDown = 0;
        (*env)->SetFloatArrayRegion(env, outArr, 0, IN_SLOTS, out);
        return;
    }

    int toggle = actionBool(ctx, ctx->toggleAction, -1);
    if (toggle && !ctx->togglePrev) {
        ctx->pointerOn = !ctx->pointerOn;
        LOGI("pointer %s", ctx->pointerOn ? "on" : "off");
    }
    ctx->togglePrev = toggle;
    out[IN_POINTER] = ctx->pointerOn ? 1.0f : 0.0f;

    XrSpace space = headLocked ? ctx->viewSpace : ctx->localSpace;
    float height = ctx->screenWidth * (float)ctx->videoHeight / (float)ctx->videoWidth;
    int curved = ctx->curveAmount > 0.01f && ctx->cylinderSupported;
    float radius = ctx->screenRadius;
    XrPosef screenPose = ctx->screenPose;

    long now = nowNs();
    float dt = ctx->lastInputNs != 0 ? (now - ctx->lastInputNs) / 1e9f : 0.0f;
    ctx->lastInputNs = now;
    if (dt > 0.1f) {
        dt = 0.1f;
    }

    XrSpaceLocation headLoc = { XR_TYPE_SPACE_LOCATION };
    int headValid = XR_SUCCEEDED(xrLocateSpace(ctx->viewSpace, space,
                                               ctx->predictedDisplayTime, &headLoc))
            && (headLoc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0;
    if (headValid) {
        ctx->headPos = headLoc.pose.position;
    }

    float hitU[SRC_COUNT], hitV[SRC_COUNT];
    int hovers[SRC_COUNT] = { HOVER_NONE, HOVER_NONE, HOVER_NONE };
    int corners[SRC_COUNT] = { 0, 0, 0 };
    int aimValid[SRC_COUNT] = { 0, 0, 0 };
    XrPosef aimPoses[SRC_COUNT];
    int moved = 0;
    for (int h = 0; h < SRC_COUNT; h++) {
        if (h < HAND_COUNT) {
            int wasDown = ctx->triggerDown[h];
            float value = actionFloat(ctx, ctx->triggerAction, h);
            // Either a bound trigger or a measured pinch will do. Runtimes
            // that offer neither leave this at rest, which is what a headset
            // with nothing in its hands should report.
            ctx->triggerDown[h] = value > (wasDown ? PRESS_OFF : PRESS_ON)
                    || jointPinching(ctx, h, space, &headLoc.pose, headValid);
            ctx->triggerEdge[h] = ctx->triggerDown[h] && !wasDown;
        }
        else if (!ctx->eyeGaze || !ctx->gazeEnabled
                 || ctx->aimSpaces[SRC_GAZE] == XR_NULL_HANDLE) {
            continue;
        }

        XrSpaceLocation loc = { XR_TYPE_SPACE_LOCATION };
        const XrSpaceLocationFlags needed = XR_SPACE_LOCATION_POSITION_VALID_BIT
                | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
        int located = XR_SUCCEEDED(xrLocateSpace(ctx->aimSpaces[h], space,
                                                 ctx->predictedDisplayTime, &loc))
                && (loc.locationFlags & needed) == needed;
        if (!located) {
            // No controller and no pointer pose from the runtime, so the ray
            // built out of the joints stands in. This is what makes hand
            // pointing work on runtimes that refuse the hand profile.
            if (h < HAND_COUNT && ctx->handRayValid[h]) {
                loc.pose = ctx->handRay[h];
            }
            else {
                continue;
            }
        }
        aimPoses[h] = loc.pose;
        aimValid[h] = 1;
        if (screenProject(loc.pose, screenPose, ctx->screenWidth, height, radius, curved,
                          &hitU[h], &hitV[h])) {
            hovers[h] = hoverTest(hitU[h], hitV[h], ctx->screenWidth, height, &corners[h]);
        }

        if (ctx->poseSeen[h] && dt > 0.0f) {
            Vec3 now3 = { loc.pose.position.x, loc.pose.position.y, loc.pose.position.z };
            Vec3 was3 = { ctx->lastAim[h].position.x, ctx->lastAim[h].position.y,
                          ctx->lastAim[h].position.z };
            Vec3 step = vecSub(now3, was3);
            float speed = sqrtf(step.x * step.x + step.y * step.y + step.z * step.z) / dt;

            // Angle between the two orientations, from the dot product of the
            // quaternions, which is half the rotation
            XrQuaternionf a = loc.pose.orientation, b = ctx->lastAim[h].orientation;
            float dot = fabsf(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
            if (dot > 1.0f) {
                dot = 1.0f;
            }
            float turn = 2.0f * acosf(dot) / dt;

            // Hands and eyes are never still, so their motion says nothing
            // about intent and the gate would just hold the pointer on forever
            if (!ctx->usingHands[h] && h != SRC_GAZE
                    && (speed > POINTER_MOVE_SPEED || turn > POINTER_TURN_SPEED)) {
                moved = 1;
            }
        }
        ctx->lastAim[h] = loc.pose;
        ctx->poseSeen[h] = 1;
    }

    updateTopBar(ctx, aimPoses, aimValid, out);

    // Gaze has no button of its own, so a pinch from either hand clicks
    // wherever the eyes have landed
    if (aimValid[SRC_GAZE]) {
        ctx->triggerDown[SRC_GAZE] = ctx->triggerDown[HAND_LEFT] || ctx->triggerDown[HAND_RIGHT];
        ctx->triggerEdge[SRC_GAZE] = ctx->triggerEdge[HAND_LEFT] || ctx->triggerEdge[HAND_RIGHT];
        ctx->usingHands[SRC_GAZE] = 1;
    }
    else {
        ctx->triggerDown[SRC_GAZE] = 0;
        ctx->triggerEdge[SRC_GAZE] = 0;
        ctx->usingHands[SRC_GAZE] = 0;
    }

    // A pinch is what a hand has instead of deliberate movement: it turns the
    // pointer on, and keeps it on for as long as pinches keep arriving. The
    // one that does the waking is swallowed rather than passed on as a click,
    // since the user was reaching for the pointer and not for the screen.
    int pinching = 0;
    for (int h = 0; h < SRC_COUNT; h++) {
        if (!ctx->usingHands[h]) {
            ctx->pinchSwallowed[h] = 0;
            continue;
        }
        if (ctx->triggerDown[h]) {
            pinching = 1;
            if (!ctx->pointerAwake) {
                ctx->pointerAwake = 1;
                ctx->pinchSwallowed[h] = 1;
            }
        }
        else {
            ctx->pinchSwallowed[h] = 0;
        }
        if (ctx->pinchSwallowed[h]) {
            ctx->triggerDown[h] = 0;
            ctx->triggerEdge[h] = 0;
        }
    }

    // Deliberate movement wakes the pointer, a controller put down retires it
    if (pinching) {
        // Only the pinch clock matters while hands are in charge
        ctx->stillFor = 0.0f;
        ctx->movingFor = 0.0f;
    }
    else if (moved) {
        ctx->movingFor += dt;
        ctx->stillFor = 0.0f;
        if (ctx->movingFor >= ctx->pointerWake) {
            ctx->pointerAwake = 1;
        }
    }
    else {
        ctx->stillFor += dt;
        ctx->movingFor = 0.0f;
        if (ctx->stillFor >= ctx->pointerSleep) {
            ctx->pointerAwake = 0;
        }
    }

    // The hand holding the trigger wins, so a drag is never stolen by the other
    // one drifting across the screen. Right hand otherwise.
    static const int order[SRC_COUNT] = { HAND_RIGHT, HAND_LEFT, SRC_GAZE };
    int hand = -1;
    for (int i = 0; i < SRC_COUNT; i++) {
        int h = order[i];
        if (hovers[h] == HOVER_SCREEN && ctx->triggerDown[h]) {
            hand = h;
            break;
        }
    }
    // A hand on something beats one merely near it, so a controller resting in
    // the margin never takes the pointer off the one being aimed
    for (int pass = 0; pass < 2 && hand < 0; pass++) {
        for (int i = 0; i < SRC_COUNT && hand < 0; i++) {
            int h = order[i];
            if (hovers[h] == HOVER_NONE || (pass == 0 && hovers[h] == HOVER_HALO)) {
                continue;
            }
            hand = h;
        }
    }

    if (!ctx->pointerAwake && ctx->grabMode == GRAB_NONE) {
        hand = -1;
        for (int h = 0; h < SRC_COUNT; h++) {
            hovers[h] = HOVER_NONE;
        }
    }

    int hover = hand >= 0 ? hovers[hand] : HOVER_NONE;
    if (hover == HOVER_CORNER) {
        ctx->hoverCorner = corners[hand];
    }

    // One line that says whether gaze is tracking, whether it is the thing
    // doing the pointing, and whether a pinch is reaching us at all. Logged
    // only when it changes, so it costs nothing while it sits still.
    int snapshot = (aimValid[SRC_GAZE] ? 1 : 0) | (hand == SRC_GAZE ? 2 : 0)
            | ((ctx->triggerDown[HAND_LEFT] || ctx->triggerDown[HAND_RIGHT]) ? 4 : 0)
            | (ctx->pointerAwake ? 8 : 0);
    if (snapshot != ctx->lastSnapshot) {
        ctx->lastSnapshot = snapshot;
        LOGI("input: gaze tracked %d, pointing by gaze %d, pinch %d, awake %d",
             (snapshot & 1) != 0, (snapshot & 2) != 0, (snapshot & 4) != 0,
             (snapshot & 8) != 0);
    }

    // Where the handle is clear of the picture, so a trigger press there cannot
    // have been meant as a click
    int offPicture = hand >= 0 && (hitU[hand] < 0.0f || hitU[hand] > 1.0f
                                   || hitV[hand] < 0.0f || hitV[hand] > 1.0f);
    // GRAB_SLIDER's whole lifecycle is self-contained inside updateTopBar()
    // above - applyGrab() doesn't know about it, and its own "is this grab
    // still held" check at the top (via grabByTrigger/grabDown, never set
    // for a slider grab) would read stale state and reset grabMode back to
    // NONE, then immediately hand it to GRAB_RESIZE if the same ray also
    // falls in the screen's corner zone. That's exactly what made dragging
    // a slider also snap the resize handle.
    if (ctx->grabMode != GRAB_SLIDER) {
        applyGrab(ctx, aimPoses, aimValid, hand, hover, ctx->hoverCorner, offPicture,
                  height, curved);
    }
    screenPose = ctx->screenPose;
    height = ctx->screenWidth * (float)ctx->videoHeight / (float)ctx->videoWidth;
    radius = ctx->screenRadius;
    computeSpatialAudio(ctx, screenPose, ctx->lastDistance, out);

    // A handle stays lit while it is being dragged, however far the ray has
    // wandered from it in the meantime
    if (ctx->grabMode == GRAB_MOVE) {
        ctx->hoverKind = HOVER_BAR;
    }
    else if (ctx->grabMode == GRAB_RESIZE) {
        ctx->hoverKind = HOVER_CORNER;
    }
    else {
        ctx->hoverKind = hover;
    }

    ctx->screenOrientation = screenPose.orientation;
    ctx->beamVisible = 0;
    ctx->beamFree = 0;
    // Eyes aim by looking, so a ray out of the face would be nonsense, and a
    // cursor riding on them shakes too much to be anything but a distraction.
    // Gaze draws nothing: the handle lighting up is the feedback.
    ctx->beamGaze = (ctx->grabMode != GRAB_NONE ? ctx->grabHand : hand) == SRC_GAZE;

    if (ctx->grabMode != GRAB_NONE) {
        // Nothing goes to the host mid drag, and the ray ends on the handle
        // being held rather than wherever it is now pointing
        ctx->buttonsDown = 0;
        ctx->scrollCarry = 0.0f;
        ctx->filterU.valid = 0;
        ctx->filterV.valid = 0;

        if (headValid) {
            Vec3 local;
            local.z = 0.0f;
            if (ctx->grabMode == GRAB_MOVE) {
                local.x = 0.0f;
                local.y = -(height * 0.5f + (BAR_GAP_FRAC + BAR_HEIGHT_FRAC * 0.5f)
                            * ctx->screenWidth);
            }
            else {
                local.x = (ctx->grabOppX > 0.0f ? -0.5f : 0.5f) * ctx->screenWidth;
                local.y = (ctx->grabOppY > 0.0f ? -0.5f : 0.5f) * height;
            }
            Vec3 handle = quatRotate(screenPose.orientation, local);
            ctx->beamStart = aimPoses[ctx->grabHand].position;
            ctx->beamEnd.x = screenPose.position.x + handle.x;
            ctx->beamEnd.y = screenPose.position.y + handle.y;
            ctx->beamEnd.z = screenPose.position.z + handle.z;
            ctx->beamVisible = 1;
        }

        writeInputPose(ctx, out);
        (*env)->SetFloatArrayRegion(env, outArr, 0, IN_SLOTS, out);
        return;
    }

    if (!ctx->pointerOn) {
        // The ray still shows on the handles and the grid, so the screen can
        // be tidied and the environment changed with the mouse switched off
        if (hand >= 0 && hover != HOVER_NONE && hover != HOVER_SCREEN) {
            Vec3 end = furniturePoint(ctx, hover, hitU[hand], hitV[hand], screenPose,
                                      height, radius, curved);
            ctx->beamStart = aimPoses[hand].position;
            ctx->beamEnd.x = end.x;
            ctx->beamEnd.y = end.y;
            ctx->beamEnd.z = end.z;
            ctx->beamVisible = headValid;
        }
        ctx->buttonsDown = 0;
        writeInputPose(ctx, out);
        (*env)->SetFloatArrayRegion(env, outArr, 0, IN_SLOTS, out);
        return;
    }

    // The bar sits off the picture, so pointing at it must not drag the host
    // cursor to the edge
    int hit = (hover == HOVER_SCREEN || hover == HOVER_CORNER) && hand != SRC_GAZE;
    if ((hover == HOVER_BAR || hover == HOVER_HALO) && headValid && hand >= 0) {
        Vec3 end = furniturePoint(ctx, hover, hitU[hand], hitV[hand], screenPose,
                                  height, radius, curved);
        ctx->beamStart = aimPoses[hand].position;
        ctx->beamEnd.x = end.x;
        ctx->beamEnd.y = end.y;
        ctx->beamEnd.z = end.z;
        ctx->beamVisible = 1;
    }
    if (hit) {
        // Filtering across a gap or a change of hands would slide the cursor
        // in from wherever it used to be
        if (hand != ctx->lastHand || now - ctx->lastHitNs > POINTER_RESET_NS) {
            ctx->filterU.valid = 0;
            ctx->filterV.valid = 0;
        }
        ctx->lastHand = hand;
        ctx->lastHitNs = now;

        float u = euroFilter(&ctx->filterU, hitU[hand], dt, ctx->pointerMinCutoff, ctx->pointerBeta);
        float v = euroFilter(&ctx->filterV, hitV[hand], dt, ctx->pointerMinCutoff, ctx->pointerBeta);
        out[IN_HIT] = 1.0f;
        out[IN_U] = u;
        out[IN_V] = v;

        // The ray is only drawn when it lands on something, which is what
        // makes a laser readable rather than a light show
        Vec3 endPoint = screenPoint(u, v, screenPose, ctx->screenWidth, height, radius, curved);
        ctx->beamStart = aimPoses[hand].position;
        ctx->beamEnd.x = endPoint.x;
        ctx->beamEnd.y = endPoint.y;
        ctx->beamEnd.z = endPoint.z;
        ctx->beamVisible = headValid;
    }

    int mask = 0;
    if (ctx->triggerDown[HAND_LEFT] || ctx->triggerDown[HAND_RIGHT]) {
        mask |= VR_BUTTON_LEFT;
    }
    if (actionBool(ctx, ctx->rightClickAction, -1)) {
        mask |= VR_BUTTON_RIGHT;
    }
    if (actionBool(ctx, ctx->middleClickAction, -1)) {
        mask |= VR_BUTTON_MIDDLE;
    }
    // A press only counts while aimed at the screen, but a release always
    // does, so walking the pointer off the edge mid drag still lets go
    int prevButtonsDown = ctx->buttonsDown;
    ctx->buttonsDown = (ctx->buttonsDown & mask) | (hit ? mask : 0);
    out[IN_BUTTONS] = (float)ctx->buttonsDown;
    // A tick on whichever hand just clicked - only newly pressed buttons
    // count, so holding one down doesn't buzz every frame
    if ((~prevButtonsDown & ctx->buttonsDown) != 0 && hand >= 0 && hand < HAND_COUNT) {
        fireHaptic(ctx, hand);
    }

    XrVector2f stick = actionVec2(ctx, ctx->scrollAction, -1);
    if (hit && fabsf(stick.y) > SCROLL_DEADZONE) {
        float past = (fabsf(stick.y) - SCROLL_DEADZONE) / (1.0f - SCROLL_DEADZONE);
        ctx->scrollCarry += copysignf(past * SCROLL_CLICKS_PER_SEC * dt, stick.y);
    }
    else {
        ctx->scrollCarry = 0.0f;
    }
    float clicks = truncf(ctx->scrollCarry);
    ctx->scrollCarry -= clicks;
    out[IN_SCROLL] = clicks;

    // Aimed at nothing at all, so the ray runs off into the room rather than
    // blinking out. A laser that comes and goes is harder to aim than one that
    // always shows where the hand is looking, so the only thing that retires it
    // is the controller being put down.
    if (!ctx->beamVisible && ctx->pointerAwake && headValid && !ctx->beamGaze) {
        int free = hand;
        if (free < 0) {
            free = aimValid[HAND_RIGHT] ? HAND_RIGHT : (aimValid[HAND_LEFT] ? HAND_LEFT : -1);
        }
        if (free >= 0) {
            Vec3 forward = { 0.0f, 0.0f, -1.0f };
            Vec3 d = quatRotate(aimPoses[free].orientation, forward);
            ctx->beamStart = aimPoses[free].position;
            ctx->beamEnd.x = ctx->beamStart.x + d.x * FREE_BEAM_M;
            ctx->beamEnd.y = ctx->beamStart.y + d.y * FREE_BEAM_M;
            ctx->beamEnd.z = ctx->beamStart.z + d.z * FREE_BEAM_M;
            ctx->beamVisible = 1;
            // No target, so no cursor. The dot is what says a click would
            // land somewhere.
            ctx->beamFree = 1;
        }
    }

    writeInputPose(ctx, out);
    (*env)->SetFloatArrayRegion(env, outArr, 0, IN_SLOTS, out);
}

// Integer valued tuning property, left alone if unset or unparseable
static void propScaled(const char* name, float* target, float scale, long maxRaw) {
    char value[PROP_VALUE_MAX];
    value[0] = '\0';
    if (__system_property_get(name, value) <= 0 || value[0] == '\0') {
        return;
    }
    char* end = NULL;
    long v = strtol(value, &end, 10);
    if (end != value && v >= 0 && v <= maxRaw) {
        *target = v * scale;
    }
}

static void propPercent(const char* name, float* target) {
    propScaled(name, target, 0.01f, 100);
}

static void propFlag(const char* name, int* target) {
    char value[PROP_VALUE_MAX];
    value[0] = '\0';
    if (__system_property_get(name, value) <= 0 || value[0] == '\0') {
        return;
    }
    *target = value[0] != '0';
}

// Fires once each time the property is set to a value it has not seen. The
// value becomes the filename tag, so setprop 1, 2, 3 gives three captures.
static void pollCaptureRequest(XrCtx* ctx) {
    if (++ctx->capturePollCounter < CAPTURE_POLL_FRAMES) {
        return;
    }
    ctx->capturePollCounter = 0;

    propPercent(PROP_DEPTH_ALPHA, &ctx->depthAlpha);
    propPercent(PROP_RANGE_ALPHA, &ctx->rangeAlpha);
    propPercent(PROP_UPSAMPLE_SIGMA, &ctx->upsampleSigmaR);
    propPercent(PROP_DEPTH_SHARP, &ctx->depthSharp);
    propFlag(PROP_OVERLAY, &ctx->overlayVisible);
    propFlag(PROP_UPSAMPLE, &ctx->upsampleEnabled);
    propFlag(PROP_OCCLUSION, &ctx->occlusionEnabled);
    propPercent(PROP_CONVERGENCE, &ctx->convergence);
    // Same tenths of a percent of frame width the preference uses
    propScaled(PROP_SEPARATION, &ctx->separationOverride, 0.001f, 50);
    // Tenths of a metre, the same units the preferences use
    propScaled(PROP_DISTANCE, &ctx->distanceOverride, 0.1f, 80);
    propScaled(PROP_SCREEN, &ctx->screenOverride, 0.1f, 120);
    propPercent(PROP_DEPTH_GLOBAL, &ctx->depthGlobal);
    propScaled(PROP_DEPTH_LOCAL, &ctx->depthLocal, 0.01f, 400);
    // Tenths of a Hz, and half units of speed sensitivity
    propScaled(PROP_POINTER_CUTOFF, &ctx->pointerMinCutoff, 0.1f, 200);
    propScaled(PROP_POINTER_BETA, &ctx->pointerBeta, 0.5f, 100);
    // Millimetres
    propScaled(PROP_BEAM_WIDTH, &ctx->beamWidth, 0.001f, 100);
    // Tenths of a second
    propScaled(PROP_POINTER_WAKE, &ctx->pointerWake, 0.1f, 100);
    propScaled(PROP_POINTER_SLEEP, &ctx->pointerSleep, 0.1f, 600);

    if (ctx->captureDir[0] == '\0') {
        return;
    }

    char value[PROP_VALUE_MAX];
    value[0] = '\0';
    if (__system_property_get(CAPTURE_PROP, value) <= 0 || value[0] == '\0') {
        return;
    }
    if (strcmp(value, ctx->lastCaptureTag) == 0) {
        return;
    }
    strncpy(ctx->lastCaptureTag, value, sizeof(ctx->lastCaptureTag) - 1);
    strncpy(ctx->captureTag, value, sizeof(ctx->captureTag) - 1);
    ctx->captureRequested = 1;
    LOGI("capture: request %s", ctx->captureTag);
}

static void writeCapture(XrCtx* ctx, const char* what, const void* data, size_t bytes) {
    if (data == NULL) {
        return;
    }
    char path[512];
    snprintf(path, sizeof(path), "%s/cap_%s_%s.raw", ctx->captureDir, ctx->captureTag, what);
    FILE* f = fopen(path, "wb");
    if (f == NULL) {
        LOGE("capture: cannot write %s", path);
        return;
    }
    size_t written = fwrite(data, 1, bytes, f);
    fclose(f);
    LOGI("capture: %s %zu bytes", path, written);
}

// Reads back the depth texture the warp actually sampled this frame, so the
// captured warp can be reproduced exactly rather than approximately. Depth is
// the alpha channel, the rgb alongside it is the guide.
static void writeCaptureDepthTexture(XrCtx* ctx) {
    const int n = DEPTH_TEX_SIZE;
    unsigned char* rgba = malloc((size_t)n * n * 4);
    unsigned char* red = malloc((size_t)n * n);
    if (rgba == NULL || red == NULL) {
        free(rgba);
        free(red);
        return;
    }

    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           ctx->depthTextures[ctx->depthReadIndex], 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
        glReadPixels(0, 0, n, n, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        for (int i = 0; i < n * n; i++) {
            red[i] = rgba[i * 4 + 3];
        }
        writeCapture(ctx, "depthtex", red, (size_t)n * n);
        writeCapture(ctx, "guidetex", rgba, (size_t)n * n * 4);
    }
    else {
        LOGW("capture: depth texture not readable");
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);
    free(rgba);
    free(red);
}

// Runs every video frame rather than only when new depth lands, which also
// re-snaps a depth map that is a few frames old onto the colour edges of the
// frame it is actually warping.
static void runUpsample(XrCtx* ctx, const float* texMatrix) {
    glBindFramebuffer(GL_FRAMEBUFFER, ctx->upsampleFbo);
    glViewport(0, 0, ctx->upsampleWidth, ctx->upsampleHeight);
    if (ctx->srgbWriteControl) {
        glDisable(GL_FRAMEBUFFER_SRGB_EXT);
    }

    glUseProgram(ctx->upsampleProgram);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, ctx->oesTexture);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, ctx->depthTextures[ctx->depthReadIndex]);
    glUniformMatrix4fv(ctx->upsampleTexMatrixUniform, 1, GL_FALSE, texMatrix);
    glUniform1f(ctx->upsampleSigmaUniform, ctx->upsampleSigmaR);
    glUniform1f(ctx->upsampleSharpUniform, ctx->depthSharp);

    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, VERTEX_DATA);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, VERTEX_DATA + 2);
    glEnableVertexAttribArray(1);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

// Both eyes in one pass, since they search the same depth reads
static void runOffsetSearch(XrCtx* ctx, float separation) {
    glBindFramebuffer(GL_FRAMEBUFFER, ctx->offsetFbo);
    glViewport(0, 0, ctx->upsampleWidth, ctx->upsampleHeight);
    if (ctx->srgbWriteControl) {
        glDisable(GL_FRAMEBUFFER_SRGB_EXT);
    }

    glUseProgram(ctx->offsetProgram);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, ctx->upsampleTexture);
    glUniform1f(ctx->offsetDispUniform, separation * ctx->upsampleWidth);
    glUniform1f(ctx->offsetConvUniform, ctx->convergence);

    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, VERTEX_DATA);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, VERTEX_DATA + 2);
    glEnableVertexAttribArray(1);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

// Box-filter the whole frame down to GLOW_TEX_SIZE, same shape as
// nativeCaptureDepthInput's downscale but independent of it, then average
// each of the eight 3x3 blocks around the edge of that 3x3 grid separately
// (the centre block is discarded) instead of the whole buffer - eight
// edge/corner colours instead of v1's single whole-frame average, so the
// halo can actually pick up what's near each edge instead of one flat
// wash. Only called while the dim sphere is actually visible (see the
// caller), so this costs nothing at the default full-passthrough level.
static void computeGlowColor(XrCtx* ctx, const float* texMatrix) {
    if (ctx->glowFbo == 0) {
        return;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, ctx->glowFbo);
    glViewport(0, 0, GLOW_TEX_SIZE, GLOW_TEX_SIZE);
    if (ctx->srgbWriteControl) {
        glDisable(GL_FRAMEBUFFER_SRGB_EXT);
    }

    glUseProgram(ctx->glowProgram);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, ctx->oesTexture);
    glUniformMatrix4fv(ctx->glowTexMatrixUniform, 1, GL_FALSE, texMatrix);

    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, VERTEX_DATA);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, VERTEX_DATA + 2);
    glEnableVertexAttribArray(1);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    unsigned char px[GLOW_TEX_SIZE * GLOW_TEX_SIZE * 4];
    glReadPixels(0, 0, GLOW_TEX_SIZE, GLOW_TEX_SIZE, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    // Row 0 of glReadPixels is measured against the actual downscaled
    // frame to be the TOP of the image here, not GL's usual bottom-up
    // convention - this app's downscale shader/texMatrix combination
    // already un-flips it, confirmed by a top/bottom-swapped glow on
    // device before this was corrected.
    static const int sampleForCell[3][3] = {
        { GLOW_SAMPLE_TL, GLOW_SAMPLE_T, GLOW_SAMPLE_TR },
        { GLOW_SAMPLE_L,  -1,            GLOW_SAMPLE_R  },
        { GLOW_SAMPLE_BL, GLOW_SAMPLE_B, GLOW_SAMPLE_BR },
    };
    const int block = GLOW_TEX_SIZE / 3;
    long sumR[GLOW_SAMPLE_COUNT] = { 0 };
    long sumG[GLOW_SAMPLE_COUNT] = { 0 };
    long sumB[GLOW_SAMPLE_COUNT] = { 0 };
    for (int y = 0; y < GLOW_TEX_SIZE; y++) {
        int gridRow = y / block;
        for (int x = 0; x < GLOW_TEX_SIZE; x++) {
            int gridCol = x / block;
            int sample = sampleForCell[gridRow][gridCol];
            if (sample < 0) {
                continue;
            }
            const unsigned char* p = px + ((y * GLOW_TEX_SIZE) + x) * 4;
            sumR[sample] += p[0];
            sumG[sample] += p[1];
            sumB[sample] += p[2];
        }
    }
    const int blockCount = block * block;
    for (int c = 0; c < GLOW_SAMPLE_COUNT; c++) {
        ctx->glowSampleR[c] = (float)sumR[c] / (blockCount * 255.0f);
        ctx->glowSampleG[c] = (float)sumG[c] / (blockCount * 255.0f);
        ctx->glowSampleB[c] = (float)sumB[c] / (blockCount * 255.0f);
    }
}

// Normalised (x, y) position of each GLOW_SAMPLE_* colour around the
// screen's own footprint, in the same 0..1 space as updateGlowHalo()'s own
// nx/ny - order matches the GLOW_SAMPLE_TL..BR indices exactly.
static const float GLOW_SAMPLE_X[GLOW_SAMPLE_COUNT] = { 0.0f, 0.5f, 1.0f, 0.0f, 1.0f, 0.0f, 0.5f, 1.0f };
static const float GLOW_SAMPLE_Y[GLOW_SAMPLE_COUNT] = { 0.0f, 0.0f, 0.0f, 0.5f, 0.5f, 1.0f, 1.0f, 1.0f };

// The halo's shape is a soft ring hugging the screen's own rectangle,
// transparent inside it (the screen quad draws on top there regardless,
// since composition layers are painter's-algorithm order, not depth
// tested) and fading to nothing within GLOW_MARGIN_FRAC of the screen's
// size - "the last few pixels bleeding out and fading over a short
// distance", not a wash over the whole surround. Recomputed whole (shape
// and colour together) every frame while visible; GLOW_HALO_TEX is small
// enough that this is free.
static void updateGlowHalo(XrCtx* ctx) {
    if (ctx->glowHaloSwapchain == XR_NULL_HANDLE) {
        return;
    }

    const int n = GLOW_HALO_TEX;
    // Fraction of the quad, on each side, that the screen's own footprint
    // occupies - the quad itself is sized WIDTH*(1+2*margin), so the screen
    // sits centred in the middle 1/(1+2*margin) of it.
    float inner = 0.5f - 0.5f / (1.0f + 2.0f * GLOW_MARGIN_FRAC);

    unsigned char px[GLOW_HALO_TEX * GLOW_HALO_TEX * 4];
    for (int y = 0; y < n; y++) {
        for (int x = 0; x < n; x++) {
            float nx = (x + 0.5f) / (float)n;
            float ny = (y + 0.5f) / (float)n;
            float dx = fabsf(nx - 0.5f) - (0.5f - inner);
            float dy = fabsf(ny - 0.5f) - (0.5f - inner);
            if (dx < 0.0f) dx = 0.0f;
            if (dy < 0.0f) dy = 0.0f;
            float d = sqrtf(dx * dx + dy * dy);
            float a = 1.0f - d / inner;
            if (a < 0.0f) a = 0.0f;
            if (a > 1.0f) a = 1.0f;
            // Smoothstep instead of a straight ramp - a linear fade over
            // only a handful of texels still reads as a hard edge, this
            // eases both ends of it so it genuinely looks like light
            // dissipating rather than a translucent rectangle with a cutoff.
            a = a * a * (3.0f - 2.0f * a);
            // Inverse-distance blend of all eight edge/corner samples - ny
            // near 0 is the top of the screen (matching computeGlowColor()'s
            // own row convention), so a texel just picks up more of
            // whichever sample(s) it's physically closest to. Generalises
            // past four corners cleanly, unlike a hand-written bilinear
            // lerp, which is why this replaced it going from four samples
            // to eight.
            float wsum = 0.0f, weight[GLOW_SAMPLE_COUNT];
            for (int s = 0; s < GLOW_SAMPLE_COUNT; s++) {
                float ddx = nx - GLOW_SAMPLE_X[s];
                float ddy = ny - GLOW_SAMPLE_Y[s];
                weight[s] = 1.0f / (ddx * ddx + ddy * ddy + 0.001f);
                wsum += weight[s];
            }
            float rf = 0.0f, gf = 0.0f, bf = 0.0f;
            for (int s = 0; s < GLOW_SAMPLE_COUNT; s++) {
                float wn = weight[s] / wsum;
                rf += ctx->glowSampleR[s] * wn;
                gf += ctx->glowSampleG[s] * wn;
                bf += ctx->glowSampleB[s] * wn;
            }
            unsigned char r = (unsigned char)(rf * 255.0f + 0.5f);
            unsigned char g = (unsigned char)(gf * 255.0f + 0.5f);
            unsigned char b = (unsigned char)(bf * 255.0f + 0.5f);
            unsigned char* p = px + ((y * n) + x) * 4;
            // Premultiplied - same fix as the slider thumb below. A fixed
            // (r,g,b) regardless of a still adds full-strength colour even
            // past the point alpha reaches 0, which is exactly what made
            // this look like a sharp-edged patch instead of a fade: the
            // layer boundary, not the alpha ramp, was the only real cutoff.
            p[0] = (unsigned char)(r * a + 0.5f);
            p[1] = (unsigned char)(g * a + 0.5f);
            p[2] = (unsigned char)(b * a + 0.5f);
            p[3] = (unsigned char)(a * 255.0f + 0.5f);
        }
    }
    ctx->glowHaloReady = uploadArt(ctx, ctx->glowHaloSwapchain, ctx->glowHaloImages, px, n, n);
}

static void renderVideoFrame(XrCtx* ctx, const float* texMatrix, float separation) {
    int upsampling = ctx->stereoMode == DEPTH_MODE_MODEL && ctx->upsampleEnabled;
    int occluding = upsampling && ctx->occlusionEnabled && separation > 0.0f;

    // Capture frames do readbacks and file writes inside what would be the
    // query window, which both ruins the number and, on this driver, leaves a
    // query that never becomes available. Skip timing them. PMode's early
    // return below never reaches the matching pfnEndQuery, so it must never
    // start one either - an unmatched glBeginQuery breaks every later query.
    int timing = ctx->timerSupported && !ctx->captureRequested && !ctx->productivityMode;

    if (timing && !ctx->timerPending[ctx->timerSlot]) {
        pfnBeginQuery(GL_TIME_ELAPSED_EXT, ctx->timerQueries[ctx->timerSlot]);
    }

    if (upsampling) {
        runUpsample(ctx, texMatrix);
    }
    if (occluding) {
        runOffsetSearch(ctx, separation);
    }

    uint32_t imageIndex = 0;
    XrSwapchainImageAcquireInfo acquireInfo = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    if (!checkXr(xrAcquireSwapchainImage(ctx->swapchain, &acquireInfo, &imageIndex), "acquire image")) {
        return;
    }
    XrSwapchainImageWaitInfo waitInfo = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    waitInfo.timeout = XR_INFINITE_DURATION;
    xrWaitSwapchainImage(ctx->swapchain, &waitInfo);

    glBindFramebuffer(GL_FRAMEBUFFER, ctx->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           ctx->swapchainImages[imageIndex].image, 0);

    if (ctx->srgbWriteControl) {
        glDisable(GL_FRAMEBUFFER_SRGB_EXT);
    }

    glUseProgram(ctx->program);

    // The unwarped frame, drawn first so the real eye passes overwrite it and
    // the submitted frame is unaffected. Readback and file writes stall the
    // frame loop for a while, which is fine for a one off debug capture.
    unsigned char* captureBuf = NULL;
    size_t captureBytes = (size_t)ctx->videoWidth * ctx->videoHeight * 4;

    if (ctx->productivityMode) {
        // N independent screens, each its own OES texture fed by its own
        // background-process decoder - not one shared frame column-cropped.
        // No depth/occlusion/debug-capture paths here; PMode forces depth
        // off and none of the debug capture tooling applies per-screen yet.
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, ctx->depthTextures[ctx->depthReadIndex]);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, ctx->offsetTexture);
        glUniform1f(ctx->occlusionUniform, 0.0f);
        glUniform1f(ctx->convergenceUniform, ctx->convergence);
        glUniform1f(ctx->dispTexelsUniform, 0.0f);
        glUniform1f(ctx->lowResWidthUniform, (float)ctx->upsampleWidth);
        glUniform1f(ctx->frameWidthUniform, (float)ctx->videoWidth);
        glUniform1f(ctx->disparityUniform, 0.0f);
        glUniform1f(ctx->eyeIndexUniform, 0.0f);
        glUniform1f(ctx->barTestUniform, 0.0f);
        glUniform3f(ctx->tintUniform, 1.0f, 1.0f, 1.0f);

        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, VERTEX_DATA);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, VERTEX_DATA + 2);
        glEnableVertexAttribArray(1);

        // ctx->videoWidth is the whole swapchain's width here (still set by
        // Game.java's width*3 request), not any one screen's actual decoded
        // resolution - each screen negotiates its own independently.
        float colWidth = (float)ctx->videoWidth / PRODUCTIVITY_SCREEN_COUNT;
        // Per-column frame size, not ctx->videoWidth above (that one is the
        // whole swapchain, deliberately, for the depth-reprojection math) -
        // each of the 3 screens gets its own independently rounded corners.
        glUniform2f(ctx->roundFrameSizeUniform, colWidth, (float)ctx->videoHeight);
        glUniform1f(ctx->cornerRadiusUniform,
                    fminf(colWidth, (float)ctx->videoHeight) * SCREEN_CORNER_RADIUS_FRAC);
        glUniform1f(ctx->edgeFeatherUniform,
                    fminf(colWidth, (float)ctx->videoHeight) * SCREEN_EDGE_FEATHER_FRAC);
        for (int i = 0; i < PRODUCTIVITY_SCREEN_COUNT; i++) {
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_EXTERNAL_OES, ctx->productivityOesTexture[i]);
            glUniformMatrix4fv(ctx->texMatrixUniform, 1, GL_FALSE, ctx->productivityTexMatrix[i]);
            glViewport((int)(i * colWidth), 0, (int)colWidth, ctx->videoHeight);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        }

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        XrSwapchainImageReleaseInfo prodReleaseInfo = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
        xrReleaseSwapchainImage(ctx->swapchain, &prodReleaseInfo);
        ctx->everRendered = 1;
        return;
    }

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, ctx->oesTexture);
    glActiveTexture(GL_TEXTURE1);
    // Either the raw 256x256 map or the edge aware upsample of it. Both carry
    // depth in alpha, so the warp shader does not care which it got.
    glBindTexture(GL_TEXTURE_2D, upsampling ? ctx->upsampleTexture
                                            : ctx->depthTextures[ctx->depthReadIndex]);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, ctx->offsetTexture);
    glUniformMatrix4fv(ctx->texMatrixUniform, 1, GL_FALSE, texMatrix);
    glUniform1f(ctx->occlusionUniform, occluding ? 1.0f : 0.0f);
    glUniform1f(ctx->convergenceUniform, ctx->convergence);
    glUniform1f(ctx->dispTexelsUniform, separation * ctx->upsampleWidth);
    glUniform1f(ctx->lowResWidthUniform, (float)ctx->upsampleWidth);
    glUniform1f(ctx->frameWidthUniform, (float)ctx->videoWidth);
    glUniform2f(ctx->roundFrameSizeUniform, (float)ctx->videoWidth, (float)ctx->videoHeight);
    glUniform1f(ctx->cornerRadiusUniform,
                fminf((float)ctx->videoWidth, (float)ctx->videoHeight) * SCREEN_CORNER_RADIUS_FRAC);
    glUniform1f(ctx->edgeFeatherUniform,
                fminf((float)ctx->videoWidth, (float)ctx->videoHeight) * SCREEN_EDGE_FEATHER_FRAC);

    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, VERTEX_DATA);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, VERTEX_DATA + 2);
    glEnableVertexAttribArray(1);

    // Mono is a single full width draw with zero disparity. Stereo draws the
    // left eye into the left half and the right eye into the right half,
    // with opposite disparity signs
    int eyes = ctx->stereoMode != DEPTH_MODE_OFF ? 2 : 1;

    if (ctx->captureRequested) {
        captureBuf = malloc(captureBytes);
        if (captureBuf != NULL) {
            glViewport(0, 0, ctx->videoWidth, ctx->videoHeight);
            glUniform1f(ctx->disparityUniform, 0.0f);
            glUniform1f(ctx->barTestUniform, 0.0f);
            glUniform3f(ctx->tintUniform, 1.0f, 1.0f, 1.0f);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            glReadPixels(0, 0, ctx->videoWidth, ctx->videoHeight, GL_RGBA, GL_UNSIGNED_BYTE,
                         captureBuf);
            writeCapture(ctx, "source", captureBuf, captureBytes);
        }
    }

    for (int eye = 0; eye < eyes; eye++) {
        glViewport(eye * ctx->videoWidth, 0, ctx->videoWidth, ctx->videoHeight);
        float disparity = 0.0f;
        if (eyes == 2 && ctx->stereoMode != DEPTH_MODE_EYETEST) {
            disparity = (eye == 0) ? separation : -separation;
        }
        glUniform1f(ctx->disparityUniform, disparity);
        glUniform1f(ctx->eyeIndexUniform, (float)eye);
        glUniform1f(ctx->barTestUniform, ctx->stereoMode == DEPTH_MODE_SHIFTTEST ? 1.0f : 0.0f);

        if (ctx->stereoMode == DEPTH_MODE_EYETEST) {
            // Half 0 red, half 1 blue
            if (eye == 0) {
                glUniform3f(ctx->tintUniform, 1.0f, 0.2f, 0.2f);
            }
            else {
                glUniform3f(ctx->tintUniform, 0.2f, 0.2f, 1.0f);
            }
        }
        else {
            glUniform3f(ctx->tintUniform, 1.0f, 1.0f, 1.0f);
        }

        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }

    // Measure where the bar actually landed in each half. Positive shift
    // means content moved right in that eye
    if (ctx->stereoMode == DEPTH_MODE_SHIFTTEST && ctx->barTestFramesLogged < 3) {
        int rowWidth = ctx->videoWidth * 2;
        unsigned char* row = malloc((size_t)rowWidth * 4);
        glReadPixels(0, ctx->videoHeight / 2, rowWidth, 1, GL_RGBA, GL_UNSIGNED_BYTE, row);
        for (int half = 0; half < 2; half++) {
            long sum = 0, count = 0;
            for (int x = 0; x < ctx->videoWidth; x++) {
                if (row[(size_t)((half * ctx->videoWidth) + x) * 4] > 128) {
                    sum += x;
                    count++;
                }
            }
            if (count > 0) {
                double center = (double)sum / (double)count / (double)ctx->videoWidth;
                LOGI("bar test: half %d (%s eye) bar center %.4f, shift %+.4f",
                     half, half == 0 ? "left" : "right", center, center - 0.5);
            }
            else {
                LOGI("bar test: half %d no bar found", half);
            }
        }
        free(row);
        ctx->barTestFramesLogged++;
    }

    if (ctx->captureRequested) {
        if (captureBuf != NULL) {
            for (int eye = 0; eye < eyes; eye++) {
                glReadPixels(eye * ctx->videoWidth, 0, ctx->videoWidth, ctx->videoHeight,
                             GL_RGBA, GL_UNSIGNED_BYTE, captureBuf);
                writeCapture(ctx, eye == 0 ? "left" : "right", captureBuf, captureBytes);
            }
            free(captureBuf);
        }
        writeCaptureDepthTexture(ctx);
        if (upsampling) {
            size_t count = (size_t)ctx->upsampleWidth * ctx->upsampleHeight;
            unsigned char* rgba = malloc(count * 4);
            unsigned char* alpha = malloc(count);
            if (rgba != NULL && alpha != NULL) {
                glBindFramebuffer(GL_FRAMEBUFFER, ctx->upsampleFbo);
                glReadPixels(0, 0, ctx->upsampleWidth, ctx->upsampleHeight, GL_RGBA,
                             GL_UNSIGNED_BYTE, rgba);
                for (size_t i = 0; i < count; i++) {
                    alpha[i] = rgba[i * 4 + 3];
                }
                writeCapture(ctx, "upsampled", alpha, count);
            }
            free(rgba);
            free(alpha);
        }
        // Best effort, the depth thread may be part way through refilling
        // these. The depth texture above is the exact one this frame sampled.
        writeCapture(ctx, "modelinput", ctx->modelInput,
                     (size_t)DEPTH_TEX_SIZE * DEPTH_TEX_SIZE * 3 * sizeof(float));
        writeCapture(ctx, "depthraw", ctx->modelOutput,
                     (size_t)DEPTH_TEX_SIZE * DEPTH_TEX_SIZE * sizeof(float));
        ctx->captureRequested = 0;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    XrSwapchainImageReleaseInfo releaseInfo = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    xrReleaseSwapchainImage(ctx->swapchain, &releaseInfo);

    // Close this frame's query and collect whichever earlier one has landed.
    // Never blocks: an unfinished query is simply left for a later frame.
    if (timing) {
        if (!ctx->timerPending[ctx->timerSlot]) {
            pfnEndQuery(GL_TIME_ELAPSED_EXT);
            ctx->timerPending[ctx->timerSlot] = 1;
            ctx->timerPendingFrames[ctx->timerSlot] = 0;
            ctx->timerSlot = 1 - ctx->timerSlot;
        }
        int other = ctx->timerSlot;
        if (ctx->timerPending[other]) {
            GLuint ready = 0;
            pfnGetQueryObjectuiv(ctx->timerQueries[other], GL_QUERY_RESULT_AVAILABLE_EXT, &ready);
            if (ready) {
                GLuint64 elapsed = 0;
                pfnGetQueryObjectui64v(ctx->timerQueries[other], GL_QUERY_RESULT_EXT, &elapsed);
                ctx->timerPending[other] = 0;
                ctx->timerPendingFrames[other] = 0;
                ctx->gpuTotalNs += (long)elapsed;
                ctx->gpuSamples++;
                ctx->overlayGpuTotalNs += (long)elapsed;
                ctx->overlayGpuSamples++;
                if ((long)elapsed > ctx->gpuMaxNs) {
                    ctx->gpuMaxNs = (long)elapsed;
                }
            }
            else if (++ctx->timerPendingFrames[other] > 90) {
                // Abandon it. Waiting forever costs every later measurement,
                // and one missed sample costs nothing.
                ctx->timerPending[other] = 0;
                ctx->timerPendingFrames[other] = 0;
                LOGW("XR warp: gave up on a GPU timer query that never landed");
            }
        }
    }

    ctx->everRendered = 1;
}

// The whole icon strip, drawn as one Bitmap in Java (one OUTLINE_TEX-wide
// cell per TOPBAR_ITEM_COUNT item) and uploaded whole. Flipped on the way
// in, since a Bitmap runs top down and a texture does not.
JNIEXPORT void JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeUploadTopBarArt(JNIEnv* env, jobject thiz,
                                                                   jlong handle, jobject strip) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx == NULL || strip == NULL) {
        return;
    }
    const unsigned char* px = (*env)->GetDirectBufferAddress(env, strip);
    if (px != NULL) {
        ctx->topBarReady = uploadFlipped(ctx, ctx->topBarSwapchain, ctx->topBarImages, px,
                                         TOPBAR_TEX_W, TOPBAR_TEX_H);
    }
    LOGI("top bar art %s", ctx->topBarReady ? "ready" : "missing");
}

// Passthrough level restored from preferences, applied once before the first
// frame so the dim layer starts at the level the user left it on, not the
// 1.0 default.
JNIEXPORT void JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeSetPassthroughLevel(JNIEnv* env, jobject thiz,
                                                                       jlong handle, jfloat level) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx == NULL) {
        return;
    }
    ctx->passthroughLevel = level < 0.0f ? 0.0f : (level > 1.0f ? 1.0f : level);
}

// Curve amount restored from preferences, applied once before the first
// frame - updatePlacement()'s first-ever seed reads ctx->curveAmount to
// compute the starting screenRadius.
JNIEXPORT void JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeSetCurvature(JNIEnv* env, jobject thiz,
                                                                 jlong handle, jfloat amount) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx == NULL) {
        return;
    }
    ctx->curveAmount = amount < 0.0f ? 0.0f : (amount > 1.0f ? 1.0f : amount);
}

// Java owns the actual on/off state (it also has to start/stop the depth
// inference thread and persist the preference) - this just echoes it into
// the live gate nativeEndFrame reads. Called both at session start and
// every time the top bar's 3D-effect icon is tapped.
JNIEXPORT void JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeSetDepthEffect(JNIEnv* env, jobject thiz,
                                                                   jlong handle, jboolean on) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx == NULL) {
        return;
    }
    ctx->depthEffectOn = on;
}

// User on/off for the ambient glow effect, restored at session start and
// flipped by tapping the icon next to the brightness slider. Skips
// computeGlowColor() entirely when off, not just the visual result.
JNIEXPORT void JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeSetGlowEnabled(JNIEnv* env, jobject thiz,
                                                                   jlong handle, jboolean on) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx == NULL) {
        return;
    }
    ctx->glowEnabled = on;
}

// The glow toggle's own icon (on/off drawn in Java, swapped whenever the
// state changes) - flipped top-down like every other Bitmap-sourced icon.
JNIEXPORT void JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeUploadGlowToggleArt(JNIEnv* env, jobject thiz,
                                                                       jlong handle, jobject icon) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx == NULL || icon == NULL) {
        return;
    }
    const unsigned char* px = (*env)->GetDirectBufferAddress(env, icon);
    if (px != NULL) {
        ctx->glowToggleReady = uploadFlipped(ctx, ctx->glowToggleSwapchain, ctx->glowToggleImages,
                                             px, OUTLINE_TEX, OUTLINE_TEX);
    }
}

// Puts back a placement saved from a previous session. Marking the sliders as
// already seen stops the first frame taking the screen straight back off it.
JNIEXPORT void JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeSetScreenPose(JNIEnv* env, jobject thiz,
                                                                jlong handle, jfloatArray poseArr) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx == NULL || poseArr == NULL) {
        return;
    }
    float p[9];
    if ((*env)->GetArrayLength(env, poseArr) < 9) {
        return;
    }
    (*env)->GetFloatArrayRegion(env, poseArr, 0, 9, p);

    if (p[7] < SCREEN_MIN_WIDTH || p[7] > SCREEN_MAX_WIDTH || p[8] <= 0.0f) {
        LOGW("stored screen placement out of range, ignoring it");
        return;
    }

    ctx->screenPose.position.x = p[0];
    ctx->screenPose.position.y = p[1];
    ctx->screenPose.position.z = p[2];
    ctx->screenPose.orientation.x = p[3];
    ctx->screenPose.orientation.y = p[4];
    ctx->screenPose.orientation.z = p[5];
    ctx->screenPose.orientation.w = p[6];
    ctx->screenPose.orientation = quatNorm(ctx->screenPose.orientation);
    ctx->screenWidth = p[7];
    ctx->screenRadius = p[8];
    ctx->placementValid = 1;
    ctx->sliderSeen = 0;
    LOGI("restored screen placement %.2f %.2f %.2f, %.2f m wide",
         p[0], p[1], p[2], p[7]);
}

// Pixels come from a Bitmap the stats are drawn into on the Java side, which
// is the only place Android will lay out text. Runs on the frame loop thread
// so the GL context is current, and only when the text actually changed.
JNIEXPORT void JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeUploadOverlay(JNIEnv* env, jobject thiz,
                                                                jlong handle, jobject buffer,
                                                                jint width, jint height) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx == NULL || ctx->overlaySwapchain == XR_NULL_HANDLE) {
        return;
    }
    void* pixels = (*env)->GetDirectBufferAddress(env, buffer);
    if (pixels == NULL || width != OVERLAY_WIDTH || height != OVERLAY_HEIGHT) {
        return;
    }

    uint32_t imageIndex = 0;
    XrSwapchainImageAcquireInfo acquireInfo = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    if (!checkXr(xrAcquireSwapchainImage(ctx->overlaySwapchain, &acquireInfo, &imageIndex),
                 "acquire overlay image")) {
        return;
    }
    XrSwapchainImageWaitInfo waitInfo = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    waitInfo.timeout = XR_INFINITE_DURATION;
    xrWaitSwapchainImage(ctx->overlaySwapchain, &waitInfo);

    glBindTexture(GL_TEXTURE_2D, ctx->overlayImages[imageIndex].image);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glBindTexture(GL_TEXTURE_2D, 0);

    XrSwapchainImageReleaseInfo releaseInfo = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    xrReleaseSwapchainImage(ctx->overlaySwapchain, &releaseInfo);
    ctx->overlayHasContent = 1;
}

// Average GPU time of the warp since this was last called, which is what the
// overlay wants. Returns 0 when the timer is unavailable.
JNIEXPORT jfloat JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeGetWarpGpuMs(JNIEnv* env, jobject thiz,
                                                                jlong handle) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx == NULL || ctx->overlayGpuSamples == 0) {
        return 0.0f;
    }
    float ms = (float)(ctx->overlayGpuTotalNs / (double)ctx->overlayGpuSamples / 1e6);
    ctx->overlayGpuTotalNs = 0;
    ctx->overlayGpuSamples = 0;
    return ms;
}

JNIEXPORT void JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeEndFrame(JNIEnv* env, jobject thiz, jlong handle,
                                                           jboolean newFrame, jfloatArray texMatrixArr,
                                                           jfloat distance, jfloat quadWidth,
                                                           jboolean headLocked,
                                                           jfloat separation, jboolean eyeSwap,
                                                           jboolean passthrough) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;

    ctx->passthrough = passthrough;
    pollCaptureRequest(ctx);
    propFlag(PROP_PASSTHROUGH, &ctx->passthrough);
    if (ctx->separationOverride >= 0.0f) {
        separation = ctx->separationOverride;
    }
    if (ctx->distanceOverride > 0.0f) {
        distance = ctx->distanceOverride;
    }
    if (ctx->screenOverride > 0.0f) {
        quadWidth = ctx->screenOverride;
    }

    // Advance the whole-view fade on wall clock time, independent of frame
    // gating below, so a couple of skipped frames don't stretch it out
    // noticeably. See FADE_IN/FADE_OUT.
    if (ctx->fadeState != FADE_NONE) {
        float target = ctx->fadeState == FADE_IN ? 0.0f : 1.0f;
        float t = (float)(nowNs() - ctx->fadeStartNs) / (float)FADE_DURATION_NS;
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;
        ctx->fadeAlpha = ctx->fadeFromAlpha + (target - ctx->fadeFromAlpha) * t;
        if (t >= 1.0f) {
            ctx->fadeAlpha = target;
            if (ctx->fadeState == FADE_OUT) {
                ctx->fadeOutComplete = 1;
            }
            ctx->fadeState = FADE_NONE;
        }
    }

    if (newFrame && ctx->shouldRender) {
        long startNs = nowNs();

        float texMatrix[16];
        (*env)->GetFloatArrayRegion(env, texMatrixArr, 0, 16, texMatrix);
        // Zero separation collapses every stage of the warp (occlusion
        // search, upsample offset, final disparity) to a flat pass without
        // needing its own gate at each one - same live on/off switch the
        // top bar's 3D-effect icon controls.
        renderVideoFrame(ctx, texMatrix, ctx->depthEffectOn ? separation : 0.0f);

        if (ctx->passthroughLevel < 0.999f && ctx->glowEnabled) {
            computeGlowColor(ctx, texMatrix);
            if (!ctx->productivityMode) {
                updateGlowHalo(ctx);
            }
        }

        long elapsed = nowNs() - startNs;
        ctx->statFrames++;
        ctx->statTotalNs += elapsed;
        if (elapsed > ctx->statMaxNs) ctx->statMaxNs = elapsed;
        if (ctx->statFrames == STATS_LOG_INTERVAL_FRAMES) {
            // Submit is the wall clock around the draw calls, which is only
            // how long the driver took to queue them. GPU is the real cost.
            if (ctx->gpuSamples > 0) {
                LOGI("XR warp: %ld frames, GPU avg %.2f ms, GPU max %.2f ms, submit avg %.2f ms",
                     ctx->statFrames, ctx->gpuTotalNs / (double)ctx->gpuSamples / 1e6,
                     ctx->gpuMaxNs / 1e6,
                     ctx->statTotalNs / (double)ctx->statFrames / 1e6);
            }
            else {
                LOGI("XR warp: %ld frames, submit avg %.2f ms, max %.2f ms (no GPU timer)",
                     ctx->statFrames, ctx->statTotalNs / (double)ctx->statFrames / 1e6,
                     ctx->statMaxNs / 1e6);
            }
            ctx->statFrames = 0;
            ctx->statTotalNs = 0;
            ctx->statMaxNs = 0;
            ctx->gpuTotalNs = 0;
            ctx->gpuMaxNs = 0;
            ctx->gpuSamples = 0;
        }
    }

    float aspect = (float)ctx->videoHeight / (float)ctx->videoWidth;
    XrSpace space = headLocked ? ctx->viewSpace : ctx->localSpace;
    int stereo = ctx->stereoMode != DEPTH_MODE_OFF;

    if (!ctx->pointerArtReady && ctx->pointerSwapchain != XR_NULL_HANDLE && ctx->shouldRender) {
        uploadPointerArt(ctx);
    }

    updatePlacement(ctx, distance, quadWidth);
    XrPosef screenPose = ctx->screenPose;
    float screenWidth = ctx->screenWidth;
    float screenHeight = screenWidth * aspect;

    XrFrameEndInfo endInfo = { XR_TYPE_FRAME_END_INFO };
    endInfo.displayTime = ctx->predictedDisplayTime;
    endInfo.environmentBlendMode = (ctx->passthrough && ctx->alphaBlendSupported)
            ? XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND : XR_ENVIRONMENT_BLEND_MODE_OPAQUE;

    XrCompositionLayerQuad quadLayers[2];
    XrCompositionLayerCylinderKHR cylLayers[2];
    XrCompositionLayerQuad overlayLayer;
    XrCompositionLayerQuad beamLayer;
    XrCompositionLayerQuad dotLayer;
    XrCompositionLayerQuad handleLayer;
    XrCompositionLayerQuad prodQuadLayers[PRODUCTIVITY_SCREEN_COUNT];
    XrCompositionLayerQuad topBarLayer;
    XrCompositionLayerQuad sliderTrackLayer;
    XrCompositionLayerQuad sliderThumbLayer;
    XrCompositionLayerQuad glowToggleLayer;
    XrCompositionLayerEquirect2KHR dimLayer;
    XrCompositionLayerQuad glowHaloLayer;
    XrCompositionLayerEquirect2KHR fadeLayer;
    XrCompositionLayerCylinderKHR glowHaloCyl;
    const XrCompositionLayerBaseHeader* layers[16];
    uint32_t layerCount = 0;

    // ctx->passthrough drives the blend mode above; the dim layer below is a
    // separate, independent full-surround occlusion the brightness slider
    // controls, layered behind everything else - flat black, on purpose:
    // the room should reach genuinely pitch black at max, independent of
    // whatever colour the glow layer (see below) is doing near the screen.
    if (ctx->equirectSupported
            && (!ctx->dimUploadedValid
                || fabsf(ctx->dimUploadedLevel - ctx->passthroughLevel) > 0.001f)) {
        unsigned char px[DIM_TEX * DIM_TEX * 4];
        unsigned char alpha = (unsigned char)((1.0f - ctx->passthroughLevel) * 255.0f + 0.5f);
        for (int i = 0; i < DIM_TEX * DIM_TEX; i++) {
            px[i * 4 + 0] = 0;
            px[i * 4 + 1] = 0;
            px[i * 4 + 2] = 0;
            px[i * 4 + 3] = alpha;
        }
        ctx->dimReady = uploadArt(ctx, ctx->dimSwapchain, ctx->dimImages, px, DIM_TEX, DIM_TEX);
        ctx->dimUploadedLevel = ctx->passthroughLevel;
        ctx->dimUploadedValid = 1;
    }

    // Same tiny alpha-only texture as the dim sphere, driven by ctx->fadeAlpha
    // instead of the passthrough slider - see the FADE_IN/FADE_OUT advance
    // above.
    if (ctx->equirectSupported
            && (!ctx->fadeUploadedValid
                || fabsf(ctx->fadeUploadedAlpha - ctx->fadeAlpha) > 0.001f)) {
        unsigned char px[DIM_TEX * DIM_TEX * 4];
        unsigned char alpha = (unsigned char)(ctx->fadeAlpha * 255.0f + 0.5f);
        for (int i = 0; i < DIM_TEX * DIM_TEX; i++) {
            px[i * 4 + 0] = 0;
            px[i * 4 + 1] = 0;
            px[i * 4 + 2] = 0;
            px[i * 4 + 3] = alpha;
        }
        ctx->fadeReady = uploadArt(ctx, ctx->fadeSwapchain, ctx->fadeImages, px, DIM_TEX, DIM_TEX);
        ctx->fadeUploadedAlpha = ctx->fadeAlpha;
        ctx->fadeUploadedValid = 1;
    }

    if (ctx->everRendered && ctx->shouldRender) {
      if (ctx->dimReady && ctx->equirectSupported) {
        memset(&dimLayer, 0, sizeof(dimLayer));
        dimLayer.type = XR_TYPE_COMPOSITION_LAYER_EQUIRECT2_KHR;
        dimLayer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        dimLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        dimLayer.space = space;
        dimLayer.subImage.swapchain = ctx->dimSwapchain;
        dimLayer.subImage.imageRect.offset.x = 0;
        dimLayer.subImage.imageRect.offset.y = 0;
        dimLayer.subImage.imageRect.extent.width = DIM_TEX;
        dimLayer.subImage.imageRect.extent.height = DIM_TEX;
        dimLayer.subImage.imageArrayIndex = 0;
        dimLayer.pose.orientation.w = 1.0f;
        dimLayer.radius = 0.0f;
        dimLayer.centralHorizontalAngle = 6.2831853f;
        dimLayer.upperVerticalAngle = 1.5707963f;
        dimLayer.lowerVerticalAngle = -1.5707963f;
        layers[layerCount++] = (const XrCompositionLayerBaseHeader*)&dimLayer;
      }

      // Glow halo: submitted before the screen itself so the screen draws
      // over it within its own footprint, and after the dim sphere so it
      // sits in front of that. Follows the screen's own curve/flat choice -
      // a flat quad's straight silhouette doesn't match a curved cylinder
      // screen's footprint, which left mismatched bright patches showing
      // past the screen's real edge instead of a clean short bleed. Gaming
      // only (see updateGlowHalo()).
      if (!ctx->productivityMode && ctx->glowHaloReady && ctx->glowEnabled
              && ctx->passthroughLevel < 0.999f) {
        float haloWidth = screenWidth * (1.0f + 2.0f * GLOW_MARGIN_FRAC);
        float haloHeight = screenHeight * (1.0f + 2.0f * GLOW_MARGIN_FRAC);
        if (ctx->curveAmount > 0.01f && ctx->cylinderSupported) {
            // Same axis/radius math as the screen's own cylinder above -
            // scaling both dimensions of the halo by the same margin factor
            // leaves the aspect ratio, and so the cylinder's aspectRatio
            // field, identical to the screen's.
            float radius = ctx->screenRadius;
            memset(&glowHaloCyl, 0, sizeof(glowHaloCyl));
            glowHaloCyl.type = XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR;
            glowHaloCyl.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            glowHaloCyl.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            glowHaloCyl.space = space;
            glowHaloCyl.subImage.swapchain = ctx->glowHaloSwapchain;
            glowHaloCyl.subImage.imageRect.offset.x = 0;
            glowHaloCyl.subImage.imageRect.offset.y = 0;
            glowHaloCyl.subImage.imageRect.extent.width = GLOW_HALO_TEX;
            glowHaloCyl.subImage.imageRect.extent.height = GLOW_HALO_TEX;
            glowHaloCyl.subImage.imageArrayIndex = 0;
            glowHaloCyl.pose.orientation = screenPose.orientation;
            Vec3 haloAxisLocal = { 0.0f, 0.0f, radius };
            Vec3 haloAxis = quatRotate(screenPose.orientation, haloAxisLocal);
            glowHaloCyl.pose.position.x = screenPose.position.x + haloAxis.x;
            glowHaloCyl.pose.position.y = screenPose.position.y + haloAxis.y;
            glowHaloCyl.pose.position.z = screenPose.position.z + haloAxis.z;
            glowHaloCyl.radius = radius;
            glowHaloCyl.centralAngle = haloWidth / radius;
            glowHaloCyl.aspectRatio = haloWidth / haloHeight;
            layers[layerCount++] = (const XrCompositionLayerBaseHeader*)&glowHaloCyl;
        }
        else {
            memset(&glowHaloLayer, 0, sizeof(glowHaloLayer));
            glowHaloLayer.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
            glowHaloLayer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            glowHaloLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            glowHaloLayer.subImage.swapchain = ctx->glowHaloSwapchain;
            glowHaloLayer.subImage.imageRect.offset.x = 0;
            glowHaloLayer.subImage.imageRect.offset.y = 0;
            glowHaloLayer.subImage.imageRect.extent.width = GLOW_HALO_TEX;
            glowHaloLayer.subImage.imageRect.extent.height = GLOW_HALO_TEX;
            glowHaloLayer.subImage.imageArrayIndex = 0;
            glowHaloLayer.space = space;
            glowHaloLayer.pose = screenPose;
            glowHaloLayer.size.width = haloWidth;
            glowHaloLayer.size.height = haloHeight;
            layers[layerCount++] = (const XrCompositionLayerBaseHeader*)&glowHaloLayer;
        }
      }

      if (ctx->productivityMode) {
        // Phase 1: N flat mono screens, fixed default arrangement, no
        // interaction yet (no beam/handles/picker/background - those are
        // all still the single-screen path below, untouched). Each screen
        // samples an equal-width column of the same decoded frame that the
        // single-screen path would otherwise show whole - see
        // renderVideoFrame, which already does a plain mono blit whenever
        // stereoMode is off, forced for productivity sessions in Game.java.
        float colWidth = (float)ctx->videoWidth / PRODUCTIVITY_SCREEN_COUNT;
        float colHeight = (float)ctx->videoHeight;
        float quadHeight = PRODUCTIVITY_SCREEN_WIDTH_M * (colHeight / colWidth);

        for (int i = 0; i < PRODUCTIVITY_SCREEN_COUNT; i++) {
            XrCompositionLayerQuad* quad = &prodQuadLayers[i];
            memset(quad, 0, sizeof(*quad));
            quad->type = XR_TYPE_COMPOSITION_LAYER_QUAD;
            // Alpha-blended so the shader's rounded-corner mask (see
            // SCREEN_CORNER_RADIUS_FRAC) actually shows passthrough at the
            // corners instead of opaque black - fragColor is premultiplied
            // by that same mask in the shader to match this blend mode.
            quad->layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            quad->eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            quad->subImage.swapchain = ctx->swapchain;
            quad->subImage.imageRect.offset.x = (int32_t)(i * colWidth);
            quad->subImage.imageRect.offset.y = 0;
            quad->subImage.imageRect.extent.width = (int32_t)colWidth;
            quad->subImage.imageRect.extent.height = (int32_t)colHeight;
            quad->subImage.imageArrayIndex = 0;
            quad->space = space;
            quad->pose = productivityScreenPose(i);
            quad->size.width = PRODUCTIVITY_SCREEN_WIDTH_M;
            quad->size.height = quadHeight;
            layers[layerCount++] = (const XrCompositionLayerBaseHeader*)quad;
        }
      } else {
        int viewCount = stereo ? 2 : 1;
        for (int eye = 0; eye < viewCount; eye++) {
            XrSwapchainSubImage subImage;
            subImage.swapchain = ctx->swapchain;
            // The swap toggle reroutes which half each eye sees. Any stereo
            // inversion bug found later is then depth or warp, not routing
            int half = eyeSwap ? (1 - eye) : eye;
            subImage.imageRect.offset.x = stereo ? half * ctx->videoWidth : 0;
            subImage.imageRect.offset.y = 0;
            subImage.imageRect.extent.width = ctx->videoWidth;
            subImage.imageRect.extent.height = ctx->videoHeight;
            subImage.imageArrayIndex = 0;

            XrEyeVisibility visibility = !stereo ? XR_EYE_VISIBILITY_BOTH :
                    (eye == 0 ? XR_EYE_VISIBILITY_LEFT : XR_EYE_VISIBILITY_RIGHT);

            if (ctx->curveAmount > 0.01f && ctx->cylinderSupported) {
                XrCompositionLayerCylinderKHR* cyl = &cylLayers[eye];
                memset(cyl, 0, sizeof(*cyl));
                cyl->type = XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR;
                // See the productivity quad's identical flag above for why -
                // same rounded-corner mask, same premultiplied-alpha match.
                cyl->layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                // Radius runs from 4x distance (slightly curved) down to the
                // distance itself (wrapped around the viewer) as curvature rises
                float radius = ctx->screenRadius;
                cyl->eyeVisibility = visibility;
                cyl->subImage = subImage;
                cyl->space = space;
                cyl->pose.orientation = screenPose.orientation;
                // The layer pose is the axis, which sits a radius behind the
                // surface the placement tracks
                Vec3 axisLocal = { 0.0f, 0.0f, radius };
                Vec3 axis = quatRotate(screenPose.orientation, axisLocal);
                cyl->pose.position.x = screenPose.position.x + axis.x;
                cyl->pose.position.y = screenPose.position.y + axis.y;
                cyl->pose.position.z = screenPose.position.z + axis.z;
                cyl->radius = radius;
                cyl->centralAngle = screenWidth / radius;
                cyl->aspectRatio = 1.0f / aspect;
                layers[layerCount++] = (const XrCompositionLayerBaseHeader*)cyl;
            }
            else {
                XrCompositionLayerQuad* quad = &quadLayers[eye];
                memset(quad, 0, sizeof(*quad));
                quad->type = XR_TYPE_COMPOSITION_LAYER_QUAD;
                // See the productivity quad's identical flag above for why -
                // same rounded-corner mask, same premultiplied-alpha match.
                quad->layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                quad->eyeVisibility = visibility;
                quad->subImage = subImage;
                quad->space = space;
                quad->pose = screenPose;
                quad->size.width = screenWidth;
                quad->size.height = screenHeight;
                layers[layerCount++] = (const XrCompositionLayerBaseHeader*)quad;
            }
        }

        // Stats sit in the top left corner of the screen, same space and
        // distance, both eyes, so they read at screen depth with no disparity
        if (ctx->overlayHasContent && ctx->overlayVisible
                && ctx->overlaySwapchain != XR_NULL_HANDLE) {
            float overlayW = screenWidth * 0.30f;
            float overlayH = overlayW * (float)OVERLAY_HEIGHT / (float)OVERLAY_WIDTH;
            float margin = screenWidth * 0.02f;

            memset(&overlayLayer, 0, sizeof(overlayLayer));
            overlayLayer.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
            overlayLayer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            overlayLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            overlayLayer.subImage.swapchain = ctx->overlaySwapchain;
            overlayLayer.subImage.imageRect.offset.x = 0;
            overlayLayer.subImage.imageRect.offset.y = 0;
            overlayLayer.subImage.imageRect.extent.width = OVERLAY_WIDTH;
            overlayLayer.subImage.imageRect.extent.height = OVERLAY_HEIGHT;
            overlayLayer.subImage.imageArrayIndex = 0;
            overlayLayer.space = space;
            // Pinned to the top left of the screen in the screen's own frame,
            // so it follows wherever the screen has been moved to
            Vec3 statsLocal = { -screenWidth * 0.5f + overlayW * 0.5f + margin,
                                screenHeight * 0.5f - overlayH * 0.5f - margin,
                                // A little in front so the two never z fight
                                0.01f };
            Vec3 stats = quatRotate(screenPose.orientation, statsLocal);
            overlayLayer.pose.orientation = screenPose.orientation;
            overlayLayer.pose.position.x = screenPose.position.x + stats.x;
            overlayLayer.pose.position.y = screenPose.position.y + stats.y;
            overlayLayer.pose.position.z = screenPose.position.z + stats.z;
            overlayLayer.size.width = overlayW;
            overlayLayer.size.height = overlayH;
            layers[layerCount++] = (const XrCompositionLayerBaseHeader*)&overlayLayer;
        }

        int barArea = ctx->hoverKind == HOVER_BAR;

        // Move bar and resize corner, shown only while the ray is over them.
        // Both live in the screen's own frame, so they travel with it.
        if (ctx->handleArtReady && (barArea || ctx->hoverKind == HOVER_CORNER)) {
            int isBar = barArea;
            Vec3 local;
            float sizeW, sizeH;
            float roll = 0.0f;

            if (isBar) {
                sizeW = screenWidth * BAR_WIDTH_FRAC;
                sizeH = screenWidth * BAR_HEIGHT_FRAC;
                local.x = 0.0f;
                local.y = -(screenHeight * 0.5f + screenWidth * BAR_GAP_FRAC + sizeH * 0.5f);
            }
            else {
                sizeW = sizeH = screenWidth * CORNER_FRAC;
                int right = ctx->hoverCorner == 1 || ctx->hoverCorner == 3;
                int bottom = ctx->hoverCorner >= 2;
                float gap = screenWidth * CORNER_GAP_FRAC;
                local.x = (right ? 0.5f : -0.5f) * screenWidth + (right ? gap : -gap);
                local.y = (bottom ? -0.5f : 0.5f) * screenHeight + (bottom ? -gap : gap);
                // The art is a top left bracket, so the other three are the
                // same picture rolled about the screen normal
                if (ctx->hoverCorner == 1) roll = -1.5707963f;
                else if (ctx->hoverCorner == 2) roll = 1.5707963f;
                else if (ctx->hoverCorner == 3) roll = 3.1415927f;
            }
            // Just off the surface so it never z fights the picture
            local.z = 0.005f;

            XrQuaternionf rollQ = { 0.0f, 0.0f, sinf(roll * 0.5f), cosf(roll * 0.5f) };
            Vec3 offset = quatRotate(screenPose.orientation, local);

            memset(&handleLayer, 0, sizeof(handleLayer));
            handleLayer.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
            handleLayer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            handleLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            handleLayer.subImage.swapchain = isBar ? ctx->barSwapchain : ctx->cornerSwapchain;
            handleLayer.subImage.imageRect.offset.x = 0;
            handleLayer.subImage.imageRect.offset.y = 0;
            handleLayer.subImage.imageRect.extent.width = isBar ? BAR_TEX_W : CORNER_TEX_W;
            handleLayer.subImage.imageRect.extent.height = isBar ? BAR_TEX_H : CORNER_TEX_H;
            handleLayer.subImage.imageArrayIndex = 0;
            handleLayer.space = space;
            handleLayer.pose.orientation = quatNorm(quatMul(screenPose.orientation, rollQ));
            handleLayer.pose.position.x = screenPose.position.x + offset.x;
            handleLayer.pose.position.y = screenPose.position.y + offset.y;
            handleLayer.pose.position.z = screenPose.position.z + offset.z;
            handleLayer.size.width = sizeW;
            handleLayer.size.height = sizeH;
            layers[layerCount++] = (const XrCompositionLayerBaseHeader*)&handleLayer;
        }

        // Laser and cursor, submitted last so they sit over the picture. Two
        // quad layers, so this costs no drawing at all: the art was uploaded
        // once and the compositor places it from these poses.
        if (ctx->beamVisible && !ctx->beamGaze && ctx->pointerArtReady) {
            Vec3 start = { ctx->beamStart.x, ctx->beamStart.y, ctx->beamStart.z };
            Vec3 end = { ctx->beamEnd.x, ctx->beamEnd.y, ctx->beamEnd.z };
            Vec3 head = { ctx->headPos.x, ctx->headPos.y, ctx->headPos.z };
            Vec3 along = vecSub(end, start);
            float length = sqrtf(along.x * along.x + along.y * along.y + along.z * along.z);

            Vec3 mid = { (start.x + end.x) * 0.5f, (start.y + end.y) * 0.5f,
                         (start.z + end.z) * 0.5f };
            Vec3 beamY = vecNorm(along);
            Vec3 toHead = vecNorm(vecSub(head, mid));
            Vec3 beamX = vecCross(beamY, toHead);
            float sideLen = sqrtf(beamX.x * beamX.x + beamX.y * beamX.y + beamX.z * beamX.z);

            // A quad has one orientation, so the ribbon is turned to face the
            // head. Aimed nearly along the line of sight there is no such
            // direction to find, and any perpendicular will do: the ribbon is
            // edge on either way. This used to give up instead, which is why
            // the ray vanished over the lower half of the screen.
            if (sideLen < 0.15f) {
                Vec3 up = { 0.0f, 1.0f, 0.0f };
                beamX = vecCross(beamY, up);
                sideLen = sqrtf(beamX.x * beamX.x + beamX.y * beamX.y + beamX.z * beamX.z);
                if (sideLen < 0.15f) {
                    Vec3 side = { 1.0f, 0.0f, 0.0f };
                    beamX = vecCross(beamY, side);
                }
            }

            if (length > 0.10f) {
                beamX = vecNorm(beamX);
                Vec3 beamZ = vecCross(beamX, beamY);

                // Pulled back from the true target point on purpose, same
                // look as Quest's own system ray - it points at the target
                // without visually touching it. Geometric, not just relying
                // on the texture's own end-fade, since which physical end
                // (hand vs target) lands on which texture row isn't
                // guaranteed (see the comment on uploadPointerArt's
                // lengthFade) - shortening the quad itself from the target
                // end guarantees the gap regardless of that mapping.
                const float END_GAP_M = 0.025f;
                float renderedLength = length > END_GAP_M * 2.0f ? length - END_GAP_M : length;
                Vec3 renderedMid = { start.x + beamY.x * (renderedLength * 0.5f),
                                     start.y + beamY.y * (renderedLength * 0.5f),
                                     start.z + beamY.z * (renderedLength * 0.5f) };

                memset(&beamLayer, 0, sizeof(beamLayer));
                beamLayer.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
                beamLayer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                beamLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                beamLayer.subImage.swapchain = ctx->pointerSwapchain;
                beamLayer.subImage.imageRect.offset.x = 0;
                beamLayer.subImage.imageRect.offset.y = 0;
                beamLayer.subImage.imageRect.extent.width = PTR_TEX_W;
                beamLayer.subImage.imageRect.extent.height = PTR_BEAM_H;
                beamLayer.subImage.imageArrayIndex = 0;
                beamLayer.space = space;
                beamLayer.pose.orientation = quatFromBasis(beamX, beamY, beamZ);
                beamLayer.pose.position.x = renderedMid.x;
                beamLayer.pose.position.y = renderedMid.y;
                beamLayer.pose.position.z = renderedMid.z;
                beamLayer.size.width = ctx->beamWidth;
                beamLayer.size.height = renderedLength;
                layers[layerCount++] = (const XrCompositionLayerBaseHeader*)&beamLayer;

            }

            // Cursor sits just off the surface facing the viewer, which works
            // on the cylinder as well as the flat screen. Independent of the
            // ribbon: a gaze has a cursor and no ray. A ray aimed at nothing
            // still gets one too (beamFree) - just a bigger, softer one,
            // since there's nothing to precisely land on and a small dot
            // reads as more precise than the aim actually is right now.
            {
                Vec3 dotZ = vecNorm(vecSub(head, end));
                Vec3 worldUp = { 0.0f, 1.0f, 0.0f };
                Vec3 dotX = vecNorm(vecCross(worldUp, dotZ));
                Vec3 dotY = vecCross(dotZ, dotX);

                memset(&dotLayer, 0, sizeof(dotLayer));
                dotLayer.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
                dotLayer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                dotLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                dotLayer.subImage.swapchain = ctx->pointerSwapchain;
                dotLayer.subImage.imageRect.offset.x = 0;
                dotLayer.subImage.imageRect.offset.y = PTR_BEAM_H;
                dotLayer.subImage.imageRect.extent.width = PTR_TEX_W;
                dotLayer.subImage.imageRect.extent.height = PTR_DOT_H;
                dotLayer.subImage.imageArrayIndex = 0;
                dotLayer.space = space;
                dotLayer.pose.orientation = quatFromBasis(dotX, dotY, dotZ);
                dotLayer.pose.position.x = end.x + dotZ.x * 0.012f;
                dotLayer.pose.position.y = end.y + dotZ.y * 0.012f;
                dotLayer.pose.position.z = end.z + dotZ.z * 0.012f;
                // On-target size shrunk alongside beamWidth above, same
                // reasoning - Quest's own system cursor for 2D panels reads
                // noticeably smaller than this used to be. Free-aim stays
                // close to that original pre-shrink size.
                float dotSize = ctx->beamFree ? 0.022f : 0.014f;
                dotLayer.size.width = dotSize;
                dotLayer.size.height = dotSize;
                layers[layerCount++] = (const XrCompositionLayerBaseHeader*)&dotLayer;
            }
        }
      }

      // Top bar: exit + brightness, one shared module for both modes (see
      // topBarPose()). Always shown, same as Productivity Mode's original -
      // it is not a hover-revealed handle like the bottom move bar.
      if (ctx->topBarReady) {
        XrPosef barPose = topBarPose(ctx);
        memset(&topBarLayer, 0, sizeof(topBarLayer));
        topBarLayer.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
        topBarLayer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        topBarLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        topBarLayer.subImage.swapchain = ctx->topBarSwapchain;
        topBarLayer.subImage.imageRect.offset.x = 0;
        topBarLayer.subImage.imageRect.offset.y = 0;
        topBarLayer.subImage.imageRect.extent.width = TOPBAR_TEX_W;
        topBarLayer.subImage.imageRect.extent.height = TOPBAR_TEX_H;
        topBarLayer.subImage.imageArrayIndex = 0;
        topBarLayer.space = space;
        topBarLayer.pose = barPose;
        topBarLayer.size.width = TOPBAR_WIDTH_M + TOPBAR_BG_MARGIN_M * 2.0f;
        topBarLayer.size.height = TOPBAR_ITEM_SIZE_M + TOPBAR_BG_MARGIN_M * 2.0f;
        layers[layerCount++] = (const XrCompositionLayerBaseHeader*)&topBarLayer;

        if (ctx->openSlider != TOPBAR_NO_SLIDER && ctx->sliderArtReady) {
            XrPosef trackPose = topBarSliderPose(ctx, ctx->openSlider);
            float sliderValue = ctx->openSlider == TOPBAR_BRIGHTNESS_INDEX
                    ? ctx->passthroughLevel : ctx->curveAmount;
            memset(&sliderTrackLayer, 0, sizeof(sliderTrackLayer));
            sliderTrackLayer.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
            sliderTrackLayer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            sliderTrackLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            sliderTrackLayer.subImage.swapchain = ctx->sliderTrackSwapchain;
            sliderTrackLayer.subImage.imageRect.offset.x = 0;
            sliderTrackLayer.subImage.imageRect.offset.y = 0;
            sliderTrackLayer.subImage.imageRect.extent.width = SLIDER_TRACK_TEX_W;
            sliderTrackLayer.subImage.imageRect.extent.height = SLIDER_TRACK_TEX_H;
            sliderTrackLayer.subImage.imageArrayIndex = 0;
            sliderTrackLayer.space = space;
            sliderTrackLayer.pose = trackPose;
            sliderTrackLayer.size.width = SLIDER_TRACK_WIDTH_M;
            sliderTrackLayer.size.height = SLIDER_TRACK_HEIGHT_M;
            layers[layerCount++] = (const XrCompositionLayerBaseHeader*)&sliderTrackLayer;

            // Placement only - the thumb's own art never changes, so dragging
            // it costs nothing beyond this per-frame pose update.
            Vec3 thumbLocal = { (sliderValue - 0.5f) * SLIDER_TRACK_WIDTH_M, 0.0f,
                                0.002f };
            Vec3 thumbOffset = quatRotate(trackPose.orientation, thumbLocal);
            memset(&sliderThumbLayer, 0, sizeof(sliderThumbLayer));
            sliderThumbLayer.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
            sliderThumbLayer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            sliderThumbLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            sliderThumbLayer.subImage.swapchain = ctx->sliderThumbSwapchain;
            sliderThumbLayer.subImage.imageRect.offset.x = 0;
            sliderThumbLayer.subImage.imageRect.offset.y = 0;
            sliderThumbLayer.subImage.imageRect.extent.width = SLIDER_THUMB_TEX;
            sliderThumbLayer.subImage.imageRect.extent.height = SLIDER_THUMB_TEX;
            sliderThumbLayer.subImage.imageArrayIndex = 0;
            sliderThumbLayer.space = space;
            sliderThumbLayer.pose.orientation = trackPose.orientation;
            sliderThumbLayer.pose.position.x = trackPose.position.x + thumbOffset.x;
            sliderThumbLayer.pose.position.y = trackPose.position.y + thumbOffset.y;
            sliderThumbLayer.pose.position.z = trackPose.position.z + thumbOffset.z;
            sliderThumbLayer.size.width = SLIDER_THUMB_SIZE_M;
            sliderThumbLayer.size.height = SLIDER_THUMB_SIZE_M;
            layers[layerCount++] = (const XrCompositionLayerBaseHeader*)&sliderThumbLayer;

            // Glow on/off - only next to the brightness slider, never curve's
            if (ctx->openSlider == TOPBAR_BRIGHTNESS_INDEX && ctx->glowToggleReady) {
                XrPosef togglePose = topBarGlowTogglePose(ctx);
                memset(&glowToggleLayer, 0, sizeof(glowToggleLayer));
                glowToggleLayer.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
                glowToggleLayer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                glowToggleLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                glowToggleLayer.subImage.swapchain = ctx->glowToggleSwapchain;
                glowToggleLayer.subImage.imageRect.offset.x = 0;
                glowToggleLayer.subImage.imageRect.offset.y = 0;
                glowToggleLayer.subImage.imageRect.extent.width = OUTLINE_TEX;
                glowToggleLayer.subImage.imageRect.extent.height = OUTLINE_TEX;
                glowToggleLayer.subImage.imageArrayIndex = 0;
                glowToggleLayer.space = space;
                glowToggleLayer.pose = togglePose;
                glowToggleLayer.size.width = GLOW_TOGGLE_SIZE_M;
                glowToggleLayer.size.height = GLOW_TOGGLE_SIZE_M;
                layers[layerCount++] = (const XrCompositionLayerBaseHeader*)&glowToggleLayer;
            }
        }
      }

      // Whole-view fade, submitted dead last so painter's-algorithm order
      // puts it in front of literally everything above - the screen and top
      // bar included, not just the room. See FADE_IN/FADE_OUT.
      if (ctx->fadeReady && ctx->equirectSupported && ctx->fadeAlpha > 0.001f) {
        memset(&fadeLayer, 0, sizeof(fadeLayer));
        fadeLayer.type = XR_TYPE_COMPOSITION_LAYER_EQUIRECT2_KHR;
        fadeLayer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        fadeLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        fadeLayer.space = space;
        fadeLayer.subImage.swapchain = ctx->fadeSwapchain;
        fadeLayer.subImage.imageRect.offset.x = 0;
        fadeLayer.subImage.imageRect.offset.y = 0;
        fadeLayer.subImage.imageRect.extent.width = DIM_TEX;
        fadeLayer.subImage.imageRect.extent.height = DIM_TEX;
        fadeLayer.subImage.imageArrayIndex = 0;
        fadeLayer.pose.orientation.w = 1.0f;
        fadeLayer.radius = 0.0f;
        fadeLayer.centralHorizontalAngle = 6.2831853f;
        fadeLayer.upperVerticalAngle = 1.5707963f;
        fadeLayer.lowerVerticalAngle = -1.5707963f;
        layers[layerCount++] = (const XrCompositionLayerBaseHeader*)&fadeLayer;
      }
    }

    endInfo.layerCount = layerCount;
    endInfo.layers = layers;
    checkXr(xrEndFrame(ctx->session, &endInfo), "xrEndFrame");
}

JNIEXPORT void JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeDestroy(JNIEnv* env, jobject thiz, jlong handle) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx == NULL) {
        return;
    }
    destroyCtx(env, ctx);
    LOGI("OpenXR renderer destroyed");
}
