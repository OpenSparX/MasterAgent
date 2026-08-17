package com.opensparx.agent

import android.app.Application
import android.content.SharedPreferences
import android.util.Log
import com.opensparx.agent.inference.BackendFactory
import com.opensparx.agent.inference.InferenceBackend
import com.opensparx.agent.inference.ModelManager
import com.opensparx.agent.inference.BackendConfig
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

    private val appScope = CoroutineScope(SupervisorJob() + Dispatchers.Main)

    override fun onCreate() {
        super.onCreate()
        instance = this
        prefs = getSharedPreferences(PREFS_NAME, MODE_PRIVATE)

        // Auto-detect best backend for this device
        chipVendor = BackendFactory.detectChipVendor()
        backend = BackendFactory.create(this)
        modelManager = ModelManager(this)

        Log.i(TAG, "Chip: $chipVendor → Backend: ${backend.name}")
    }

    /**
     * Initialize the inference backend with the downloaded model.
     * Call from MainActivity after model download completes.
     */
    suspend fun initializeEngine(): Boolean {
        val modelKey = modelManager.getModelKeyForBackend(backend)
        val modelPath = modelManager.getModelPath(modelKey)

        if (modelPath == null) {
            Log.e(TAG, "Model not downloaded for backend: ${backend.name}")
            return false
        }

        val budget = prefs.getFloat(KEY_NPU_BUDGET, 0.3f)
        val config = BackendConfig(npuBudget = budget)

        isEngineReady = backend.initialize(modelPath, config)

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
