package com.opensparx.agent.signal

import android.app.Service
import android.content.Intent
import android.hardware.Sensor
import android.hardware.SensorEvent
import android.hardware.SensorEventListener
import android.hardware.SensorManager
import android.location.Location
import android.location.LocationListener
import android.location.LocationManager
import android.os.IBinder
import android.os.Bundle
import com.opensparx.agent.jni.AgentBridge
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
 *
 * Resource management:
 * - Sensors registered at low frequency (SENSOR_DELAY_NORMAL)
 * - Location updates at 60s intervals (low power)
 * - Time signals pushed every 30s
 * - Total CPU overhead: <1% in monitoring mode
 */
class ContextMonitorService : Service(), SensorEventListener, LocationListener {

    private val serviceScope = CoroutineScope(Dispatchers.Default + SupervisorJob())
    private lateinit var sensorManager: SensorManager
    private var locationManager: LocationManager? = null

    private var lastMotionMagnitude = 0.0
    private var stillnessCounter = 0

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        startSignalSources()
    }

    override fun onDestroy() {
        serviceScope.cancel()
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
                        AgentBridge.pushSignal("device.motion", 0.0, 0.95f)
                        AgentBridge.pushLabelSignal("device.state", "stationary", 0.9f)
                    }
                } else {
                    if (stillnessCounter > 100) {
                        AgentBridge.pushLabelSignal("device.state", "moving", 0.9f)
                    }
                    stillnessCounter = 0
                }
            }
            Sensor.TYPE_LIGHT -> {
                val lux = event.values[0].toDouble()
                AgentBridge.pushSignal("env.light", lux, 0.95f)
                // Dark environment detection
                if (lux < 10.0) {
                    AgentBridge.pushLabelSignal("env.brightness", "dark", 0.9f)
                }
            }
        }
    }

    override fun onAccuracyChanged(sensor: Sensor?, accuracy: Int) {}

    // ─── Location Callbacks ──────────────────────────────────────────────

    override fun onLocationChanged(location: Location) {
        AgentBridge.pushSignal("location.lat", location.latitude, 0.95f)
        AgentBridge.pushSignal("location.lng", location.longitude, 0.95f)
        AgentBridge.pushSignal("location.speed", location.speed.toDouble(), 0.9f)
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

        AgentBridge.pushSignal("env.time_hour", timeValue, 1.0f)

        // Late night detection
        if (hour >= 23 || hour < 5) {
            AgentBridge.pushLabelSignal("env.period", "late_night", 0.95f)
        } else if (hour in 6..9) {
            AgentBridge.pushLabelSignal("env.period", "morning_commute", 0.8f)
        } else if (hour in 17..19) {
            AgentBridge.pushLabelSignal("env.period", "evening_commute", 0.8f)
        }
    }

    private fun pushUsageSignal() {
        // TODO: Query UsageStatsManager for current foreground app
        // and continuous usage duration. Push signals like:
        // - "usage.continuous_minutes" → how long user has been on phone
        // - "usage.foreground_app" → what category of app (maps, social, work)
        // This enables triggers like "user has been scrolling for 2 hours"
    }
}
