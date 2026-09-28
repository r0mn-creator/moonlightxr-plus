package com.limelight.binding.video;

import android.app.Activity;
import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.BlurMaskFilter;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.PorterDuff;
import android.graphics.RectF;
import android.graphics.SurfaceTexture;
import android.graphics.Typeface;
import android.preference.PreferenceManager;
import android.view.Surface;

import com.limelight.LimeLog;
import com.limelight.R;
import com.limelight.preferences.PreferenceConfiguration;

import java.io.File;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;

/**
 * Presents the decoded stream in an OpenXR session. Same input contract as
 * GlPassthroughRenderer: the decoder renders into our SurfaceTexture, and we
 * consume it from the frame loop thread. All OpenXR work happens in native
 * code, this class owns the thread and the SurfaceTexture plumbing.
 */
public class XrRenderer implements SurfaceTexture.OnFrameAvailableListener {

    static {
        System.loadLibrary("xr-renderer");
    }

    private static final int FRAME_EXIT = -1;
    private static final int FRAME_IDLE = 0;
    private static final int FRAME_RENDER = 1;

    private static final int DEPTH_MODE_OFF = 0;
    private static final int DEPTH_MODE_MODEL = 6;

    // Averaged over this many inferences before hitting logcat
    private static final int DEPTH_STATS_INTERVAL = 30;
    private static final int DEPTH_AGE_INTERVAL = 300;

    // Matches OVERLAY_WIDTH and OVERLAY_HEIGHT in xr_renderer.c
    private static final int OVERLAY_WIDTH = 768;
    private static final int OVERLAY_HEIGHT = 512;
    private static final float OVERLAY_TEXT_SIZE = 22.0f;
    private static final float OVERLAY_LINE_HEIGHT = 28.0f;

    // Matches PRODUCTIVITY_SCREEN_COUNT in xr_renderer.c
    private static final int PRODUCTIVITY_SCREEN_COUNT = 3;

    private long nativeCtx;
    private Thread renderThread;
    private Thread depthThread;
    private SurfaceTexture surfaceTexture;
    private Surface inputSurface;

    // One independent OES texture/SurfaceTexture per PMode screen, each fed
    // by its own background-process decoder (PModeScreenServiceBase) -
    // replaces the old design where one shared decoded frame was
    // column-cropped into N quads. Only populated when productivityMode.
    private final SurfaceTexture[] productivitySurfaceTextures = new SurfaceTexture[PRODUCTIVITY_SCREEN_COUNT];
    private final Surface[] productivityInputSurfaces = new Surface[PRODUCTIVITY_SCREEN_COUNT];
    private final AtomicInteger[] productivityPendingFrames = new AtomicInteger[PRODUCTIVITY_SCREEN_COUNT];
    private final float[][] productivityTexMatrix = new float[PRODUCTIVITY_SCREEN_COUNT][16];

    private final AtomicInteger pendingFrames = new AtomicInteger(0);
    private final float[] texMatrix = new float[16];
    private volatile boolean stopping;
    private long videoFrameIndex;

    // Handoff to the depth thread. The frame loop fills the model input and
    // sets pending, the depth thread runs inference and uploads the result.
    // If it is still busy when the next frame is due, the frame loop skips
    // rather than waits, so depth just runs at whatever rate it manages.
    private final Object depthLock = new Object();
    private boolean depthPending;
    private boolean depthBusy;
    private boolean depthExit;
    private int skippedFrames;
    private volatile boolean depthReady;
    private volatile long lastCaptureNs;

    // How far behind the picture the depth map is. The map warping a frame was
    // computed from an earlier one, and then reused until the next inference
    // lands, so during camera motion it is spatially offset from the colour it
    // is warping. Measured rather than assumed: these are the frame index and
    // clock reading of the frame the live depth map came from.
    private long captureFrameIndex;
    private long captureFrameNs;
    private volatile long publishedFrameIndex;
    private volatile long publishedFrameNs;

    // Stats overlay. Text is drawn to a bitmap on whichever thread reports the
    // stats, then handed to the frame loop, which owns the GL context. Two
    // buffers so the drawing side never writes one the renderer is reading.
    private final AtomicReference<ByteBuffer> pendingOverlay = new AtomicReference<>();
    private ByteBuffer[] overlayBuffers;
    private int overlayBufferIndex;
    private Bitmap overlayBitmap;
    private Canvas overlayCanvas;
    private Paint overlayPaint;
    private volatile float lastInferenceMs;
    private volatile float lastDepthAgeMs;
    private volatile int lastDepthSkips;

