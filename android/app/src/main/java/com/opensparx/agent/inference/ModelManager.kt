package com.opensparx.agent.inference

import android.content.Context
import android.util.Log
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.flow
import kotlinx.coroutines.flow.flowOn
import kotlinx.coroutines.withContext
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.net.HttpURLConnection
import java.net.URL

/**
 * Model catalog and download manager.
 *
 * Supports multiple models for different Agent roles:
 * - Intent recognition (small, fast)
 * - Main conversation (medium, balanced)
 * - Reasoning/planning (large, thinking)
 * - Voice input (Whisper)
 * - Vision (future)
 *
 * Each model has backend variants (GenieX/QNN/GGUF) selected automatically.
 */
class ModelManager(private val context: Context) {

    companion object {
        private const val TAG = "ModelManager"
        private const val CDN_BASE = "https://models.opensparx.com/v1"

        // HuggingFace mirror (hf-mirror.com) for China mainland access
        // Using unsloth repos — they have all Qwen3 GGUF quantizations
        private const val HF_QWEN3 = "https://hf-mirror.com/unsloth/Qwen3-0.6B-GGUF/resolve/main"
        private const val HF_QWEN3_4B = "https://hf-mirror.com/unsloth/Qwen3-4B-GGUF/resolve/main"
        private const val HF_QWEN3_8B = "https://hf-mirror.com/unsloth/Qwen3-8B-GGUF/resolve/main"
        private const val HF_DEEPSEEK = "https://hf-mirror.com/bartowski/DeepSeek-R1-Distill-Qwen-1.5B-GGUF/resolve/main"
        private const val HF_VLM = "https://hf-mirror.com/unsloth/Qwen3-VL-2B-Instruct-GGUF/resolve/main"
        private const val HF_VLM_MINI = "https://hf-mirror.com/ggml-org/MiniCPM-V-4.6-GGUF/resolve/main"

        // ─── Model Catalog (embedded manifest) ────────────────────────────
        // In production this would be fetched from CDN; embedded for offline use.
        val CATALOG: List<ModelEntry> = listOf(
            // ── Lightweight: intent + context sensing ──
            ModelEntry(
                id = "qwen3-0.6b",
                name = "Qwen3 0.6B",
                description = "Ultra-fast intent recognition & context sensing",
                role = ModelRole.INTENT,
                parameterCount = "0.6B",
                variants = listOf(
                    ModelVariant("geniex", "qwen3-0.6b-int4-geniex.bin", 380_000_000L,
                        "$CDN_BASE/qwen3-0.6b-int4-geniex.bin", ""),
                    ModelVariant("qnn", "qwen3-0.6b-int4-qnn.bin", 380_000_000L,
                        "$CDN_BASE/qwen3-0.6b-int4-qnn.bin", ""),
                    ModelVariant("gguf", "Qwen3-0.6B-Q4_K_M.gguf", 397_000_000L,
                        "$HF_QWEN3/Qwen3-0.6B-Q4_K_M.gguf", ""),
                ),
                contextLength = 4096,
                expectedSpeed = "80-120 tok/s (NPU)",
                hfRepo = "unsloth/Qwen3-0.6B-GGUF",
                hfPrecision = "Q4_K_M",
            ),
            // ── Main conversation model ──
            ModelEntry(
                id = "qwen3-4b",
                name = "Qwen3 4B",
                description = "Primary conversation & task execution",
                role = ModelRole.CONVERSATION,
                parameterCount = "4B",
                variants = listOf(
                    ModelVariant("geniex", "qwen3-4b-int4-geniex.bin", 2_684_354_560L,
                        "$CDN_BASE/qwen3-4b-int4-geniex.bin", ""),
                    ModelVariant("qnn", "qwen3-4b-int4-qnn.bin", 2_684_354_560L,
                        "$CDN_BASE/qwen3-4b-int4-qnn.bin", ""),
                    ModelVariant("gguf", "Qwen3-4B-Q4_K_M.gguf", 2_600_000_000L,
                        "$HF_QWEN3_4B/Qwen3-4B-Q4_K_M.gguf", ""),
                ),
                contextLength = 32768,
                expectedSpeed = "30-50 tok/s (NPU)",
                hfRepo = "unsloth/Qwen3-4B-GGUF",
                hfPrecision = "Q4_K_M",
            ),
            // ── Reasoning / chain-of-thought ──
            ModelEntry(
                id = "deepseek-r1-1.5b",
                name = "DeepSeek-R1 1.5B",
                description = "Chain-of-thought reasoning & planning",
                role = ModelRole.REASONING,
                parameterCount = "1.5B",
                variants = listOf(
                    ModelVariant("geniex", "deepseek-r1-1.5b-int4-geniex.bin", 980_000_000L,
                        "$CDN_BASE/deepseek-r1-1.5b-int4-geniex.bin", ""),
                    ModelVariant("gguf", "DeepSeek-R1-Distill-Qwen-1.5B-Q4_K_M.gguf", 1_100_000_000L,
                        "$HF_DEEPSEEK/DeepSeek-R1-Distill-Qwen-1.5B-Q4_K_M.gguf", ""),
                ),
                contextLength = 16384,
                expectedSpeed = "40-60 tok/s (NPU)",
                hfRepo = "bartowski/DeepSeek-R1-Distill-Qwen-1.5B-GGUF",
                hfPrecision = "Q4_K_M",
            ),
            // ── Large reasoning (high-end devices) ──
            ModelEntry(
                id = "qwen3-8b",
                name = "Qwen3 8B",
                description = "Full-capability conversation + tool use",
                role = ModelRole.CONVERSATION,
                parameterCount = "8B",
                variants = listOf(
                    ModelVariant("geniex", "qwen3-8b-int4-geniex.bin", 5_200_000_000L,
                        "$CDN_BASE/qwen3-8b-int4-geniex.bin", ""),
                    ModelVariant("gguf", "Qwen3-8B-Q4_K_M.gguf", 5_000_000_000L,
                        "$HF_QWEN3_8B/Qwen3-8B-Q4_K_M.gguf", ""),
                ),
                contextLength = 131072,
                expectedSpeed = "15-25 tok/s (NPU)",
                hfRepo = "unsloth/Qwen3-8B-GGUF",
                hfPrecision = "Q4_K_M",
            ),
            // ── Voice input ──
            ModelEntry(
                id = "whisper-small",
                name = "Whisper Small",
                description = "Voice-to-text for hands-free interaction",
                role = ModelRole.VOICE,
                parameterCount = "244M",
                variants = listOf(
                    ModelVariant("geniex", "whisper-small-geniex.bin", 290_000_000L,
                        "$CDN_BASE/whisper-small-geniex.bin", ""),
                    ModelVariant("gguf", "whisper-small-q5.bin", 310_000_000L,
                        "$CDN_BASE/whisper-small-q5.bin", ""),
                ),
                contextLength = 0, // N/A for audio
                expectedSpeed = "~5x realtime (NPU)",
            ),
            // ── Code generation ──
            ModelEntry(
                id = "qwen3-coder-3b",
                name = "Qwen3 Coder 3B",
                description = "Code completion & script generation",
                role = ModelRole.CODE,
                parameterCount = "3B",
                variants = listOf(
                    ModelVariant("geniex", "qwen3-coder-3b-int4-geniex.bin", 1_900_000_000L,
                        "$CDN_BASE/qwen3-coder-3b-int4-geniex.bin", ""),
                    ModelVariant("gguf", "qwen3-coder-3b-q4km.gguf", 2_100_000_000L,
                        "$CDN_BASE/qwen3-coder-3b-q4km.gguf", ""),
                ),
                contextLength = 32768,
                expectedSpeed = "35-50 tok/s (NPU)",
            ),
            // ── Vision: lightweight (MiniCPM-V 4.6, ~600MB total) ──
            ModelEntry(
                id = "minicpm-v-4.6",
                name = "MiniCPM-V 4.6",
                description = "Ultra-light vision model — 0.8B, fast image understanding",
                role = ModelRole.VISION,
                parameterCount = "0.8B",
                variants = listOf(
                    ModelVariant("gguf", "MiniCPM-V-4.6-Q4_K_M.gguf", 580_000_000L,
                        "$HF_VLM_MINI/MiniCPM-V-4.6-Q4_K_M.gguf", ""),
                    ModelVariant("mmproj", "MiniCPM-V-4.6-mmproj-f16.gguf", 250_000_000L,
                        "$HF_VLM_MINI/MiniCPM-V-4.6-mmproj-f16.gguf", ""),
                ),
                contextLength = 4096,
                expectedSpeed = "30-50 tok/s (NPU+GPU)",
                hfRepo = "ggml-org/MiniCPM-V-4.6-GGUF",
                hfPrecision = "Q4_K_M",
            ),
            // ── Vision: full (Qwen3-VL 2B, ~2.2GB total) ──
            ModelEntry(
                id = "qwen3-vl-2b",
                name = "Qwen3-VL 2B",
                description = "Vision-language model — real image understanding",
                role = ModelRole.VISION,
                parameterCount = "2B",
                variants = listOf(
                    ModelVariant("gguf", "Qwen3-VL-2B-Instruct-Q4_K_M.gguf", 1_100_000_000L,
                        "$HF_VLM/Qwen3-VL-2B-Instruct-Q4_K_M.gguf", ""),
                    ModelVariant("mmproj", "mmproj-Qwen3VL-2B-Instruct-F16.gguf", 1_100_000_000L,
                        "https://hf-mirror.com/Qwen/Qwen3-VL-2B-Instruct-GGUF/resolve/main/mmproj-Qwen3VL-2B-Instruct-F16.gguf", ""),
                ),
                contextLength = 4096,
                expectedSpeed = "10-20 tok/s (NPU+GPU)",
                hfRepo = "unsloth/Qwen3-VL-2B-Instruct-GGUF",
                hfPrecision = "Q4_K_M",
            ),
        )
    }

