package com.opensparx.agent.inference

import android.content.Context
import android.util.Log
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.flow
import kotlinx.coroutines.flow.flowOn
import kotlinx.coroutines.withContext
import java.io.File
import java.net.HttpURLConnection
import java.net.URL

/**
 * Handles model provisioning — download, verify, cache.
 *
 * Strategy:
 * 1. Check if model already cached on device
 * 2. If not, download from SparX CDN (not Qualcomm's — we host our own)
 * 3. Verify SHA-256 checksum
 * 4. Return path ready for [InferenceBackend.initialize]
 *
 * Model variants per backend:
 * - GenieX:      qwen3-4b-int4-geniex.bin  (~2.5 GB, QNN-compiled graph)
 * - NeuroPilot:  qwen3-4b-int4-mtk.bin     (~2.5 GB, NeuroPilot format)
 * - CPU:         qwen3-4b-q4km.gguf        (~2.7 GB, GGUF for llama.cpp)
 */
class ModelManager(private val context: Context) {

    companion object {
        private const val TAG = "ModelManager"

        // CDN base URL (your own hosting — no Qualcomm redistribution)
        private const val CDN_BASE = "https://models.opensparx.com/v1"

        // Model specs per backend
        val MODEL_SPECS = mapOf(
            "geniex" to ModelSpec(
                filename = "qwen3-4b-int4-geniex.bin",
                sizeBytes = 2_684_354_560L, // ~2.5 GB
                sha256 = "", // filled at release time
                url = "$CDN_BASE/qwen3-4b-int4-geniex.bin",
            ),
            "neuropilot" to ModelSpec(
                filename = "qwen3-4b-int4-mtk.bin",
                sizeBytes = 2_684_354_560L,
                sha256 = "",
                url = "$CDN_BASE/qwen3-4b-int4-mtk.bin",
            ),
            "cpu" to ModelSpec(
                filename = "qwen3-4b-q4km.gguf",
                sizeBytes = 2_831_155_200L, // ~2.7 GB
                sha256 = "",
                url = "$CDN_BASE/qwen3-4b-q4km.gguf",
            ),
        )
    }

    data class ModelSpec(
        val filename: String,
        val sizeBytes: Long,
        val sha256: String,
        val url: String,
    )

    sealed class DownloadState {
        data object Checking : DownloadState()
        data class Downloading(val progress: Float, val bytesDownloaded: Long, val totalBytes: Long) : DownloadState()
        data object Verifying : DownloadState()
        data class Ready(val path: String) : DownloadState()
        data class Error(val message: String) : DownloadState()
    }

    private val modelsDir: File
        get() = File(context.filesDir, "models").also { it.mkdirs() }

    /**
     * Get the model file path for a backend. Returns null if not downloaded.
     */
    fun getModelPath(backendKey: String): String? {
        val spec = MODEL_SPECS[backendKey] ?: return null
        val file = File(modelsDir, spec.filename)
        return if (file.exists() && file.length() == spec.sizeBytes) {
            file.absolutePath
        } else null
    }

    /**
     * Determine which model variant to use based on the detected backend.
     */
    fun getModelKeyForBackend(backend: InferenceBackend): String = when (backend) {
        is GenieXBackend -> "geniex"
        is MtkNeuroPilotBackend -> "neuropilot"
        is QnnDlopenBackend -> "geniex" // same format
        else -> "cpu"
    }

    /**
     * Ensure model is available — download if needed.
     * Returns a Flow of progress states for UI binding.
     */
    fun ensureModel(backendKey: String): Flow<DownloadState> = flow {
        val spec = MODEL_SPECS[backendKey]
        if (spec == null) {
            emit(DownloadState.Error("Unknown backend: $backendKey"))
            return@flow
        }

        val targetFile = File(modelsDir, spec.filename)

        // Already cached?
        emit(DownloadState.Checking)
        if (targetFile.exists() && targetFile.length() == spec.sizeBytes) {
            Log.i(TAG, "Model cached: ${targetFile.absolutePath}")
            emit(DownloadState.Ready(targetFile.absolutePath))
            return@flow
        }

        // Download
        Log.i(TAG, "Downloading model: ${spec.url}")
        val tempFile = File(modelsDir, "${spec.filename}.tmp")
        try {
            val conn = URL(spec.url).openConnection() as HttpURLConnection
            conn.connectTimeout = 15_000
            conn.readTimeout = 30_000

            // Resume support
            if (tempFile.exists()) {
                conn.setRequestProperty("Range", "bytes=${tempFile.length()}-")
            }

            conn.connect()
            val totalBytes = spec.sizeBytes
            var downloaded = if (tempFile.exists()) tempFile.length() else 0L

            conn.inputStream.use { input ->
                tempFile.outputStream().let { output ->
                    val buffer = ByteArray(256 * 1024) // 256KB chunks
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

            // Verify
            emit(DownloadState.Verifying)
            // TODO: SHA-256 verification when checksums are populated
            // val hash = computeSha256(tempFile)
            // if (hash != spec.sha256) { ... }

            // Atomic rename
            tempFile.renameTo(targetFile)
            Log.i(TAG, "Model ready: ${targetFile.absolutePath}")
            emit(DownloadState.Ready(targetFile.absolutePath))

        } catch (e: Exception) {
            Log.e(TAG, "Download failed", e)
            emit(DownloadState.Error(e.message ?: "Download failed"))
        }
    }.flowOn(Dispatchers.IO)

    /**
     * Delete cached model to free space.
     */
    suspend fun deleteModel(backendKey: String) = withContext(Dispatchers.IO) {
        val spec = MODEL_SPECS[backendKey] ?: return@withContext
        File(modelsDir, spec.filename).delete()
        File(modelsDir, "${spec.filename}.tmp").delete()
    }

    /**
     * Total cached model size in bytes.
     */
    fun getCachedSize(): Long = modelsDir.listFiles()
        ?.filter { !it.name.endsWith(".tmp") }
        ?.sumOf { it.length() } ?: 0L
}
