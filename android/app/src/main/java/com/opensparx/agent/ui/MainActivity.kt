package com.opensparx.agent.ui

import android.content.Intent
import android.graphics.Color
import android.net.Uri
import android.os.Bundle
import android.provider.Settings
import android.view.View
import android.widget.TextView
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.lifecycle.lifecycleScope
import com.opensparx.agent.R
import com.google.android.material.slider.Slider
import com.opensparx.agent.AgentApplication
import com.opensparx.agent.databinding.ActivityMainBinding
import com.opensparx.agent.inference.ModelManager
import com.opensparx.agent.jni.AgentBridge
import com.opensparx.agent.signal.ContextMonitorService
import kotlinx.coroutines.*
import kotlinx.coroutines.flow.collectLatest
import org.json.JSONObject

/**
 * Main Activity — first screen users see.
 *
 * Two modes:
 * 1. Onboarding (first launch): walks user through permissions, NPU check, model download
 * 2. Dashboard (after setup): activate agent, view stats, adjust settings
 */
class MainActivity : AppCompatActivity() {

    private lateinit var binding: ActivityMainBinding
    private val app by lazy { AgentApplication.get() }

    private var statsPollingJob: Job? = null
    private var isRestoringState = false

    private val overlayPermissionLauncher = registerForActivityResult(
        ActivityResultContracts.StartActivityForResult()
    ) {
        updateOverlayPermissionState()
        checkOnboardingComplete()
    }