    // ─── Data Classes ───────────────────────────────────────────────────────

    enum class ModelRole(val label: String, val icon: String) {
        INTENT("Intent", "⚡"),
        CONVERSATION("Conversation", "💬"),
        REASONING("Reasoning", "🧠"),
        VOICE("Voice", "🎤"),
        CODE("Code", "💻"),
        VISION("Vision", "👁️"),
    }

    data class ModelEntry(
        val id: String,
        val name: String,
        val description: String,
        val role: ModelRole,
        val parameterCount: String,
        val variants: List<ModelVariant>,
        val contextLength: Int,
        val expectedSpeed: String,
        val hfRepo: String = "",      // e.g. "Qwen/Qwen3-0.6B-GGUF"
        val hfPrecision: String = "", // e.g. "Q4_K_M"
    )

    data class ModelVariant(
        val backend: String,  // "geniex", "qnn", "gguf"
        val filename: String,
        val sizeBytes: Long,
        val url: String,
        val sha256: String,
    )

    sealed class DownloadState {
        data object Checking : DownloadState()
        data class Downloading(val progress: Float, val bytesDownloaded: Long, val totalBytes: Long) : DownloadState()
        data object Verifying : DownloadState()
        data class Ready(val path: String) : DownloadState()
        data class Error(val message: String) : DownloadState()
    }

