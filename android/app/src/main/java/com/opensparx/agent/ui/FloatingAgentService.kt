package com.opensparx.agent.ui

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Intent
import android.graphics.PixelFormat
import android.os.IBinder
import android.view.Gravity
import android.view.LayoutInflater
import android.view.MotionEvent
import android.view.View
import android.view.WindowManager
import android.widget.ImageView
import android.widget.TextView
import androidx.core.app.NotificationCompat
import com.opensparx.agent.R
import com.opensparx.agent.jni.AgentBridge
import kotlinx.coroutines.*

/**
 * Floating Agent Service — the "desktop pet" overlay.
 *
 * Displays a small floating widget that:
 * - Shows agent status (idle/thinking/acting)
 * - Pulses when proactive engine detects something
 * - Tap to expand into full Agent Panel
 * - Drag to reposition
 * - Shows mini DAG animation when tasks are running
 */
class FloatingAgentService : Service() {

    private lateinit var windowManager: WindowManager
    private lateinit var floatingView: View
    private lateinit var statusIcon: ImageView
    private lateinit var statusText: TextView

    private val serviceScope = CoroutineScope(Dispatchers.Main + SupervisorJob())
    private var isExpanded = false

    companion object {
        const val CHANNEL_ID = "sparx_floating_agent"
        const val NOTIFICATION_ID = 1001
        const val ACTION_SHOW_PANEL = "com.opensparx.agent.SHOW_PANEL"
    }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        createNotificationChannel()
        startForeground(NOTIFICATION_ID, buildNotification())
        setupFloatingWidget()
        startStatusPolling()
    }

    override fun onDestroy() {
        serviceScope.cancel()
        if (::floatingView.isInitialized) {
            windowManager.removeView(floatingView)
        }
        super.onDestroy()
    }

    private fun setupFloatingWidget() {
        windowManager = getSystemService(WINDOW_SERVICE) as WindowManager

        floatingView = LayoutInflater.from(this)
            .inflate(R.layout.floating_agent_widget, null)

        statusIcon = floatingView.findViewById(R.id.agent_icon)
        statusText = floatingView.findViewById(R.id.agent_status_text)

        val params = WindowManager.LayoutParams(
            WindowManager.LayoutParams.WRAP_CONTENT,
            WindowManager.LayoutParams.WRAP_CONTENT,
            WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY,
            WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE,
            PixelFormat.TRANSLUCENT
        ).apply {
            gravity = Gravity.TOP or Gravity.END
            x = 16
            y = 200
        }

        // Drag + tap + long-press handling
        var initialX = 0
        var initialY = 0
        var initialTouchX = 0f
        var initialTouchY = 0f
        var isDragging = false
        var touchDownTime = 0L

        floatingView.setOnTouchListener { _, event ->
            when (event.action) {
                MotionEvent.ACTION_DOWN -> {
                    initialX = params.x
                    initialY = params.y
                    initialTouchX = event.rawX
                    initialTouchY = event.rawY
                    isDragging = false
                    touchDownTime = System.currentTimeMillis()
                    true
                }
                MotionEvent.ACTION_MOVE -> {
                    val dx = (event.rawX - initialTouchX).toInt()
                    val dy = (event.rawY - initialTouchY).toInt()
                    if (dx * dx + dy * dy > 100) isDragging = true
                    params.x = initialX - dx
                    params.y = initialY + dy
                    windowManager.updateViewLayout(floatingView, params)
                    true
                }
                MotionEvent.ACTION_UP -> {
                    if (!isDragging) {
                        val holdTime = System.currentTimeMillis() - touchDownTime
                        if (holdTime > 500) onWidgetLongPressed() else onWidgetTapped()
                    }
                    true
                }
                else -> false
            }
        }

        windowManager.addView(floatingView, params)
    }

    private fun onWidgetTapped() {
        // Open chat bubble overlay (short tap)
        val intent = Intent(this, ChatBubbleService::class.java).apply {
            putExtra(ChatBubbleService.EXTRA_ANCHOR_Y, floatingView.y.toInt())
        }
        startService(intent)
    }

    private fun onWidgetLongPressed() {
        // Long press opens full Agent Panel
        val intent = Intent(this, AgentPanelActivity::class.java).apply {
            addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        }
        startActivity(intent)
    }

    /**
     * Poll agent status and update the floating widget appearance.
     * - Idle: subtle glow, minimal animation
     * - Thinking: pulsing, show "planning..." text
     * - Acting: active animation, show task count
     * - Alert: highlight pulse when proactive trigger fires
     */
    private fun startStatusPolling() {
        serviceScope.launch {
            while (isActive) {
                try {
                    val npuLoad = AgentBridge.getNpuLoad()
                    val agentsJson = AgentBridge.getAgentsState()
                    updateWidgetState(npuLoad, agentsJson)
                } catch (e: Exception) {
                    // Engine not ready yet, show idle
                    updateWidgetState(0f, "{}")
                }
                delay(500) // Update every 500ms
            }
        }
    }

    private fun updateWidgetState(npuLoad: Float, agentsJson: String) {
        val state = when {
            npuLoad > 0.5f -> AgentVisualState.ACTING
            npuLoad > 0.15f -> AgentVisualState.THINKING
            else -> AgentVisualState.IDLE
        }

        statusText.text = when (state) {
            AgentVisualState.IDLE -> ""
            AgentVisualState.THINKING -> "..."
            AgentVisualState.ACTING -> "⚡"
            AgentVisualState.ALERT -> "★"
        }

        // TODO: Animate icon based on state (pulse, glow, etc.)
    }

    private fun createNotificationChannel() {
        val channel = NotificationChannel(
            CHANNEL_ID,
            "Agent Service",
            NotificationManager.IMPORTANCE_LOW
        ).apply {
            description = "OpenSparX Agent is running in background"
        }
        val manager = getSystemService(NotificationManager::class.java)
        manager.createNotificationChannel(channel)
    }

    private fun buildNotification(): Notification {
        val pendingIntent = PendingIntent.getActivity(
            this, 0,
            Intent(this, MainActivity::class.java),
            PendingIntent.FLAG_IMMUTABLE
        )
        return NotificationCompat.Builder(this, CHANNEL_ID)
            .setContentTitle("OpenSparX Agent")
            .setContentText("On-device AI agent active")
            .setSmallIcon(R.drawable.ic_agent)
            .setContentIntent(pendingIntent)
            .setOngoing(true)
            .build()
    }

    enum class AgentVisualState {
        IDLE, THINKING, ACTING, ALERT
    }
}