    // Controller pointer. The native side does the ray maths and hands back a
    // hit point and a button mask, this side turns that into host events.
    private static final int IN_HIT = 0;
    private static final int IN_U = 1;
    private static final int IN_V = 2;
    private static final int IN_BUTTONS = 3;
    private static final int IN_SCROLL = 4;
    private static final int IN_POSE_DIRTY = 6;
    // Passthrough brightness slider, both modes - only sent when a drag just
    // ended (IN_PASSTHROUGH_DIRTY), same one-shot pattern as IN_POSE_DIRTY.
    private static final int IN_PASSTHROUGH_LEVEL = 7;
    private static final int IN_POSE = 8;
    // Top menu bar exit button, pressed this frame - shared by both modes
    private static final int IN_EXIT_PRESSED = 18;
    // Spatial audio: -1..1 left/right balance and 0..1 distance gain
    private static final int IN_AUDIO_PAN = 19;
    private static final int IN_AUDIO_GAIN = 20;
    // Phase 2: pointer/click on the 3 PMode screens themselves, not just the
    // exit button. IN_PMODE_SCREEN is -1 when nothing is being pointed at.
    private static final int IN_PMODE_HIT = 21;
    private static final int IN_PMODE_SCREEN = 22;
    private static final int IN_PMODE_U = 23;
    private static final int IN_PMODE_V = 24;
    private static final int IN_PMODE_BUTTONS = 25;
    // Reuses slot 17 (the environment-picker's old pick slot, retired with
    // it) - see IN_PASSTHROUGH_LEVEL above.
    private static final int IN_PASSTHROUGH_DIRTY = 17;
    // Screen curvature slider, same one-shot pattern as IN_PASSTHROUGH_*
    private static final int IN_CURVE_LEVEL = 26;
    private static final int IN_CURVE_DIRTY = 27;
    // Top bar keyboard button, pressed this frame - shows/hides the system
    // soft keyboard, same as the flat-mode gesture already does
    private static final int IN_KEYBOARD_TOGGLE = 28;
    // Top bar 3D-effect button, pressed this frame - a request to flip it,
    // not the resulting state (this class owns and echoes the real value
    // back via nativeSetDepthEffect, same shape as IN_KEYBOARD_TOGGLE)
    private static final int IN_DEPTH_TOGGLE = 29;
    // Glow on/off, next to the brightness slider - only meaningful (and
    // only hit-tested natively) while that slider is open
    private static final int IN_GLOW_TOGGLE = 30;
    // Top bar ultra-wide button, pressed this frame - a request to flip it,
    // same fire-and-forget shape as IN_KEYBOARD_TOGGLE. Unlike the other
    // toggles this always routes to the InputListener (Game), never handled
    // locally, since it needs a reconnect, not just a render-side flag flip.
    private static final int IN_ULTRAWIDE_TOGGLE = 31;
    private static final int IN_SLOTS = 32;
    private static final int POSE_VALUES = 9;
    private final float[] inputState = new float[IN_SLOTS];
    private int heldButtons;
    private int pmodeHeldButtons;
    private int pmodeLastScreenIndex = -1;
    private InputListener inputListener;
    private Context prefsContext;

    // Top menu bar: exit, brightness, curve - one shared module for both
    // modes (see topBarPose() natively - the only difference between them is
    // which screen(s) it floats above). One bitmap, one cell per item, built
    // once and uploaded whole.
    private final AtomicReference<ByteBuffer> pendingTopBarArt = new AtomicReference<>();
    // Keyboard icon pulled for now - see the matching comment in
    // xr_renderer.c next to TOPBAR_ITEM_COUNT for why.
    private static final int TOPBAR_ITEM_COUNT = 5;
    private static final int TOPBAR_EXIT_INDEX = 0;
    private static final int TOPBAR_BRIGHTNESS_INDEX = 1;
    private static final int TOPBAR_CURVE_INDEX = 2;
    private static final int TOPBAR_DEPTH_INDEX = 3;
    private static final int TOPBAR_ULTRAWIDE_INDEX = 4;
    // Live 3D-effect state - toggled from the top bar, started from
    // PreferenceConfiguration.VR_DEPTH_EFFECT_PREF_STRING. Read by
    // buildTopBarArt() to pick which of the two icon variants to draw.
    private volatile boolean depthEffectOn = true;
    // Ultra-wide is fixed for the life of this XrRenderer instance (a
    // reconnect is required to change it - see onVrUltrawideToggleRequested
    // in Game.java), so this is only ever read once at session start to pick
    // the right icon; the toggle itself never flips this field directly.
    private boolean ultrawideOn = false;
    // True unless the advanced "Realtime 3D mode" list was explicitly set
    // to a debug test pattern - those don't run the real inference thread,
    // so the top bar's toggle only starts/stops it when this is true.
    private boolean depthModelCapable = true;
    // Live ambient-glow on/off - toggled from the icon next to the
    // brightness slider, started from
    // PreferenceConfiguration.VR_GLOW_ENABLED_PREF_STRING. Its icon is its
    // own single small texture, not part of the topbar strip, since it
    // isn't part of the fixed icon row.
    private volatile boolean glowEnabled = true;
    private final AtomicReference<ByteBuffer> pendingGlowToggleArt = new AtomicReference<>();
    // Matches OUTLINE_TEX in xr_renderer.c - size of one cell in the strip
    private static final int TOPBAR_CELL_TEX = 128;
    // Bleed margin for the background pill's feather - matches
    // TOPBAR_TEX_MARGIN natively, which sizes the swapchain/quad to match
    private static final int TOPBAR_TEX_MARGIN = 40;

    /**
     * Pointer events out of the VR session. Called on the frame loop thread.
     * Buttons are 0 left, 1 right, 2 middle.
     */
    public interface InputListener {
        void onVrPointerMove(float u, float v);
        void onVrButton(int button, boolean down);
        void onVrScroll(int clicks);
        // Top menu bar exit button. There's no Android back gesture inside
        // an immersive session, so this is the way out.
        void onVrExitRequested();
        // Top menu bar keyboard button - shows/hides the system soft
        // keyboard, same as the existing flat-mode gesture.
        void onVrKeyboardToggleRequested();
        // Top menu bar ultra-wide toggle. Resolution is fixed for the life
        // of a connection, so unlike the other top-bar toggles this can't
        // just flip a rendering flag in place - the listener (Game) has to
        // tear down and reconnect at the new width.
        void onVrUltrawideToggleRequested();
        // Desktop audio balance/gain relative to the user's head and the
        // centre screen. Always (0, 1) outside Productivity mode.
        void onVrSpatialAudio(float pan, float gain);
        // Pointer/click on one of the N PMode screens (screenIndex 0-based).
        // Never fires outside Productivity mode.
        void onVrProductivityPointerMove(int screenIndex, float u, float v);
        void onVrProductivityButton(int screenIndex, int button, boolean down);
    }