    data class ModelStatus(
        val entry: ModelEntry,
        val downloaded: Boolean,
        val activeVariant: String?,
        val fileSizeOnDisk: Long,
    )

    // ─── State ──────────────────────────────────────────────────────────────

    private val modelsDir: File
        get() = File(context.filesDir, "models").also { it.mkdirs() }

    // ─── Public API ─────────────────────────────────────────────────────────

    /**
     * Get status of all models in catalog.
     */
    fun getModelStatuses(backendType: String): List<ModelStatus> {
        return CATALOG.map { entry ->
            val variant = selectVariant(entry, backendType)
            val file = variant?.let { File(modelsDir, it.filename) }
            val downloaded = file?.let { it.exists() && it.length() > 1_000_000 } ?: false
            ModelStatus(
                entry = entry,
                downloaded = downloaded,
                activeVariant = if (downloaded) variant?.backend else null,
                fileSizeOnDisk = file?.length() ?: 0L,
            )
        }
    }

    /**
     * Select best variant for the current device's backend.
     */
    fun selectVariant(entry: ModelEntry, backendType: String): ModelVariant? {
        // Prefer native backend, fall back to GGUF
        return entry.variants.find { it.backend == backendType }
            ?: entry.variants.find { it.backend == "gguf" }
    }

    /**
     * Get model path if already downloaded. Returns null otherwise.
     */
    fun getModelPath(modelId: String, backendType: String): String? {
        val entry = CATALOG.find { it.id == modelId } ?: return null
        val variant = selectVariant(entry, backendType) ?: return null
        val file = File(modelsDir, variant.filename)
        return if (file.exists() && file.length() > 1_000_000) {
            file.absolutePath
        } else null
    }

