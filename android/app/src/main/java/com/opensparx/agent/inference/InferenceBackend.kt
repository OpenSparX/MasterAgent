package com.opensparx.agent.inference

/**
 * Abstract inference backend interface.
 *
 * Implementations wrap platform-specific SDKs:
 * - [GenieXBackend]: Qualcomm GenieX (Snapdragon 8 Gen 3, 8s Gen 3, 8 Elite)
 * - [MtkNeuroPilotBackend]: MediaTek NeuroPilot (Dimensity 9300+)
 * - [CpuFallbackBackend]: llama.cpp for unsupported chipsets
 *
 * The app never imports platform SDK classes directly — all access goes
 * through this interface. This keeps the SparX agent logic clean and
 * allows swapping backends without touching upper layers.
 */
interface InferenceBackend {

    /** Human-readable backend name for logging/UI (e.g., "GenieX HTP") */
    val name: String

    /** Whether this backend can run on the current device. */
    suspend fun isSupported(): Boolean

    /**
     * Initialize the backend: load runtime, allocate NPU context.
     * @param modelPath Path to the quantized model file on device storage.
     * @param config Backend-specific configuration.
     * @return true if ready for inference.
     */
    suspend fun initialize(modelPath: String, config: BackendConfig = BackendConfig()): Boolean

    /** Release all resources. Safe to call multiple times. */
    suspend fun release()

    // ─── Generation ─────────────────────────────────────────────────────

    /**
     * Generate tokens from a prompt.
     * @param prompt Tokenized or raw text prompt.
     * @param params Sampling parameters.
     * @param onToken Called for each generated token (streaming).
     * @return Full generated text.
     */
    suspend fun generate(
        prompt: String,
        params: SamplingParams = SamplingParams(),
        onToken: ((String) -> Unit)? = null
    ): GenerationResult

    /** Cancel an in-progress generation. */
    fun cancelGeneration()

    // ─── Telemetry ──────────────────────────────────────────────────────

    /** Current accelerator load [0.0, 1.0]. */
    fun getLoad(): Float

    /** Tokens per second (last generation, or rolling average). */
    fun getSpeed(): Float

    /** KV cache utilization [0.0, 1.0]. */
    fun getKvCacheUsage(): Float

    /** Model metadata JSON. */
    fun getModelInfo(): String
}

data class BackendConfig(
    /** Max fraction of NPU compute budget to use [0.1, 1.0]. */
    val npuBudget: Float = 0.3f,
    /** Max KV cache size in tokens. */
    val maxKvTokens: Int = 4096,
    /** Number of threads for CPU fallback. */
    val cpuThreads: Int = 4,
)

data class SamplingParams(
    val maxTokens: Int = 512,
    val temperature: Float = 0.7f,
    val topP: Float = 0.9f,
    val topK: Int = 40,
    val repetitionPenalty: Float = 1.1f,
)

data class GenerationResult(
    val text: String,
    val tokenCount: Int,
    val durationMs: Long,
    val tokensPerSecond: Float,
    val stoppedByEos: Boolean,
)
