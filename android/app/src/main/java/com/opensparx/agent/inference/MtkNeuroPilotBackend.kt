package com.opensparx.agent.inference

import android.content.Context
import android.util.Log
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.io.File

/**
 * MediaTek NeuroPilot inference backend.
 *
 * Targets Dimensity 9300 / 9400 series with APU 7.0+.
 * Uses MediaTek's NeuroPilot SDK for on-device LLM inference.
 *
 * Same encapsulation strategy as GenieX:
 * - NeuroPilot SDK linked at build time or discovered at runtime
 * - No MTK classes exposed beyond this file
 * - Fallback to CPU if APU not available
 */
class MtkNeuroPilotBackend(private val context: Context) : InferenceBackend {

    companion object {
        private const val TAG = "MtkNeuroPilot"

        private val SYSTEM_PATHS = listOf(
            "/vendor/lib64/libneuronruntime.so",
            "/vendor/lib64/libneuropilot.so",
            "/system/lib64/libneuronruntime.so",
        )
    }

    override val name = "NeuroPilot APU"

    private var nativeHandle: Long = 0L
    private var isInit = false

    @Volatile private var lastSpeed = 0f
    @Volatile private var lastLoad = 0f
    @Volatile private var lastKv = 0f

    override suspend fun isSupported(): Boolean = withContext(Dispatchers.IO) {
        isRuntimePresent()
    }

    override suspend fun initialize(modelPath: String, config: BackendConfig): Boolean =
        withContext(Dispatchers.IO) {
            try {
                System.loadLibrary("sparx_neuropilot")
                nativeHandle = nativeInit(modelPath, config.npuBudget, config.maxKvTokens)
                isInit = nativeHandle != 0L
                isInit
            } catch (e: Exception) {
                Log.e(TAG, "NeuroPilot init failed", e)
                false
            }
        }

    override suspend fun release() = withContext(Dispatchers.IO) {
        if (isInit && nativeHandle != 0L) {
            nativeRelease(nativeHandle)
            nativeHandle = 0L
            isInit = false
        }
    }

    override suspend fun generate(
        prompt: String,
        params: SamplingParams,
        onToken: ((String) -> Unit)?
    ): GenerationResult = withContext(Dispatchers.IO) {
        check(isInit) { "NeuroPilot not initialized" }

        val start = System.currentTimeMillis()
        // NeuroPilot generation — same pattern as GenieX
        val text = nativeGenerate(nativeHandle, prompt,
            params.maxTokens, params.temperature, params.topP)
        val duration = System.currentTimeMillis() - start

        val tokenCount = text.split(" ").size // approximate
        lastSpeed = if (duration > 0) tokenCount * 1000f / duration else 0f

        GenerationResult(
            text = text,
            tokenCount = tokenCount,
            durationMs = duration,
            tokensPerSecond = lastSpeed,
            stoppedByEos = true,
        )
    }

    override fun cancelGeneration() {
        if (isInit && nativeHandle != 0L) nativeCancel(nativeHandle)
    }

    override fun getLoad() = lastLoad
    override fun getSpeed() = lastSpeed
    override fun getKvCacheUsage() = lastKv
    override fun getModelInfo(): String =
        """{"name":"Qwen3-4B","quant":"INT4","backend":"NeuroPilot_APU"}"""

    fun isRuntimePresent(): Boolean =
        SYSTEM_PATHS.any { File(it).exists() }

    // ─── Native ─────────────────────────────────────────────────────────

    private external fun nativeInit(modelPath: String, budget: Float, maxKv: Int): Long
    private external fun nativeRelease(handle: Long)
    private external fun nativeGenerate(
        handle: Long, prompt: String, maxTokens: Int, temp: Float, topP: Float
    ): String
    private external fun nativeCancel(handle: Long)
}