    /**
     * Legacy single-arg — get model path by backend key.
     * Looks for the primary conversation model for that backend.
     */
    fun getModelPath(backendKey: String): String? {
        return getPrimaryModelPath(backendKey)
    }

    /**
     * Get path for the primary conversation model (used by ChatBubbleService).
     */
    fun getPrimaryModelPath(backendType: String): String? {
        // Priority: conversation model > any downloaded model
        val conversation = CATALOG.filter { it.role == ModelRole.CONVERSATION }
        for (entry in conversation) {
            val path = getModelPath(entry.id, backendType)
            if (path != null) return path
        }
        // Fallback: any downloaded model
        for (entry in CATALOG) {
            val path = getModelPath(entry.id, backendType)
            if (path != null) return path
        }
        return null
    }

    /**
     * Get the mmproj file path for a VLM model (needed for vision inference).
     */
    fun getMmprojPath(modelId: String): String? {
        val entry = CATALOG.find { it.id == modelId } ?: return null
        val variant = entry.variants.find { it.backend == "mmproj" } ?: return null
        val file = File(modelsDir, variant.filename)
        return if (file.exists() && file.length() > 1_000_000) file.absolutePath else null
    }

    /**
     * Download all variants for a model (e.g. both gguf and mmproj for VLMs).
     * Reports progress for each variant sequentially.
     */
    fun downloadAllVariants(modelId: String): Flow<DownloadState> = flow {
        val entry = CATALOG.find { it.id == modelId }
        if (entry == null) {
            emit(DownloadState.Error("Unknown model: $modelId"))
            return@flow
        }

        for (variant in entry.variants) {
            val targetFile = File(modelsDir, variant.filename)

            // Already cached?
            if (targetFile.exists() && targetFile.length() > 1_000_000) {
                Log.i(TAG, "Variant cached: ${variant.filename}")
                continue
            }

            Log.i(TAG, "Downloading variant: ${variant.filename} (${variant.backend})")
            val tempFile = File(modelsDir, "${variant.filename}.tmp")
            try {
                var conn = URL(variant.url).openConnection() as HttpURLConnection
                conn.connectTimeout = 30_000
                conn.readTimeout = 120_000
                conn.instanceFollowRedirects = true

                if (tempFile.exists() && tempFile.length() > 0) {
                    conn.setRequestProperty("Range", "bytes=${tempFile.length()}-")
                }

                conn.connect()

                var redirectCount = 0
                while (conn.responseCode in listOf(301, 302, 303, 307, 308) && redirectCount < 5) {
                    val location = conn.getHeaderField("Location") ?: break
                    conn.disconnect()
                    conn = URL(location).openConnection() as HttpURLConnection
                    conn.connectTimeout = 30_000
                    conn.readTimeout = 120_000
                    if (tempFile.exists() && tempFile.length() > 0) {
                        conn.setRequestProperty("Range", "bytes=${tempFile.length()}-")
                    }
                    conn.connect()
                    redirectCount++
                }

                if (conn.responseCode !in listOf(200, 206)) {
                    emit(DownloadState.Error("HTTP ${conn.responseCode} for ${variant.filename}"))
                    return@flow
                }

                val totalBytes = variant.sizeBytes
                var downloaded = if (conn.responseCode == 206) tempFile.length() else 0L

                conn.inputStream.use { input ->
                    val append = conn.responseCode == 206
                    java.io.FileOutputStream(tempFile, append).use { output ->
                        val buffer = ByteArray(256 * 1024)
                        var read: Int
                        while (input.read(buffer).also { read = it } != -1) {
                            output.write(buffer, 0, read)
                            downloaded += read
                            emit(DownloadState.Downloading(
                                progress = downloaded.toFloat() / totalBytes,
                                bytesDownloaded = downloaded,
                                totalBytes = totalBytes,
                            ))
                        }
                        output.flush()
                    }
                }

                emit(DownloadState.Verifying)
                if (variant.sha256.isNotEmpty()) {
                    val hash = computeSha256(tempFile)
                    if (hash != variant.sha256) {
                        tempFile.delete()
                        emit(DownloadState.Error("Checksum mismatch for ${variant.filename}"))
                        return@flow
                    }
                }

                tempFile.renameTo(targetFile)
                Log.i(TAG, "Variant ready: ${variant.filename} (${targetFile.length()} bytes)")
            } catch (e: Exception) {
                Log.e(TAG, "Download variant failed: ${variant.filename}", e)
                emit(DownloadState.Error(e.message ?: "Download failed for ${variant.filename}"))
                return@flow
            }
        }

        // All variants downloaded
        val primaryFile = File(modelsDir, entry.variants.first().filename)
        emit(DownloadState.Ready(primaryFile.absolutePath))
    }.flowOn(Dispatchers.IO)

