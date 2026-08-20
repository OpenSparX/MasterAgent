package com.opensparx.agent.ui

import android.content.Intent
import android.os.Bundle
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.Button
import android.widget.ProgressBar
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import androidx.lifecycle.lifecycleScope
import androidx.recyclerview.widget.LinearLayoutManager
import androidx.recyclerview.widget.RecyclerView
import com.opensparx.agent.AgentApplication
import com.google.android.material.snackbar.Snackbar
import com.opensparx.agent.R
import com.opensparx.agent.inference.BackendFactory
import com.opensparx.agent.inference.GenieXSdkBackend
import com.opensparx.agent.inference.ModelManager
import com.opensparx.agent.inference.ModelManager.DownloadState
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.collectLatest
import kotlinx.coroutines.launch
import java.io.File

/**
 * Model Library — browse, download, and manage on-device models.
 *
 * Shows all available models with:
 * - Role icon and description
 * - Size and expected performance
 * - Download/delete actions
 * - Progress tracking
 */
class ModelLibraryActivity : AppCompatActivity() {

    private lateinit var app: AgentApplication
    private lateinit var recyclerView: RecyclerView
    private lateinit var adapter: ModelAdapter
    private var backendType = "gguf"

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_model_library)

        app = AgentApplication.get()

        // Default to GGUF for all devices (universally downloadable from HuggingFace)
        // GenieX/NeuroPilot .bin files require custom compilation and can't be downloaded directly
        backendType = "gguf"
        val chipVendor = BackendFactory.detectChipVendor()

        recyclerView = findViewById(R.id.recycler_models)
        recyclerView.layoutManager = LinearLayoutManager(this)

        adapter = ModelAdapter(
            models = app.modelManager.getModelStatuses(backendType),
            onDownload = { modelId -> startDownload(modelId) },
            onDelete = { modelId -> deleteModel(modelId) },
        )
        recyclerView.adapter = adapter

        val chipLabel = when (chipVendor) {
            BackendFactory.ChipVendor.QUALCOMM -> "Snapdragon ${android.os.Build.SOC_MODEL}"
            BackendFactory.ChipVendor.MEDIATEK -> "Dimensity"
            else -> "CPU"
        }
        findViewById<TextView>(R.id.text_backend_info).text =
            "Chip: $chipLabel | Format: GGUF (CPU) — NPU models require qai_hub compile"
        findViewById<TextView>(R.id.text_storage_used).text =
            "Storage: ${formatSize(app.modelManager.getCachedSize())}"

        findViewById<Button>(R.id.btn_npu_setup).setOnClickListener {
            startActivity(Intent(this, NpuSetupActivity::class.java))
        }

        // Auto-load first downloaded model if not already loaded
        if (!app.genieX.isModelLoaded()) {
            val downloaded = app.modelManager.getModelStatuses(backendType)
                .firstOrNull { it.downloaded }
            if (downloaded != null) {
                autoLoadModel(downloaded.entry)
            }
        }
    }

    private var downloadJob: Job? = null

    private var currentDownloadId: String? = null

    private fun startDownload(modelId: String) {
        // Don't cancel if already downloading this same model
        if (downloadJob?.isActive == true && currentDownloadId == modelId) return
        downloadJob?.cancel()
        currentDownloadId = modelId
        val entry = ModelManager.CATALOG.find { it.id == modelId } ?: return

        // Use downloadAllVariants for VLM models (downloads both gguf + mmproj)
        val hasMultipleVariants = entry.variants.any { it.backend == "mmproj" }
        adapter.updateDownloadState(modelId, DownloadState.Checking)
        downloadJob = lifecycleScope.launch {
            try {
                val flow = if (hasMultipleVariants) {
                    app.modelManager.downloadAllVariants(modelId)
                } else {
                    app.modelManager.downloadModel(modelId, backendType)
                }
                flow.collectLatest { state ->
                    adapter.updateDownloadState(modelId, state)
                    if (state is DownloadState.Ready) {
                        refreshList()
                        autoLoadModel(entry)
                    }
                    if (state is DownloadState.Error) {
                        refreshList()
                    }
                }
            } catch (_: Exception) {
                adapter.updateDownloadState(modelId, DownloadState.Error("Download failed"))
            }
        }
    }

    private fun autoLoadModel(entry: ModelManager.ModelEntry) {
        lifecycleScope.launch {
            // First try GenieX SDK loadModel (for models pulled via GenieX)
            var loaded = app.genieX.loadModel(
                modelName = entry.hfRepo.ifEmpty { entry.id },
                computeUnit = null,
                contextSize = entry.contextLength.coerceAtMost(4096),
                runtimeId = "llama_cpp"
            )

            // If that fails, try loading from the local file path directly
            if (!loaded) {
                val modelPath = app.modelManager.getModelPath(entry.id, backendType)
                if (modelPath != null) {
                    loaded = app.genieX.loadModelFromPath(
                        modelPath = modelPath,
                        modelName = entry.name,
                        contextSize = entry.contextLength.coerceAtMost(4096),
                        runtimeId = "llama_cpp"
                    )
                }
            }

            if (loaded) {
                Snackbar.make(recyclerView, "✓ ${entry.name} ready", Snackbar.LENGTH_SHORT).show()
            }
        }
    }

    private fun deleteModel(modelId: String) {
        lifecycleScope.launch {
            app.modelManager.deleteModel(modelId, backendType)
            refreshList()
        }
    }

    private fun refreshList() {
        adapter.updateModels(app.modelManager.getModelStatuses(backendType))
        findViewById<TextView>(R.id.text_storage_used).text =
            "Storage: ${formatSize(app.modelManager.getCachedSize())}"
    }

    private fun formatSize(bytes: Long): String = when {
        bytes >= 1_000_000_000 -> String.format("%.1f GB", bytes / 1e9)
        bytes >= 1_000_000 -> String.format("%.0f MB", bytes / 1e6)
        else -> "0 MB"
    }
}

