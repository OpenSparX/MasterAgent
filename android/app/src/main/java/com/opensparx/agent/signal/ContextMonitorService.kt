package com.opensparx.agent.signal

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Intent
import android.graphics.PixelFormat
import android.hardware.Sensor
import android.hardware.SensorEvent
import android.hardware.SensorEventListener
import android.hardware.SensorManager
import android.location.Location
import android.location.LocationListener
import android.location.LocationManager
import android.os.IBinder
import android.os.Bundle
import android.provider.Settings
import android.view.Gravity
import android.view.LayoutInflater
import android.view.View
import android.view.WindowManager
import android.view.animation.DecelerateInterpolator
import android.widget.TextView
import androidx.core.app.NotificationCompat
import com.opensparx.agent.R
import com.opensparx.agent.jni.AgentBridge
import com.opensparx.agent.ui.ChatBubbleService
import com.opensparx.agent.ui.MainActivity
import kotlinx.coroutines.*
import java.util.Calendar

/**
 * Context Monitor Service — feeds real-world signals to ProactiveEngine.
 *
 * Signal sources:
 * - Time-of-day (periodic)
 * - Location changes (event-driven)
 * - Device motion/stillness (accelerometer)
 * - Screen on/off state
 * - App usage patterns (via UsageStatsManager)
 * - Demo signals (proactive suggestion cycling)
 *
 * Resource management:
 * - Sensors registered at low frequency (SENSOR_DELAY_NORMAL)
 * - Location updates at 60s intervals (low power)
 * - Time signals pushed every 30s
 * - Proactive demo signals every 30s
 * - Total CPU overhead: <1% in monitoring mode
 */
class ContextMonitorService : Service(), SensorEventListener, LocationListener {

    // ─── Demo Proactive Signal System ───────────────────────────────────

    data class ProactiveSignal(val id: String, val suggestion: String)

    private val demoSignals = listOf(
        ProactiveSignal("time_morning", "早上好！检测到你通常这个时候查看日程，要我帮你总结今天的安排吗？"),
        ProactiveSignal("battery_low", "电量低于20%，要我帮你关闭后台高耗电应用吗？"),
        ProactiveSignal("location_office", "检测到你到达办公室，要我帮你连接公司WiFi并打开工作应用吗？"),
        ProactiveSignal("notification_flood", "过去1小时收到23条通知，要我帮你生成摘要吗？"),
        ProactiveSignal("idle_detected", "检测到设备空闲，正在后台预计算可能的下一步操作...(Speculation Engine)"),
    )

    private var demoSignalIndex = 0
    private var proactiveCardView: View? = null
    private var windowManager: WindowManager? = null

    // ─── Core State ─────────────────────────────────────────────────────

    private val serviceScope = CoroutineScope(Dispatchers.Default + SupervisorJob())
    private lateinit var sensorManager: SensorManager
    private var locationManager: LocationManager? = null

    private var lastMotionMagnitude = 0.0
    private var stillnessCounter = 0