    /**
     * Legacy compatibility — get model key for a backend instance.
     */
    fun getModelKeyForBackend(backend: InferenceBackend): String = when (backend) {
        is GenieXBackend -> "geniex"
        is MtkNeuroPilotBackend -> "neuropilot"
        is QnnDlopenBackend -> "geniex"
        else -> "gguf"
    }

    /**
     * Download a specific model. Returns Flow of progress.
     */
    fun downloadModel(modelId: String, backendType: String): Flow<DownloadState> = flow {
        val entry = CATALOG.find { it.id == modelId }
        if (entry == null) {
            emit(DownloadState.Error("Unknown model: $modelId"))
            return@flow
        }

        val variant = selectVariant(entry, backendType)
        if (variant == null) {
            emit(DownloadState.Error("No variant available for backend: $backendType"))
            return@flow
        }

        val targetFile = File(modelsDir, variant.filename)

        // Already cached?
        emit(DownloadState.Checking)
        if (targetFile.exists() && targetFile.length() > 1_000_000) {
            Log.i(TAG, "Model cached: ${targetFile.absolutePath}")
            emit(DownloadState.Ready(targetFile.absolutePath))
            return@flow
        }

        // Download with resume support
        Log.i(TAG, "Downloading: ${variant.url}")
        val tempFile = File(modelsDir, "${variant.filename}.tmp")
        try {
            var conn = URL(variant.url).openConnection() as HttpURLConnection
            conn.connectTimeout = 30_000
            conn.readTimeout = 120_000
            conn.instanceFollowRedirects = true

            if (tempFile.exists() && tempFile.length() > 0) {
                conn.setRequestProperty("Range", "bytes=${tempFile.length()}-")
            }

            conn.connect()

            // Handle redirects manually (some CDNs do HTTPS→HTTPS redirect)
            var redirectCount = 0
            while (conn.responseCode in listOf(301, 302, 303, 307, 308) && redirectCount < 5) {
                val location = conn.getHeaderField("Location") ?: break
                conn.disconnect()
                conn = URL(location).openConnection() as HttpURLConnection
                conn.connectTimeout = 30_000
                conn.readTimeout = 120_000
                if (tempFile.exists() && tempFile.length() > 0) {
                    conn.setRequestProperty("Range", "bytes=${tempFile.length()}-")
                }
                conn.connect()
                redirectCount++
            }

            if (conn.responseCode !in listOf(200, 206)) {
                emit(DownloadState.Error("HTTP ${conn.responseCode}: ${conn.responseMessage}"))
                return@flow
            }

            val totalBytes = variant.sizeBytes
            var downloaded = if (conn.responseCode == 206) tempFile.length() else 0L

            conn.inputStream.use { input ->
                val append = conn.responseCode == 206
                java.io.FileOutputStream(tempFile, append).use { output ->
                    val buffer = ByteArray(256 * 1024)
                    var read: Int
                    while (input.read(buffer).also { read = it } != -1) {
                        output.write(buffer, 0, read)
                        downloaded += read
                        emit(DownloadState.Downloading(
                            progress = downloaded.toFloat() / totalBytes,
                            bytesDownloaded = downloaded,
                            totalBytes = totalBytes,
                        ))
                    }
                    output.flush()
                }
            }

            // Verify checksum if available
            emit(DownloadState.Verifying)
            if (variant.sha256.isNotEmpty()) {
                val hash = computeSha256(tempFile)
                if (hash != variant.sha256) {
                    tempFile.delete()
                    emit(DownloadState.Error("Checksum mismatch"))
                    return@flow
                }
            }

            tempFile.renameTo(targetFile)
            Log.i(TAG, "Model ready: ${targetFile.absolutePath} (${targetFile.length()} bytes)")
            emit(DownloadState.Ready(targetFile.absolutePath))

        } catch (e: Exception) {
            Log.e(TAG, "Download failed", e)
            emit(DownloadState.Error(e.message ?: "Download failed"))
        }
    }.flowOn(Dispatchers.IO)

