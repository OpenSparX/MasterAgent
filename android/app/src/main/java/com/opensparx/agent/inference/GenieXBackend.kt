package com.opensparx.agent.inference

import android.content.Context
import android.util.Log
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.io.File
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicReference

/**
 * GenieX inference backend for Qualcomm Snapdragon devices.
 *
 * Encapsulates the Qualcomm GenieX SDK — the app's public API never
 * exposes GenieX classes. From the outside, this is just "NPU inference".
 *
 * GenieX handles:
 * - Model loading + graph compilation for Hexagon NPU
 * - KV cache management (paged attention)
 * - Token generation with NPU-optimized kernels
 * - Power/thermal throttling awareness
 *
 * Runtime discovery:
 * - GenieX SDK is bundled as a local AAR (not published to Maven Central)
 * - On devices with Qualcomm AI Runtime pre-installed, we use the system libs
 * - Falls back to QnnDlopenBackend if GenieX AAR is not linked at build time
 */
class GenieXBackend(private val context: Context) : InferenceBackend {

    companion object {
        private const val TAG = "GenieXBackend"

        // GenieX native library names (loaded in order)
        private val GENIEX_LIBS = listOf(
            "geniex_runtime",      // Core runtime
            "geniex_htp",          // Hexagon backend
            "geniex_tokenizer",    // Built-in tokenizer
        )

        // System paths where GenieX runtime may be pre-installed
        private val SYSTEM_PATHS = listOf(
            "/vendor/lib64/libgeniex_runtime.so",
            "/system/lib64/libgeniex_runtime.so",
            "/odm/lib64/libgeniex_runtime.so",
        )
    }

    override val name = "GenieX HTP"

    private val initialized = AtomicBoolean(false)
    private val generating = AtomicBoolean(false)
    private val cancelled = AtomicBoolean(false)
    private val currentModel = AtomicReference<String>(null)

    // Native handle (opaque pointer from JNI)
    private var nativeHandle: Long = 0L

    // Telemetry cache (updated during generation)
    @Volatile private var lastLoad: Float = 0f
    @Volatile private var lastSpeed: Float = 0f
    @Volatile private var lastKvUsage: Float = 0f
    @Volatile private var modelInfoJson: String = "{}"

    // ─── Lifecycle ──────────────────────────────────────────────────────

    override suspend fun isSupported(): Boolean = withContext(Dispatchers.IO) {
        isRuntimePresent() && isNpuCapable()
    }

    override suspend fun initialize(modelPath: String, config: BackendConfig): Boolean =
        withContext(Dispatchers.IO) {
            if (initialized.get()) {
                Log.w(TAG, "Already initialized, releasing first")
                release()
            }

            try {
                // Step 1: Load GenieX native libraries
                loadNativeLibs()

                // Step 2: Create GenieX session via JNI
                nativeHandle = nativeCreateSession(
                    modelPath = modelPath,
                    npuBudget = config.npuBudget,
                    maxKvTokens = config.maxKvTokens,
                    // PLACEHOLDER_1
                )

                if (nativeHandle == 0L) {
                    Log.e(TAG, "Failed to create GenieX session")
                    return@withContext false
                }

                currentModel.set(modelPath)
                modelInfoJson = nativeGetModelInfo(nativeHandle)
                initialized.set(true)
                Log.i(TAG, "GenieX initialized: $modelInfoJson")
                true
            } catch (e: Exception) {
                Log.e(TAG, "GenieX initialization failed", e)
                false
            }
        }

    override suspend fun release() = withContext(Dispatchers.IO) {
        if (initialized.compareAndSet(true, false)) {
            cancelGeneration()
            if (nativeHandle != 0L) {
                nativeDestroySession(nativeHandle)
                nativeHandle = 0L
            }
            currentModel.set(null)
            Log.i(TAG, "GenieX released")
        }
    }

    // ─── Generation ─────────────────────────────────────────────────────