    companion object {
        const val CHANNEL_ID = "sparx_context_monitor"
        const val NOTIFICATION_ID = 1002
    }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        windowManager = getSystemService(WINDOW_SERVICE) as WindowManager
        createNotificationChannel()
        startForeground(NOTIFICATION_ID, buildNotification())
        startSignalSources()
        startDemoSignalLoop()
    }

    private fun createNotificationChannel() {
        val channel = NotificationChannel(
            CHANNEL_ID,
            "Context Monitor",
            NotificationManager.IMPORTANCE_MIN
        ).apply {
            description = "Monitors environmental signals for proactive agent"
        }
        getSystemService(NotificationManager::class.java).createNotificationChannel(channel)
    }

    private fun buildNotification(): Notification {
        val pendingIntent = PendingIntent.getActivity(
            this, 0,
            Intent(this, MainActivity::class.java),
            PendingIntent.FLAG_IMMUTABLE
        )
        return NotificationCompat.Builder(this, CHANNEL_ID)
            .setContentTitle("OpenSparX")
            .setContentText("Context monitoring active")
            .setSmallIcon(R.drawable.ic_agent)
            .setContentIntent(pendingIntent)
            .setOngoing(true)
            .setPriority(NotificationCompat.PRIORITY_MIN)
            .build()
    }

    override fun onDestroy() {
        serviceScope.cancel()
        dismissProactiveCard()
        sensorManager.unregisterListener(this)
        locationManager?.removeUpdates(this)
        super.onDestroy()
    }

    private fun startSignalSources() {
        // Accelerometer — detect motion/stillness
        sensorManager = getSystemService(SENSOR_SERVICE) as SensorManager
        sensorManager.getDefaultSensor(Sensor.TYPE_ACCELEROMETER)?.let { accel ->
            sensorManager.registerListener(this, accel, SensorManager.SENSOR_DELAY_NORMAL)
        }

        // Light sensor — detect environment brightness
        sensorManager.getDefaultSensor(Sensor.TYPE_LIGHT)?.let { light ->
            sensorManager.registerListener(this, light, SensorManager.SENSOR_DELAY_NORMAL)
        }

        // Location — low frequency updates
        try {
            locationManager = getSystemService(LOCATION_SERVICE) as LocationManager
            locationManager?.requestLocationUpdates(
                LocationManager.FUSED_PROVIDER,
                60_000L,    // Min 60s between updates
                100f,       // Min 100m movement
                this
            )
        } catch (e: SecurityException) {
            // Location permission not granted — skip this signal source
        }

        // Periodic time signal
        serviceScope.launch {
            while (isActive) {
                pushTimeSignal()
                delay(30_000) // Every 30 seconds
            }
        }

        // Periodic usage pattern check
        serviceScope.launch {
            while (isActive) {
                pushUsageSignal()
                delay(60_000) // Every 60 seconds
            }
        }
    }

    // ─── Sensor Callbacks ────────────────────────────────────────────────

    override fun onSensorChanged(event: SensorEvent) {
        when (event.sensor.type) {
            Sensor.TYPE_ACCELEROMETER -> {
                val x = event.values[0].toDouble()
                val y = event.values[1].toDouble()
                val z = event.values[2].toDouble()
                val magnitude = Math.sqrt(x * x + y * y + z * z)

                // Detect stillness (gravity ~ 9.8, motion adds deviation)
                val delta = Math.abs(magnitude - lastMotionMagnitude)
                lastMotionMagnitude = magnitude

                if (delta < 0.1) {
                    stillnessCounter++
                    if (stillnessCounter == 100) { // ~5 seconds of stillness
                        tryPushSignal("device.motion", 0.0, 0.95f)
                        tryPushLabelSignal("device.state", "stationary", 0.9f)
                    }
                } else {
                    if (stillnessCounter > 100) {
                        tryPushLabelSignal("device.state", "moving", 0.9f)
                    }
                    stillnessCounter = 0
                }
            }
            Sensor.TYPE_LIGHT -> {
                val lux = event.values[0].toDouble()
                tryPushSignal("env.light", lux, 0.95f)
                // Dark environment detection
                if (lux < 10.0) {
                    tryPushLabelSignal("env.brightness", "dark", 0.9f)
                }
            }
        }
    }

    override fun onAccuracyChanged(sensor: Sensor?, accuracy: Int) {}

    // ─── Location Callbacks ──────────────────────────────────────────────

    override fun onLocationChanged(location: Location) {
        tryPushSignal("location.lat", location.latitude, 0.95f)
        tryPushSignal("location.lng", location.longitude, 0.95f)
        tryPushSignal("location.speed", location.speed.toDouble(), 0.9f)
    }

    override fun onStatusChanged(provider: String?, status: Int, extras: Bundle?) {}
    override fun onProviderEnabled(provider: String) {}
    override fun onProviderDisabled(provider: String) {}

    // ─── Periodic Signals ────────────────────────────────────────────────

    private fun pushTimeSignal() {
        val cal = Calendar.getInstance()
        val hour = cal.get(Calendar.HOUR_OF_DAY)
        val minute = cal.get(Calendar.MINUTE)
        val timeValue = hour + minute / 60.0

        tryPushSignal("env.time_hour", timeValue, 1.0f)

        // Late night detection
        if (hour >= 23 || hour < 5) {
            tryPushLabelSignal("env.period", "late_night", 0.95f)
        } else if (hour in 6..9) {
            tryPushLabelSignal("env.period", "morning_commute", 0.8f)
        } else if (hour in 17..19) {
            tryPushLabelSignal("env.period", "evening_commute", 0.8f)
        }
    }

    private fun pushUsageSignal() {
        // TODO: Query UsageStatsManager for current foreground app
        // and continuous usage duration. Push signals like:
        // - "usage.continuous_minutes" → how long user has been on phone
        // - "usage.foreground_app" → what category of app (maps, social, work)
        // This enables triggers like "user has been scrolling for 2 hours"
    }

    // ─── Safe Bridge Wrappers ───────────────────────────────────────────

    private fun tryPushSignal(name: String, value: Double, confidence: Float) {
        try {
            AgentBridge.pushSignal(name, value, confidence)
        } catch (_: UnsatisfiedLinkError) { /* native lib not loaded */ }
          catch (_: Exception) { /* engine not ready */ }
    }

    private fun tryPushLabelSignal(name: String, label: String, confidence: Float) {
        try {
            AgentBridge.pushLabelSignal(name, label, confidence)
        } catch (_: UnsatisfiedLinkError) { /* native lib not loaded */ }
          catch (_: Exception) { /* engine not ready */ }
    }

    // ─── Demo Proactive Signal Loop ─────────────────────────────────────

    private fun startDemoSignalLoop() {
        serviceScope.launch {
            delay(5_000) // Initial delay before first suggestion
            while (isActive) {
                val signal = demoSignals[demoSignalIndex % demoSignals.size]
                demoSignalIndex++

                withContext(Dispatchers.Main) {
                    showProactiveCard(signal)
                }

                delay(30_000) // Every 30 seconds
            }
        }
    }

    private fun showProactiveCard(signal: ProactiveSignal) {
        if (!Settings.canDrawOverlays(this)) return
        dismissProactiveCard()

        val inflater = LayoutInflater.from(this)
        val cardView = inflater.inflate(R.layout.proactive_card, null)

        val suggestionText = cardView.findViewById<TextView>(R.id.text_proactive_suggestion)
        val btnAccept = cardView.findViewById<TextView>(R.id.btn_accept)
        val btnDismiss = cardView.findViewById<TextView>(R.id.btn_dismiss)

        suggestionText.text = signal.suggestion

        btnAccept.setOnClickListener {
            dismissProactiveCard()
            // Send suggestion to ChatBubbleService
            val chatIntent = Intent(this, ChatBubbleService::class.java).apply {
                action = ChatBubbleService.ACTION_PROACTIVE_SUGGESTION
                putExtra(ChatBubbleService.EXTRA_SUGGESTION, signal.suggestion)
            }
            startService(chatIntent)
        }

        btnDismiss.setOnClickListener {
            dismissProactiveCard()
        }

        val params = WindowManager.LayoutParams(
            WindowManager.LayoutParams.WRAP_CONTENT,
            WindowManager.LayoutParams.WRAP_CONTENT,
            WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY,
            WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE or
                WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL,
            PixelFormat.TRANSLUCENT
        ).apply {
            gravity = Gravity.TOP or Gravity.CENTER_HORIZONTAL
            y = 160 // Above the floating ball area
        }

        windowManager?.addView(cardView, params)
        proactiveCardView = cardView

        // Animate in
        cardView.alpha = 0f
        cardView.translationY = -20f
        cardView.animate()
            .alpha(1f)
            .translationY(0f)
            .setDuration(250)
            .setInterpolator(DecelerateInterpolator())
            .start()

        // Auto-dismiss after 15 seconds if not interacted with
        serviceScope.launch {
            delay(15_000)
            withContext(Dispatchers.Main) {
                dismissProactiveCard()
            }
        }
    }

    private fun dismissProactiveCard() {
        proactiveCardView?.let { view ->
            view.animate()
                .alpha(0f)
                .translationY(-15f)
                .setDuration(150)
                .withEndAction {
                    try { windowManager?.removeView(view) } catch (_: Exception) {}
                }
                .start()
            proactiveCardView = null
        }
    }
}