    public void setInputListener(InputListener listener) {
        this.inputListener = listener;
    }

    private native long nativeInit(Activity activity, int width, int height, int stereoMode,
                                   boolean depthDebug, int convergence, int depthScale,
                                   boolean productivityMode);
    private native void nativeSetCaptureDir(long ctx, String dir);
    private native int nativeGetTexId(long ctx);
    private native ByteBuffer nativeGetModelInput(long ctx);
    private native ByteBuffer nativeGetModelOutput(long ctx);
    private native long nativeCaptureDepthInput(long ctx, float[] texMatrix);
    private native long nativeUploadDepth(long ctx);
    private native boolean nativeBindDepthContext(long ctx);
    private native void nativeUnbindDepthContext(long ctx);
    private native int nativeWaitBeginFrame(long ctx);
    private native void nativeEndFrame(long ctx, boolean newFrame, float[] texMatrix,
                                       float distance, float quadWidth,
                                       boolean headLocked, float separation, boolean eyeSwap,
                                       boolean passthrough);
    private native void nativeUpdateInput(long ctx, float distance, float quadWidth,
                                          boolean headLocked,
                                          boolean pointerEnabled, boolean gazeEnabled,
                                          float[] out);
    private native void nativeSetScreenPose(long ctx, float[] pose);
    private native void nativeUploadTopBarArt(long ctx, ByteBuffer strip);
    private native void nativeSetPassthroughLevel(long ctx, float level);
    private native void nativeSetCurvature(long ctx, float amount);
    private native void nativeSetDepthEffect(long ctx, boolean on);
    private native void nativeSetGlowEnabled(long ctx, boolean on);
    private native void nativeUploadGlowToggleArt(long ctx, ByteBuffer icon);
    private native void nativeUploadOverlay(long ctx, ByteBuffer pixels, int width, int height);
    private native float nativeGetWarpGpuMs(long ctx);
    private native void nativeDestroy(long ctx);
    private native int nativeGetProductivityTexId(long ctx, int screenIndex);
    private native void nativeUpdateProductivityTexture(long ctx, int screenIndex, float[] texMatrix);

    public boolean start(final Activity activity, final int videoWidth, final int videoHeight,
                         final PreferenceConfiguration prefs) {
        final CountDownLatch initLatch = new CountDownLatch(1);
        final boolean[] initOk = new boolean[1];

        renderThread = new Thread() {
            @Override
            public void run() {
                // Always init at least MODEL-capable (never OFF) so the
                // swapchain is allocated stereo-sized regardless of the
                // saved 3D-effect toggle - that's what lets the top bar
                // flip it on live later without resizing anything. An
                // explicit debug pattern (flat/ramp/blob/eyetest/shifttest)
                // from the advanced list is still respected as-is.
                int nativeStereoMode = prefs.vrDepthMode == DEPTH_MODE_OFF
                        ? DEPTH_MODE_MODEL : prefs.vrDepthMode;
                depthModelCapable = nativeStereoMode == DEPTH_MODE_MODEL;
                nativeCtx = nativeInit(activity, videoWidth, videoHeight, nativeStereoMode,
                        prefs.vrDepthDebug, prefs.vrConvergence, prefs.vrDepthScale,
                        prefs.productivityMode);
                if (nativeCtx == 0) {
                    initLatch.countDown();
                    return;
                }

                prefsContext = activity.getApplicationContext();
                restoreScreenPose();
                depthEffectOn = prefs.vrDepthEffect;
                ultrawideOn = prefs.vrUltrawide;
                pendingTopBarArt.set(toBuffer(buildTopBarArt()));
                nativeSetPassthroughLevel(nativeCtx, PreferenceManager
                        .getDefaultSharedPreferences(prefsContext)
                        .getFloat(PreferenceConfiguration.VR_PASSTHROUGH_LEVEL_PREF_STRING, 1.0f));
                nativeSetCurvature(nativeCtx, prefs.vrCurvature / 100.0f);
                nativeSetDepthEffect(nativeCtx, depthEffectOn);
                glowEnabled = PreferenceManager.getDefaultSharedPreferences(prefsContext)
                        .getBoolean(PreferenceConfiguration.VR_GLOW_ENABLED_PREF_STRING, true);
                pendingGlowToggleArt.set(toBuffer(buildGlowToggleArt()));
                nativeSetGlowEnabled(nativeCtx, glowEnabled);

                File captureDir = activity.getExternalFilesDir(null);
                if (captureDir != null) {
                    nativeSetCaptureDir(nativeCtx, captureDir.getAbsolutePath());
                }

                // The EGL context is current on this thread now, so the
                // SurfaceTexture attaches to it here
                surfaceTexture = new SurfaceTexture(nativeGetTexId(nativeCtx));
                surfaceTexture.setDefaultBufferSize(videoWidth, videoHeight);
                surfaceTexture.setOnFrameAvailableListener(XrRenderer.this);
                inputSurface = new Surface(surfaceTexture);

                if (prefs.productivityMode) {
                    // Each screen gets its own independent decoded resolution,
                    // not a slice of the combined videoWidth (that number only
                    // sizes the output swapchain - see xr_renderer.c).
                    int screenWidth = videoWidth / PRODUCTIVITY_SCREEN_COUNT;
                    for (int i = 0; i < PRODUCTIVITY_SCREEN_COUNT; i++) {
                        final int screenIndex = i;
                        SurfaceTexture st = new SurfaceTexture(
                                nativeGetProductivityTexId(nativeCtx, screenIndex));
                        st.setDefaultBufferSize(screenWidth, videoHeight);
                        st.setOnFrameAvailableListener(new SurfaceTexture.OnFrameAvailableListener() {
                            @Override
                            public void onFrameAvailable(SurfaceTexture surfaceTexture) {
                                productivityPendingFrames[screenIndex].incrementAndGet();
                            }
                        });
                        productivitySurfaceTextures[screenIndex] = st;
                        productivityInputSurfaces[screenIndex] = new Surface(st);
                        productivityPendingFrames[screenIndex] = new AtomicInteger(0);
                    }
                }

                if (nativeStereoMode == DEPTH_MODE_MODEL && depthEffectOn) {
                    startDepthThread(activity);
                }

                initOk[0] = true;
                initLatch.countDown();

                runFrameLoop(prefs);

                stopDepthThread();

                // Tear down on the same thread that owns the GL context.
                // The SurfaceTexture and Surface stay alive for the codec
                // until cleanup().
                long ctx = nativeCtx;
                nativeCtx = 0;
                nativeDestroy(ctx);
            }
        };
        renderThread.setName("Video - XR Renderer");
        renderThread.start();

        boolean initFinished;
        try {
            // Session setup can take a moment on a cold runtime
            initFinished = initLatch.await(5, TimeUnit.SECONDS);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            initFinished = false;
        }

        if (!initFinished || !initOk[0]) {
            LimeLog.severe("XR renderer init failed");
            prepareForStop();
            cleanup();
            return false;
        }

        LimeLog.info("XR renderer initialized at "+videoWidth+"x"+videoHeight);
        return true;
    }

