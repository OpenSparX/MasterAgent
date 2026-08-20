package com.opensparx.agent

import android.app.Application
import android.content.SharedPreferences
import android.util.Log
import com.opensparx.agent.inference.BackendFactory
import com.opensparx.agent.inference.CpuFallbackBackend
import com.opensparx.agent.inference.InferenceBackend
import com.opensparx.agent.inference.ModelManager
import com.opensparx.agent.inference.BackendConfig
import com.opensparx.agent.inference.GenieXSdkBackend
import com.opensparx.agent.jni.AgentBridge
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.launch

/**
 * Application class — engine lifecycle and global state.
 *
 * Responsibilities:
 * - Auto-detect chipset and select inference backend (GenieX / NeuroPilot / CPU)
 * - Manage model download via [ModelManager]
 * - Initialize the selected backend transparently
 * - Hold SharedPreferences for settings (proactive mode, NPU budget)
 */
class AgentApplication : Application() {

    companion object {
        private const val TAG = "SparxApp"
        const val PREFS_NAME = "sparx_prefs"
        const val KEY_PROACTIVE_ENABLED = "proactive_enabled"
        const val KEY_NPU_BUDGET = "npu_budget"
        const val KEY_ONBOARDING_COMPLETE = "onboarding_complete"

        @Volatile
        private lateinit var instance: AgentApplication
        fun get(): AgentApplication = instance
    }

    lateinit var prefs: SharedPreferences
        private set

    /** The auto-detected inference backend. Never null after onCreate. */
    lateinit var backend: InferenceBackend
        private set

    /** Model download/cache manager. */
    lateinit var modelManager: ModelManager
        private set

    /** Detected chip vendor for UI display. */
    lateinit var chipVendor: BackendFactory.ChipVendor
        private set

    var isEngineReady = false
        private set

    /** Loading state for UI to show progress instead of "not downloaded" */
    enum class LoadingState { NOT_STARTED, LOADING_LLM, LOADING_VLM, READY, FAILED }
    @Volatile var modelLoadingState = LoadingState.NOT_STARTED

    /** GenieX SDK backend — official Qualcomm inference runtime. */
    lateinit var genieX: GenieXSdkBackend
        private set

    private val appScope = CoroutineScope(SupervisorJob() + Dispatchers.Main)

    override fun onCreate() {
        super.onCreate()
        instance = this
        prefs = getSharedPreferences(PREFS_NAME, MODE_PRIVATE)

        // Auto-detect best backend for this device
        chipVendor = BackendFactory.detectChipVendor()
        backend = BackendFactory.create(this)
        modelManager = ModelManager(this)

        // Initialize GenieX SDK
        genieX = GenieXSdkBackend(this)
        genieX.initSdk {
            Log.i(TAG, "GenieX SDK ready — attempting to load model")
            // SDK is now ready, try to auto-load downloaded model
            appScope.launch {
                try {
                    val modelPath = modelManager.getPrimaryModelPath("gguf")
                    if (modelPath != null) {
                        modelLoadingState = LoadingState.LOADING_LLM
                        Log.i(TAG, "Loading model from: $modelPath")
                        val loaded = genieX.loadModelFromPath(
                            modelPath = modelPath,
                            modelName = "Qwen3-0.6B",
                            contextSize = 4096,
                            runtimeId = "llama_cpp"
                        )
                        Log.i(TAG, "Model load result: $loaded")
                        if (loaded) isEngineReady = true
                    }

                    // Also try to load VLM if available (prefer MiniCPM-V as it's smaller)
                    var vlmPath = modelManager.getModelPath("minicpm-v-4.6", "gguf")
                    var mmprojPath = modelManager.getMmprojPath("minicpm-v-4.6")
                    var vlmName = "MiniCPM-V-4.6"
                    // Fallback to Qwen3-VL if MiniCPM not available
                    if (vlmPath == null || mmprojPath == null) {
                        vlmPath = modelManager.getModelPath("qwen3-vl-2b", "gguf")
                        mmprojPath = modelManager.getMmprojPath("qwen3-vl-2b")
                        vlmName = "Qwen3-VL-2B"
                    }
                    if (vlmPath != null && mmprojPath != null) {
                        modelLoadingState = LoadingState.LOADING_VLM
                        val vlmLoaded = genieX.loadVlm(vlmPath, mmprojPath, vlmName)
                        Log.i(TAG, "VLM load result: $vlmLoaded ($vlmName)")
                    }
                    modelLoadingState = LoadingState.READY
                } catch (e: Exception) {
                    Log.e(TAG, "Auto-load in GenieX callback failed: ${e.message}")
                    modelLoadingState = LoadingState.FAILED
                }
            }
        }

        Log.i(TAG, "Chip: $chipVendor → Backend: ${backend.name}")
    }

    /**
     * Initialize the inference backend with the downloaded model.
     * Call from MainActivity after model download completes.
     */
    suspend fun initializeEngine(): Boolean {
        // Try native backend first, then fall back to GGUF
        var modelPath = modelManager.getModelPath(modelManager.getModelKeyForBackend(backend))
        if (modelPath == null) {
            // Fallback: try GGUF model path
            modelPath = modelManager.getPrimaryModelPath("gguf")
        }

        if (modelPath == null) {
            Log.e(TAG, "Model not downloaded for backend: ${backend.name}")
            return false
        }

        val budget = prefs.getFloat(KEY_NPU_BUDGET, 0.3f)
        val config = BackendConfig(npuBudget = budget)

        try {
            isEngineReady = backend.initialize(modelPath, config)
        } catch (e: Throwable) {
            Log.e(TAG, "Backend ${backend.name} init threw: ${e.message}")
            isEngineReady = false
        }

        // If primary backend failed, skip CPU fallback (no native lib available)
        // Just rely on GenieX SDK path which has its own llama_cpp bundled
        if (!isEngineReady) {
            Log.w(TAG, "${backend.name} init failed — will use GenieX llama_cpp runtime for inference")
        }

        // Also init legacy JNI bridge for proactive engine
        if (isEngineReady) {
            AgentBridge.initialize(modelPath)
        }

        Log.i(TAG, "Engine ready=$isEngineReady via ${backend.name}")
        return isEngineReady
    }

    suspend fun shutdownEngine() {
        if (isEngineReady) {
            backend.release()
            AgentBridge.shutdown()
            isEngineReady = false
        }
    }

    /**
     * Quick check: is the model already cached for the current backend?
     */
    fun isModelDownloaded(): Boolean {
        val key = modelManager.getModelKeyForBackend(backend)
        return modelManager.getModelPath(key) != null
    }

    /**
     * Get the model key string for the current backend (for ModelManager calls).
     */
    fun currentModelKey(): String = modelManager.getModelKeyForBackend(backend)
}