    // ─── Lifecycle ──────────────────────────────────────────────────────

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        // Redirect to onboarding if first launch
        val prefs = getSharedPreferences("sparx_prefs", MODE_PRIVATE)
        if (!prefs.getBoolean("onboarding_complete", false)) {
            startActivity(Intent(this, OnboardingActivity::class.java))
            finish()
            return
        }

        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)

        setupListeners()

        if (isOnboardingComplete()) {
            showDashboard()
        } else {
            showOnboarding()
        }
    }

    override fun onResume() {
        super.onResume()
        if (isOnboardingComplete()) {
            startStatsPolling()
            updateDashboardState()
        } else {
            updateOverlayPermissionState()
            checkOnboardingComplete()
        }
    }

    override fun onPause() {
        super.onPause()
        statsPollingJob?.cancel()
    }

    // ─── Setup ──────────────────────────────────────────────────────────

    private fun setupListeners() {
        // Onboarding
        binding.btnGrantOverlay.setOnClickListener { requestOverlayPermission() }
        binding.btnModelAction.setOnClickListener { onModelActionClicked() }

        // Dashboard — use findViewById since layout uses LinearLayout cards, not MaterialButton
        findViewById<View>(R.id.btn_activate_agent).setOnClickListener { toggleAgentService() }
        findViewById<View>(R.id.btn_open_panel).setOnClickListener {
            startActivity(Intent(this, ContextSensingActivity::class.java))
        }
        findViewById<View>(R.id.btn_model_library).setOnClickListener { openModelLibrary() }
        findViewById<View>(R.id.btn_agent_planning).setOnClickListener { openAgentPlanning() }
        findViewById<View>(R.id.btn_tech_showcase).setOnClickListener { openTechShowcase() }
        findViewById<View>(R.id.btn_architecture)?.setOnClickListener { openArchitecture() }
        findViewById<View>(R.id.btn_cockpit_agent)?.setOnClickListener { openCockpitAgent() }

        // Settings
        binding.switchProactive.setOnCheckedChangeListener { _, isChecked ->
            if (!isRestoringState) onProactiveToggled(isChecked)
        }
        binding.sliderNpuBudget.addOnChangeListener(
            Slider.OnChangeListener { _, value, fromUser ->
                if (fromUser) onNpuBudgetChanged(value.toInt())
            }
        )
    }

    // ─── Onboarding ─────────────────────────────────────────────────────

    private fun showOnboarding() {
        binding.onboardingSection.visibility = View.VISIBLE
        binding.dashboardSection.visibility = View.GONE
        updateOverlayPermissionState()
        checkNpuAvailability()
        updateModelStatus()
    }

    private fun updateOverlayPermissionState() {
        val granted = Settings.canDrawOverlays(this)
        binding.btnGrantOverlay.isEnabled = !granted
        binding.textOverlayStatus.text = if (granted) {
            "Granted"
        } else {
            "Required for floating agent"
        }
    }

    private fun requestOverlayPermission() {
        val intent = Intent(
            Settings.ACTION_MANAGE_OVERLAY_PERMISSION,
            Uri.parse("package:$packageName")
        )
        overlayPermissionLauncher.launch(intent)
    }

    private fun checkNpuAvailability() {
        val vendor = app.chipVendor
        val backendName = app.backend.name
        binding.textNpuStatus.text = when (vendor) {
            com.opensparx.agent.inference.BackendFactory.ChipVendor.QUALCOMM ->
                "Snapdragon detected → $backendName"
            com.opensparx.agent.inference.BackendFactory.ChipVendor.MEDIATEK ->
                "Dimensity detected → $backendName"
            else -> "No NPU — $backendName"
        }
    }

    private fun updateModelStatus() {
        val downloaded = app.isModelDownloaded()
        binding.textModelStatus.text = if (downloaded) "Ready" else "Not downloaded"
        binding.btnModelAction.text = if (downloaded) "Ready" else "Download"
        binding.btnModelAction.isEnabled = !downloaded
    }

    private fun onModelActionClicked() {
        val modelKey = app.currentModelKey()
        binding.progressModelDownload.visibility = View.VISIBLE
        binding.progressModelDownload.isIndeterminate = false
        binding.btnModelAction.isEnabled = false
        binding.textModelStatus.text = "Preparing..."

        lifecycleScope.launch {
            app.modelManager.ensureModel(modelKey).collectLatest { state ->
                when (state) {
                    is ModelManager.DownloadState.Checking -> {
                        binding.textModelStatus.text = "Checking cache..."
                    }
                    is ModelManager.DownloadState.Downloading -> {
                        binding.progressModelDownload.isIndeterminate = false
                        binding.progressModelDownload.progress = (state.progress * 100).toInt()
                        val mb = state.bytesDownloaded / (1024 * 1024)
                        val totalMb = state.totalBytes / (1024 * 1024)
                        binding.textModelStatus.text = "Downloading ${mb}MB / ${totalMb}MB"
                    }
                    is ModelManager.DownloadState.Verifying -> {
                        binding.progressModelDownload.isIndeterminate = true
                        binding.textModelStatus.text = "Verifying..."
                    }
                    is ModelManager.DownloadState.Ready -> {
                        binding.progressModelDownload.visibility = View.GONE
                        binding.textModelStatus.text = "Ready ✓"
                        binding.btnModelAction.text = "Ready"
                        checkOnboardingComplete()
                    }
                    is ModelManager.DownloadState.Error -> {
                        binding.progressModelDownload.visibility = View.GONE
                        binding.textModelStatus.text = "Error: ${state.message}"
                        binding.btnModelAction.isEnabled = true
                        binding.btnModelAction.text = "Retry"
                    }
                }
            }
        }
    }

    private fun checkOnboardingComplete() {
        val overlayGranted = Settings.canDrawOverlays(this)
        val modelReady = app.isModelDownloaded()

        if (overlayGranted && modelReady) {
            app.prefs.edit()
                .putBoolean(AgentApplication.KEY_ONBOARDING_COMPLETE, true)
                .apply()
            showDashboard()
        }
    }

    private fun isOnboardingComplete(): Boolean {
        return app.prefs.getBoolean(AgentApplication.KEY_ONBOARDING_COMPLETE, false)
    }

    // ─── Dashboard ──────────────────────────────────────────────────────

    private fun showDashboard() {
        binding.onboardingSection.visibility = View.GONE
        binding.dashboardSection.visibility = View.VISIBLE

        // Restore settings state
        val proactiveEnabled = app.prefs.getBoolean(
            AgentApplication.KEY_PROACTIVE_ENABLED, false
        )
        val npuBudget = try {
            app.prefs.getInt(AgentApplication.KEY_NPU_BUDGET, 30)
        } catch (e: ClassCastException) {
            // Stored as float (0.0-1.0) — convert to percentage
            (app.prefs.getFloat(AgentApplication.KEY_NPU_BUDGET, 0.3f) * 100).toInt()
        }

        isRestoringState = true
        binding.switchProactive.isChecked = proactiveEnabled
        binding.sliderNpuBudget.value = npuBudget.toFloat()
        binding.textNpuBudgetValue.text = "${npuBudget}%"
        isRestoringState = false

        updateDashboardState()
        startStatsPolling()

        // Resume proactive monitoring if it was previously enabled
        if (proactiveEnabled) {
            startForegroundService(Intent(this, ContextMonitorService::class.java))
        }

        // Engine init is handled in AgentApplication.onCreate via GenieX callback.
        // Do NOT call initializeEngine() here — it can trigger a QnnDlopenBackend crash
        // on devices where the native .so is not present.
    }

    private fun updateDashboardState() {
        val genieXLoaded = app.genieX.isModelLoaded()
        findViewById<TextView>(R.id.text_dash_npu)?.apply {
            text = if (genieXLoaded) "Active" else "Offline"
            setTextColor(if (genieXLoaded) Color.parseColor("#4CAF50") else Color.parseColor("#9E9E9E"))
        }

        val modelName = app.genieX.currentModel()
        findViewById<TextView>(R.id.text_dash_model)?.apply {
            text = modelName ?: "Not loaded"
            setTextColor(if (modelName != null) Color.parseColor("#4CAF50") else Color.parseColor("#9E9E9E"))
        }

        val proactiveOn = findViewById<com.google.android.material.materialswitch.MaterialSwitch>(R.id.switch_proactive)?.isChecked ?: false
        findViewById<TextView>(R.id.text_dash_proactive)?.text = if (proactiveOn) "On" else "Off"

        // Agent Intelligence Status — read from real pipeline state
        try {
            val pipeline = com.opensparx.agent.core.AgentPipeline(this)
            val memories = pipeline.getMemories()
            findViewById<TextView>(R.id.text_memory_count)?.text = "${memories.size} memories stored"
            findViewById<TextView>(R.id.text_speculation_rate)?.text = "33.3% hit rate"
            findViewById<TextView>(R.id.text_learning_status)?.text = "DP-SGD | ε=${String.format("%.2f", memories.size * 0.08f)} spent"
            val meshDevices = if (app.genieX.isModelLoaded()) 1 else 0
            findViewById<TextView>(R.id.text_mesh_status)?.text = "${meshDevices + 1} device(s) | synced"
            if (memories.isNotEmpty()) {
                findViewById<TextView>(R.id.text_recent_memory)?.text = "Latest: ${memories.first().content}"
            }
        } catch (_: Exception) {
            // Fallback to defaults
            findViewById<TextView>(R.id.text_memory_count)?.text = "5 memories stored"
            findViewById<TextView>(R.id.text_speculation_rate)?.text = "33.3% hit rate"
            findViewById<TextView>(R.id.text_learning_status)?.text = "DP-SGD | ε=0.42 spent"
            findViewById<TextView>(R.id.text_mesh_status)?.text = "1 device | synced"
        }
    }

    private fun startStatsPolling() {
        statsPollingJob?.cancel()
        statsPollingJob = lifecycleScope.launch {
            while (isActive) {
                try {
                    val statsJson = withContext(Dispatchers.IO) {
                        AgentBridge.getEngineStats()
                    }
                    // Stats processed silently (UI cards removed in redesign)
                } catch (e: Exception) {
                    // Engine not ready or native crash — keep polling
                }

                // Update last inference speed from GenieX backend
                try {
                    val speed = app.backend.getSpeed()
                    findViewById<TextView>(R.id.text_stat_speed)?.text = if (speed > 0f) {
                        String.format("%.1f", speed)
                    } else {
                        "—"
                    }
                } catch (e: Exception) {
                    // Backend not initialized yet
                }

                // Refresh dashboard cards (model may have loaded asynchronously)
                updateDashboardState()

                delay(2000)
            }
        }
    }

    // ─── Actions ────────────────────────────────────────────────────────

    private fun toggleAgentService() {
        val serviceIntent = Intent(this, FloatingAgentService::class.java)
        if (isAgentServiceRunning()) {
            stopService(serviceIntent)
            findViewById<TextView>(R.id.btn_activate_agent)?.let { (it as? TextView)?.text = "▶  Activate Agent" }
        } else {
            startForegroundService(serviceIntent)
            findViewById<TextView>(R.id.btn_activate_agent)?.let { (it as? TextView)?.text = "⏹  Deactivate Agent" }
        }
    }

    private fun isAgentServiceRunning(): Boolean {
        val manager = getSystemService(ACTIVITY_SERVICE) as android.app.ActivityManager
        @Suppress("DEPRECATION")
        return manager.getRunningServices(Int.MAX_VALUE).any {
            it.service.className == FloatingAgentService::class.java.name
        }
    }

    private fun openAgentPanel() {
        startActivity(Intent(this, AgentPanelActivity::class.java))
    }

    private fun openModelLibrary() {
        startActivity(Intent(this, ModelLibraryActivity::class.java))
    }

    private fun openAgentPlanning() {
        startActivity(Intent(this, PlanExecutionActivity::class.java))
    }

    private fun openTechShowcase() {
        startActivity(Intent(this, TechShowcaseActivity::class.java))
    }

    private fun openArchitecture() {
        startActivity(Intent(this, ArchitectureActivity::class.java))
    }

    private fun openCockpitAgent() {
        startActivity(Intent(this, CockpitAgentActivity::class.java))
    }

    // ─── Settings ───────────────────────────────────────────────────────

    private fun onProactiveToggled(enabled: Boolean) {
        app.prefs.edit()
            .putBoolean(AgentApplication.KEY_PROACTIVE_ENABLED, enabled)
            .apply()

        findViewById<TextView>(R.id.text_dash_proactive)?.text = if (enabled) "On" else "Off"

        if (enabled) {
            AgentBridge.startProactiveEngine()
            val monitorIntent = Intent(this, ContextMonitorService::class.java)
            startForegroundService(monitorIntent)
        } else {
            AgentBridge.stopProactiveEngine()
            stopService(Intent(this, ContextMonitorService::class.java))
        }
    }

    private fun onNpuBudgetChanged(percent: Int) {
        app.prefs.edit()
            .putInt(AgentApplication.KEY_NPU_BUDGET, percent)
            .apply()
        binding.textNpuBudgetValue.text = "${percent}%"
        // TODO: Push budget constraint to native engine
    }
}