    /**
     * Inference is longer than a display frame, so it lives on its own
     * thread with its own context in the render context's share group. The
     * frame loop hands over a captured frame and carries on submitting.
     */
    private void startDepthThread(final Context activity) {
        // Originally start-once/stop-once per session; the top bar's live
        // toggle can now call this again after a stop, so the exit/pending
        // state from any previous run needs clearing first.
        synchronized (depthLock) {
            depthExit = false;
            depthPending = false;
            depthBusy = false;
        }
        depthThread = new Thread() {
            @Override
            public void run() {
                if (!nativeBindDepthContext(nativeCtx)) {
                    return;
                }

                DepthSource source = null;
                try {
                    ByteBuffer input = nativeGetModelInput(nativeCtx);
                    ByteBuffer output = nativeGetModelOutput(nativeCtx);
                    if (input == null || output == null) {
                        LimeLog.severe("Depth staging buffers missing");
                        return;
                    }

                    source = new MidasDepthSource();
                    if (!source.initialize(activity, input, output)) {
                        // The depth texture keeps the flat map it was
                        // initialized with, so zero disparity, and the
                        // stream stays watchable
                        LimeLog.severe("Depth source init failed, stereo will be flat");
                        return;
                    }

                    depthReady = true;
                    runDepthLoop(source);
                } finally {
                    depthReady = false;
                    if (source != null) {
                        source.release();
                    }
                    nativeUnbindDepthContext(nativeCtx);
                }
            }
        };
        depthThread.setName("Video - XR Depth");
        depthThread.start();
    }

    private void runDepthLoop(DepthSource source) {
        long runs = 0, skipped = 0;
        long inferenceNs = 0, uploadNs = 0, captureNs = 0, worstNs = 0;

        while (true) {
            synchronized (depthLock) {
                while (!depthPending && !depthExit) {
                    try {
                        depthLock.wait();
                    } catch (InterruptedException e) {
                        Thread.currentThread().interrupt();
                        return;
                    }
                }
                if (depthExit) {
                    return;
                }
                depthPending = false;
                depthBusy = true;
            }

            long start = System.nanoTime();
            long upload = 0;
            boolean ok = source.estimate();
            if (ok) {
                upload = nativeUploadDepth(nativeCtx);
                publishedFrameIndex = captureFrameIndex;
                publishedFrameNs = captureFrameNs;
            }

            synchronized (depthLock) {
                depthBusy = false;
                skipped += skippedFrames;
                skippedFrames = 0;
            }

            if (!ok) {
                continue;
            }

            captureNs += lastCaptureNs;
            inferenceNs += (long)(source.getLastInferenceMs() * 1000000.0f);
            lastInferenceMs = source.getLastInferenceMs();
            uploadNs += upload;
            long total = System.nanoTime() - start;
            if (total > worstNs) {
                worstNs = total;
            }
            if (++runs == DEPTH_STATS_INTERVAL) {
                LimeLog.info("Depth stage ("+(source.isGpuAccelerated() ? "GPU" : "CPU")
                        +"): capture "+msPer(captureNs, runs)
                        +" ms, inference "+msPer(inferenceNs, runs)
                        +" ms, upload "+msPer(uploadNs, runs)
                        +" ms, worst "+msPer(worstNs, 1)
                        +" ms, frames skipped while busy "+skipped);
                lastDepthSkips = (int)skipped;
                runs = 0;
                skipped = 0;
                captureNs = inferenceNs = uploadNs = worstNs = 0;
            }
        }
    }