    override suspend fun generate(
        prompt: String,
        params: SamplingParams,
        onToken: ((String) -> Unit)?
    ): GenerationResult = withContext(Dispatchers.IO) {
        check(initialized.get()) { "GenieX not initialized" }
        check(!generating.get()) { "Generation already in progress" }

        generating.set(true)
        cancelled.set(false)

        val startTime = System.currentTimeMillis()
        val tokens = StringBuilder()
        var tokenCount = 0
        var stoppedByEos = false

        try {
            // Start generation on NPU
            nativeBeginGeneration(nativeHandle, prompt,
                params.maxTokens, params.temperature, params.topP,
                params.topK, params.repetitionPenalty)

            // Stream tokens
            while (!cancelled.get()) {
                val token = nativeNextToken(nativeHandle) ?: break

                if (token == "<|endoftext|>" || token == "<|im_end|>") {
                    stoppedByEos = true
                    break
                }

                tokens.append(token)
                tokenCount++
                onToken?.invoke(token)

                // Update telemetry periodically
                if (tokenCount % 10 == 0) {
                    lastLoad = nativeGetLoad(nativeHandle)
                    lastSpeed = nativeGetSpeed(nativeHandle)
                    lastKvUsage = nativeGetKvUsage(nativeHandle)
                }
            }
        } finally {
            nativeEndGeneration(nativeHandle)
            generating.set(false)
        }

        val duration = System.currentTimeMillis() - startTime
        val speed = if (duration > 0) tokenCount * 1000f / duration else 0f
        lastSpeed = speed

        GenerationResult(
            text = tokens.toString(),
            tokenCount = tokenCount,
            durationMs = duration,
            tokensPerSecond = speed,
            stoppedByEos = stoppedByEos,
        )
    }

    override fun cancelGeneration() {
        if (generating.get()) {
            cancelled.set(true)
            if (nativeHandle != 0L) {
                nativeCancelGeneration(nativeHandle)
            }
        }
    }

    // ─── Telemetry ──────────────────────────────────────────────────────

    override fun getLoad(): Float = if (initialized.get() && nativeHandle != 0L)
        nativeGetLoad(nativeHandle) else lastLoad

    override fun getSpeed(): Float = lastSpeed

    override fun getKvCacheUsage(): Float = if (initialized.get() && nativeHandle != 0L)
        nativeGetKvUsage(nativeHandle) else lastKvUsage

    override fun getModelInfo(): String = modelInfoJson

    // ─── Runtime detection ──────────────────────────────────────────────

    /**
     * Check if GenieX runtime is available on device WITHOUT loading it.
     * Used by BackendFactory to decide which backend to use.
     */
    fun isRuntimePresent(): Boolean {
        // Check system-installed GenieX
        for (path in SYSTEM_PATHS) {
            if (File(path).exists()) return true
        }

        // Check if our AAR includes the native lib
        val nativeDir = File(context.applicationInfo.nativeLibraryDir)
        return nativeDir.resolve("libgeniex_runtime.so").exists()
    }

    private fun isNpuCapable(): Boolean {
        // Quick check: can we dlopen the HTP backend?
        return try {
            nativeProbeHtp()
        } catch (e: UnsatisfiedLinkError) {
            false
        }
    }

    private fun loadNativeLibs() {
        // Load our wrapper that links against GenieX
        System.loadLibrary("sparx_geniex")
    }

    // ─── Native methods (implemented in geniex_backend_jni.cpp) ─────────

    private external fun nativeCreateSession(
        modelPath: String, npuBudget: Float, maxKvTokens: Int
    ): Long

    private external fun nativeDestroySession(handle: Long)
    private external fun nativeBeginGeneration(
        handle: Long, prompt: String, maxTokens: Int,
        temperature: Float, topP: Float, topK: Int, repPenalty: Float
    )
    private external fun nativeNextToken(handle: Long): String?
    private external fun nativeEndGeneration(handle: Long)
    private external fun nativeCancelGeneration(handle: Long)
    private external fun nativeGetLoad(handle: Long): Float
    private external fun nativeGetSpeed(handle: Long): Float
    private external fun nativeGetKvUsage(handle: Long): Float
    private external fun nativeGetModelInfo(handle: Long): String
    private external fun nativeProbeHtp(): Boolean
}
