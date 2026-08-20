package com.opensparx.agent.inference

import android.content.Context
import android.util.Log
import com.geniex.sdk.GenieXSdk
import com.geniex.sdk.LlmWrapper
import com.geniex.sdk.VlmWrapper
import com.geniex.sdk.ModelManagerWrapper
import com.geniex.sdk.bean.*
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.*
import kotlinx.coroutines.withContext

/**
 * GenieX SDK backend — official Qualcomm on-device inference.
 *
 * Maven: com.qualcomm.qti:geniex-android:0.3.1
 * Docs: https://geniex.aihub.qualcomm.com/en/run/android/quickstart
 *
 * Supports SM8750 (8 Elite) and SM8850 (8 Elite Gen 5).
 * Runtime: llama_cpp (any GGUF, NPU/GPU/CPU) or qairt (AI Hub models, NPU only).
 */
class GenieXSdkBackend(private val context: Context) {

    companion object {
        private const val TAG = "GenieXSdkBackend"
    }

    private var llm: LlmWrapper? = null
    private var vlm: VlmWrapper? = null
    private var currentModelName: String? = null
    @Volatile var sdkReady = false
        private set

    // ─── Init ───────────────────────────────────────────────────────────

    fun initSdk(onReady: (() -> Unit)? = null) {
        GenieXSdk.getInstance().init(context, object : GenieXSdk.InitCallback {
            override fun onSuccess() {
                sdkReady = true
                Log.i(TAG, "GenieX SDK ready")
                onReady?.invoke()
            }
            override fun onFailure(msg: String) {
                Log.e(TAG, "GenieX SDK init failed: $msg")
            }
        })
    }

    // ─── Download ───────────────────────────────────────────────────────

    fun pullModel(modelName: String, precision: String = "Q4_0"): Flow<PullProgress> = flow {
        emit(PullProgress.Starting)
        val input = ModelPullInput(modelName, precision, HubSource.HUGGINGFACE,
            null, null, null, null, null)
        ModelManagerWrapper.pullFlow(input).collect { event ->
            when (event) {
                is ModelManagerWrapper.PullEvent.Progress -> {
                    var down = 0L; var total = 0L
                    for (f in event.files) { down += f.downloaded_bytes; total += f.total_bytes }
                    emit(PullProgress.Downloading(down, total,
                        if (total > 0) down.toFloat() / total else 0f))
                }
                is ModelManagerWrapper.PullEvent.Completed -> emit(PullProgress.Done)
                is ModelManagerWrapper.PullEvent.Error -> emit(PullProgress.Failed(event.toString()))
            }
        }
    }.flowOn(Dispatchers.IO)

    fun pullAiHubModel(modelName: String, chipset: String = "SM8750"): Flow<PullProgress> = flow {
        emit(PullProgress.Starting)
        val input = ModelPullInput(modelName, null, HubSource.AUTO,
            null, null, chipset, null, null)
        ModelManagerWrapper.pullFlow(input).collect { event ->
            when (event) {
                is ModelManagerWrapper.PullEvent.Progress -> {
                    var down = 0L; var total = 0L
                    for (f in event.files) { down += f.downloaded_bytes; total += f.total_bytes }
                    emit(PullProgress.Downloading(down, total,
                        if (total > 0) down.toFloat() / total else 0f))
                }
                is ModelManagerWrapper.PullEvent.Completed -> emit(PullProgress.Done)
                is ModelManagerWrapper.PullEvent.Error -> emit(PullProgress.Failed(event.toString()))
            }
        }
    }.flowOn(Dispatchers.IO)

    // ─── Load ───────────────────────────────────────────────────────────

    suspend fun loadModel(
        modelName: String,
        computeUnit: String? = null,
        contextSize: Int = 4096,
        runtimeId: String = "llama_cpp",
    ): Boolean = withContext(Dispatchers.IO) {
        try {
            val paths = ModelManagerWrapper.getPaths(modelName)
                ?: throw IllegalStateException("Model not downloaded: $modelName")

            val config = ModelConfig().apply { nCtx = contextSize }
            val createInput = LlmCreateInput(
                modelName, paths.model_path, null, config, runtimeId, computeUnit
            )

            val result = LlmWrapper.builder().llmCreateInput(createInput).build()
            llm = result.getOrThrow()
            currentModelName = modelName
            Log.i(TAG, "Loaded: $modelName ($runtimeId, ${computeUnit ?: "npu"})")
            true
        } catch (e: Exception) {
            Log.e(TAG, "Load failed", e)
            false
        }
    }

    /**
     * Load model directly from a file path (for models downloaded via HTTP, not GenieX pull).
     */
    suspend fun loadModelFromPath(
        modelPath: String,
        modelName: String = "local",
        computeUnit: String? = null,
        contextSize: Int = 4096,
        runtimeId: String = "llama_cpp",
    ): Boolean = withContext(Dispatchers.IO) {
        try {
            if (!sdkReady) {
                Log.e(TAG, "SDK not ready, cannot load model")
                return@withContext false
            }
            val config = ModelConfig().apply { nCtx = contextSize }
            val createInput = LlmCreateInput(
                modelName, modelPath, null, config, runtimeId, computeUnit
            )

            val result = LlmWrapper.builder().llmCreateInput(createInput).build()
            llm = result.getOrThrow()
            currentModelName = modelName
            Log.i(TAG, "Loaded from path: $modelPath ($runtimeId)")
            true
        } catch (e: Exception) {
            Log.e(TAG, "Load from path failed: ${e.message}", e)
            false
        }
    }