    private void stopDepthThread() {
        if (depthThread == null) {
            return;
        }
        synchronized (depthLock) {
            depthExit = true;
            depthLock.notifyAll();
        }
        try {
            depthThread.join(2000);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
        if (depthThread.isAlive()) {
            LimeLog.warning("XR depth thread did not stop in time");
        }
        depthThread = null;
    }

    private void runFrameLoop(PreferenceConfiguration prefs) {
        float distance = prefs.vrDistance / 10.0f;
        float quadWidth = prefs.vrScreenSize / 10.0f;
        boolean headLocked = prefs.vrHeadLocked;
        // Stored as tenths of a percent of frame width
        float separation = prefs.vrStereoSeparation / 1000.0f;
        boolean eyeSwap = prefs.vrEyeSwap;
        boolean pointer = prefs.vrPointer;
        boolean gaze = prefs.vrGaze;
        int cadence = Math.max(1, prefs.vrInferenceCadence);

        long ageFrames = 0, ageNs = 0, ageSamples = 0, worstAgeNs = 0;

        while (!stopping) {
            int r = nativeWaitBeginFrame(nativeCtx);
            if (r == FRAME_EXIT) {
                break;
            }
            if (r == FRAME_IDLE) {
                // Native side slept already while the session is not running
                continue;
            }

            nativeUpdateInput(nativeCtx, distance, quadWidth, headLocked,
                    pointer, gaze, inputState);
            dispatchInput();

            if (prefs.productivityMode) {
                // Each screen only pulls a new frame from its own decoder
                // when one is actually pending - matches how the single-
                // screen path below only calls updateTexImage() on newFrame,
                // so an idle screen just keeps showing its last frame.
                for (int i = 0; i < PRODUCTIVITY_SCREEN_COUNT; i++) {
                    if (productivityPendingFrames[i].getAndSet(0) > 0) {
                        SurfaceTexture st = productivitySurfaceTextures[i];
                        st.updateTexImage();
                        st.getTransformMatrix(productivityTexMatrix[i]);
                        nativeUpdateProductivityTexture(nativeCtx, i, productivityTexMatrix[i]);
                    }
                }
            }

            boolean newFrame = pendingFrames.getAndSet(0) > 0;
            if (newFrame) {
                surfaceTexture.updateTexImage();
                surfaceTexture.getTransformMatrix(texMatrix);

                if (depthReady) {
                    if ((videoFrameIndex % cadence) == 0) {
                        handOffDepthFrame();
                    }
                    if (publishedFrameNs != 0) {
                        long age = System.nanoTime() - publishedFrameNs;
                        // Smoothed for the overlay, the raw value swings a lot
                        // between one inference landing and the next
                        float ageMs = age / 1000000.0f;
                        lastDepthAgeMs = lastDepthAgeMs == 0.0f ? ageMs
                                : lastDepthAgeMs * 0.95f + ageMs * 0.05f;
                        ageFrames += videoFrameIndex - publishedFrameIndex;
                        ageNs += age;
                        ageSamples++;
                        if (age > worstAgeNs) {
                            worstAgeNs = age;
                        }
                        if (ageSamples == DEPTH_AGE_INTERVAL) {
                            LimeLog.info("Depth age: "+String.format("%.1f", ageFrames
                                    / (double)ageSamples)+" video frames, "
                                    +msPer(ageNs, ageSamples)+" ms avg, "
                                    +msPer(worstAgeNs, 1)+" ms worst");
                            ageFrames = ageNs = ageSamples = worstAgeNs = 0;
                        }
                    }
                }
                videoFrameIndex++;
            }
            // Upload here rather than from the reporting thread, since this is
            // the thread that owns the GL context
            ByteBuffer overlay = pendingOverlay.getAndSet(null);
            if (overlay != null) {
                nativeUploadOverlay(nativeCtx, overlay, OVERLAY_WIDTH, OVERLAY_HEIGHT);
            }

            ByteBuffer topBarArt = pendingTopBarArt.getAndSet(null);
            if (topBarArt != null) {
                nativeUploadTopBarArt(nativeCtx, topBarArt);
            }

            ByteBuffer glowToggleArt = pendingGlowToggleArt.getAndSet(null);
            if (glowToggleArt != null) {
                nativeUploadGlowToggleArt(nativeCtx, glowToggleArt);
            }

            nativeEndFrame(nativeCtx, newFrame, texMatrix, distance, quadWidth,
                    headLocked, separation, eyeSwap, true);
        }
    }

    /**
     * The top bar's icon strip: one TOPBAR_CELL_TEX-wide cell per item, drawn
     * once and uploaded whole. Shared by both modes - see topBarPose() and
     * updateTopBar() natively, which only differ in which screen(s) the bar
     * floats above.
     */
    private Bitmap buildTopBarArt() {
        int iconAreaW = TOPBAR_CELL_TEX * TOPBAR_ITEM_COUNT;
        int iconAreaH = TOPBAR_CELL_TEX;
        Bitmap strip = Bitmap.createBitmap(iconAreaW + TOPBAR_TEX_MARGIN * 2,
                                           iconAreaH + TOPBAR_TEX_MARGIN * 2,
                                           Bitmap.Config.ARGB_8888);
        Canvas canvas = new Canvas(strip);
        canvas.drawColor(0, PorterDuff.Mode.CLEAR);

        // A soft, mostly-see-through pill so the icons stay legible against a
        // bright wall. Sized to the icon area itself; the blur is what
        // carries it out into the margin the bitmap was padded with above.
        RectF pillArea = new RectF(TOPBAR_TEX_MARGIN, TOPBAR_TEX_MARGIN,
                                   TOPBAR_TEX_MARGIN + iconAreaW, TOPBAR_TEX_MARGIN + iconAreaH);
        drawTopBarBackground(canvas, pillArea);

        // Both icons are pre-made assets (exit: a plain white glyph; brightness:
        // a two-tone glyph depicting a screen occluding the room behind it, for
        // the passthrough slider) rather than drawn here - see
        // res/drawable-nodpi/ic_topbar_*.png.
        drawIcon(canvas, R.drawable.ic_topbar_exit, cellRect(TOPBAR_EXIT_INDEX));
        drawIcon(canvas, R.drawable.ic_topbar_brightness, cellRect(TOPBAR_BRIGHTNESS_INDEX));
        drawIcon(canvas, R.drawable.ic_topbar_curve, cellRect(TOPBAR_CURVE_INDEX));
        drawIcon(canvas, depthEffectOn ? R.drawable.ic_topbar_3d_on : R.drawable.ic_topbar_3d_off,
                cellRect(TOPBAR_DEPTH_INDEX));
        drawIcon(canvas, ultrawideOn ? R.drawable.ic_topbar_ultrawide_on : R.drawable.ic_topbar_ultrawide_off,
                cellRect(TOPBAR_ULTRAWIDE_INDEX));

        return strip;
    }

    // Dark and nearly transparent on purpose - just enough to separate the
    // icons from whatever is behind them, not a solid panel.
    private static void drawTopBarBackground(Canvas canvas, RectF area) {
        Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        paint.setColor(0x50000000);
        paint.setMaskFilter(new BlurMaskFilter(TOPBAR_TEX_MARGIN * 0.45f, BlurMaskFilter.Blur.NORMAL));
        float radius = area.height() * 0.5f;
        canvas.drawRoundRect(area, radius, radius, paint);
    }

    private void drawIcon(Canvas canvas, int drawableRes, RectF cell) {
        Bitmap icon = BitmapFactory.decodeResource(prefsContext.getResources(), drawableRes);
        if (icon == null) {
            return;
        }
        // A little inset so the glyph doesn't touch the cell's own edges
        float pad = TOPBAR_CELL_TEX * 0.12f;
        RectF dst = new RectF(cell.left + pad, cell.top + pad, cell.right - pad, cell.bottom - pad);
        canvas.drawBitmap(icon, null, dst, null);
        icon.recycle();
    }

    // Its own single-icon texture rather than a cell in the topbar strip,
    // since it isn't part of the fixed icon row - appears/disappears with
    // the brightness slider instead.
    private Bitmap buildGlowToggleArt() {
        Bitmap icon = Bitmap.createBitmap(TOPBAR_CELL_TEX, TOPBAR_CELL_TEX, Bitmap.Config.ARGB_8888);
        Canvas canvas = new Canvas(icon);
        canvas.drawColor(0, PorterDuff.Mode.CLEAR);
        drawIcon(canvas, glowEnabled ? R.drawable.ic_topbar_glow_on : R.drawable.ic_topbar_glow_off,
                new RectF(0.0f, 0.0f, TOPBAR_CELL_TEX, TOPBAR_CELL_TEX));
        return icon;
    }

    private static RectF cellRect(int index) {
        float left = TOPBAR_TEX_MARGIN + index * (float)TOPBAR_CELL_TEX;
        return new RectF(left, TOPBAR_TEX_MARGIN, left + TOPBAR_CELL_TEX,
                         TOPBAR_TEX_MARGIN + TOPBAR_CELL_TEX);
    }

    private static ByteBuffer toBuffer(Bitmap bitmap) {
        ByteBuffer pixels = ByteBuffer.allocateDirect(
                bitmap.getWidth() * bitmap.getHeight() * 4);
        bitmap.copyPixelsToBuffer(pixels);
        pixels.rewind();
        return pixels;
    }

    // Moves the pointer before any press, so a click lands where the user is
    // pointing rather than where they pointed last frame
    private void dispatchInput() {
        // The screen placement and the environment grid are ours either way,
        // only the host events need somewhere to go
        if (inputListener != null) {
            if (inputState[IN_HIT] != 0.0f) {
                inputListener.onVrPointerMove(inputState[IN_U], inputState[IN_V]);
            }

            int buttons = (int)inputState[IN_BUTTONS];
            int changed = buttons ^ heldButtons;
            if (changed != 0) {
                for (int i = 0; i < 3; i++) {
                    int mask = 1 << i;
                    if ((changed & mask) != 0) {
                        inputListener.onVrButton(i, (buttons & mask) != 0);
                    }
                }
                heldButtons = buttons;
            }

            int clicks = (int)inputState[IN_SCROLL];
            if (clicks != 0) {
                inputListener.onVrScroll(clicks);
            }
        }

        if (inputState[IN_POSE_DIRTY] != 0.0f) {
            saveScreenPose();
        }

        if (inputState[IN_PASSTHROUGH_DIRTY] != 0.0f) {
            savePassthroughLevel(inputState[IN_PASSTHROUGH_LEVEL]);
        }

        if (inputState[IN_CURVE_DIRTY] != 0.0f) {
            saveCurvature(inputState[IN_CURVE_LEVEL]);
        }

        if (inputState[IN_EXIT_PRESSED] != 0.0f && inputListener != null) {
            inputListener.onVrExitRequested();
        }

        if (inputState[IN_KEYBOARD_TOGGLE] != 0.0f && inputListener != null) {
            inputListener.onVrKeyboardToggleRequested();
        }

        if (inputState[IN_ULTRAWIDE_TOGGLE] != 0.0f && inputListener != null) {
            inputListener.onVrUltrawideToggleRequested();
        }

        if (inputState[IN_DEPTH_TOGGLE] != 0.0f) {
            toggleDepthEffect();
        }

        if (inputState[IN_GLOW_TOGGLE] != 0.0f) {
            toggleGlowEnabled();
        }

        if (inputListener != null) {
            inputListener.onVrSpatialAudio(inputState[IN_AUDIO_PAN], inputState[IN_AUDIO_GAIN]);
        }

        if (inputListener != null) {
            int screenIndex = (int)inputState[IN_PMODE_SCREEN];
            if (inputState[IN_PMODE_HIT] != 0.0f && screenIndex >= 0) {
                pmodeLastScreenIndex = screenIndex;
                inputListener.onVrProductivityPointerMove(screenIndex, inputState[IN_PMODE_U],
                        inputState[IN_PMODE_V]);

                int pmodeButtons = (int)inputState[IN_PMODE_BUTTONS];
                int pmodeChanged = pmodeButtons ^ pmodeHeldButtons;
                if (pmodeChanged != 0) {
                    for (int i = 0; i < 3; i++) {
                        int mask = 1 << i;
                        if ((pmodeChanged & mask) != 0) {
                            inputListener.onVrProductivityButton(screenIndex, i, (pmodeButtons & mask) != 0);
                        }
                    }
                    pmodeHeldButtons = pmodeButtons;
                }
            }
            else if (pmodeHeldButtons != 0 && pmodeLastScreenIndex >= 0) {
                // The ray left the screen mid-press - release whatever was
                // still held on whichever screen it was last down on, rather
                // than leaving the host thinking a button is stuck down.
                // Same "release always counts" rule the single-screen path
                // already follows.
                for (int i = 0; i < 3; i++) {
                    int mask = 1 << i;
                    if ((pmodeHeldButtons & mask) != 0) {
                        inputListener.onVrProductivityButton(pmodeLastScreenIndex, i, false);
                    }
                }
                pmodeHeldButtons = 0;
            }
        }
    }

    // Written once when a grab ends, so the screen is where it was left next
    // time. Cleared by the reset in settings.
    private void saveScreenPose() {
        if (prefsContext == null) {
            return;
        }

        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < POSE_VALUES; i++) {
            if (i > 0) {
                sb.append(',');
            }
            sb.append(inputState[IN_POSE + i]);
        }

        PreferenceManager.getDefaultSharedPreferences(prefsContext).edit()
                .putString(PreferenceConfiguration.VR_SCREEN_POSE_PREF_STRING, sb.toString())
                .apply();
    }

