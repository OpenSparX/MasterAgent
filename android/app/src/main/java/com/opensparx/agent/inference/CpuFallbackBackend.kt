package com.opensparx.agent.inference

import android.content.Context
import android.util.Log
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.withContext
import java.util.concurrent.atomic.AtomicBoolean

/**
 * CPU fallback using llama.cpp (or equivalent).
 *
 * Used when no NPU runtime is available. Slower but works everywhere.
 * Targets ~5-8 tok/s on ARM big cores (vs 30-40 on NPU).
 *
 * Links against libsparx_llamacpp.so which bundles ggml + llama.cpp
 * compiled for arm64-v8a with NEON/SVE optimizations.
 */
class CpuFallbackBackend(private val context: Context) : InferenceBackend {

    companion object {
        private const val TAG = "CpuFallback"
    }

    override val name = "CPU (llama.cpp)"
    private val initialized = AtomicBoolean(false)
    private val generating = AtomicBoolean(false)
    private val cancelled = AtomicBoolean(false)

    @Volatile private var speed = 0f

    override suspend fun isSupported(): Boolean = true // always works

    override suspend fun initialize(modelPath: String, config: BackendConfig): Boolean =
        withContext(Dispatchers.IO) {
            try {
                System.loadLibrary("sparx_llamacpp")
                val ok = nativeLlamaInit(modelPath, config.cpuThreads, config.maxKvTokens)
                initialized.set(ok)
                if (ok) Log.i(TAG, "llama.cpp initialized with ${config.cpuThreads} threads")
                ok
            } catch (e: Exception) {
                Log.e(TAG, "CPU backend init failed", e)
                false
            }
        }

    override suspend fun release() = withContext(Dispatchers.IO) {
        if (initialized.compareAndSet(true, false)) {
            nativeLlamaRelease()
        }
    }

    override suspend fun generate(
        prompt: String,
        params: SamplingParams,
        onToken: ((String) -> Unit)?
    ): GenerationResult = withContext(Dispatchers.IO) {
        check(initialized.get()) { "CPU backend not initialized" }
        generating.set(true)
        cancelled.set(false)

        val start = System.currentTimeMillis()
        val tokens = StringBuilder()
        var count = 0

        try {
            nativeLlamaBegin(prompt, params.maxTokens, params.temperature,
                params.topP, params.topK, params.repetitionPenalty)

            while (!cancelled.get()) {
                val tok = nativeLlamaNext() ?: break
                if (tok == "<|endoftext|>" || tok == "<|im_end|>") break
                tokens.append(tok)
                count++
                onToken?.invoke(tok)
            }
        } finally {
            nativeLlamaEnd()
            generating.set(false)
        }

        val duration = System.currentTimeMillis() - start
        speed = if (duration > 0) count * 1000f / duration else 0f

        GenerationResult(tokens.toString(), count, duration, speed, !cancelled.get())
    }

    override fun cancelGeneration() {
        cancelled.set(true)
        if (generating.get()) nativeLlamaCancel()
    }

    override fun getLoad() = 0f // no dedicated accelerator
    override fun getSpeed() = speed
    override fun getKvCacheUsage() = 0f
    override fun getModelInfo() = """{"name":"Qwen3-4B","quant":"Q4_K_M","backend":"CPU_NEON"}"""

    private external fun nativeLlamaInit(path: String, threads: Int, maxKv: Int): Boolean
    private external fun nativeLlamaRelease()
    private external fun nativeLlamaBegin(
        prompt: String, maxTokens: Int, temp: Float, topP: Float, topK: Int, repPen: Float
    )
    private external fun nativeLlamaNext(): String?
    private external fun nativeLlamaEnd()
    private external fun nativeLlamaCancel()
}