    // ─── Generate ───────────────────────────────────────────────────────

    fun generate(
        userMessage: String,
        history: List<Pair<String, String>> = emptyList(),
        maxTokens: Int = 2048,
    ): Flow<GenEvent> = flow {
        val w = llm ?: throw IllegalStateException("No model loaded")

        val msgs = ArrayList<ChatMessage>()
        for ((role, content) in history) msgs.add(ChatMessage(role, content))
        msgs.add(ChatMessage("user", userMessage))

        val templated = w.applyChatTemplate(msgs.toTypedArray(), null, false, false)
            .getOrThrow()

        val cfg = GenerationConfig().apply { this.maxTokens = maxTokens }
        val sb = StringBuilder()

        w.generateStreamFlow(templated.formattedText, cfg).collect { r ->
            when (r) {
                is LlmStreamResult.Token -> { sb.append(r.text); emit(GenEvent.Token(r.text)) }
                is LlmStreamResult.Completed -> emit(GenEvent.Done(sb.toString(), r.profile))
                is LlmStreamResult.Error -> emit(GenEvent.Error(r.throwable.message ?: "error"))
                else -> {}
            }
        }
    }.flowOn(Dispatchers.IO)

    // ─── Cleanup ────────────────────────────────────────────────────────

    fun release() { llm?.close(); llm = null; vlm?.close(); vlm = null; currentModelName = null }
    fun isModelLoaded() = llm != null
    fun currentModel() = currentModelName

    // ─── VLM (Vision-Language Model) ───────────────────────────────────

    suspend fun loadVlm(
        modelPath: String,
        mmprojPath: String,
        modelName: String = "vlm",
        contextSize: Int = 4096,
    ): Boolean = withContext(Dispatchers.IO) {
        try {
            if (!sdkReady) return@withContext false
            val config = ModelConfig().apply { nCtx = contextSize }
            val createInput = VlmCreateInput(modelName, modelPath, mmprojPath, config, "llama_cpp", null)
            val result = VlmWrapper.Builder().vlmCreateInput(createInput).build()
            vlm = result.getOrThrow()
            Log.i(TAG, "VLM loaded: $modelName")
            true
        } catch (e: Exception) {
            Log.e(TAG, "VLM load failed: ${e.message}", e)
            false
        }
    }

    fun isVlmLoaded() = vlm != null

    fun analyzeImage(
        imagePath: String,
        prompt: String = "请描述你在这张图片中看到了什么，包括物体、场景、文字等。然后给出智能建议。",
        maxTokens: Int = 256,
    ): Flow<GenEvent> = flow {
        val v = vlm ?: throw IllegalStateException("VLM not loaded")

        // Verify image file exists and is readable
        val imageFile = java.io.File(imagePath)
        if (!imageFile.exists() || imageFile.length() == 0L) {
            emit(GenEvent.Error("Image file not found: $imagePath"))
            return@flow
        }
        Log.i(TAG, "analyzeImage: path=$imagePath, size=${imageFile.length()}")

        // Reset VLM state to clear any stale HTP/DSP sessions
        try { v.reset() } catch (_: Exception) {}

        val contents = listOf(
            VlmContent("image_url", imagePath),
            VlmContent("text", prompt)
        )
        val message = VlmChatMessage("user", contents)
        val messages = arrayOf(message)

        val templateResult = v.applyChatTemplate(messages, null, false)
        val formatted = templateResult.getOrThrow().formattedText
        Log.i(TAG, "VLM template formatted, length=${formatted.length}")

        // Manually set imagePaths on GenerationConfig (injectMediaPathsToConfig may not work correctly)
        val genConfig = GenerationConfig().apply {
            this.maxTokens = maxTokens
            this.imagePaths = arrayOf(imagePath)
            this.imageCount = 1
        }

        val sb = StringBuilder()
        v.generateStreamFlow(formatted, genConfig).collect { r ->
            when (r) {
                is LlmStreamResult.Token -> { sb.append(r.text); emit(GenEvent.Token(r.text)) }
                is LlmStreamResult.Completed -> emit(GenEvent.Done(sb.toString(), r.profile))
                is LlmStreamResult.Error -> emit(GenEvent.Error(r.throwable.message ?: "VLM error"))
                else -> {}
            }
        }
    }.flowOn(Dispatchers.IO)

    // ─── Types ──────────────────────────────────────────────────────────

    sealed class PullProgress {
        object Starting : PullProgress()
        data class Downloading(val down: Long, val total: Long, val progress: Float) : PullProgress()
        object Done : PullProgress()
        data class Failed(val error: String) : PullProgress()
    }

    sealed class GenEvent {
        data class Token(val text: String) : GenEvent()
        data class Done(val fullText: String, val profile: ProfilingData?) : GenEvent()
        data class Error(val msg: String) : GenEvent()
    }
}