    // Written once when a slider drag ends, mirroring saveScreenPose() -
    // restored in start() via nativeSetPassthroughLevel.
    private void savePassthroughLevel(float level) {
        if (prefsContext == null) {
            return;
        }
        PreferenceManager.getDefaultSharedPreferences(prefsContext).edit()
                .putFloat(PreferenceConfiguration.VR_PASSTHROUGH_LEVEL_PREF_STRING, level)
                .apply();
    }

    // Same as savePassthroughLevel(), but this preference is shared with the
    // flat Settings screen's SeekBarPreference, which stores an int 0-100 -
    // match that format so either control can move the other's value.
    private void saveCurvature(float amount) {
        if (prefsContext == null) {
            return;
        }
        PreferenceManager.getDefaultSharedPreferences(prefsContext).edit()
                .putInt(PreferenceConfiguration.VR_CURVATURE_PREF_STRING, Math.round(amount * 100.0f))
                .apply();
    }

    // A tap-to-flip toggle rather than a dragged value, so unlike
    // brightness/curve this owns its own state here (native only echoes it
    // back for the render-side gate) - flipping it also has to start/stop
    // the depth inference thread, which only Java can do.
    private void toggleDepthEffect() {
        depthEffectOn = !depthEffectOn;
        // Instant and self-contained: this alone makes the visual change
        // immediate regardless of whether the inference thread has actually
        // stopped/started yet (see reconcileDepthThread()).
        nativeSetDepthEffect(nativeCtx, depthEffectOn);

        if (depthModelCapable) {
            reconcileDepthThread();
        }

        if (prefsContext != null) {
            PreferenceManager.getDefaultSharedPreferences(prefsContext).edit()
                    .putBoolean(PreferenceConfiguration.VR_DEPTH_EFFECT_PREF_STRING, depthEffectOn)
                    .apply();
        }

        pendingTopBarArt.set(toBuffer(buildTopBarArt()));
    }