// ─── RecyclerView Adapter ───────────────────────────────────────────────────

class ModelAdapter(
    private var models: List<ModelManager.ModelStatus>,
    private val onDownload: (String) -> Unit,
    private val onDelete: (String) -> Unit,
) : RecyclerView.Adapter<ModelAdapter.ViewHolder>() {

    private val downloadStates = mutableMapOf<String, DownloadState>()

    class ViewHolder(view: View) : RecyclerView.ViewHolder(view) {
        val icon: TextView = view.findViewById(R.id.text_model_icon)
        val name: TextView = view.findViewById(R.id.text_model_name)
        val description: TextView = view.findViewById(R.id.text_model_desc)
        val meta: TextView = view.findViewById(R.id.text_model_meta)
        val size: TextView = view.findViewById(R.id.text_model_size)
        val status: TextView = view.findViewById(R.id.text_model_status)
        val progress: ProgressBar = view.findViewById(R.id.progress_download)
        val btnAction: Button = view.findViewById(R.id.btn_model_action)
    }

    override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): ViewHolder {
        val view = LayoutInflater.from(parent.context)
            .inflate(R.layout.item_model, parent, false)
        return ViewHolder(view)
    }

    override fun onBindViewHolder(holder: ViewHolder, position: Int) {
        val item = models[position]
        val entry = item.entry

        holder.icon.text = entry.role.icon
        holder.name.text = "${entry.name} (${entry.parameterCount})"
        holder.description.text = entry.description
        holder.meta.text = buildString {
            if (entry.contextLength > 0) append("${entry.contextLength / 1024}K ctx | ")
            append(entry.expectedSpeed)
        }

        val variant = ModelManager.CATALOG
            .find { it.id == entry.id }
            ?.variants?.firstOrNull()
        holder.size.text = formatSize(variant?.sizeBytes ?: 0)

        // Download state
        val state = downloadStates[entry.id]
        when {
            state is DownloadState.Checking -> {
                holder.progress.visibility = View.VISIBLE
                holder.progress.isIndeterminate = true
                holder.status.text = "Connecting..."
                holder.status.setTextColor(0xFFFFAB00.toInt())
                holder.btnAction.text = "..."
                holder.btnAction.isEnabled = false
                holder.itemView.setOnClickListener(null)
            }
            state is DownloadState.Downloading -> {
                holder.progress.visibility = View.VISIBLE
                holder.progress.isIndeterminate = false
                holder.progress.progress = (state.progress * 100).toInt()
                holder.status.text = "${(state.progress * 100).toInt()}%"
                holder.status.setTextColor(0xFF00E676.toInt())
                holder.btnAction.text = "⏸"
                holder.btnAction.isEnabled = false
                holder.itemView.setOnClickListener(null)
            }
            item.downloaded -> {
                // Check if model has mmproj variant that's missing
                val hasMmproj = entry.variants.any { it.backend == "mmproj" }
                val mmprojDownloaded = if (hasMmproj) {
                    val mmprojVariant = entry.variants.find { it.backend == "mmproj" }
                    val mmprojFile = mmprojVariant?.let {
                        File(holder.itemView.context.filesDir.path + "/models/" + it.filename)
                    }
                    mmprojFile != null && mmprojFile.exists() && mmprojFile.length() > 1_000_000
                } else true

                if (!mmprojDownloaded) {
                    // Partial download — main GGUF is there but mmproj is missing
                    holder.progress.visibility = View.GONE
                    holder.status.text = "⚠️ mmproj missing"
                    holder.status.setTextColor(0xFFFFAB00.toInt())
                    holder.btnAction.text = "Fix"
                    holder.btnAction.isEnabled = true
                    holder.btnAction.setOnClickListener { onDownload(entry.id) }
                } else {
                    holder.progress.visibility = View.GONE
                    holder.status.text = "✓ Ready"
                    holder.status.setTextColor(0xFF00FF88.toInt())
                    holder.btnAction.text = "Delete"
                    holder.btnAction.setOnClickListener { onDelete(entry.id) }
                    holder.btnAction.isEnabled = true
                }
            }
            state is DownloadState.Error -> {
                holder.progress.visibility = View.GONE
                holder.status.text = "✗ ${state.message}"
                holder.status.setTextColor(0xFFFF4444.toInt())
                holder.btnAction.text = "Retry"
                holder.btnAction.isEnabled = true
                holder.btnAction.setOnClickListener { onDownload(entry.id) }
                holder.itemView.setOnClickListener { onDownload(entry.id) }
            }
            else -> {
                holder.progress.visibility = View.GONE
                holder.status.text = ""
                holder.btnAction.text = "Download"
                holder.btnAction.isEnabled = true
                holder.btnAction.setOnClickListener { onDownload(entry.id) }
            }
        }
    }

    override fun getItemCount() = models.size

    fun updateDownloadState(modelId: String, state: DownloadState) {
        downloadStates[modelId] = state
        val idx = models.indexOfFirst { it.entry.id == modelId }
        if (idx >= 0) notifyItemChanged(idx)
    }

    fun updateModels(newModels: List<ModelManager.ModelStatus>) {
        models = newModels
        notifyDataSetChanged()
    }

    private fun formatSize(bytes: Long): String = when {
        bytes >= 1_000_000_000 -> String.format("%.1f GB", bytes / 1e9)
        bytes >= 1_000_000 -> String.format("%.0f MB", bytes / 1e6)
        else -> "$bytes B"
    }
}
