package com.opensparx.agent.ui

import android.content.Intent
import android.net.Uri
import android.os.Bundle
import android.provider.Settings
import android.view.View
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.lifecycle.lifecycleScope
import com.google.android.material.slider.Slider
import com.opensparx.agent.AgentApplication
import com.opensparx.agent.databinding.ActivityMainBinding
import com.opensparx.agent.jni.AgentBridge
import com.opensparx.agent.signal.ContextMonitorService
import kotlinx.coroutines.*
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

    private val overlayPermissionLauncher = registerForActivityResult(
        ActivityResultContracts.StartActivityForResult()
    ) {
        updateOverlayPermissionState()
        checkOnboardingComplete()
    }

    // ─── Lifecycle ──────────────────────────────────────────────────────

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
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

        // Dashboard
        binding.btnActivateAgent.setOnClickListener { toggleAgentService() }
        binding.btnOpenPanel.setOnClickListener { openAgentPanel() }

        // Settings
        binding.switchProactive.setOnCheckedChangeListener { _, isChecked ->
            onProactiveToggled(isChecked)
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
        lifecycleScope.launch(Dispatchers.IO) {
            val available = try {
                AgentBridge.isNpuAvailable()
            } catch (e: UnsatisfiedLinkError) {
                false
            }
            withContext(Dispatchers.Main) {
                binding.textNpuStatus.text = if (available) {
                    "Snapdragon NPU detected"
                } else {
                    "Not available — CPU fallback"
                }
            }
        }
    }

    private fun updateModelStatus() {
        val downloaded = app.isModelDownloaded()
        binding.textModelStatus.text = if (downloaded) "Ready" else "Not downloaded"
        binding.btnModelAction.text = if (downloaded) "Ready" else "Download"
        binding.btnModelAction.isEnabled = !downloaded
    }

    private fun onModelActionClicked() {
        // TODO: Trigger actual model download from CDN
        // For now, show progress indicator as placeholder
        binding.progressModelDownload.visibility = View.VISIBLE
        binding.progressModelDownload.isIndeterminate = true
        binding.btnModelAction.isEnabled = false
        binding.textModelStatus.text = "Downloading..."

        lifecycleScope.launch {
            // Placeholder — real implementation will use DownloadManager or OkHttp
            delay(2000)
            binding.progressModelDownload.visibility = View.GONE
            updateModelStatus()
            checkOnboardingComplete()
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
        val npuBudget = app.prefs.getInt(AgentApplication.KEY_NPU_BUDGET, 30)

        binding.switchProactive.isChecked = proactiveEnabled
        binding.sliderNpuBudget.value = npuBudget.toFloat()
        binding.textNpuBudgetValue.text = "${npuBudget}%"

        updateDashboardState()
        startStatsPolling()

        // Initialize engine if not already done
        if (!app.isEngineReady) {
            lifecycleScope.launch(Dispatchers.IO) {
                app.initializeEngine()
                withContext(Dispatchers.Main) { updateDashboardState() }
            }
        }
    }

    private fun updateDashboardState() {
        val npuAvailable = try {
            AgentBridge.isNpuAvailable()
        } catch (e: UnsatisfiedLinkError) {
            false
        }
        binding.textDashNpu.text = if (npuAvailable) "Ready" else "CPU"
        binding.textDashModel.text = if (app.isEngineReady) "Loaded" else "..."
        binding.textDashProactive.text = if (
            binding.switchProactive.isChecked
        ) "On" else "Off"
    }

    private fun startStatsPolling() {
        statsPollingJob?.cancel()
        statsPollingJob = lifecycleScope.launch {
            while (isActive) {
                try {
                    val statsJson = withContext(Dispatchers.IO) {
                        AgentBridge.getEngineStats()
                    }
                    val stats = JSONObject(statsJson)
                    binding.textStatSignals.text = stats.optLong("signals_processed", 0).toString()
                    binding.textStatTriggers.text = stats.optLong("triggers_fired", 0).toString()
                    binding.textStatTasks.text = stats.optLong("tasks_completed", 0).toString()
                } catch (e: Exception) {
                    // Engine not ready or native crash — keep polling
                }
                delay(2000)
            }
        }
    }

    // ─── Actions ────────────────────────────────────────────────────────

    private fun toggleAgentService() {
        val serviceIntent = Intent(this, FloatingAgentService::class.java)
        if (isAgentServiceRunning()) {
            stopService(serviceIntent)
            binding.btnActivateAgent.text = "Activate Agent"
        } else {
            startForegroundService(serviceIntent)
            binding.btnActivateAgent.text = "Deactivate Agent"
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

    // ─── Settings ───────────────────────────────────────────────────────

    private fun onProactiveToggled(enabled: Boolean) {
        app.prefs.edit()
            .putBoolean(AgentApplication.KEY_PROACTIVE_ENABLED, enabled)
            .apply()

        binding.textDashProactive.text = if (enabled) "On" else "Off"

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