    // Much simpler than toggleDepthEffect() - purely a render-side flag on
    // the native side, no thread to start/stop, so this is the whole thing.
    private void toggleGlowEnabled() {
        glowEnabled = !glowEnabled;
        nativeSetGlowEnabled(nativeCtx, glowEnabled);

        if (prefsContext != null) {
            PreferenceManager.getDefaultSharedPreferences(prefsContext).edit()
                    .putBoolean(PreferenceConfiguration.VR_GLOW_ENABLED_PREF_STRING, glowEnabled)
                    .apply();
        }

        pendingGlowToggleArt.set(toBuffer(buildGlowToggleArt()));
    }

    private final Object depthThreadOpLock = new Object();
    private boolean depthThreadOpInFlight;

    /**
     * Starts or stops the depth thread to match depthEffectOn, off the
     * render thread - stopDepthThread() blocks on a join(), which would
     * stall the whole VR view for however long that takes if called from
     * dispatchInput() directly. Only one of these runs at a time; a toggle
     * that lands while one is already in flight is picked up by a follow-up
     * pass once it finishes, rather than racing a second start/stop against
     * it (both touch the same depth EGL context).
     */
    private void reconcileDepthThread() {
        synchronized (depthThreadOpLock) {
            if (depthThreadOpInFlight) {
                return;
            }
            depthThreadOpInFlight = true;
        }
        new Thread() {
            @Override
            public void run() {
                boolean want = depthEffectOn;
                boolean have = depthThread != null;
                if (want != have) {
                    if (want) {
                        startDepthThread(prefsContext);
                    }
                    else {
                        stopDepthThread();
                    }
                }
                synchronized (depthThreadOpLock) {
                    depthThreadOpInFlight = false;
                }
                if (want != depthEffectOn) {
                    reconcileDepthThread();
                }
            }
        }.start();
    }