    /**
     * Legacy compatibility — ensureModel by backend key.
     */
    fun ensureModel(backendKey: String): Flow<DownloadState> {
        // Map old backend keys to new model IDs
        return downloadModel("qwen3-4b", backendKey)
    }

    /**
     * Delete a downloaded model to free space.
     */
    suspend fun deleteModel(modelId: String, backendType: String) = withContext(Dispatchers.IO) {
        val entry = CATALOG.find { it.id == modelId } ?: return@withContext
        val variant = selectVariant(entry, backendType) ?: return@withContext
        File(modelsDir, variant.filename).delete()
        File(modelsDir, "${variant.filename}.tmp").delete()
    }

    /**
     * Total disk usage of all downloaded models.
     */
    fun getCachedSize(): Long = modelsDir.listFiles()
        ?.filter { !it.name.endsWith(".tmp") }
        ?.sumOf { it.length() } ?: 0L

    /**
     * Get catalog as JSON (for exposing to UI via WebView or native list).
     */
    fun getCatalogJson(backendType: String): String {
        val arr = JSONArray()
        getModelStatuses(backendType).forEach { status ->
            arr.put(JSONObject().apply {
                put("id", status.entry.id)
                put("name", status.entry.name)
                put("description", status.entry.description)
                put("role", status.entry.role.name)
                put("roleIcon", status.entry.role.icon)
                put("params", status.entry.parameterCount)
                put("contextLength", status.entry.contextLength)
                put("expectedSpeed", status.entry.expectedSpeed)
                put("downloaded", status.downloaded)
                put("sizeBytes", status.fileSizeOnDisk)
                val variant = selectVariant(status.entry, backendType)
                put("downloadSize", variant?.sizeBytes ?: 0)
                put("downloadSizeLabel", formatSize(variant?.sizeBytes ?: 0))
            })
        }
        return arr.toString(2)
    }

    private fun formatSize(bytes: Long): String = when {
        bytes >= 1_000_000_000 -> String.format("%.1f GB", bytes / 1e9)
        bytes >= 1_000_000 -> String.format("%.0f MB", bytes / 1e6)
        else -> "$bytes B"
    }

    private fun computeSha256(file: File): String {
        val digest = java.security.MessageDigest.getInstance("SHA-256")
        file.inputStream().use { input ->
            val buffer = ByteArray(256 * 1024)
            var read: Int
            while (input.read(buffer).also { read = it } != -1) {
                digest.update(buffer, 0, read)
            }
        }
        return digest.digest().joinToString("") { "%02x".format(it) }
    }

    /**
     * Import a model file manually pushed via adb.
     * Usage: adb push model.gguf /sdcard/Download/
     * Then call importModel("qwen3-4b", "/sdcard/Download/model.gguf")
     */
    suspend fun importModel(modelId: String, sourcePath: String, backendType: String): Boolean =
        withContext(Dispatchers.IO) {
            val entry = CATALOG.find { it.id == modelId } ?: return@withContext false
            val variant = selectVariant(entry, backendType) ?: return@withContext false
            val source = File(sourcePath)
            if (!source.exists()) return@withContext false
            val target = File(modelsDir, variant.filename)
            source.copyTo(target, overwrite = true)
            Log.i(TAG, "Imported $sourcePath → ${target.absolutePath}")
            true
        }
}
