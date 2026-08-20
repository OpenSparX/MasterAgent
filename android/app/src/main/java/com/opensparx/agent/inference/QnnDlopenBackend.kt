package com.opensparx.agent.inference

import android.content.Context
import android.util.Log
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.io.File
import java.util.concurrent.atomic.AtomicBoolean

/**
 * Legacy QNN dlopen backend — direct system library loading.
 *
 * Used when GenieX runtime is not available but QNN system libs are
 * present in /vendor/lib64/. This was the original approach before
 * GenieX SDK integration.
 *
 * Limitations vs GenieX:
 * - No built-in tokenizer (must bundle separately)
 * - Manual KV cache management
 * - No power/thermal awareness
 * - Requires pre-compiled model graph (.qnn format)
 */
class QnnDlopenBackend(private val context: Context) : InferenceBackend {

    companion object {
        private const val TAG = "QnnDlopen"
    }

    override val name = "QNN HTP (dlopen)"
    private val initialized = AtomicBoolean(false)

    @Volatile private var speed = 0f
    @Volatile private var load = 0f
    @Volatile private var kvUsage = 0f

    override suspend fun isSupported(): Boolean = withContext(Dispatchers.IO) {
        // Check if QNN libs are on the device
        listOf(
            "/vendor/lib64/libQnnHtp.so",
            "/vendor/lib64/libQnnSystem.so"
        ).all { File(it).exists() }
    }

    override suspend fun initialize(modelPath: String, config: BackendConfig): Boolean =
        withContext(Dispatchers.IO) {
            try {
                System.loadLibrary("sparx_agent") // existing native lib
                val ok = nativeQnnInit(modelPath, config.npuBudget)
                initialized.set(ok)
                ok
            } catch (e: Exception) {
                Log.e(TAG, "QNN init failed", e)
                false
            }
        }

    override suspend fun release() = withContext(Dispatchers.IO) {
        if (initialized.compareAndSet(true, false)) {
            nativeQnnShutdown()
        }
    }

    override suspend fun generate(
        prompt: String,
        params: SamplingParams,
        onToken: ((String) -> Unit)?
    ): GenerationResult = withContext(Dispatchers.IO) {
        check(initialized.get()) { "QNN not initialized" }
        val start = System.currentTimeMillis()
        val text = nativeQnnGenerate(prompt, params.maxTokens, params.temperature)
        val duration = System.currentTimeMillis() - start
        val tokenCount = text.length / 4 // rough estimate
        speed = if (duration > 0) tokenCount * 1000f / duration else 0f

        GenerationResult(text, tokenCount, duration, speed, true)
    }

    override fun cancelGeneration() {
        if (initialized.get()) {
            try { nativeQnnCancel() } catch (_: UnsatisfiedLinkError) {}
        }
    }
    override fun getLoad() = load
    override fun getSpeed() = speed
    override fun getKvCacheUsage() = kvUsage
    override fun getModelInfo() = """{"name":"Qwen3-4B","quant":"INT4","backend":"QNN_HTP"}"""

    private external fun nativeQnnInit(modelPath: String, budget: Float): Boolean
    private external fun nativeQnnShutdown()
    private external fun nativeQnnGenerate(prompt: String, maxTokens: Int, temp: Float): String
    private external fun nativeQnnCancel()
}
