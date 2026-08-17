package com.opensparx.agent

import android.app.Application
import android.content.SharedPreferences
import com.opensparx.agent.jni.AgentBridge

/**
 * Application class — engine lifecycle and global state.
 *
 * Responsibilities:
 * - Initialize AgentBridge native engine on cold start
 * - Hold SharedPreferences for settings (proactive mode, NPU budget)
 * - Track model download/readiness state
 */
class AgentApplication : Application() {

    lateinit var prefs: SharedPreferences
        private set

    var isEngineReady = false
        private set

    override fun onCreate() {
        super.onCreate()
        instance = this
        prefs = getSharedPreferences(PREFS_NAME, MODE_PRIVATE)
    }

    /**
     * Initialize the native engine with the downloaded model.
     * Call from MainActivity after model is confirmed ready.
     */
    fun initializeEngine(): Boolean {
        val modelPath = getModelPath()
        if (modelPath == null || !java.io.File(modelPath).exists()) return false

        isEngineReady = AgentBridge.initialize(modelPath)
        return isEngineReady
    }

    fun shutdownEngine() {
        if (isEngineReady) {
            AgentBridge.shutdown()
            isEngineReady = false
        }
    }

    fun getModelPath(): String? {
        val modelFile = java.io.File(filesDir, MODEL_FILENAME)
        return if (modelFile.exists()) modelFile.absolutePath else null
    }

    fun isModelDownloaded(): Boolean {
        return java.io.File(filesDir, MODEL_FILENAME).exists()
    }

    companion object {
        const val PREFS_NAME = "sparx_prefs"
        const val KEY_PROACTIVE_ENABLED = "proactive_enabled"
        const val KEY_NPU_BUDGET = "npu_budget"
        const val KEY_ONBOARDING_COMPLETE = "onboarding_complete"
        const val MODEL_FILENAME = "qwen3-4b-int4.bin"

        @Volatile
        private lateinit var instance: AgentApplication

        fun get(): AgentApplication = instance
    }
}