    private void restoreScreenPose() {
        String saved = PreferenceManager.getDefaultSharedPreferences(prefsContext)
                .getString(PreferenceConfiguration.VR_SCREEN_POSE_PREF_STRING, null);
        if (saved == null) {
            return;
        }

        String[] parts = saved.split(",");
        if (parts.length < POSE_VALUES) {
            return;
        }

        float[] pose = new float[POSE_VALUES];
        try {
            for (int i = 0; i < POSE_VALUES; i++) {
                pose[i] = Float.parseFloat(parts[i]);
            }
        } catch (NumberFormatException e) {
            return;
        }

        nativeSetScreenPose(nativeCtx, pose);
    }

    /**
     * Downscales the frame just latched and wakes the depth thread. Only the
     * capture stays on the frame loop, since it has to sample the video
     * texture this context owns, and it is short.
     */
    private void handOffDepthFrame() {
        synchronized (depthLock) {
            if (depthPending || depthBusy) {
                skippedFrames++;
                return;
            }
        }

        lastCaptureNs = nativeCaptureDepthInput(nativeCtx, texMatrix);
        captureFrameIndex = videoFrameIndex;
        captureFrameNs = System.nanoTime();

        synchronized (depthLock) {
            depthPending = true;
            depthLock.notify();
        }
    }

    /**
     * Draws the stats into the overlay layer. Called about once a second from
     * whichever thread produced them, never from the frame loop, so the
     * bitmap work cannot stall frame submission.
     *
     * The renderer appends its own numbers, since decode and network stats
     * come from the decoder but warp, inference and depth age only exist here.
     */
    public void setOverlayText(String text) {
        if (nativeCtx == 0) {
            return;
        }
        // The previous one has not been picked up yet, so skip this update
        // rather than write a buffer the frame loop may be reading
        if (pendingOverlay.get() != null) {
            return;
        }

        if (overlayBitmap == null) {
            overlayBitmap = Bitmap.createBitmap(OVERLAY_WIDTH, OVERLAY_HEIGHT,
                    Bitmap.Config.ARGB_8888);
            overlayCanvas = new Canvas(overlayBitmap);
            overlayPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
            overlayPaint.setTypeface(Typeface.MONOSPACE);
            overlayPaint.setTextSize(OVERLAY_TEXT_SIZE);
            overlayPaint.setColor(Color.WHITE);
            overlayBuffers = new ByteBuffer[2];
            for (int i = 0; i < overlayBuffers.length; i++) {
                overlayBuffers[i] = ByteBuffer.allocateDirect(OVERLAY_WIDTH * OVERLAY_HEIGHT * 4);
                overlayBuffers[i].order(ByteOrder.nativeOrder());
            }
        }

        // Dark backing so the text stays readable over any content
        overlayCanvas.drawColor(0xB0000000, PorterDuff.Mode.SRC);
        // Texture rows run bottom up, so draw mirrored and let the upload put
        // it back the right way round
        overlayCanvas.save();
        overlayCanvas.translate(0.0f, OVERLAY_HEIGHT);
        overlayCanvas.scale(1.0f, -1.0f);
        float y = OVERLAY_LINE_HEIGHT;
        for (String line : (text + '\n' + rendererStats()).split("\n")) {
            overlayCanvas.drawText(line, 8.0f, y, overlayPaint);
            y += OVERLAY_LINE_HEIGHT;
            if (y > OVERLAY_HEIGHT) {
                break;
            }
        }
        overlayCanvas.restore();

        ByteBuffer buf = overlayBuffers[overlayBufferIndex];
        overlayBufferIndex = (overlayBufferIndex + 1) % overlayBuffers.length;
        buf.rewind();
        overlayBitmap.copyPixelsToBuffer(buf);
        buf.rewind();
        pendingOverlay.set(buf);
    }

    private String rendererStats() {
        StringBuilder sb = new StringBuilder();
        sb.append(String.format("Warp GPU: %.2f ms", nativeGetWarpGpuMs(nativeCtx)));
        if (depthReady) {
            sb.append('\n').append(String.format("Depth inference: %.1f ms", lastInferenceMs));
            sb.append('\n').append(String.format("Depth age: %.0f ms", lastDepthAgeMs));
            sb.append('\n').append("Depth frames skipped: ").append(lastDepthSkips);
        }
        return sb.toString();
    }

    private static String msPer(long totalNs, long count) {
        return String.format("%.2f", totalNs / (double)count / 1000000.0);
    }

    public Surface getInputSurface() {
        return inputSurface;
    }

    /**
     * The Surface a PMode screen's decoder should target - only valid after
     * start() has run with productivityMode set (they're created on the
     * render thread once the EGL context is current, same as
     * getInputSurface()'s single-screen counterpart). Game.java hands these
     * to the bound PModeScreenService instances via AIDL, one per screen.
     */
    public Surface getProductivityInputSurface(int screenIndex) {
        return productivityInputSurfaces[screenIndex];
    }

    // May run on any thread, the frame loop picks the counter up on its own
    @Override
    public void onFrameAvailable(SurfaceTexture st) {
        pendingFrames.incrementAndGet();
    }

    /**
     * Stops the frame loop and destroys the OpenXR session. The codec-facing
     * surface stays valid until cleanup(). The join is bounded by one
     * xrWaitFrame period plus teardown.
     */
    public void prepareForStop() {
        stopping = true;

        if (renderThread != null) {
            try {
                renderThread.join(2000);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
            }
            if (renderThread.isAlive()) {
                LimeLog.warning("XR render thread did not stop in time");
            }
        }
    }

    /**
     * Releases the surface handed to MediaCodec. Only call after the codec
     * has been released.
     */
    public void cleanup() {
        if (inputSurface != null) {
            inputSurface.release();
            inputSurface = null;
        }
        if (surfaceTexture != null) {
            surfaceTexture.release();
            surfaceTexture = null;
        }
    }
}
