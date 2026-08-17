package com.opensparx.agent.signal

import android.service.notification.NotificationListenerService
import android.service.notification.StatusBarNotification
import com.opensparx.agent.jni.AgentBridge

/**
 * NotificationSignalService — Monitors incoming notifications as context signals.
 *
 * When a notification arrives (e.g., message, alarm, calendar), the service
 * converts it into a labeled signal for the ProactiveEngine. This allows the
 * agent to be aware of external events without the user explicitly typing.
 *
 * Requires BIND_NOTIFICATION_LISTENER_SERVICE permission (user grants in settings).
 */
class NotificationSignalService : NotificationListenerService() {

    override fun onNotificationPosted(sbn: StatusBarNotification?) {
        sbn ?: return

        val pkg = sbn.packageName ?: return
        val category = sbn.notification?.category ?: "unknown"

        // Map notification to a context signal
        val label = categorize(pkg, category)
        if (label != null) {
            AgentBridge.pushLabelSignal("notification", label, 0.8f)
        }
    }

    override fun onNotificationRemoved(sbn: StatusBarNotification?) {
        // No action needed — we only care about arrivals
    }

    /**
     * Map package + category to a semantic label the ProactiveEngine understands.
     * Returns null for apps we don't want to react to (noise filtering).
     */
    private fun categorize(pkg: String, category: String): String? {
        return when {
            category == "msg" || category == "social" -> "message_received"
            category == "call" -> "incoming_call"
            category == "alarm" -> "alarm_triggered"
            category == "event" || category == "reminder" -> "calendar_event"
            category == "navigation" -> "nav_update"
            pkg.contains("maps") || pkg.contains("navigation") -> "nav_update"
            pkg.contains("weather") -> "weather_alert"
            else -> null // Suppress noise
        }
    }
}
